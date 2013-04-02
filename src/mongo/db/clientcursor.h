/* clientcursor.h */

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our database
   application's perspective.
*/

#pragma once

#include "mongo/pch.h"

#include <boost/thread/recursive_mutex.hpp>

#include "cursor.h"
#include "jsobj.h"
#include "../util/net/message.h"
#include "../util/net/listen.h"
#include "../util/background.h"
#include "dbhelpers.h"
#include "matcher.h"
#include "projection.h"
#include "s/d_chunk_manager.h"

namespace mongo {

    typedef boost::recursive_mutex::scoped_lock recursive_scoped_lock;
    typedef long long CursorId; /* passed to the client so it can send back on getMore */
    static const CursorId INVALID_CURSOR_ID = -1; // But see SERVER-5726.
    class Cursor; /* internal server cursor base class */
    class ClientCursor;
    class ParsedQuery;

    /* todo: make this map be per connection.  this will prevent cursor hijacking security attacks perhaps.
     *       ERH: 9/2010 this may not work since some drivers send getMore over a different connection
    */
    typedef map<CursorId, ClientCursor*> CCById;

    extern BSONObj id_obj;

    class ClientCursor : private boost::noncopyable {
        friend class CmdCursorInfo;
    public:
        static void assertNoCursors();

        /* use this to assure we don't in the background time out cursor while it is under use.
           if you are using noTimeout() already, there is no risk anyway.
           Further, this mechanism guards against two getMore requests on the same cursor executing
           at the same time - which might be bad.  That should never happen, but if a client driver
           had a bug, it could (or perhaps some sort of attack situation).
        */
        class Pin : boost::noncopyable {
        public:
            Pin( long long cursorid ) :
                _cursorid( INVALID_CURSOR_ID ) {
                recursive_scoped_lock lock( ccmutex );
                ClientCursor *cursor = ClientCursor::find_inlock( cursorid, true );
                if ( cursor ) {
                    uassert( 12051, "clientcursor already in use? driver problem?",
                            cursor->_pinValue < 100 );
                    cursor->_pinValue += 100;
                    _cursorid = cursorid;
                }
            }
            void release() {
                ClientCursor *cursor = c();
                _cursorid = INVALID_CURSOR_ID;
                if ( cursor ) {
                    verify( cursor->_pinValue >= 100 );
                    cursor->_pinValue -= 100;
                }
            }
            ~Pin() { DESTRUCTOR_GUARD( release(); ) }
            ClientCursor *c() const { return ClientCursor::find( _cursorid ); }
        private:
            CursorId _cursorid;
        };

        /** Assures safe and reliable cleanup of a ClientCursor. */
        class Holder : boost::noncopyable {
        public:
            Holder( ClientCursor *c = 0 ) :
                _c( 0 ),
                _id( INVALID_CURSOR_ID ) {
                reset( c );
            }
            void reset( ClientCursor *c = 0 ) {
                if ( c == _c )
                    return;
                if ( _c ) {
                    // be careful in case cursor was deleted by someone else
                    ClientCursor::erase( _id );
                }
                if ( c ) {
                    _c = c;
                    _id = c->_cursorid;
                }
                else {
                    _c = 0;
                    _id = INVALID_CURSOR_ID;
                }
            }
            ~Holder() {
                DESTRUCTOR_GUARD ( reset(); );
            }
            operator bool() { return _c; }
            ClientCursor * operator-> () { return _c; }
            const ClientCursor * operator-> () const { return _c; }
            /** Release ownership of the ClientCursor. */
            void release() {
                _c = 0;
                _id = INVALID_CURSOR_ID;
            }
        private:
            ClientCursor *_c;
            CursorId _id;
        };

        /**
         * Iterates through all ClientCursors, under its own ccmutex lock.
         * Also supports deletion on the fly.
         */
        class LockedIterator : boost::noncopyable {
        public:
            LockedIterator() : _lock( ccmutex ), _i( clientCursorsById.begin() ) {}
            bool ok() const { return _i != clientCursorsById.end(); }
            ClientCursor *current() const { return _i->second; }
            void advance() { ++_i; }
            /**
             * Delete 'current' and advance. Properly handles cascading deletions that may occur
             * when one ClientCursor is directly deleted.
             */
            void deleteAndAdvance();
        private:
            recursive_scoped_lock _lock;
            CCById::const_iterator _i;
        };
        
        ClientCursor(int queryOptions, const shared_ptr<Cursor>& c, const string& ns, BSONObj query = BSONObj() );

        ~ClientCursor();

        // ***************  basic accessors *******************

        CursorId cursorid() const { return _cursorid; }
        string ns() const { return _ns; }
        Database * db() const { return _db; }
        const BSONObj& query() const { return _query; }
        int queryOptions() const { return _queryOptions; }

        /* Get rid of cursors for namespaces 'ns'. When dropping a db, ns is "dbname."
           Used by drop, dropIndexes, dropDatabase.
        */
        static void invalidate(const char *ns);

        // --- some pass through helpers for Cursor ---

        Cursor* c() const { return _c.get(); }
        int pos() const { return _pos; }

        void incPos( int n ) { _pos += n; } // TODO: this is bad
        void setPos( int n ) { _pos = n; } // TODO : this is bad too

        BSONObj indexKeyPattern() { return _c->indexKeyPattern();  }
        bool modifiedKeys() const { return _c->modifiedKeys(); }
        bool isMultiKey() const { return _c->isMultiKey(); }

        bool ok() { return _c->ok(); }
        bool advance() { return _c->advance(); }
        BSONObj current() { return _c->current(); }
        BSONObj currPK() { return _c->currPK(); }
        BSONObj currKey() const { return _c->currKey(); }

        /**
         * same as BSONObj::getFieldsDotted
         * if it can be retrieved from key, it is
         * @param holder keeps the currKey in scope by keeping a reference to it here. generally you'll want 
         *        holder and ret to destruct about the same time.
         * @return if this was retrieved from key
         */
        bool getFieldsDotted( const string& name, BSONElementSet &ret, BSONObj& holder );

        /**
         * same as BSONObj::getFieldDotted
         * if it can be retrieved from key, it is
         * @return if this was retrieved from key
         */
        BSONElement getFieldDotted( const string& name , BSONObj& holder , bool * fromKey = 0 ) ;
        
        /** extract items from object which match a pattern object.
         * e.g., if pattern is { x : 1, y : 1 }, builds an object with
         * x and y elements of this object, if they are present.
         * returns elements with original field names
         * NOTE: copied from BSONObj::extractFields
        */
        BSONObj extractFields(const BSONObj &pattern , bool fillWithNull = false) ;

        void fillQueryResultFromObj( BufBuilder &b, const MatchDetails* details = NULL ) const;

        bool currentIsDup() {
            return _c->getsetdup( _c->currPK() );
        }

        bool currentMatches() {
            if ( ! _c->matcher() )
                return true;
            return _c->matcher()->matchesCurrent( _c.get() );
        }

        void setChunkManager( ShardChunkManagerPtr manager ){ _chunkManager = manager; }
        ShardChunkManagerPtr getChunkManager(){ return _chunkManager; }

    private:
        static ClientCursor* find_inlock(CursorId id, bool warn = true) {
            CCById::iterator it = clientCursorsById.find(id);
            if ( it == clientCursorsById.end() ) {
                if ( warn )
                    OCCASIONALLY out() << "ClientCursor::find(): cursor not found in map " << id << " (ok after a drop)\n";
                return 0;
            }
            return it->second;
        }
        
    public:
        static ClientCursor* find(CursorId id, bool warn = true) {
            recursive_scoped_lock lock(ccmutex);
            ClientCursor *c = find_inlock(id, warn);
            // if this asserts, your code was not thread safe - you either need to set no timeout
            // for the cursor or keep a ClientCursor::Pointer in scope for it.
            massert( 12521, "internal error: use of an unlocked ClientCursor", c == 0 || c->_pinValue );
            return c;
        }

        /**
         * Deletes the cursor with the provided @param 'id' if one exists.
         * @throw if the cursor with the provided id is pinned.
         */
        static bool erase(CursorId id);

        /**
         * @return number of cursors found
         */
        static int erase( int n , long long * ids );

        /**
         * @param millis amount of idle passed time since last call
         */
        bool shouldTimeout( unsigned millis );

        //void storeOpForSlave( DiskLoc last );
        void updateSlaveLocation( CurOp& curop );

        unsigned idleTime() const { return _idleAgeMillis; }

        void slaveReadTill( const OpTime& t ) { _slaveReadTill = t; }
        
        /** Just for testing. */
        OpTime getSlaveReadTill() const { return _slaveReadTill; }

    public: // static methods

        static void idleTimeReport(unsigned millis);

        static void appendStats( BSONObjBuilder& result );
        static unsigned numCursors() { return clientCursorsById.size(); }
        static void find( const string& ns , set<CursorId>& all );

    public:

        // The Cursor destructor closes its ydb cursor, which reqquires that the
        // transaction that created it be live. Becuse of this, the transaction
        // stack pointer needs to stay in scope around the cursor, and so we
        // declare it first.
        shared_ptr<Client::TransactionStack> transactions; // the transaction this cursor is under,
                                                           // only set to support getMore() 

    private: // methods

        // cursors normally timeout after an inactivy period to prevent excess memory use
        // setting this prevents timeout of the cursor in question.
        void noTimeout() { _pinValue++; }

        CursorId _cursorid;

        const string _ns;
        Database * _db;

        const shared_ptr<Cursor> _c;
        map<string,int> _indexedFields;  // map from indexed field to offset in key object
        int _pos;                        // # objects into the cursor so far

        const BSONObj _query;            // used for logging diags only; optional in constructor
        int _queryOptions;        // see enum QueryOptions dbclient.h

        OpTime _slaveReadTill;

        unsigned _idleAgeMillis;                 // how long has the cursor been around, relative to server idle time

        /* 0 = normal
           1 = no timeout allowed
           100 = in use (pinned) -- see Pointer class
        */
        unsigned _pinValue;

        ShardChunkManagerPtr _chunkManager;

    public:
        shared_ptr<ParsedQuery> pq;
        shared_ptr<Projection> fields; // which fields query wants returned

    private: // static members

        static CCById clientCursorsById;
        static long long numberTimedOut;
        static boost::recursive_mutex& ccmutex;   // must use this for all statics above!
        static CursorId allocCursorId_inlock();

    };

    class ClientCursorMonitor : public BackgroundJob {
    public:
        string name() const { return "ClientCursorMonitor"; }
        void run();
    };

} // namespace mongo

// ClientCursor should only be used with auto_ptr because it needs to be
// release()ed after a yield if stillOk() returns false and these pointer types
// do not support releasing. This will prevent them from being used accidentally
// Instead of auto_ptr<>, which still requires some degree of manual management
// of this, consider using ClientCursor::Holder which handles ClientCursor's
// unusual self-deletion mechanics.
namespace boost{
    template<> class scoped_ptr<mongo::ClientCursor> {};
    template<> class shared_ptr<mongo::ClientCursor> {};
}
