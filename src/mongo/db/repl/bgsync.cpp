/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs_sync.h"

namespace mongo {
    BackgroundSync* BackgroundSync::s_instance = 0;
    boost::mutex BackgroundSync::s_mutex;


    BackgroundSync::BackgroundSync() : _opSyncShouldRun(false),
                                            _opSyncRunning(false),
                                            _currentSyncTarget(NULL)
    {
    }

    BackgroundSync::QueueCounter::QueueCounter() : waitTime(0), numElems(0) {
    }

    BackgroundSync* BackgroundSync::get() {
        boost::unique_lock<boost::mutex> lock(s_mutex);
        if (s_instance == NULL && !inShutdown()) {
            s_instance = new BackgroundSync();
        }
        return s_instance;
    }

    BSONObj BackgroundSync::getCounters() {
        BSONObjBuilder counters;
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            counters.appendIntOrLL("waitTimeMs", _queueCounter.waitTime);
            counters.append("numElems", _queueCounter.numElems);
        }
        return counters.obj();
    }

    void BackgroundSync::shutdown() {
    }

    void BackgroundSync::applierThread() {
        Client::initThread("applier");
        replLocalAuth();
        applyOpsFromOplog();
        cc().shutdown();
    }

    void BackgroundSync::applyOpsFromOplog() {
        GTID lastLiveGTID;
        GTID lastUnappliedGTID;
        while (1) {
            _mutex.lock();
            // wait until we know an item has been produced
            while (_queueCounter.numElems == 0) {
                _queueDone.notify_all();
                _queueCond.wait(_mutex);
            }

            theReplSet->gtidManager->getLiveGTIDs(
                &lastLiveGTID, 
                &lastUnappliedGTID
                );
            dassert(GTID::cmp(lastUnappliedGTID, lastLiveGTID) < 0);
            _mutex.unlock();

            // at this point, we know we have data that has not been caught up
            // with the producer, because queueCounter.numElems > 0
            // we apply it
            Client::Transaction transaction(DB_READ_UNCOMMITTED);
            BSONObjBuilder query;
            addGTIDToBSON("$gt", lastUnappliedGTID, query);
            shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor(
                rsoplog, 
                query.done()
                );

            while( c->ok() ) {
                if (c->currentMatches()) {
                    BSONObj curr = c->current();
                    GTID currEntry = getGTIDFromOplogEntry(curr);
                    theReplSet->gtidManager->noteApplyingGTID(currEntry);
                    applyTransactionFromOplog(curr);
                    
                    _mutex.lock();
                    theReplSet->gtidManager->noteGTIDApplied(currEntry);
                    dassert(_queueCounter.numElems > 0);
                    _queueCounter.numElems--;
                    // this is a flow control mechanism, with bad numbers
                    // hard coded for now just to get something going.
                    // If the opSync thread notices that we have over 20000
                    // transactions in the queue, it waits until we get below
                    // 10000. This is where we signal that we have gotten there
                    // Once we have spilling of transactions working, this
                    // logic will need to be redone
                    if (_queueCounter.numElems == 10000) {
                        _queueCond.notify_all();
                    }
                    _mutex.unlock();
                }
                c->advance();
            }
            transaction.commit(0);
        }
    }
    
    void BackgroundSync::producerThread() {
        Client::initThread("rsBackgroundSync");
        replLocalAuth();
        uint32_t timeToSleep = 0;

        while (!inShutdown()) {
            if (timeToSleep) {
                {
                    boost::unique_lock<boost::mutex> lck(_mutex);
                    _opSyncRunning = false;
                    // notify other threads that we are not running
                    _opSyncRunningCondVar.notify_all();
                }
                sleepsecs(timeToSleep);
                timeToSleep = 0;
            }
            {
                boost::unique_lock<boost::mutex> lck(_mutex);
                _opSyncRunning = false;

                while (!_opSyncShouldRun) {
                    // notify other threads that we are not running
                    _opSyncRunningCondVar.notify_all();
                    // wait for permission that we can run
                    _opSyncCanRunCondVar.wait(lck);
                }

                // notify other threads that we are running
                _opSyncRunningCondVar.notify_all();
                _opSyncRunning = true;
            }

            if (!theReplSet) {
                log() << "replSet warning did not receive a valid config yet, sleeping 20 seconds " << rsLog;
                timeToSleep = 20;
                continue;
            }

            try {
                MemberState state = theReplSet->state();
                if (state.fatal() || state.startup()) {
                    timeToSleep = 5;
                    continue;
                }

                produce();
            }
            catch (DBException& e) {
                sethbmsg(str::stream() << "db exception in producer: " << e.toString());
                timeToSleep = 10;
            }
            catch (std::exception& e2) {
                sethbmsg(str::stream() << "exception in producer: " << e2.what());
                timeToSleep = 60;
            }
        }

        cc().shutdown();
    }

    // returns number of seconds to sleep, if any
    uint32_t BackgroundSync::produce() {
        // this oplog reader does not do a handshake because we don't want the server it's syncing
        // from to track how far it has synced
        OplogReader r(false /* doHandshake */);

        // find a target to sync from the last op time written
        getOplogReader(r);

        // no server found
        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (_currentSyncTarget == NULL) {
                lock.unlock();
                // if there is no one to sync from
                return 1; //sleep one second
            }
            r.tailingQueryGTE(rsoplog, _lastGTIDFetched);
        }

        // if target cut connections between connecting and querying (for
        // example, because it stepped down) we might not have a cursor
        if (!r.haveCursor()) {
            return 0;
        }

        if (isRollbackRequired(r)) {
            return 0;
        }

        while (!inShutdown()) {
            while (!inShutdown()) {
                if (!r.moreInCurrentBatch()) {
                    // check to see if we have a request to sync
                    // from a specific target. If so, get out so that
                    // we can restart the act of syncing and
                    // do so from the correct target
                    if (theReplSet->gotForceSync()) {
                        return 0;
                    }

                    // if we are the primary, get out
                    // TODO: this should not be checked here
                    // if we get here and are the primary, something went wrong
                    if (theReplSet->isPrimary()) {
                        return 0;
                    }

                    {
                        boost::unique_lock<boost::mutex> lock(_mutex);
                        if (!_currentSyncTarget || !_currentSyncTarget->hbinfo().hbstate.readable()) {
                            return 0;
                        }
                    }

                    r.more();
                }

                if (!r.more()) {
                    break;
                }

                // This is the operation we have received from the target
                // that we must put in our oplog with an applied field of false
                BSONObj o = r.nextSafe().getOwned();
                Timer timer;
                replicateTransactionToOplog(o);
                GTID currEntry = getGTIDFromOplogEntry(o);
                {
                    boost::unique_lock<boost::mutex> lock(_mutex);
                    // update counters
                    theReplSet->gtidManager->noteGTIDAdded(currEntry);
                    _queueCounter.waitTime += timer.millis();
                    // notify applier thread that data exists
                    if (_queueCounter.numElems == 0) {
                        _queueCond.notify_all();
                    }
                    _queueCounter.numElems++;
                    // this is a flow control mechanism, with bad numbers
                    // hard coded for now just to get something going.
                    // If the opSync thread notices that we have over 20000
                    // transactions in the queue, it waits until we get below
                    // 10000. This is where we wait if we get too high
                    // Once we have spilling of transactions working, this
                    // logic will need to be redone
                    if (_queueCounter.numElems > 20000) {
                        _queueCond.wait(lock);
                    }
                }
            } // end while

            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                if (!_currentSyncTarget || !_currentSyncTarget->hbinfo().hbstate.readable()) {
                    return 0;
                }
            }


            r.tailCheck();
            if( !r.haveCursor() ) {
                LOG(1) << "replSet end opSync pass" << rsLog;
                return 0;
            }

            // looping back is ok because this is a tailable cursor
        }
        return 0;
    }

    bool BackgroundSync::isStale(OplogReader& r, BSONObj& remoteOldestOp) {
        remoteOldestOp = r.findOne(rsoplog, Query());
        GTID remoteOldestGTID = getGTIDFromBSON("_id", remoteOldestOp);
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            GTID currLiveState = theReplSet->gtidManager->getLiveState();
            if (GTID::cmp(currLiveState, remoteOldestGTID) <= 0) {
                return true;
            }
        }
        return false;
    }

    void BackgroundSync::getOplogReader(OplogReader& r) {
        Member *target = NULL, *stale = NULL;
        BSONObj oldest;

        verify(r.conn() == NULL);
        while ((target = theReplSet->getMemberToSyncTo()) != NULL) {
            string current = target->fullName();

            if (!r.connect(current)) {
                LOG(2) << "replSet can't connect to " << current << " to read operations" << rsLog;
                r.resetConnection();
                theReplSet->veto(current);
                continue;
            }

            if (isStale(r, oldest)) {
                r.resetConnection();
                theReplSet->veto(current, 600);
                stale = target;
                continue;
            }

            // if we made it here, the target is up and not stale
            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _currentSyncTarget = target;
            }

            return;
        }

        // the only viable sync target was stale
        if (stale) {
            GTID remoteOldestGTID = getGTIDFromBSON("_id", oldest);
            theReplSet->goStale(stale, remoteOldestGTID);
            sleepsecs(120);
        }

        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            _currentSyncTarget = NULL;
        }
    }

    bool BackgroundSync::isRollbackRequired(OplogReader& r) {
        // TODO: reimplement this
        ::abort();
        return false;
    }

    Member* BackgroundSync::getSyncTarget() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _currentSyncTarget;
    }

    void BackgroundSync::stopOpSyncThread() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        _opSyncShouldRun = false;
        while (_opSyncRunning) {
            _opSyncRunningCondVar.wait(lock);
        }
        // sanity checks
        verify(!_opSyncShouldRun);

        // wait for all things to be applied
        while (_queueCounter.numElems > 0) {
            _queueDone.wait(lock);
        }

        // do a sanity check on the GTID Manager
        GTID lastLiveGTID;
        GTID lastUnappliedGTID;
        theReplSet->gtidManager->getLiveGTIDs(
            &lastLiveGTID, 
            &lastUnappliedGTID
            );
        dassert(GTID::cmp(lastUnappliedGTID, lastLiveGTID) == 0);
    }

    void BackgroundSync::startOpSyncThread() {
        boost::unique_lock<boost::mutex> lock(_mutex);

        // do a sanity check on the GTID Manager
        GTID lastLiveGTID;
        GTID lastUnappliedGTID;
        theReplSet->gtidManager->getLiveGTIDs(
            &lastLiveGTID, 
            &lastUnappliedGTID
            );
        dassert(GTID::cmp(lastUnappliedGTID, lastLiveGTID) == 0);

        verify(_queueCounter.numElems == 0);
        _opSyncShouldRun = true;
        _opSyncCanRunCondVar.notify_all();
        while (!_opSyncRunning) {
            _opSyncRunningCondVar.wait(lock);
        }
        // sanity check that no one has changed this variable
        verify(_opSyncShouldRun);
    }

} // namespace mongo
