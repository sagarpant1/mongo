/*
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/auth/authz_manager_external_state_mock.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespacestring.h"
// #include "mongo/db/ops/update_driver.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/map_util.h"

namespace mongo {

    Status AuthzManagerExternalStateMock::updatePrivilegeDocument(const UserName& user,
                                                                  const BSONObj& updateObj,
                                                                  const BSONObj&) {
        return Status(ErrorCodes::InternalError, "Not implemented in mock.");
    }

    Status AuthzManagerExternalStateMock::removePrivilegeDocuments(const BSONObj& query,
                                                                   const BSONObj&,
                                                                   int* numRemoved) {
        return Status(ErrorCodes::InternalError, "Not implemented in mock.");
    }

    Status AuthzManagerExternalStateMock::insertPrivilegeDocument(const std::string& dbname,
                                                                  const BSONObj& userObj,
                                                                  const BSONObj& writeConcern) {
        NamespaceString usersCollection("admin.system.users");
        return insert(usersCollection, userObj, writeConcern);
    }

    void AuthzManagerExternalStateMock::clearPrivilegeDocuments() {
        _documents.clear();
    }

    Status AuthzManagerExternalStateMock::getAllDatabaseNames(
            std::vector<std::string>* dbnames) {
        unordered_set<std::string> dbnameSet;
        NamespaceDocumentMap::const_iterator it;
        for (it = _documents.begin(); it != _documents.end(); ++it) {
            dbnameSet.insert(it->first.db);
        }
        *dbnames = std::vector<std::string>(dbnameSet.begin(), dbnameSet.end());
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::getAllV1PrivilegeDocsForDB(
            const std::string& dbname, BSONObjCollection* privDocs) {
        NamespaceDocumentMap::const_iterator iter =
            _documents.find(NamespaceString(dbname + ".system.users"));
        if (iter == _documents.end())
            return Status::OK();  // No system.users collection in DB "dbname".
        const BSONObjCollection& dbDocs = iter->second;
        for (BSONObjCollection::const_iterator it = dbDocs.begin(); it != dbDocs.end(); ++it) {
            privDocs->push_back(*it);
        }
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::_findUser(
            const std::string& usersNamespace,
            const BSONObj& query,
            BSONObj* result) {
        if (!findOne(NamespaceString(usersNamespace), query, result).isOK()) {
            return Status(ErrorCodes::UserNotFound,
                          "No matching user for query " + query.toString());
        }
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::findOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObj* result) {
        BSONObjCollection::iterator iter;
        Status status = _findOneIter(collectionName, query, &iter);
        if (!status.isOK())
            return status;
        *result = iter->copy();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::query(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const boost::function<void(const BSONObj&)>& resultProcessor) {
        std::vector<BSONObjCollection::iterator> iterVector;
        Status status = _queryVector(collectionName, query, &iterVector);
        if (!status.isOK()) {
            return status;
        }
        for (std::vector<BSONObjCollection::iterator>::iterator it = iterVector.begin();
                it != iterVector.end(); ++it) {
            resultProcessor(**it);
        }
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::insert(
            const NamespaceString& collectionName,
            const BSONObj& document,
            const BSONObj&) {
        _documents[collectionName].push_back(document.copy());
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::updateOne(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj& updatePattern,
            bool upsert,
            const BSONObj& writeConcern) {
// TODO: switch to the mongo implementation once we merge update changes
#if 1

        msgasserted(17378, "AuthzManagerExternalStateMock::updateOne unimplemented pending update driver merge");

#else

        namespace mmb = mutablebson;
        UpdateDriver::Options updateOptions;
        updateOptions.upsert = upsert;
        UpdateDriver driver(updateOptions);
        Status status = driver.parse(updatePattern);
        if (!status.isOK())
            return status;

        BSONObjCollection::iterator iter;
        status = _findOneIter(collectionName, query, &iter);
        mmb::Document document;
        if (status.isOK()) {
            document.reset(*iter, mmb::Document::kInPlaceDisabled);
            status = driver.update(StringData(), &document, NULL);
            if (!status.isOK())
                return status;
            *iter = document.getObject().copy();
            return Status::OK();
        }
        else if (status == ErrorCodes::NoMatchingDocument && upsert) {
            if (query.hasField("_id")) {
                document.root().appendElement(query["_id"]);
            }
            status = driver.createFromQuery(query, document);
            if (!status.isOK()) {
                return status;
            }
            status = driver.update(StringData(), &document, NULL);
            if (!status.isOK()) {
                return status;
            }
            return insert(collectionName, document.getObject(), writeConcern);
        }
        else {
            return status;
        }

#endif
    }

    Status AuthzManagerExternalStateMock::remove(
            const NamespaceString& collectionName,
            const BSONObj& query,
            const BSONObj&,
            int* numRemoved) {
        int n = 0;
        BSONObjCollection::iterator iter;
        while (_findOneIter(collectionName, query, &iter).isOK()) {
            _documents[collectionName].erase(iter);
            ++n;
        }
        *numRemoved = n;
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::createIndex(
            const NamespaceString& collectionName,
            const BSONObj& pattern,
            bool unique,
            const BSONObj&) {
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::dropCollection(const NamespaceString& collectionName,
                                                         const BSONObj&) {
        _documents.erase(collectionName);
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::renameCollection(const NamespaceString& oldName,
                                                           const NamespaceString& newName,
                                                           const BSONObj& writeConcern) {
        if (_documents.count(oldName) == 0) {
            return Status(ErrorCodes::NamespaceNotFound,
                          "No collection to rename named " + oldName.ns());
        }
        std::swap(_documents[newName], _documents[oldName]);
        return dropCollection(oldName, writeConcern);
    }

    Status AuthzManagerExternalStateMock::copyCollection(const NamespaceString& fromName,
                                                         const NamespaceString& toName,
                                                         const BSONObj&) {
        if (_documents.count(fromName) == 0) {
            return Status(ErrorCodes::NamespaceNotFound,
                          "No collection to copy named " + fromName.ns());
        }
        if (_documents.count(toName) > 0) {
            return Status(ErrorCodes::NamespaceExists,
                          "Cannot copy into existing namespace " + fromName.ns());
        }

        _documents[toName] = _documents[fromName];
        return Status::OK();
    }

    bool AuthzManagerExternalStateMock::tryAcquireAuthzUpdateLock(const StringData&) {
        return true;
    }

    void AuthzManagerExternalStateMock::releaseAuthzUpdateLock() {}

    std::vector<BSONObj> AuthzManagerExternalStateMock::getCollectionContents(
            const NamespaceString& collectionName) {
        return mapFindWithDefault(_documents, collectionName, std::vector<BSONObj>());
    }

    Status AuthzManagerExternalStateMock::_findOneIter(
            const NamespaceString& collectionName,
            const BSONObj& query,
            BSONObjCollection::iterator* result) {
        std::vector<BSONObjCollection::iterator> iterVector;
        Status status = _queryVector(collectionName, query, &iterVector);
        if (!status.isOK()) {
            return status;
        }
        if (!iterVector.size()) {
            return Status(ErrorCodes::NoMatchingDocument, "No matching document");
        }
        *result = iterVector.front();
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::_queryVector(
            const NamespaceString& collectionName,
            const BSONObj& query,
            std::vector<BSONObjCollection::iterator>* result) {

        StatusWithMatchExpression parseResult = MatchExpressionParser::parse(query);
        if (!parseResult.isOK()) {
            return parseResult.getStatus();
        }
        MatchExpression* matcher = parseResult.getValue();

        NamespaceDocumentMap::iterator mapIt = _documents.find(collectionName);
        if (mapIt == _documents.end())
            return Status(ErrorCodes::NoMatchingDocument,
                          "No collection named " + collectionName.ns());

        for (BSONObjCollection::iterator vecIt = mapIt->second.begin();
             vecIt != mapIt->second.end();
             ++vecIt) {

            if (matcher->matchesBSON(*vecIt)) {
                result->push_back(vecIt);
            }
        }
        return Status::OK();
    }

} // namespace mongo
