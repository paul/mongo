// dbclient.cpp - connect to a Mongo database as a database, from C++

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "dbclient.h"
#include "../bson/util/builder.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include "../db/dbmessage.h"
#include "connpool.h"
#include "dbclient_rs.h"
#include "../util/background.h"

namespace mongo {

    // --------------------------------
    // ----- ReplicaSetMonitor ---------
    // --------------------------------

    // global background job responsible for checking every X amount of time
    class ReplicaSetMonitorWatcher : public BackgroundJob {
    protected:
        void run() {
            while ( ! inShutdown() ) {
                sleepsecs( 20 );
                try {
                    ReplicaSetMonitor::checkAll();
                }
                catch ( std::exception& e ) {
                    error() << "ReplicaSetMonitorWatcher: check failed: " << e.what() << endl;
                }
            }
        }

    } replicaSetMonitorWatcher;


    ReplicaSetMonitor::ReplicaSetMonitor( const string& name , const vector<HostAndPort>& servers )
        : _lock( "ReplicaSetMonitor instance" ) , _name( name ) , _master(-1) {

        string errmsg;

        for ( unsigned i=0; i<servers.size(); i++ ) {
            auto_ptr<DBClientConnection> conn( new DBClientConnection( true , 0, 5.0 ) );
            if (!conn->connect( servers[i] , errmsg ) ) {
                log(1) << "error connecting to seed " << servers[i] << ": " << errmsg << endl;
                // skip seeds that don't work
                continue;
            }

            _nodes.push_back( Node( servers[i] , conn.release() ) );

            string maybePrimary;
            if (_checkConnection( _nodes[_nodes.size()-1].conn , maybePrimary, false)) {
                break;
            }
        }
    }

    ReplicaSetMonitor::~ReplicaSetMonitor() {
        for ( unsigned i=0; i<_nodes.size(); i++ )
            delete _nodes[i].conn;
        _nodes.clear();
        _master = -1;
    }

    ReplicaSetMonitorPtr ReplicaSetMonitor::get( const string& name , const vector<HostAndPort>& servers ) {
        scoped_lock lk( _setsLock );
        ReplicaSetMonitorPtr& m = _sets[name];
        if ( ! m )
            m.reset( new ReplicaSetMonitor( name , servers ) );

        if ( replicaSetMonitorWatcher.getState() == BackgroundJob::NotStarted )
            replicaSetMonitorWatcher.go();

        return m;
    }

    void ReplicaSetMonitor::checkAll() {
        set<string> seen;

        while ( true ) {
            ReplicaSetMonitorPtr m;
            {
                for ( map<string,ReplicaSetMonitorPtr>::iterator i=_sets.begin(); i!=_sets.end(); ++i ) {
                    string name = i->first;
                    if ( seen.count( name ) )
                        continue;
                    LOG(0) << "checking replica set: " << name << endl;
                    seen.insert( name );
                    m = i->second;
                    break;
                }
            }

            if ( ! m )
                break;

            m->check();
        }


    }

    void ReplicaSetMonitor::setConfigChangeHook( ConfigChangeHook hook ) {
        massert( 13610 , "ConfigChangeHook already specified" , _hook == 0 );
        _hook = hook;
    }

    string ReplicaSetMonitor::getServerAddress() const {
        StringBuilder ss;
        if ( _name.size() )
            ss << _name << "/";

        {
            scoped_lock lk( _lock );
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                if ( i > 0 )
                    ss << ",";
                ss << _nodes[i].addr.toString();
            }
        }
        return ss.str();
    }

    void ReplicaSetMonitor::notifyFailure( const HostAndPort& server ) {
        if ( _master >= 0 ) {
            scoped_lock lk( _lock );
            if ( server == _nodes[_master].addr )
                _master = -1;
        }
    }



    HostAndPort ReplicaSetMonitor::getMaster() {
        if ( _master < 0 || !_nodes[_master].ok )
            _check();

        uassert( 10009 , str::stream() << "ReplicaSetMonitor no master found for set: " << _name , _master >= 0 );

        scoped_lock lk( _lock );
        return _nodes[_master].addr;
    }

    HostAndPort ReplicaSetMonitor::getSlave() {
        int x = rand() % _nodes.size();
        {
            scoped_lock lk( _lock );
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                int p = ( i + x ) % _nodes.size();
                if ( p == _master )
                    continue;
                if ( _nodes[p].ok )
                    return _nodes[p].addr;
            }
        }

        return _nodes[0].addr;
    }

    /**
     * notify the monitor that server has faild
     */
    void ReplicaSetMonitor::notifySlaveFailure( const HostAndPort& server ) {
        int x = _find( server );
        if ( x >= 0 ) {
            scoped_lock lk( _lock );
            _nodes[x].ok = false;
        }
    }

    void ReplicaSetMonitor::_checkStatus(DBClientConnection *conn) {
        BSONObj status;

        if (!conn->runCommand("admin", BSON("replSetGetStatus" << 1), status) ||
                !status.hasField("members") ||
                status["members"].type() != Array) {
            return;
        }

        BSONObjIterator hi(status["members"].Obj());
        while (hi.more()) {
            BSONObj member = hi.next().Obj();
            string host = member["name"].String();

            int m = -1;
            if ((m = _find(host)) <= 0) {
                continue;
            }

            double state = member["state"].Number();
            if (member["health"].Number() == 1 && (state == 1 || state == 2)) {
                scoped_lock lk( _lock );
                _nodes[m].ok = true;
            }
            else {
                scoped_lock lk( _lock );
                _nodes[m].ok = false;
            }
        }
    }

    void ReplicaSetMonitor::_checkHosts( const BSONObj& hostList, bool& changed ) {
        BSONObjIterator hi(hostList);
        while ( hi.more() ) {
            string toCheck = hi.next().String();

            if ( _find( toCheck ) >= 0 )
                continue;

            HostAndPort h( toCheck );
            DBClientConnection * newConn = new DBClientConnection( true, 0, 5.0 );
            string temp;
            newConn->connect( h , temp );
            {
                scoped_lock lk( _lock );
                _nodes.push_back( Node( h , newConn ) );
            }
            log() << "updated set (" << _name << ") to: " << getServerAddress() << endl;
            changed = true;
        }
    }

    bool ReplicaSetMonitor::_checkConnection( DBClientConnection * c , string& maybePrimary , bool verbose ) {
        bool isMaster = false;
        bool changed = false;
        try {
            BSONObj o;
            c->isMaster(isMaster, &o);

            log( ! verbose ) << "ReplicaSetMonitor::_checkConnection: " << c->toString() << ' ' << o << '\n';

            // add other nodes
            string maybePrimary;
            if ( o["hosts"].type() == Array ) {
                if ( o["primary"].type() == String )
                    maybePrimary = o["primary"].String();

                _checkHosts(o["hosts"].Obj(), changed);
            }
            if (o.hasField("passives") && o["passives"].type() == Array) {
                _checkHosts(o["passives"].Obj(), changed);
            }

            _checkStatus(c);
        }
        catch ( std::exception& e ) {
            log( ! verbose ) << "ReplicaSetMonitor::_checkConnection: caught exception " << c->toString() << ' ' << e.what() << endl;
        }

        if ( changed && _hook )
            _hook( this );

        return isMaster;
    }

    void ReplicaSetMonitor::_check() {

        bool triedQuickCheck = false;

        LOG(1) <<  "_check : " << getServerAddress() << endl;

        for ( int retry = 0; retry < 2; retry++ ) {
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                DBClientConnection * c;
                {
                    scoped_lock lk( _lock );
                    c = _nodes[i].conn;
                }

                string maybePrimary;
                if ( _checkConnection( c , maybePrimary , retry ) ) {
                    _master = i;
                    return;
                }

                if ( ! triedQuickCheck && maybePrimary.size() ) {
                    int x = _find( maybePrimary );
                    if ( x >= 0 ) {
                        triedQuickCheck = true;
                        string dummy;
                        DBClientConnection * testConn;
                        {
                            scoped_lock lk( _lock );
                            testConn = _nodes[x].conn;
                        }
                        if ( _checkConnection( testConn , dummy , false ) ) {
                            _master = x;
                            return;
                        }
                    }
                }

            }
            sleepsecs(1);
        }

    }

    void ReplicaSetMonitor::check() {
        // first see if the current master is fine
        if ( _master >= 0 ) {
            string temp;
            if ( _checkConnection( _nodes[_master].conn , temp , false ) ) {
                // current master is fine, so we're done
                return;
            }
        }

        // we either have no master, or the current is dead
        _check();
    }

    int ReplicaSetMonitor::_find( const string& server ) const {
        scoped_lock lk( _lock );
        for ( unsigned i=0; i<_nodes.size(); i++ )
            if ( _nodes[i].addr == server )
                return i;
        return -1;
    }

    int ReplicaSetMonitor::_find( const HostAndPort& server ) const {
        scoped_lock lk( _lock );
        for ( unsigned i=0; i<_nodes.size(); i++ )
            if ( _nodes[i].addr == server )
                return i;
        return -1;
    }


    mongo::mutex ReplicaSetMonitor::_setsLock( "ReplicaSetMonitor" );
    map<string,ReplicaSetMonitorPtr> ReplicaSetMonitor::_sets;
    ReplicaSetMonitor::ConfigChangeHook ReplicaSetMonitor::_hook;
    // --------------------------------
    // ----- DBClientReplicaSet ---------
    // --------------------------------

    DBClientReplicaSet::DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers )
        : _monitor( ReplicaSetMonitor::get( name , servers ) ) {
    }

    DBClientReplicaSet::~DBClientReplicaSet() {
    }

    DBClientConnection * DBClientReplicaSet::checkMaster() {
        HostAndPort h = _monitor->getMaster();

        if ( h == _masterHost ) {
            // a master is selected.  let's just make sure connection didn't die
            if ( ! _master->isFailed() )
                return _master.get();
            _monitor->notifyFailure( _masterHost );
        }

        _masterHost = _monitor->getMaster();
        _master.reset( new DBClientConnection( true ) );
        _master->connect( _masterHost );
        _auth( _master.get() );
        return _master.get();
    }

    DBClientConnection * DBClientReplicaSet::checkSlave() {
        if ( _slave ) {
            if ( ! _slave->isFailed() )
                return _slave.get();
            _monitor->notifySlaveFailure( _slaveHost );
        }

        HostAndPort h = _monitor->getSlave();
        if ( h != _slaveHost ) {
            _slaveHost = h;
            _slave.reset( new DBClientConnection( true ) );
            _slave->connect( _slaveHost );
            _auth( _slave.get() );
        }
        return _slave.get();
    }


    void DBClientReplicaSet::_auth( DBClientConnection * conn ) {
        for ( list<AuthInfo>::iterator i=_auths.begin(); i!=_auths.end(); ++i ) {
            const AuthInfo& a = *i;
            string errmsg;
            if ( ! conn->auth( a.dbname , a.username , a.pwd , errmsg, a.digestPassword ) )
                warning() << "cached auth failed for set: " << _monitor->getName() << " db: " << a.dbname << " user: " << a.username << endl;

        }

    }

    DBClientConnection& DBClientReplicaSet::masterConn() {
        return *checkMaster();
    }

    DBClientConnection& DBClientReplicaSet::slaveConn() {
        return *checkSlave();
    }

    bool DBClientReplicaSet::connect() {
        try {
            checkMaster();
        }
        catch (AssertionException&) {
            if (_master && _monitor) {
                _monitor->notifyFailure(_masterHost);
            }
            return false;
        }
        return true;
    }

    bool DBClientReplicaSet::auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword ) {
        DBClientConnection * m = checkMaster();

        // first make sure it actually works
        if( ! m->auth(dbname, username, pwd, errmsg, digestPassword ) )
            return false;

        // now that it does, we should save so that for a new node we can auth
        _auths.push_back( AuthInfo( dbname , username , pwd , digestPassword ) );
        return true;
    }

    // ------------- simple functions -----------------

    void DBClientReplicaSet::insert( const string &ns , BSONObj obj ) {
        checkMaster()->insert(ns, obj);
    }

    void DBClientReplicaSet::insert( const string &ns, const vector< BSONObj >& v ) {
        checkMaster()->insert(ns, v);
    }

    void DBClientReplicaSet::remove( const string &ns , Query obj , bool justOne ) {
        checkMaster()->remove(ns, obj, justOne);
    }

    void DBClientReplicaSet::update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi ) {
        return checkMaster()->update(ns, query, obj, upsert,multi);
    }

    auto_ptr<DBClientCursor> DBClientReplicaSet::query(const string &ns, Query query, int nToReturn, int nToSkip,
            const BSONObj *fieldsToReturn, int queryOptions, int batchSize) {

        if ( queryOptions & QueryOption_SlaveOk ) {
            // we're ok sending to a slave
            // we'll try 2 slaves before just using master
            // checkSlave will try a different slave automatically after a failure
            for ( int i=0; i<2; i++ ) {
                try {
                    return checkSlave()->query(ns,query,nToReturn,nToSkip,fieldsToReturn,queryOptions,batchSize);
                }
                catch ( DBException & ) {
                    LOG(1) << "can't query replica set slave: " << _slaveHost << endl;
                }
            }
        }

        return checkMaster()->query(ns,query,nToReturn,nToSkip,fieldsToReturn,queryOptions,batchSize);
    }

    BSONObj DBClientReplicaSet::findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn, int queryOptions) {
        if ( queryOptions & QueryOption_SlaveOk ) {
            // we're ok sending to a slave
            // we'll try 2 slaves before just using master
            // checkSlave will try a different slave automatically after a failure
            for ( int i=0; i<2; i++ ) {
                try {
                    return checkSlave()->findOne(ns,query,fieldsToReturn,queryOptions);
                }
                catch ( DBException & ) {
                    LOG(1) << "can't query replica set slave: " << _slaveHost << endl;
                }
            }
        }

        return checkMaster()->findOne(ns,query,fieldsToReturn,queryOptions);
    }

    void DBClientReplicaSet::killCursor( long long cursorID ) {
        checkMaster()->killCursor( cursorID );
    }


    bool DBClientReplicaSet::call( Message &toSend, Message &response, bool assertOk ) {
        if ( toSend.operation() == dbQuery ) {
            // TODO: might be possible to do this faster by changing api
            DbMessage dm( toSend );
            QueryMessage qm( dm );
            if ( qm.queryOptions & QueryOption_SlaveOk ) {
                for ( int i=0; i<2; i++ ) {
                    try {
                        return checkSlave()->call( toSend , response , assertOk );
                    }
                    catch ( DBException & ) {
                        log(1) << "can't query replica set slave: " << _slaveHost << endl;
                    }
                }
            }
        }
        return checkMaster()->call( toSend , response , assertOk );
    }

}
