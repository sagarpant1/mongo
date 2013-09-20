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

#include "mongo/db/auth/authz_manager_external_state_s.h"

#include <boost/thread/mutex.hpp>
#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/client/dbclientinterface.h"
#include "mongo/client/distlock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/config.h"
#include "mongo/s/type_database.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    static Status userNotFoundStatus(ErrorCodes::UserNotFound, "User not found");
}

    AuthzManagerExternalStateMongos::AuthzManagerExternalStateMongos() {}

    AuthzManagerExternalStateMongos::~AuthzManagerExternalStateMongos() {}

    namespace {
        ScopedDbConnection* getConnectionForAuthzCollection(const std::string& ns) {
            //
            // Note: The connection mechanism here is *not* ideal, and should not be used elsewhere.
            // If the primary for the collection moves, this approach may throw rather than handle
            // version exceptions.
            //

            DBConfigPtr config = grid.getDBConfig(ns);
            Shard s = config->getShard(ns);

            return new ScopedDbConnection(s.getConnString(), 30.0);
        }
    }

    Status AuthzManagerExternalStateMongos::_findUser(const string& usersNamespace,
                                                      const BSONObj& query,
                                                      BSONObj* result) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(usersNamespace));
            *result = conn->get()->findOne(usersNamespace, query).getOwned();
            conn->done();
            if (result->isEmpty()) {
                return userNotFoundStatus;
            }
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::query(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const boost::function<void(const BSONObj&)>& resultProcessor) {
        try {
            scoped_ptr<ScopedDbConnection> conn(
                    getConnectionForAuthzCollection(collectionName.ns()));
            conn->get()->query(resultProcessor, collectionName.ns(), query);
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getAllDatabaseNames(
            std::vector<std::string>* dbnames) {
        try {
            scoped_ptr<ScopedDbConnection> conn(
                    getConnectionForAuthzCollection(DatabaseType::ConfigNS));
            auto_ptr<DBClientCursor> c = conn->get()->query(DatabaseType::ConfigNS, Query());

            while (c->more()) {
                DatabaseType dbInfo;
                std::string errmsg;
                if (!dbInfo.parseBSON( c->nextSafe(), &errmsg) || !dbInfo.isValid( &errmsg )) {
                    return Status(ErrorCodes::FailedToParse, errmsg);
                }
                dbnames->push_back(dbInfo.getName());
            }
            conn->done();
            dbnames->push_back("config"); // config db isn't listed in config.databases
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::getAllV1PrivilegeDocsForDB(
            const std::string& dbname, std::vector<BSONObj>* privDocs) {
        try {
            std::string usersNamespace = dbname + ".system.users";
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(usersNamespace));
            auto_ptr<DBClientCursor> c = conn->get()->query(usersNamespace, Query());

            while (c->more()) {
                privDocs->push_back(c->nextSafe().getOwned());
            }
            conn->done();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::findOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObj* result) {
        fassertFailed(17101);
    }

    Status AuthzManagerExternalStateMongos::insert(
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj& writeConcern) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));

            conn->get()->insert(collectionName, document);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string errstr = conn->get()->getLastErrorString(res);
            conn->done();

            if (errstr.empty()) {
                return Status::OK();
            }
            if (res.hasField("code") && res["code"].Int() == ASSERT_ID_DUPKEY) {
                return Status(ErrorCodes::DuplicateKey, errstr);
            }
            return Status(ErrorCodes::UnknownError, errstr);
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::updateOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& updatePattern,
            bool upsert,
            const BSONObj& writeConcern) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));

            conn->get()->update(collectionName, query, updatePattern, upsert);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

            if (!err.empty()) {
                return Status(ErrorCodes::UnknownError, err);
            }

            int numUpdated = res["n"].numberInt();
            dassert(numUpdated <= 1 && numUpdated >= 0);
            if (numUpdated == 0) {
                return Status(ErrorCodes::NoMatchingDocument, "No document found");
            }

            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::remove(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& writeConcern,
            int* numRemoved) {
        try {
            scoped_ptr<ScopedDbConnection> conn(getConnectionForAuthzCollection(collectionName));

            conn->get()->remove(collectionName, query);

            // Handle write concern
            BSONObjBuilder gleBuilder;
            gleBuilder.append("getLastError", 1);
            gleBuilder.appendElements(writeConcern);
            BSONObj res;
            conn->get()->runCommand("admin", gleBuilder.done(), res);
            string err = conn->get()->getLastErrorString(res);
            conn->done();

            if (!err.empty()) {
                return Status(ErrorCodes::UnknownError, err);
            }

            *numRemoved = res["n"].numberInt();
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    Status AuthzManagerExternalStateMongos::createIndex(
            const NamespaceString& collectionName,
            const BSONObj& pattern,
            bool unique,
            const BSONObj& writeConcern) {
        fassertFailed(17105);
    }

    Status AuthzManagerExternalStateMongos::dropCollection(
            const NamespaceString& collectionName, const BSONObj& writeConcern) {
        fassertFailed(17106);
    }

    Status AuthzManagerExternalStateMongos::renameCollection(
            const NamespaceString& oldName,
            const NamespaceString& newName,
            const BSONObj& writeConcern) {
        fassertFailed(17107);
    }

    Status AuthzManagerExternalStateMongos::copyCollection(
            const NamespaceString& fromName,
            const NamespaceString& toName,
            const BSONObj& writeConcern) {
        fassertFailed(17108);
    }

    bool AuthzManagerExternalStateMongos::tryAcquireAuthzUpdateLock(const StringData& why) {
        boost::lock_guard<boost::mutex> lkLocal(_distLockGuard);
        if (_authzDataUpdateLock.get()) {
            return false;
        }

        // Temporarily put into an auto_ptr just in case there is an exception thrown during
        // lock acquisition.
        std::auto_ptr<ScopedDistributedLock> lockHolder(new ScopedDistributedLock(
                configServer.getConnectionString(), "authorizationData"));
        lockHolder->setLockMessage(why.toString());

        std::string errmsg;
        if (!lockHolder->tryAcquire(&errmsg)) {
            warning() <<
                    "Error while attempting to acquire distributed lock for user modification: " <<
                    errmsg << endl;
            return false;
        }
        _authzDataUpdateLock.reset(lockHolder.release());
        return true;
    }

    void AuthzManagerExternalStateMongos::releaseAuthzUpdateLock() {
        boost::lock_guard<boost::mutex> lkLocal(_distLockGuard);
        _authzDataUpdateLock.reset();
    }

} // namespace mongo
