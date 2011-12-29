// clientOnly.cpp

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
#include "../client/dbclient.h"
#include "../db/cmdline.h"
#include "../db/client_common.h"
#include "../s/shard.h"
#include "../util/timer.h"
#include "clientOnly-private.h"

namespace mongo {

    CmdLine cmdLine;

    const char * curNs = "in client mode";

    bool dbexitCalled = false;
    // This mutex helps the shell serialize output on exit,
    // to avoid deadlocks at shutdown.  So it also protects
    // the global dbexitCalled.
    namespace shellUtils {
        mongo::mutex &mongoProgramOutputMutex(*(new mongo::mutex("mongoProgramOutputMutex")));
    }

    string dynHostMyName() { return ""; }

    void dynHostResolve(string& name, int& port) {
        assert(false);
    }

    void exitCleanly( ExitCode code ) {
        dbexit( code );
    }

    void dbexit( ExitCode returnCode, const char *whyMsg , bool tryToGetLock ) {
        {
            mongo::mutex::scoped_lock lk( shellUtils::mongoProgramOutputMutex );
            dbexitCalled = true;
        }
        out() << "dbexit called" << endl;
        if ( whyMsg )
            out() << " b/c " << whyMsg << endl;
        out() << "exiting" << endl;
        ::exit( returnCode );
    }

    bool inShutdown() {
        return dbexitCalled;
    }

    void setupSignals() {
        // maybe should do SIGPIPE here, not sure
    }

    string getDbContext() {
        return "in client only mode";
    }

    bool haveLocalShardingInfo( const string& ns ) {
        return false;
    }

    DBClientBase * createDirectClient() {
        uassert( 10256 ,  "no createDirectClient in clientOnly" , 0 );
        return 0;
    }

    void Shard::getAllShards( vector<Shard>& all ) {
        assert(0);
    }

    bool Shard::isAShardNode( const string& ident ) {
        assert(0);
        return false;
    }

    string prettyHostName() {
        assert(0);
        return "";
    }

    ClientBasic* ClientBasic::getCurrent() {
        return 0;
    }


}
