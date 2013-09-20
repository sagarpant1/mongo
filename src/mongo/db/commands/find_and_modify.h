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

#pragma once

#include <string>
#include <vector>

#include "mongo/db/auth/privilege.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class Command;

namespace find_and_modify {

    void addPrivilegesRequiredForFindAndModify(Command* commandTemplate,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out);

} // namespace find_and_modify
} // namespace mongo


