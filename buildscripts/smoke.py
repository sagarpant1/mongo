#!/usr/bin/env python

# smoke.py: run some mongo tests.

# Bugs, TODOs:

# 0 Some tests hard-code pathnames relative to the mongo repository,
#   so the smoke.py process and all its children must be run with the
#   mongo repo as current working directory.  That's kinda icky.

# 1 The tests that are implemented as standalone executables ("test",
#   "perftest"), don't take arguments for the dbpath, but
#   unconditionally use "/tmp/unittest".

# 2 mongod output gets intermingled with mongo output, and it's often
#   hard to find error messages in the slop.  Maybe have smoke.py do
#   some fancier wrangling of child process output?

# 3 Some test suites run their own mongods, and so don't need us to
#   run any mongods around their execution.  (It's harmless to do so,
#   but adds noise in the output.)

# 4 Running a separate mongo shell for each js file is slower than
#   loading js files into one mongo shell process.  Maybe have runTest
#   queue up all filenames ending in ".js" and run them in one mongo
#   shell at the "end" of testing?

# 5 Right now small-oplog implies master/slave replication.  Maybe
#   running with replication should be an orthogonal concern.  (And
#   maybe test replica set replication, too.)

# 6 We use cleanbb.py to clear out the dbpath, but cleanbb.py kills
#   off all mongods on a box, which means you can't run two smoke.py
#   jobs on the same host at once.  So something's gotta change.

from datetime import datetime
from itertools import izip
import glob
from optparse import OptionParser
import os
import parser
import pprint
import re
import shutil
import shlex
import socket
import stat
from subprocess import (Popen,
                        PIPE,
                        call,
                        check_output)
import sys
from tempfile import SpooledTemporaryFile
import time

from pymongo import Connection
from pymongo.errors import OperationFailure

import utils

try:
    import cPickle as pickle
except ImportError:
    import pickle

try:
    from hashlib import md5 # new in 2.5
except ImportError:
    from md5 import md5 # deprecated in 2.5

try:
    import json
except:
    try:
        import simplejson as json
    except:
        json = None


# TODO clean this up so we don't need globals...
mongo_repo = os.getcwd() #'./'
failfile = os.path.join(mongo_repo, 'failfile.smoke')
test_path = None
mongod_executable = None
mongod_port = None
shell_executable = None
continue_on_failure = None
file_of_commands_mode = False
start_mongod = True
valgrind = False
drd = False
temp_path = None

tests = []
winners = []
losers = {}
fails = [] # like losers but in format of tests

# For replication hash checking
replicated_collections = []
lost_in_slave = []
lost_in_master = []
screwy_in_slave = {}

smoke_db_prefix = ''
smoke_server_opts = ''
small_oplog = False
small_oplog_rs = False

tests_log = sys.stdout
server_log_file = ''
quiet = False

_debug = False

test_report = { "results": [] }
report_file = None

# This class just implements the with statement API, for a sneaky
# purpose below.
class Nothing(object):
    def __enter__(self):
        return self
    def __exit__(self, type, value, traceback):
        return not isinstance(value, Exception)

def buildlogger(cmd, is_global=False):
    # if the environment variable MONGO_USE_BUILDLOGGER
    # is set to 'true', then wrap the command with a call
    # to buildlogger.py, which sends output to the buidlogger
    # machine; otherwise, return as usual.
    if os.environ.get('MONGO_USE_BUILDLOGGER', '').lower().strip() == 'true':
        if is_global:
            return [utils.find_python(), 'buildscripts/buildlogger.py', '-g'] + cmd
        else:
            return [utils.find_python(), 'buildscripts/buildlogger.py'] + cmd
    return cmd

class mongod(object):
    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.proc = None
        self.auth = False

    def __enter__(self):
        if quiet:
            self.outfile = open(os.devnull, "w")
        else:
            self.outfile = sys.stdout
        self.start()
        return self

    def __exit__(self, type, value, traceback):
        try:
            self.stop()
            if quiet:
                self.outfile.close()
        except Exception, e:
            print >> sys.stderr, "error shutting down mongod"
            print >> sys.stderr, e
        return not isinstance(value, Exception)

    def ensure_test_dirs(self):
        utils.ensureDir(smoke_db_prefix + "/tmp/unittest/")
        utils.ensureDir(smoke_db_prefix + "/data/")
        utils.ensureDir(smoke_db_prefix + "/data/db/")

    def check_mongo_port(self, port=27017):
        sock = socket.socket()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(1)
        sock.connect(("localhost", int(port)))
        sock.close()

    def is_mongod_alive(self):
        try:
            self.check_mongo_port(self.port)
            return True
        except Exception, e:
            return False

    def is_mongod_up(self, port=mongod_port):
        try:
            self.check_mongo_port(int(port))
            return True
        except Exception,e:
            print >> sys.stderr, e
            return False
        
    def did_mongod_start(self, port=mongod_port, timeout=300):
        while timeout > 0:
            time.sleep(1)
            is_up = self.is_mongod_up(port)
            if is_up:
                return True
            timeout = timeout - 1
        print >> sys.stderr, "timeout starting mongod"
        return False

    def setup_admin_user(self, port=mongod_port):
        db = Connection( "localhost" , int(port) ).admin
        try:
            db.authenticate("admin","password")
        except OperationFailure, e:
            try:
                db.add_user("admin","password")
            except OperationFailure, e:
                if e.message == 'need to login':
                    pass # SERVER-4225
                else:
                    raise e

    def start(self):
        global mongod_port
        global mongod
        global shell_executable
        global _debug
        global valgrind
        global drd
        if self.proc:
            print >> sys.stderr, "probable bug: self.proc already set in start()"
            return
        self.ensure_test_dirs()
        dir_name = smoke_db_prefix + "/data/db/sconsTests/"
        self.port = int(mongod_port)
        self.slave = False
        if 'slave' in self.kwargs:
            dir_name = smoke_db_prefix + '/data/db/sconsTestsSlave/'
            srcport = mongod_port
            self.port += 1
            self.slave = True
        if os.path.exists(dir_name):
            pass
            """ Cleanbb is the most irritating script ever created.
            if 'slave' in self.kwargs:
                argv = [utils.find_python(), "buildscripts/cleanbb.py", '--nokill', dir_name]
            else:
                argv = [utils.find_python(), "buildscripts/cleanbb.py", dir_name]
            if quiet:
                f = open(server_log_file, "a")
                try:
                    call(argv, stdout=f)
                finally:
                    f.close()
            else:
                call(argv)
            """
        utils.ensureDir(dir_name)
        argv = [mongod_executable, "--port", str(self.port), "--dbpath", dir_name]
        # These parameters are always set for tests
        # SERVER-9137 Added httpinterface parameter to keep previous behavior
        argv += ['--setParameter', 'enableTestCommands=1', '--httpinterface']
        if valgrind or drd:
            argv += ['--setParameter', 'numCachetableBucketMutexes=32']
        if self.kwargs.get('small_oplog'):
            argv += ["--master", "--oplogSize", "511"]
        params = self.kwargs.get('set_parameters', None)
        if params:
            for p in params.split(','): argv += ['--setParameter', p]
        if self.kwargs.get('small_oplog_rs'):
            argv += ["--replSet", "foo", "--oplogSize", "511"]
        if self.slave:
            argv += ['--slave', '--source', 'localhost:' + str(srcport)]
        if self.kwargs.get('no_journal'):
            argv += ['--nojournal']
        if self.kwargs.get('no_preallocj'):
            argv += ['--nopreallocj']
        if self.kwargs.get('auth'):
            argv += ['--auth']
            authMechanism = self.kwargs.get('authMechanism', 'MONGODB-CR')
            if authMechanism != 'MONGODB-CR':
                argv += ['--setParameter', 'authenticationMechanisms=' + authMechanism]
            self.auth = True
        if len(server_log_file) > 0:
            argv += ['--logpath', server_log_file]
        if len(smoke_server_opts) > 0:
            argv += [smoke_server_opts]
        if self.kwargs.get('use_ssl'):
            argv += ['--sslOnNormalPorts',
                     '--sslPEMKeyFile', 'jstests/libs/server.pem',
                     '--sslCAFile', 'jstests/libs/ca.pem',
                     '--sslWeakCertificateValidation']

        if not quiet:
            print "running " + " ".join(argv)
        self.proc = self._start(buildlogger(argv, is_global=True))

        if not self.did_mongod_start(self.port):
            raise Exception("Failed to start mongod")

        run_mongo_cmd = [shell_executable, '--port', str(self.port), '--quiet']
        if self.kwargs.get('use_ssl'):
            run_mongo_cmd += ['--ssl',
                              '--sslPEMKeyFile', 'jstests/libs/server.pem',
                              '--sslCAFile', 'jstests/libs/ca.pem']
        if "true" == check_output(run_mongo_cmd + ['--eval', 'print(db.runCommand("buildInfo").debug)']).strip():
            _debug = True

        if self.auth:
            self.setup_admin_user(self.port)

        if self.slave:
            local = Connection(port=self.port, slave_okay=True).local
            synced = False
            while not synced:
                synced = True
                for source in local.sources.find(fields=["syncedTo"]):
                    synced = synced and "syncedTo" in source and source["syncedTo"]

    def _start(self, argv):
        """In most cases, just call subprocess.Popen(). On windows,
        add the started process to a new Job Object, so that any
        child processes of this process can be killed with a single
        call to TerminateJobObject (see self.stop()).
        """
        if valgrind:
            argv = [ 'buildscripts/valgrind.bash', '--show-reachable=yes', '--leak-check=full', '--suppressions=valgrind.suppressions' ] + argv
        elif drd:
            argv = [ 'buildscripts/valgrind.bash', '--tool=drd' ] + argv

        if os.sys.platform == "win32":
            # Create a job object with the "kill on job close"
            # flag; this is inherited by child processes (ie
            # the mongod started on our behalf by buildlogger)
            # and lets us terminate the whole tree of processes
            # rather than orphaning the mongod.
            import win32job

            # Magic number needed to allow job reassignment in Windows 7
            # see: MSDN - Process Creation Flags - ms684863
            CREATE_BREAKAWAY_FROM_JOB = 0x01000000

            proc = Popen(argv, creationflags=CREATE_BREAKAWAY_FROM_JOB, stdout=self.outfile)

            self.job_object = win32job.CreateJobObject(None, '')

            job_info = win32job.QueryInformationJobObject(
                self.job_object, win32job.JobObjectExtendedLimitInformation)
            job_info['BasicLimitInformation']['LimitFlags'] |= win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            win32job.SetInformationJobObject(
                self.job_object,
                win32job.JobObjectExtendedLimitInformation,
                job_info)

            win32job.AssignProcessToJobObject(self.job_object, proc._handle)

        else:
            proc = Popen(argv, stdout=self.outfile)

        return proc

    def stop(self):
        if not self.proc:
            print >> sys.stderr, "probable bug: self.proc unset in stop()"
            return
        try:
            if os.sys.platform == "win32":
                import win32job
                win32job.TerminateJobObject(self.job_object, -1)
                import time
                # Windows doesn't seem to kill the process immediately, so give it some time to die
                time.sleep(5)
            else:
                # This function not available in Python 2.5
                self.proc.terminate()
        except AttributeError:
            from os import kill
            kill(self.proc.pid, 15)
        self.proc.wait()
        sys.stderr.flush()
        sys.stdout.flush()

    def wait_for_repl(self):
        Connection(port=self.port).test.smokeWait.insert({}, w=2, wtimeout=5*60*1000)

class Bug(Exception):
    def __str__(self):
        return 'bug in smoke.py: ' + super(Bug, self).__str__()

class TestFailure(Exception):
    pass

class TestExitFailure(TestFailure):
    def __init__(self, *args):
        self.path = args[0]
        self.status=args[1]

    def __str__(self):
        return "test %s exited with status %d" % (self.path, self.status)

class TestServerFailure(TestFailure):
    def __init__(self, *args):
        self.path = args[0]
        self.status = -1 # this is meaningless as an exit code, but
                         # that's the point.
    def __str__(self):
        return 'mongod not running after executing test %s' % self.path

def check_db_hashes(master, slave):
    # Need to pause a bit so a slave might catch up...
    if not slave.slave:
        raise(Bug("slave instance doesn't have slave attribute set"))

    print "waiting for slave to catch up"
    master.wait_for_repl()
    print "caught up!"

    # FIXME: maybe make this run dbhash on all databases?
    for mongod in [master, slave]:
        mongod.dbhash = Connection(port=mongod.port, slave_okay=True).test.command("dbhash")
        mongod.dict = mongod.dbhash["collections"]

    global lost_in_slave, lost_in_master, screwy_in_slave, replicated_collections

    replicated_collections += master.dict.keys()

    for coll in replicated_collections:
        if coll not in slave.dict and coll not in lost_in_slave:
            lost_in_slave.append(coll)
        mhash = master.dict[coll]
        shash = slave.dict[coll]
        if mhash != shash:
            mTestDB = Connection(port=master.port, slave_okay=True).test
            sTestDB = Connection(port=slave.port, slave_okay=True).test
            mCount = mTestDB[coll].count()
            sCount = sTestDB[coll].count()
            stats = {'hashes': {'master': mhash, 'slave': shash},
                     'counts':{'master': mCount, 'slave': sCount}}
            try:
                mDocs = list(mTestDB[coll].find().sort("_id", 1))
                sDocs = list(sTestDB[coll].find().sort("_id", 1))
                mDiffDocs = list()
                sDiffDocs = list()
                for left, right in izip(mDocs, sDocs):
                    if left != right:
                        mDiffDocs.append(left)
                        sDiffDocs.append(right)

                stats["docs"] = {'master': mDiffDocs, 'slave': sDiffDocs }
            except Exception, e:
                stats["error-docs"] = e;
            screwy_in_slave[coll] = stats
    for db in slave.dict.keys():
        if db not in master.dict and db not in lost_in_master:
            lost_in_master.append(db)


def ternary( b , l="true", r="false" ):
    if b:
        return l
    return r


# Blech.
def skipTest(path):
    basename = os.path.basename(path)
    parentPath = os.path.dirname(path)
    parentDir = os.path.basename(parentPath)
    if small_oplog: # For tests running in parallel
        if basename in ["cursor8.js", "indexh.js", "dropdb.js", "dropdb_race.js", 
                        "connections_opened.js", "opcounters.js", "dbadmin.js"]:
            return True
    if auth or keyFile: # For tests running with auth
        # Skip any tests that run with auth explicitly
        if parentDir == "auth" or "auth" in basename:
            return True
        # These tests don't pass with authentication due to limitations of the test infrastructure,
        # not due to actual bugs.
        if parentDir == "tool": # SERVER-6368
            return True
        if parentPath == mongo_repo: # Skip client tests
            return True
        # SERVER-6388
        if os.path.join(parentDir,basename) in ["sharding/sync3.js", "sharding/sync6.js", "sharding/parallel.js", "jstests/bench_test1.js", "jstests/bench_test2.js", "jstests/bench_test3.js"]:
            return True
        # These tests fail due to bugs
        if os.path.join(parentDir,basename) in ["sharding/sync_conn_cmd.js"]: # SERVER-6327
            return True
        if parentDir == "disk": # SERVER-7356
            return True
        if parentDir == "dur": # SERVER-7317
            return True

        authTestsToSkip = [("sharding", "gle_with_conf_servers.js"), # SERVER-6972
                           ("sharding", "read_pref.js"), # SERVER-6972
                           ("sharding", "read_pref_cmd.js"), # SERVER-6972
                           ("sharding", "gle_with_conf_servers.js"), # SERVER-6972
                           ("sharding", "sync3.js"), # SERVER-6388 for this and those below
                           ("sharding", "sync6.js"),
                           ("sharding", "test_all_commands.js"), # some commands will fail due to auth, we don't need to test that they're linked again, testing without auth is sufficient
                           ("sharding", "parallel.js"),
                           ("jstests", "bench_test1.js"),
                           ("jstests", "bench_test2.js"),
                           ("jstests", "bench_test3.js"),
                           ("jstests", "max_message_size.js"), # SERVER-8414
                           ("jstests", "drop2.js"), # SERVER-8589
                           ("jstests", "killop.js") # SERVER-10128
                           ]

        if os.path.join(parentDir,basename) in [ os.path.join(*test) for test in authTestsToSkip ]:
            return True

    if _debug:
        # MutexDebugger complains about this test, it's not unique to tokumx though, see #79
        if os.path.join(parentDir,basename) in ["sharding/remove2.js"]:
            return True

    return False

def runTest(test, testnum, result):
    # result is a map containing test result details, like result["url"]

    # test is a tuple of ( filename , usedb<bool> )
    # filename should be a js file to run
    # usedb is true if the test expects a mongod to be running

    (path, usedb) = test
    (ignore, ext) = os.path.splitext(path)
    # the dbtests know how to format themselves nicely, we'll detect if we're running them and if
    # so, we won't mess with the output
    is_test_binary = False
    test_mongod = mongod()
    mongod_is_up = test_mongod.is_mongod_up(mongod_port)
    result["mongod_running_at_start"] = mongod_is_up;

    if file_of_commands_mode:
        # smoke.py was invoked like "--mode files --from-file foo",
        # so don't try to interpret the test path too much
        if os.sys.platform == "win32":
            argv = [path]
        else:
            argv = shlex.split(path)
        path = argv[0]
        # if the command is a python script, use the script name
        if os.path.basename(path) in ('python', 'python.exe'):
            path = argv[1]
    elif ext == ".js":
        argv = [shell_executable, "--port", mongod_port, '--authenticationMechanism', authMechanism]
        if not usedb:
            argv += ["--nodb"]
        if small_oplog or small_oplog_rs:
            argv += ["--eval", 'testingReplication = true;']
        if use_ssl:
            argv += ["--ssl",
                     "--sslPEMKeyFile", "jstests/libs/client.pem",
                     "--sslCAFile", "jstests/libs/ca.pem"]
        argv += [path]
    elif ext in ["", ".exe"]:
        # Blech.
        if os.path.basename(path) in ["test", "test.exe", "perftest", "perftest.exe"]:
            argv = [path]
            if os.path.basename(path) in ["test", "test.exe"]:
                is_test_binary = True
            if "newUpdateFrameworkEnabled" in set_parameters:
                argv += ["--testNewUpdateFramework"]
        # more blech
        elif os.path.basename(path) in ['mongos', 'mongos.exe']:
            argv = [path, "--test"]
        else:
            argv = [test_path and os.path.abspath(os.path.join(test_path, path)) or path,
                    "--port", mongod_port]
    else:
        raise Bug("fell off in extension case: %s" % path)

    if keyFile:
        f = open(keyFile, 'r')
        keyFileData = re.sub(r'\s', '', f.read()) # Remove all whitespace
        f.close()
        os.chmod(keyFile, stat.S_IRUSR | stat.S_IWUSR)
    else:
        keyFileData = None

    mongo_test_filename = os.path.basename(path)
    if 'sharedclient' in path:
        mongo_test_filename += "-sharedclient"

    # sys.stdout.write() is more atomic than print, so using it prevents
    # lines being interrupted by, e.g., child processes
    if quiet and not is_test_binary:
        vlog = tests_log
        qlog = sys.stdout
        tlog = sys.stderr
    else:
        vlog = sys.stdout
        qlog = None
        tlog = None

    vlog.write(" *******************************************\n")
    vlog.write("         Test : %s ...\n" % mongo_test_filename)
    vlog.flush()

    # FIXME: we don't handle the case where the subprocess
    # hangs... that's bad.
    if ( argv[0].endswith( 'mongo' ) or argv[0].endswith( 'mongo.exe' ) ) and not '--eval' in argv :
        evalString = 'TestData = new Object();' + \
                     'TestData.testPath = "' + path + '";' + \
                     'TestData.testFile = "' + os.path.basename( path ) + '";' + \
                     'TestData.testName = "' + re.sub( ".js$", "", os.path.basename( path ) ) + '";' + \
                     'TestData.setParameters = "' + ternary( set_parameters, set_parameters, "" )  + '";' + \
                     'TestData.noJournal = ' + ternary( no_journal )  + ";" + \
                     'TestData.noJournalPrealloc = ' + ternary( no_preallocj )  + ";" + \
                     'TestData.auth = ' + ternary( auth ) + ";" + \
                     'TestData.keyFile = ' + ternary( keyFile , '"' + str(keyFile) + '"' , 'null' ) + ";" + \
                     'TestData.keyFileData = ' + ternary( keyFile , '"' + str(keyFileData) + '"' , 'null' ) + ";" + \
                     'TestData.authMechanism = ' + ternary( authMechanism, '"' + str(authMechanism) + '"', 'null') + ";"
        if os.sys.platform == "win32":
            # double quotes in the evalString on windows; this
            # prevents the backslashes from being removed when
            # the shell (i.e. bash) evaluates this string. yuck.
            evalString = evalString.replace('\\', '\\\\')

        if auth and usedb:
            evalString += 'jsTest.authenticate(db.getMongo());'

        argv = argv + [ '--eval', evalString]

    if argv[0].endswith( 'test' ) or argv[0].endswith( 'test.exe' ):
        if no_preallocj :
            argv = argv + [ '--nopreallocj' ]
        if temp_path:
            argv = argv + [ '--tempPath', temp_path ]

    vlog.write("      Command : %s\n" % ' '.join(argv))
    vlog.write("         Date : %s\n" % datetime.now().ctime())
    vlog.flush()

    tempfile = SpooledTemporaryFile(max_size=16*1024*1024)

    try:
        os.environ['MONGO_TEST_FILENAME'] = mongo_test_filename
        t1 = time.time()
        r = call(buildlogger(argv), cwd=test_path,
                 # the dbtests know how to format their own output nicely
                 stdout=ternary(is_test_binary, vlog, tempfile))
        t2 = time.time()
        del os.environ['MONGO_TEST_FILENAME']

        vlog.write("                %fms\n" % ((t2 - t1) * 1000))
        vlog.flush()

        if not is_test_binary:
            tempfile.seek(0)
            for line in tempfile:
                vlog.write(line)
            vlog.flush()

            if quiet:
                if r == 0:
                    qlog.write('ok %d %s\n' % (testnum, os.path.basename(path)))
                else:
                    qlog.write('not ok %d %s # exit %d\n' % (testnum, os.path.basename(path), r))
                qlog.flush()
                if r != 0:
                    tempfile.seek(0)
                    for line in tempfile:
                        tlog.write(line)
                    tlog.flush()

        result["exit_code"] = r

        if r != 0:
            raise TestExitFailure(path, r)
    finally:
        tempfile.close()

    is_mongod_still_up = test_mongod.is_mongod_up(mongod_port)
    if not is_mongod_still_up:
        print "mongod is not running after test"
        result["mongod_running_at_end"] = is_mongod_still_up;
        if start_mongod:
            raise TestServerFailure(path)

    result["mongod_running_at_end"] = is_mongod_still_up;

    if r != 0:
        raise TestExitFailure(path, r)

def run_tests(tests):
    # FIXME: some suites of tests start their own mongod, so don't
    # need this.  (So long as there are no conflicts with port,
    # dbpath, etc., and so long as we shut ours down properly,
    # starting this mongod shouldn't break anything, though.)

    # The reason we want to use "with" is so that we get __exit__ semantics
    # but "with" is only supported on Python 2.5+

    if start_mongod:
        master = mongod(small_oplog_rs=small_oplog_rs,
                        small_oplog=small_oplog,
                        no_journal=no_journal,
                        set_parameters=set_parameters,
                        no_preallocj=no_preallocj,
                        auth=auth,
                        authMechanism=authMechanism,
                        use_ssl=use_ssl).__enter__()
    else:
        master = Nothing()
    try:
        if small_oplog:
            slave = mongod(slave=True,
                           set_parameters=set_parameters).__enter__()
        elif small_oplog_rs:
            slave = mongod(slave=True,
                           small_oplog_rs=small_oplog_rs,
                           small_oplog=small_oplog,
                           no_journal=no_journal,
                           set_parameters=set_parameters,
                           no_preallocj=no_preallocj,
                           auth=auth,
                           authMechanism=authMechanism,
                           use_ssl=use_ssl).__enter__()
            primary = Connection(port=master.port, slave_okay=True);

            primary.admin.command({'replSetInitiate' : {'_id' : 'foo', 'members' : [
                            {'_id': 0, 'host':'localhost:%s' % master.port},
                            {'_id': 1, 'host':'localhost:%s' % slave.port,'priority':0}]}})

            ismaster = False
            while not ismaster:
                result = primary.admin.command("ismaster");
                ismaster = result["ismaster"]
                time.sleep(1)
        else:
            slave = Nothing()

        try:
            if small_oplog or small_oplog_rs:
                master.wait_for_repl()

            tests_run = 0
            if quiet:
                sys.stdout.write('1..%d\n' % len(tests))
            for tests_run, test in enumerate(tests):
                test_result = { "start": time.time() }

                (test_path, use_db) = test

                if test_path.startswith(mongo_repo + os.path.sep):
                    test_result["test_file"] = test_path[len(mongo_repo)+1:]
                else:
                    # user could specify a file not in repo. leave it alone.
                    test_result["test_file"] = test_path

                try:
                    if skipTest(test_path):
                        test_result["status"] = "skip"

                        if quiet:
                            sys.stdout.write("skip %d %s\n" % (tests_run + 1, os.path.basename(path)))
                            sys.stdout.flush()
                        else:
                            print "skipping " + path
                            return
                    else:
                        fails.append(test)
                        runTest(test, tests_run + 1, test_result)
                        fails.pop()
                        winners.append(test)

                        test_result["status"] = "pass"

                    test_result["end"] = time.time()
                    test_result["elapsed"] = test_result["end"] - test_result["start"]
                    test_report["results"].append( test_result )
                    if small_oplog or small_oplog_rs:
                        master.wait_for_repl()

                except TestFailure, f:
                    test_result["end"] = time.time()
                    test_result["elapsed"] = test_result["end"] - test_result["start"]
                    test_result["error"] = str(f)
                    test_result["status"] = "fail"
                    test_report["results"].append( test_result )
                    try:
                        if not quiet:
                            print f
                        if not master.is_mongod_alive():
                            print 'restarting mongod...'
                            master = mongod(small_oplog_rs=small_oplog_rs,small_oplog=small_oplog,no_journal=no_journal,no_preallocj=no_preallocj,auth=auth).__enter__()
                        # Record the failing test and re-raise.
                        losers[f.path] = f.status
                        raise f
                    except TestServerFailure, f:
                        return 2
                    except TestFailure, f:
                        if not continue_on_failure:
                            return 1
            if isinstance(slave, mongod):
                check_db_hashes(master, slave)
        finally:
            slave.__exit__(None, None, None)
    finally:
        master.__exit__(None, None, None)
    return 0


def report():
    print "%d tests succeeded" % len(winners)
    num_missed = len(tests) - (len(winners) + len(losers.keys()))
    if num_missed:
        print "%d tests didn't get run" % num_missed
    if losers:
        print "The following tests failed (with exit code):"
        for loser in sorted(losers.keys()):
            print "%s\t%d" % (loser, losers[loser])

    def missing(lst, src, dst):
        if lst:
            print """The following collections were present in the %s but not the %s
at the end of testing:""" % (src, dst)
            for db in lst:
                print db
    missing(lost_in_slave, "master", "slave")
    missing(lost_in_master, "slave", "master")
    if screwy_in_slave:
        print """The following collections has different hashes in master and slave
at the end of testing:"""
        for coll in screwy_in_slave.keys():
            stats = screwy_in_slave[coll]
            print "collection: %s\t (master/slave) hashes: %s/%s counts: %i/%i" % (coll, stats['hashes']['master'], stats['hashes']['slave'], stats['counts']['master'], stats['counts']['slave'])
            if "docs" in stats:
                if (("master" in stats["docs"] and len(stats["docs"]["master"]) != 0) or
                    ("slave" in stats["docs"] and len(stats["docs"]["slave"]) != 0)):
                    print "All docs matched!"
                else:
                    print "Different Docs"
                    print "Master docs:"
                    pprint.pprint(stats["docs"]["master"], indent=2)
                    print "Slave docs:"
                    pprint.pprint(stats["docs"]["slave"], indent=2)
            if "error-docs" in stats:
                print "Error getting docs to diff:"
                pprint.pprint(stats["error-docs"])

    if (small_oplog or small_oplog_rs) and not (lost_in_master or lost_in_slave or screwy_in_slave):
        print "replication ok for %d collections" % (len(replicated_collections))
    if losers or lost_in_slave or lost_in_master or screwy_in_slave:
        raise Exception("Test failures")

# Keys are the suite names (passed on the command line to smoke.py)
# Values are pairs: (filenames, <start mongod before running tests>)
suiteGlobalConfig = {"js": ("[!_]*.js", True),
                     "quota": ("quota/*.js", True),
                     "jsPerf": ("perf/*.js", True),
                     "disk": ("disk/*.js", True),
                     "jsSlowNightly": ("slowNightly/*.js", True),
                     "jsSlowWeekly": ("slowWeekly/*.js", False),
                     "parallel": ("parallel/*.js", True),
                     "clone": ("clone/*.js", False),
                     "repl": ("repl/*.js", False),
                     "replSets": ("replsets/*.js", False),
                     "dur": ("dur/*.js", False),
                     "auth": ("auth/*.js", False),
                     "sharding": ("sharding/*.js", False),
                     "tool": ("tool/*.js", False),
                     "aggregation": ("aggregation/*.js", True),
                     "multiVersion": ("multiVersion/*.js", True),
                     "failPoint": ("fail_point/*.js", False),
                     "ssl": ("ssl/*.js", True),
                     "sslSpecial": ("sslSpecial/*.js", True),
                     "jsCore": ("core/[!_]*.js", True),
                     }

def get_module_suites():
    """Attempts to discover and return information about module test suites

    Returns a dictionary of module suites in the format:

    {
        "<suite_name>" : "<full_path_to_suite_directory/[!_]*.js>",
        ...
    }

    This means the values of this dictionary can be used as "glob"s to match all jstests in the
    suite directory that don't start with an underscore

    The module tests should be put in 'src/mongo/db/modules/<module_name>/<suite_name>/*.js'

    NOTE: This assumes that if we have more than one module the suite names don't conflict
    """
    modules_directory = 'src/mongo/db/modules'
    test_suites = {}

    # Return no suites if we have no modules
    if not os.path.exists(modules_directory) or not os.path.isdir(modules_directory):
        return {}

    module_directories = os.listdir(modules_directory)
    for module_directory in module_directories:

        test_directory = os.path.join(modules_directory, module_directory, "jstests")

        # Skip this module if it has no "jstests" directory
        if not os.path.exists(test_directory) or not os.path.isdir(test_directory):
            continue

        # Get all suites for this module
        for test_suite in os.listdir(test_directory):
            test_suites[test_suite] = os.path.join(test_directory, test_suite, "[!_]*.js")

    return test_suites

def expand_suites(suites,expandUseDB=True):
    """Takes a list of suites and expands to a list of tests according to a set of rules.

    Keyword arguments:
        suites -- list of suites specified by the user
        expandUseDB -- expand globs (such as [!_]*.js) for tests that are run against a database
                       (default True)

    This function handles expansion of globs (such as [!_]*.js), aliases (such as "client" and
    "all"), detection of suites in the "modules" directory, and enumerating the test files in a
    given suite.  It returns a list of tests of the form (path_to_test, usedb), where the second
    part of the tuple specifies whether the test is run against the database (see --nodb in the
    mongo shell)

    """
    globstr = None
    tests = []
    module_suites = get_module_suites()
    for suite in suites:
        if suite == 'all':
            return expand_suites(['test', 'perf', 'client', 'js', 'jsPerf', 'jsSlowNightly', 'jsSlowWeekly', 'clone', 'parallel', 'repl', 'auth', 'sharding', 'tool'],expandUseDB=expandUseDB)
        if suite == 'test':
            if os.sys.platform == "win32":
                program = 'test.exe'
            else:
                program = 'test'
            (globstr, usedb) = (program, False)
        elif suite == 'perf':
            if os.sys.platform == "win32":
                program = 'perftest.exe'
            else:
                program = 'perftest'
            (globstr, usedb) = (program, False)
        elif suite == 'client':
            paths = ["firstExample", "secondExample", "whereExample", "authTest", "clientTest", "httpClientTest"]
            if os.sys.platform == "win32":
                paths = [path + '.exe' for path in paths]

            if not test_path:
                # If we are testing 'in-tree', then add any files of the same name from the
                # sharedclient directory. The out of tree client build doesn't have shared clients.
                scpaths = ["sharedclient/" + path for path in paths]
                scfiles = glob.glob("sharedclient/*")
                paths += [scfile for scfile in scfiles if scfile in scpaths]

            # hack
            tests += [(test_path and path or os.path.join(mongo_repo, path), False) for path in paths]
        elif suite == 'mongosTest':
            if os.sys.platform == "win32":
                program = 'mongos.exe'
            else:
                program = 'mongos'
            tests += [(os.path.join(mongo_repo, program), False)]
        elif os.path.exists( suite ):
            usedb = True
            for name in suiteGlobalConfig:
                if suite in glob.glob( "jstests/" + suiteGlobalConfig[name][0] ):
                    usedb = suiteGlobalConfig[name][1]
                    break
            tests += [ ( os.path.join( mongo_repo , suite ) , usedb ) ]
        elif suite in module_suites:
            # Currently we connect to a database in all module tests since there's no mechanism yet
            # to configure it independently
            usedb = True
            paths = glob.glob(module_suites[suite])
            paths.sort()
            tests += [(path, usedb) for path in paths]
        else:
            try:
                globstr, usedb = suiteGlobalConfig[suite]
            except KeyError:
                raise Exception('unknown test suite %s' % suite)

        if globstr:
            if usedb and not expandUseDB:
                tests += [ (suite,False) ]
            else:
                if globstr.endswith('.js'):
                    loc = 'jstests/'
                else:
                    loc = ''
                globstr = os.path.join(mongo_repo, (os.path.join(loc, globstr)))
                globstr = os.path.normpath(globstr)
                paths = glob.glob(globstr)
                paths.sort()
                tests += [(path, usedb) for path in paths]

    return tests

def add_exe(e):
    if os.sys.platform.startswith( "win" ) and not e.endswith( ".exe" ):
        e += ".exe"
    return e

def set_globals(options, tests):
    global mongod_executable, mongod_port, shell_executable, continue_on_failure, small_oplog, small_oplog_rs
    global no_journal, set_parameters, no_preallocj, auth, authMechanism, keyFile, smoke_db_prefix, smoke_server_opts, server_log_file, tests_log, quiet, test_path, start_mongod
    global use_ssl
    global file_of_commands_mode
    global valgrind, drd
    global report_file
    global temp_path
    start_mongod = options.start_mongod
    if hasattr(options, 'use_ssl'):
        use_ssl = options.use_ssl
    #Careful, this can be called multiple times
    test_path = options.test_path

    mongod_executable = add_exe(options.mongod_executable)
    if not os.path.exists(mongod_executable):
        raise Exception("no mongod found in this directory.")

    mongod_port = options.mongod_port

    shell_executable = add_exe( options.shell_executable )
    if not os.path.exists(shell_executable):
        raise Exception("no mongo shell found in this directory.")

    continue_on_failure = options.continue_on_failure
    smoke_db_prefix = options.smoke_db_prefix
    smoke_server_opts = options.smoke_server_opts
    small_oplog = options.small_oplog
    if hasattr(options, "small_oplog_rs"):
        small_oplog_rs = options.small_oplog_rs
    no_journal = options.no_journal
    set_parameters = options.set_parameters
    no_preallocj = options.no_preallocj
    if options.mode == 'suite' and tests == ['client']:
        # The client suite doesn't work with authentication
        if options.auth:
            print "Not running client suite with auth even though --auth was provided"
        auth = False;
        keyFile = False;
        authMechanism = None
    else:
        auth = options.auth
        authMechanism = options.authMechanism
        keyFile = options.keyFile

    if auth and not keyFile:
        # if only --auth was given to smoke.py, load the
        # default keyFile from jstests/libs/authTestsKey
        keyFile = os.path.join(mongo_repo, 'jstests', 'libs', 'authTestsKey')

    # if smoke.py is running a list of commands read from a
    # file (or stdin) rather than running a suite of js tests
    file_of_commands_mode = options.File and options.mode == 'files'
    # generate json report
    report_file = options.report_file
    temp_path = options.temp_path

    quiet = options.quiet
    if options.tests_log_file is not None and len(options.tests_log_file) > 0:
        tests_log = open(options.tests_log_file, "w")
    elif quiet:
        if not os.path.exists(smoke_db_prefix):
            os.mkdir(smoke_db_prefix)
        tests_log = open(os.path.join(smoke_db_prefix, "tests.log"), "w")
    if options.server_log_file is not None and len(options.server_log_file) > 0:
        server_log_file = options.server_log_file
    elif quiet:
        server_log_file = os.path.join(smoke_db_prefix, "server.log")

    valgrind = options.valgrind
    drd = options.drd
    if valgrind and drd:
        print "Both --valgrind and --drd specified: assuming drd..."
        valgrind = False

def file_version():
    return md5(open(__file__, 'r').read()).hexdigest()

def clear_failfile():
    if os.path.exists(failfile):
        os.remove(failfile)

def run_old_fails():
    global tests

    try:
        f = open(failfile, 'r')
        state = pickle.load(f)
        f.close()
    except Exception:
        try:
            f.close()
        except:
            pass
        clear_failfile()
        return # This counts as passing so we will run all tests

    if ('version' not in state or state['version'] != file_version()):
        print "warning: old version of failfile.smoke detected. skipping recent fails"
        clear_failfile()
        return

    testsAndOptions = state['testsAndOptions']
    tests = [x[0] for x in testsAndOptions]
    passed = []
    try:
        for (i, (test, options)) in enumerate(testsAndOptions):
            # SERVER-5102: until we can figure out a better way to manage
            # dependencies of the --only-old-fails build phase, just skip
            # tests which we can't safely run at this point
            path, usedb = test

            if not os.path.exists(path):
                passed.append(i)
                winners.append(test)
                continue

            filename = os.path.basename(path)
            if filename in ('test', 'test.exe') or filename.endswith('.js'):
                set_globals(options, [filename])
                oldWinners = len(winners)
                run_tests([test])
                if len(winners) != oldWinners: # can't use return value due to continue_on_failure
                    passed.append(i)
    finally:
        for offset, i in enumerate(passed):
            testsAndOptions.pop(i - offset)

        if testsAndOptions:
            f = open(failfile, 'w')
            state = {'version':file_version(), 'testsAndOptions':testsAndOptions}
            pickle.dump(state, f)
        else:
            clear_failfile()

        report() # exits with failure code if there is an error

def add_to_failfile(tests, options):
    try:
        f = open(failfile, 'r')
        testsAndOptions = pickle.load(f)["testsAndOptions"]
    except Exception:
        testsAndOptions = []

    for test in tests:
        if (test, options) not in testsAndOptions:
            testsAndOptions.append( (test, options) )

    state = {'version':file_version(), 'testsAndOptions':testsAndOptions}
    f = open(failfile, 'w')
    pickle.dump(state, f)



def main():
    global mongod_executable, mongod_port, shell_executable, continue_on_failure, small_oplog, no_journal, set_parameters, no_preallocj, auth, keyFile, smoke_db_prefix, smoke_server_opts, test_path
    parser = OptionParser(usage="usage: smoke.py [OPTIONS] ARGS*")
    parser.add_option('--mode', dest='mode', default='suite',
                      help='If "files", ARGS are filenames; if "suite", ARGS are sets of tests (%default)')
    # Some of our tests hard-code pathnames e.g., to execute, so until
    # that changes we don't have the freedom to run from anyplace.
    # parser.add_option('--mongo-repo', dest='mongo_repo', default=None,
    parser.add_option('--test-path', dest='test_path', default=None,
                      help="Path to the test executables to run, "
                      "currently only used for 'client' (%default)")
    parser.add_option('--mongod', dest='mongod_executable', default=os.path.join(mongo_repo, 'mongod'),
                      help='Path to mongod to run (%default)')
    parser.add_option('--port', dest='mongod_port', default="27999",
                      help='Port the mongod will bind to (%default)')
    parser.add_option('--mongo', dest='shell_executable', default=os.path.join(mongo_repo, 'mongo'),
                      help='Path to mongo, for .js test files (%default)')
    parser.add_option('--continue-on-failure', dest='continue_on_failure',
                      action="store_true", default=False,
                      help='If supplied, continue testing even after a test fails')
    parser.add_option('--from-file', dest='File',
                      help="Run tests/suites named in FILE, one test per line, '-' means stdin")
    parser.add_option('--smoke-db-prefix', dest='smoke_db_prefix', default=smoke_db_prefix,
                      help="Prefix to use for the mongods' dbpaths ('%default')")
    parser.add_option('--smoke-server-opts', dest='smoke_server_opts', default=smoke_server_opts,
                      help="Additional options for the mongod server ('%default')")
    parser.add_option('--small-oplog', dest='small_oplog', default=False,
                      action="store_true",
                      help='Run tests with master/slave replication & use a small oplog')
    parser.add_option('--small-oplog-rs', dest='small_oplog_rs', default=False,
                      action="store_true",
                      help='Run tests with replica set replication & use a small oplog')
    parser.add_option('--nojournal', dest='no_journal', default=False,
                      action="store_true",
                      help='Do not turn on journaling in tests')
    parser.add_option('--nopreallocj', dest='no_preallocj', default=False,
                      action="store_true",
                      help='Do not preallocate journal files in tests')
    parser.add_option('--auth', dest='auth', default=False,
                      action="store_true",
                      help='Run standalone mongods in tests with authentication enabled')
    parser.add_option('--authMechanism', dest='authMechanism', default='MONGODB-CR',
                      help='Use the given authentication mechanism, when --auth is used.')
    parser.add_option('--keyFile', dest='keyFile', default=None,
                      help='Path to keyFile to use to run replSet and sharding tests with authentication enabled')
    parser.add_option('--ignore', dest='ignore_files', default=None,
                      help='Pattern of files to ignore in tests')
    parser.add_option('--only-old-fails', dest='only_old_fails', default=False,
                      action="store_true",
                      help='Check the failfile and only run all tests that failed last time')
    parser.add_option('--reset-old-fails', dest='reset_old_fails', default=False,
                      action="store_true",
                      help='Clear the failfile. Do this if all tests pass')
    parser.add_option('--with-cleanbb', dest='with_cleanbb', default=False,
                      action="store_true",
                      help='Clear database files from previous smoke.py runs')
    parser.add_option('--server-log', dest='server_log_file', default=server_log_file,
                      help="Log file for the mongod server ('%default')")
    parser.add_option('--tests-log', dest='tests_log_file', default='',
                      help="Log file for the tests ('%default')")
    parser.add_option('--quiet', dest='quiet', default=False,
                      action="store_true",
                      help='Generate a quieter report (use with --tests-log)')
    parser.add_option('--valgrind', dest='valgrind', default=False,
                      action="store_true",
                      help='Run mongod under valgrind')
    parser.add_option('--drd', dest='drd', default=False,
                      action="store_true",
                      help='Run mongod under drd')
    parser.add_option('--shuffle', dest='shuffle', default=False,
                      action="store_true",
                      help='Shuffle tests instead of running them in alphabetical order')
    parser.add_option('--skip-until', dest='skip_tests_until', default="",
                      action="store",
                      help='Skip all tests alphabetically less than the given name.')
    parser.add_option('--dont-start-mongod', dest='start_mongod', default=True,
                      action='store_false',
                      help='Do not start mongod before commencing test running')
    parser.add_option('--use-ssl', dest='use_ssl', default=False,
                      action='store_true',
                      help='Run mongo shell and mongod instances with SSL encryption')
    parser.add_option('--set-parameters', dest='set_parameters', default="",
                      help='Adds --setParameter for each passed in items in the csv list - ex. "param1=1,param2=foo" ')
    parser.add_option('--temp-path', dest='temp_path', default=None,
                      help='If present, passed as --tempPath to unittests and dbtests')
    # Buildlogger invocation from command line
    parser.add_option('--buildlogger-builder', dest='buildlogger_builder', default=None,
                      action="store", help='Set the "builder name" for buildlogger')
    parser.add_option('--buildlogger-buildnum', dest='buildlogger_buildnum', default=None,
                      action="store", help='Set the "build number" for buildlogger')
    parser.add_option('--buildlogger-url', dest='buildlogger_url', default=None,
                      action="store", help='Set the url root for the buildlogger service')
    parser.add_option('--buildlogger-credentials', dest='buildlogger_credentials', default=None,
                      action="store", help='Path to Python file containing buildlogger credentials')
    parser.add_option('--buildlogger-phase', dest='buildlogger_phase', default=None,
                      action="store", help='Set the "phase" for buildlogger (e.g. "core", "auth") for display in the webapp (optional)')
    parser.add_option('--report-file', dest='report_file', default=None,
                      action='store',
                      help='Path to generate detailed json report containing all test details')


    global tests
    (options, tests) = parser.parse_args()

    set_globals(options, tests)

    buildlogger_opts = (options.buildlogger_builder, options.buildlogger_buildnum, options.buildlogger_credentials)
    if all(buildlogger_opts):
        os.environ['MONGO_USE_BUILDLOGGER'] = 'true'
        os.environ['MONGO_BUILDER_NAME'] = options.buildlogger_builder
        os.environ['MONGO_BUILD_NUMBER'] = options.buildlogger_buildnum
        os.environ['BUILDLOGGER_CREDENTIALS'] = options.buildlogger_credentials
        if options.buildlogger_phase:
            os.environ['MONGO_PHASE'] = options.buildlogger_phase
    elif any(buildlogger_opts):
        # some but not all of the required options were sete
        raise Exception("you must set all of --buildlogger-builder, --buildlogger-buildnum, --buildlogger-credentials")

    if options.buildlogger_url: #optional; if None, defaults to const in buildlogger.py
        os.environ['BUILDLOGGER_URL'] = options.buildlogger_url

    if options.File:
        if options.File == '-':
            tests = sys.stdin.readlines()
        else:
            f = open(options.File)
            tests = f.readlines()
    tests = [t.rstrip('\n') for t in tests]

    if options.only_old_fails:
        run_old_fails()
        return
    elif options.reset_old_fails:
        clear_failfile()
        return

    # If we're in suite mode, tests is a list of names of sets of tests.
    if options.mode == 'suite':
        tests = expand_suites(tests)
    elif options.mode == 'files':
        tests = [(os.path.abspath(test), True) for test in tests]

    if options.ignore_files != None :
        ignore_patt = re.compile( options.ignore_files )
        print "Ignoring files with pattern: ", ignore_patt

        def ignore_test( test ):
            if ignore_patt.search( test[0] ) != None:
                print "Ignoring test ", test[0]
                return False
            else:
                return True

        tests = filter( ignore_test, tests )

    if not tests:
        print "warning: no tests specified"
        return

    if options.with_cleanbb:
        dbroot = os.path.join(options.smoke_db_prefix, 'data', 'db')
        if options.quiet:
            f = open(server_log_file, "a")
            try:
                call([utils.find_python(), "buildscripts/cleanbb.py", "--nokill", dbroot], stdout=f)
            finally:
                f.close()
        else:
            call([utils.find_python(), "buildscripts/cleanbb.py", "--nokill", dbroot])

    if options.shuffle:
        import random
        random.shuffle(tests)

    if options.skip_tests_until != "":
        filtered_tests = []
        for t in tests:
            name = t[0]
            if name >= name[:name.rfind('/')] + "/" + options.skip_tests_until:
                filtered_tests.append(t)
        tests = filtered_tests

    test_report["start"] = time.time()
    test_report["mongod_running_at_start"] = mongod().is_mongod_up(mongod_port)
    try:
        run_tests(tests)
    finally:
        add_to_failfile(fails, options)

        test_report["end"] = time.time()
        test_report["elapsed"] = test_report["end"] - test_report["start"]
        test_report["failures"] = len(losers.keys())
        test_report["mongod_running_at_end"] = mongod().is_mongod_up(mongod_port)
        if report_file:
            f = open( report_file, "wb" )
            f.write( json.dumps( test_report, indent=4, separators=(',', ': ')) )
            f.close()

        report()

if __name__ == "__main__":
    main()
