/*    Copyright 2012 10gen Inc.
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

#pragma once

#if defined(_MSC_VER) && _MSC_VER >= 1500

#include <unordered_map>

namespace mongo {

#if _MSC_VER >= 1600  /* Visual Studio 2010+ */
    using std::unordered_map;
#else
    using std::tr1::unordered_map;
#endif

}  // namespace mongo

#elif defined(__GNUC__)

#include <tr1/unordered_map>

namespace mongo {

    using std::tr1::unordered_map;

}  // namespace mongo

#else
#error "Compiler's standard library does not provide a C++ unordered_map implementation."
#endif
