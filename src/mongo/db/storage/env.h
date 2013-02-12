/**
*    Copyright (C) 2012 Tokutek Inc.
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

#ifndef MONGO_DB_STORAGE_ENV_H
#define MONGO_DB_STORAGE_ENV_H

#include <db.h>

#include "mongo/db/client.h"

namespace mongo {

    namespace storage {

        extern DB_ENV *env;

        void startup(void);
        void shutdown(void);

        // Creates the db if it doesn't already exist.
        DB *db_open(const Client::Transaction &txn, const char *name);
        void db_close(DB *db);

    } // namespace storage

} // namespace mongo

#endif // MONGO_DB_STORAGE_ENV_H
