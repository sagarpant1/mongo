// @file rwlockimpl.cpp

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <map>
#include <set>
#include <boost/version.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>

using namespace std;

#include "../assert_util.h"
#include "../time_support.h"
#include "rwlockimpl.h"
#include "simplerwlock.h"
#include "threadlocal.h"

namespace mongo {

#if defined(_WIN32) && defined(_DEBUG)
    // the code below in _DEBUG build will check that we don't try to recursively lock, 
    // which is not supported by this class.  also checks that you don't unlock without 
    // having locked
    void SimpleRWLock::lock() {
        unsigned me = GetCurrentThreadId();
        int& state = s.getRef();
        dassert( state == 0 );
        state--;
        m.lock();
        tid = me; // this is for use in the debugger to see who does have the lock
    }
    void SimpleRWLock::unlock() { 
        int& state = s.getRef();
        dassert( state == -1 );
        state++;
        tid = 0xffffffff;
        m.unlock(); 
    }
    void SimpleRWLock::lock_shared() { 
        int& state = s.getRef();
        dassert( state == 0 );
        state++;
        m.lock_shared(); 
        shares++;
    }
    void SimpleRWLock::unlock_shared() { 
        int& state = s.getRef();
        dassert( state == 1 );
        state--;
        shares--;
        m.unlock_shared(); 
    }
#else
    void SimpleRWLock::lock() { m.lock(); }
    void SimpleRWLock::unlock() { m.unlock(); }
    void SimpleRWLock::lock_shared() { m.lock_shared(); }
    void SimpleRWLock::unlock_shared() { m.unlock_shared(); }
#endif

}
