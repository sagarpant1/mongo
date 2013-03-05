// index.h

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

#pragma once

#include "mongo/pch.h"

#include <db.h>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/indexkey.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace.h"
#include "mongo/db/storage/env.h"
#include "mongo/db/storage/txn.h"

namespace mongo {

    /* Details about a particular index. There is one of these effectively for each object in
       system.namespaces (although this also includes the head pointer, which is not in that
       collection).
     */
    class IndexDetails {
    public:
        explicit IndexDetails(const BSONObj &info, bool may_create=true);

        ~IndexDetails();

        BSONObj getKeyFromQuery(const BSONObj& query) const {
            BSONObj k = keyPattern();
            BSONObj res = query.extractFieldsUnDotted(k);
            return res;
        }

        /* pull out the relevant key objects from obj, so we
           can index them.  Note that the set is multiple elements
           only when it's a "multikey" array.
           keys will be left empty if key not found in the object.
        */
        void getKeysFromObject( const BSONObj& obj, BSONObjSet& keys) const;

        /* get the key pattern for this object.
           e.g., { lastname:1, firstname:1 }
        */
        BSONObj keyPattern() const {
            return _info["key"].Obj();
        }

        /**
         * @return offset into keyPattern for key
                   -1 if doesn't exist
         */
        int keyPatternOffset( const string& key ) const;
        bool inKeyPattern( const string& key ) const { return keyPatternOffset( key ) >= 0; }

        /* true if the specified key is in the index */
        bool hasKey(const BSONObj& key);

        // returns name of this index's storage area
        // database.table.$index
        string indexNamespace() const {
            string s;
            s.reserve(Namespace::MaxNsLen);
            s = parentNS();
            verify( !s.empty() );
            s += ".$";
            s += indexName();
            return s;
        }

        string indexName() const { // e.g. "ts_1"
            return _info["name"].String();
        }

        static bool isIdIndexPattern( const BSONObj &pattern ) {
            BSONObjIterator i(pattern);
            BSONElement e = i.next();
            // _id index must have form exactly {_id : 1} or {_id : -1}.
            // Allows an index of form {_id : "hashed"} to exist but
            // do not consider it to be the primary _id index
            if (!(strcmp(e.fieldName(), "_id") == 0 && (e.numberInt() == 1 || e.numberInt() == -1))) {
                return false;
            }
            return i.next().eoo();
        }

        /* returns true if this is the _id index. */
        bool isIdIndex() const {
            return isIdIndexPattern( keyPattern() );
        }

        /* gets not our namespace name (indexNamespace for that),
           but the collection we index, its name.
           */
        string parentNS() const {
            return _info["ns"].String();
        }

        /** @return true if index has unique constraint */
        bool unique() const {
            bool ret = _info["unique"].trueValue();
            if (isIdIndex()) {
                uassert(16434, "_id index must be unique", ret);
            }
            return ret;
        }

        /** @return true if index is clustering */
        bool clustering() const {
            bool ret = _info["clustering"].trueValue();
            if (isIdIndex()) {
                uassert(16437, "_id index must be clustering", ret);
            }
            return ret;
        }

        /** delete this index. */
        void kill_idx(bool can_drop_system = false);

        const IndexSpec& getSpec() const;

        string toString() const {
            return _info.toString();
        }

        const BSONObj &info() const { return _info; }

        void insert(const BSONObj &obj, const BSONObj &primary_key, bool overwrite);
        void deleteObject(const BSONObj &pk, const BSONObj &obj);
        DBC *cursor() const;
        enum toku_compression_method getCompressionMethod();
        uint32_t getPageSize();
        uint32_t getReadPageSize();
        void getStat64(DB_BTREE_STAT64* stats);
        void uniqueCheckCallback(const BSONObj &newkey, const BSONObj &oldkey, bool &isUnique) const;
        void uniqueCheck(const BSONObj &key) const;

    private:
        // Open dictionary representing the index on disk.
        DB *_db;

        void insertPair(const BSONObj &key, const BSONObj *pk, const BSONObj &val, bool overwrite);

        /* Info about the index. Stored on disk in the .ns file for this database
         * as a NamespaceDetails object.
         */
        /* Currenty known format:
             { name:"nameofindex", ns:"parentnsname", key: {keypattobject}
               [, unique: <bool>, background: <bool>, v:<version>]
             }
        */
        BSONObj _info;

        friend class NamespaceDetails;
    };

    // class to store statistics about an IndexDetails
    class IndexStats {
    public:
        IndexStats() {
            _readPageSize = 0;
            _pageSize = 0;
        }
        void fillStats(IndexDetails* idx);
        void fillBSONWithStats(BSONObjBuilder* bson_stats, int scale);
        uint64_t getCount() {
            return _stats.bt_nkeys;
        }
        uint64_t getDataSize() {
            return _stats.bt_dsize;
        }
        uint64_t getStorageSize() {
            return _stats.bt_fsize;
        }
    private:
        string name;
        DB_BTREE_STAT64 _stats;
        enum toku_compression_method _compressionMethod;
        uint32_t _readPageSize;
        uint32_t _pageSize;
    };


    int removeFromSysIndexes(const char *ns, const char *name);

} // namespace mongo
