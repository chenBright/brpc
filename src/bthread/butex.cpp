// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - An M:N threading library to make applications more concurrent.

// Date: Tue Jul 22 17:30:12 CST 2014

#include "butil/atomicops.h"                // butil::atomic
#include "butil/scoped_lock.h"              // BAIDU_SCOPED_LOCK
#include "butil/macros.h"
#include "butil/containers/flat_map.h"
#include "butil/containers/linked_list.h"   // LinkNode
#ifdef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
#include "butil/memory/singleton_on_pthread_once.h"
#endif
#include "butil/logging.h"
#include "butil/object_pool.h"
#include "bthread/errno.h"                 // EWOULDBLOCK
#include "bthread/sys_futex.h"             // futex_*
#include "bthread/processor.h"             // cpu_relax
#include "bthread/task_control.h"          // TaskControl
#include "bthread/task_group.h"            // TaskGroup
#include "bthread/timer_thread.h"
#include "bthread/butex.h"
#include "bthread/mutex.h"

// This file implements butex.h
// Provides futex-like semantics which is sequenced wait and wake operations
// and guaranteed visibilities.
//
// If wait is sequenced before wake:
//    [thread1]             [thread2]
//    wait()                value = new_value
//                          wake()
// wait() sees unmatched value(fail to wait), or wake() sees the waiter.
//
// If wait is sequenced after wake:
//    [thread1]             [thread2]
//                          value = new_value
//                          wake()
//    wait()
// wake() must provide some sort of memory fence to prevent assignment
// of value to be reordered after it. Thus the value is visible to wait()
// as well.

namespace bthread {

#ifdef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
struct ButexWaiterCount : public bvar::Adder<int64_t> {
    ButexWaiterCount() : bvar::Adder<int64_t>("bthread_butex_waiter_count") {}
};
inline bvar::Adder<int64_t>& butex_waiter_count() {
    return *butil::get_leaky_singleton<ButexWaiterCount>();
}
#endif

enum WaiterState {
    WAITER_STATE_NONE,
    WAITER_STATE_READY,
    WAITER_STATE_TIMEDOUT,
    WAITER_STATE_UNMATCHEDVALUE,
    WAITER_STATE_INTERRUPTED,
};

struct Butex;

struct ButexWaiter : public butil::LinkNode<ButexWaiter> {
    // tids of pthreads are 0
    bthread_t tid;

    // Erasing node from middle of LinkedList is thread-unsafe, we need
    // to hold its container's lock.
    butil::atomic<Butex*> container;
};

// non_pthread_task allocates this structure on stack and queue it in
// Butex::waiters.
struct ButexBthreadWaiter : public ButexWaiter {
    TaskMeta* task_meta;
    TimerThread::TaskId sleep_id;
    WaiterState waiter_state;
    int expected_value;
    Butex* initial_butex;
    TaskControl* control;
    const timespec* abstime;
    bthread_tag_t tag;
};

// pthread_task or main_task allocates this structure on stack and queue it
// in Butex::waiters.
struct ButexPthreadWaiter : public ButexWaiter {
    butil::atomic<int> sig;
};

typedef butil::LinkedList<ButexWaiter> ButexWaiterList;

enum ButexPthreadSignal { PTHREAD_NOT_SIGNALLED, PTHREAD_SIGNALLED };

struct BAIDU_CACHELINE_ALIGNMENT Butex {
    Butex() {}
    ~Butex() {}

    butil::atomic<int> value;
    ButexWaiterList waiters;
    FastPthreadMutex waiter_lock;
};

BAIDU_CASSERT(offsetof(Butex, value) == 0, offsetof_value_must_0);
BAIDU_CASSERT(sizeof(Butex) == BAIDU_CACHELINE_SIZE, butex_fits_in_one_cacheline);

} // namespace bthread

namespace butil {
// Butex object returned to the ObjectPool<Butex> may be accessed,
// so ObjectPool<Butex> can not poison the memory region of Butex.
template <>
struct ObjectPoolWithASanPoison<bthread::Butex> : false_type {};
} // namespace butil

namespace bthread {

static void wakeup_pthread(ButexPthreadWaiter* pw) {
    // release fence makes wait_pthread see changes before wakeup.
    pw->sig.store(PTHREAD_SIGNALLED, butil::memory_order_release);
    // At this point, wait_pthread() possibly has woken up and destroyed `pw'.
    // In which case, futex_wake_private() should return EFAULT.
    // If crash happens in future, `pw' can be made TLS and never destroyed
    // to solve the issue.
    futex_wake_private(&pw->sig, 1);
}

bool erase_from_butex(ButexWaiter*, bool, WaiterState);

int wait_pthread(ButexPthreadWaiter& pw, const timespec* abstime) {
    timespec* ptimeout = NULL;
    timespec timeout;
    int64_t timeout_us = 0;
    int rc;

    while (true) {
        if (abstime != NULL) {
            timeout_us = butil::timespec_to_microseconds(*abstime) - butil::gettimeofday_us();
            timeout = butil::microseconds_to_timespec(timeout_us);
            ptimeout = &timeout;
        }
        if (timeout_us > MIN_SLEEP_US || abstime == NULL) {
            rc = futex_wait_private(&pw.sig, PTHREAD_NOT_SIGNALLED, ptimeout);
            if (PTHREAD_NOT_SIGNALLED != pw.sig.load(butil::memory_order_acquire)) {
                // If `sig' is changed, wakeup_pthread() must be called and `pw'
                // is already removed from the butex.
                // Acquire fence makes this thread sees changes before wakeup.
                return rc;
            }
        } else {
            errno = ETIMEDOUT;
            rc = -1;
        }
        // Handle ETIMEDOUT when abstime is valid.
        // If futex_wait_private return EINTR, just continue the loop.
        if (rc != 0 && errno == ETIMEDOUT) {
            // wait futex timeout, `pw' is still in the queue, remove it.
            if (!erase_from_butex(&pw, false, WAITER_STATE_TIMEDOUT)) {
                // Another thread is erasing `pw' as well, wait for the signal.
                // Acquire fence makes this thread sees changes before wakeup.
                if (pw.sig.load(butil::memory_order_acquire) == PTHREAD_NOT_SIGNALLED) {
                    // already timedout, abstime and ptimeout are expired.
                    abstime = NULL;
                    ptimeout = NULL;
                    continue;
                }
            }
            return rc;
        }
    }
}

extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;

// Returns 0 when no need to unschedule or successfully unscheduled,
// -1 otherwise.
inline int unsleep_if_necessary(ButexBthreadWaiter* w,
                                TimerThread* timer_thread) {
    if (!w->sleep_id) {
        return 0;
    }
    if (timer_thread->unschedule(w->sleep_id) > 0) {
        // the callback is running.
        return -1;
    }
    w->sleep_id = 0;
    return 0;
}

// Use ObjectPool(which never frees memory) to solve the race between
// butex_wake() and butex_destroy(). The race is as follows:
//
//   class Event {
//   public:
//     void wait() {
//       _mutex.lock();
//       if (!_done) {
//         _cond.wait(&_mutex);
//       }
//       _mutex.unlock();
//     }
//     void signal() {
//       _mutex.lock();
//       if (!_done) {
//         _done = true;
//         _cond.signal();
//       }
//       _mutex.unlock();  /*1*/
//     }
//   private:
//     bool _done = false;
//     Mutex _mutex;
//     Condition _cond;
//   };
//
//   [Thread1]                         [Thread2]
//   foo() {
//     Event event;
//     pass_to_thread2(&event);  --->  event.signal();
//     event.wait();
//   } <-- event destroyed
//   
// Summary: Thread1 passes a stateful condition to Thread2 and waits until
// the condition being signalled, which basically means the associated
// job is done and Thread1 can release related resources including the mutex
// and condition. The scenario is fine and the code is correct.
// The race needs a closer look. The unlock at /*1*/ may have different 
// implementations, but in which the last step is probably an atomic store
// and butex_wake(), like this:
//
//   locked->store(0);
//   butex_wake(locked);
//
// The `locked' represents the locking status of the mutex. The issue is that
// just after the store(), the mutex is already unlocked and the code in
// Event.wait() may successfully grab the lock and go through everything
// left and leave foo() function, destroying the mutex and butex, making
// the butex_wake(locked) crash.
// To solve this issue, one method is to add reference before store and
// release the reference after butex_wake. However reference countings need
// to be added in nearly every user scenario of butex_wake(), which is very
// error-prone. Another method is never freeing butex, with the side effect 
// that butex_wake() may wake up an unrelated butex(the one reuses the memory)
// and cause spurious wakeups. According to our observations, the race is 
// infrequent, even rare. The extra spurious wakeups should be acceptable.

void* butex_create() {
    Butex* b = butil::get_object<Butex>();
    if (b) {
        return &b->value;
    }
    return NULL;
}

void butex_destroy(void* butex) {
    if (!butex) {
        return;
    }
    Butex* b = static_cast<Butex*>(
        container_of(static_cast<butil::atomic<int>*>(butex), Butex, value));
    butil::return_object(b);
}

// if TaskGroup tls_task_group is belong to tag
inline bool is_same_tag(bthread_tag_t tag) {
    return tls_task_group && tls_task_group->tag() == tag;
}

//  nosignal is true & tag is same can return true
inline bool check_nosignal(bool nosignal, bthread_tag_t tag) {
    return nosignal && is_same_tag(tag);
}

// if tag is same return tls_task_group else choose one group with tag
inline TaskGroup* get_task_group(TaskControl* c, bthread_tag_t tag) {
    return is_same_tag(tag) ? tls_task_group : c->choose_one_group(tag);
}

inline void run_in_local_task_group(TaskGroup* g, TaskMeta* next_meta, bool nosignal) {
    if (!nosignal) {
        TaskGroup::exchange(&g, next_meta);
    } else {
        g->ready_to_run(next_meta, nosignal);
    }
}

int butex_wake(void* arg, bool nosignal) {
    Butex* b = container_of(static_cast<butil::atomic<int>*>(arg), Butex, value);
    ButexWaiter* front = NULL;
    {
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        if (b->waiters.empty()) {
            return 0;
        }
        front = b->waiters.head()->value();
        front->RemoveFromList();
        front->container.store(NULL, butil::memory_order_relaxed);
    }
    if (front->tid == 0) {
        wakeup_pthread(static_cast<ButexPthreadWaiter*>(front));
        return 1;
    }
    ButexBthreadWaiter* bbw = static_cast<ButexBthreadWaiter*>(front);
    unsleep_if_necessary(bbw, get_global_timer_thread());
    TaskGroup* g = get_task_group(bbw->control, bbw->tag);
    if (g == tls_task_group) {
        run_in_local_task_group(g, bbw->task_meta, nosignal);
    } else {
        g->ready_to_run_remote(bbw->task_meta, check_nosignal(nosignal, g->tag()));
    }
    return 1;
}

int butex_wake_n(void* arg, size_t n, bool nosignal) {
    Butex* b = container_of(static_cast<butil::atomic<int>*>(arg), Butex, value);

    ButexWaiterList bthread_waiters;
    ButexWaiterList pthread_waiters;
    {
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        for (size_t i = 0; (n == 0 || i < n) && !b->waiters.empty(); ++i) {
            ButexWaiter* bw = b->waiters.head()->value();
            bw->RemoveFromList();
            bw->container.store(NULL, butil::memory_order_relaxed);
            if (bw->tid) {
                bthread_waiters.Append(bw);
            } else {
                pthread_waiters.Append(bw);
            }
        }
    }

    int nwakeup = 0;
    while (!pthread_waiters.empty()) {
        ButexPthreadWaiter* bw = static_cast<ButexPthreadWaiter*>(
            pthread_waiters.head()->value());
        bw->RemoveFromList();
        wakeup_pthread(bw);
        ++nwakeup;
    }
    if (bthread_waiters.empty()) {
        return nwakeup;
    }
    butil::FlatMap<bthread_tag_t, TaskGroup*> nwakeups;
    nwakeups.init(FLAGS_task_group_ntags);
    // We will exchange with first waiter in the end.
    ButexBthreadWaiter* next = static_cast<ButexBthreadWaiter*>(
        bthread_waiters.head()->value());
    next->RemoveFromList();
    unsleep_if_necessary(next, get_global_timer_thread());
    ++nwakeup;
    while (!bthread_waiters.empty()) {
        // pop reversely
        ButexBthreadWaiter* w = static_cast<ButexBthreadWaiter*>(
            bthread_waiters.tail()->value());
        w->RemoveFromList();
        unsleep_if_necessary(w, get_global_timer_thread());
        auto g = get_task_group(w->control, w->tag);
        g->ready_to_run_general(w->task_meta, true);
        nwakeups[g->tag()] = g;
        ++nwakeup;
    }
    for (auto it = nwakeups.begin(); it != nwakeups.end(); ++it) {
        auto g = it->second;
        if (!check_nosignal(nosignal, g->tag())) {
            g->flush_nosignal_tasks_general();
        }
    }
    auto g = get_task_group(next->control, next->tag);
    if (g == tls_task_group) {
        run_in_local_task_group(g, next->task_meta, nosignal);
    } else {
        g->ready_to_run_remote(next->task_meta, check_nosignal(nosignal, g->tag()));
    }
    return nwakeup;
}

int butex_wake_all(void* arg, bool nosignal) {
    return butex_wake_n(arg, 0, nosignal);
}

int butex_wake_except(void* arg, bthread_t excluded_bthread) {
    Butex* b = container_of(static_cast<butil::atomic<int>*>(arg), Butex, value);

    ButexWaiterList bthread_waiters;
    ButexWaiterList pthread_waiters;
    {
        ButexWaiter* excluded_waiter = NULL;
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        while (!b->waiters.empty()) {
            ButexWaiter* bw = b->waiters.head()->value();
            bw->RemoveFromList();

            if (bw->tid) {
                if (bw->tid != excluded_bthread) {
                    bthread_waiters.Append(bw);
                    bw->container.store(NULL, butil::memory_order_relaxed);
                } else {
                    excluded_waiter = bw;
                }
            } else {
                bw->container.store(NULL, butil::memory_order_relaxed);
                pthread_waiters.Append(bw);
            }
        }

        if (excluded_waiter) {
            b->waiters.Append(excluded_waiter);
        }
    }

    int nwakeup = 0;
    while (!pthread_waiters.empty()) {
        ButexPthreadWaiter* bw = static_cast<ButexPthreadWaiter*>(
            pthread_waiters.head()->value());
        bw->RemoveFromList();
        wakeup_pthread(bw);
        ++nwakeup;
    }

    if (bthread_waiters.empty()) {
        return nwakeup;
    }
    butil::FlatMap<bthread_tag_t, TaskGroup*> nwakeups;
    nwakeups.init(FLAGS_task_group_ntags);
    do {
        // pop reversely
        ButexBthreadWaiter* w = static_cast<ButexBthreadWaiter*>(bthread_waiters.tail()->value());
        w->RemoveFromList();
        unsleep_if_necessary(w, get_global_timer_thread());
        auto g = get_task_group(w->control, w->tag);
        g->ready_to_run_general(w->task_meta, true);
        nwakeups[g->tag()] = g;
        ++nwakeup;
    } while (!bthread_waiters.empty());
    for (auto it = nwakeups.begin(); it != nwakeups.end(); ++it) {
        auto g = it->second;
        g->flush_nosignal_tasks_general();
    }
    return nwakeup;
}

int butex_requeue(void* arg, void* arg2) {
    Butex* b = container_of(static_cast<butil::atomic<int>*>(arg), Butex, value);
    Butex* m = container_of(static_cast<butil::atomic<int>*>(arg2), Butex, value);

    ButexWaiter* front = NULL;
    {
        std::unique_lock<FastPthreadMutex> lck1(b->waiter_lock, std::defer_lock);
        std::unique_lock<FastPthreadMutex> lck2(m->waiter_lock, std::defer_lock);
        butil::double_lock(lck1, lck2);
        if (b->waiters.empty()) {
            return 0;
        }

        front = b->waiters.head()->value();
        front->RemoveFromList();
        front->container.store(NULL, butil::memory_order_relaxed);

        while (!b->waiters.empty()) {
            ButexWaiter* bw = b->waiters.head()->value();
            bw->RemoveFromList();
            m->waiters.Append(bw);
            bw->container.store(m, butil::memory_order_relaxed);
        }
    }

    if (front->tid == 0) {  // which is a pthread
        wakeup_pthread(static_cast<ButexPthreadWaiter*>(front));
        return 1;
    }
    ButexBthreadWaiter* bbw = static_cast<ButexBthreadWaiter*>(front);
    unsleep_if_necessary(bbw, get_global_timer_thread());
    auto g = is_same_tag(bbw->tag) ? tls_task_group : NULL;
    if (g) {
        TaskGroup::exchange(&g, bbw->task_meta);
    } else {
        bbw->control->choose_one_group(bbw->tag)->ready_to_run_remote(bbw->task_meta);
    }
    return 1;
}

// Callable from multiple threads, at most one thread may wake up the waiter.
static void erase_from_butex_and_wakeup(void* arg) {
    erase_from_butex(static_cast<ButexWaiter*>(arg), true, WAITER_STATE_TIMEDOUT);
}

// Used in task_group.cpp
bool erase_from_butex_because_of_interruption(ButexWaiter* bw) {
    return erase_from_butex(bw, true, WAITER_STATE_INTERRUPTED);
}

inline bool erase_from_butex(ButexWaiter* bw, bool wakeup, WaiterState state) {
    // `bw' is guaranteed to be valid inside this function because waiter
    // will wait until this function being cancelled or finished.
    // NOTE: This function must be no-op when bw->container is NULL.
    bool erased = false;
    Butex* b;
    int saved_errno = errno;
    while ((b = bw->container.load(butil::memory_order_acquire))) {
        // b can be NULL when the waiter is scheduled but queued.
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        if (b == bw->container.load(butil::memory_order_relaxed)) {
            bw->RemoveFromList();
            bw->container.store(NULL, butil::memory_order_relaxed);
            if (bw->tid) {
                static_cast<ButexBthreadWaiter*>(bw)->waiter_state = state;
            }
            erased = true;
            break;
        }
    }
    if (erased && wakeup) {
        if (bw->tid) {
            ButexBthreadWaiter* bbw = static_cast<ButexBthreadWaiter*>(bw);
            get_task_group(bbw->control, bbw->tag)->ready_to_run_general(bbw->task_meta);
        } else {
            ButexPthreadWaiter* pw = static_cast<ButexPthreadWaiter*>(bw);
            wakeup_pthread(pw);
        }
    }
    errno = saved_errno;
    return erased;
}

struct WaitForButexArgs {
    ButexBthreadWaiter* bw;
    bool prepend;
};

void wait_for_butex(void* arg) {
    auto args = static_cast<WaitForButexArgs*>(arg);
    ButexBthreadWaiter* const bw = args->bw;
    Butex* const b = bw->initial_butex;
    // 1: waiter with timeout should have waiter_state == WAITER_STATE_READY
    //    before they're queued, otherwise the waiter is already timedout
    //    and removed by TimerThread, in which case we should stop queueing.
    //
    // Visibility of waiter_state:
    //    [bthread]                         [TimerThread]
    //    waiter_state = TIMED
    //    tt_lock { add task }
    //                                      tt_lock { get task }
    //                                      waiter_lock { waiter_state=TIMEDOUT }
    //    waiter_lock { use waiter_state }
    // tt_lock represents TimerThread::_mutex. Visibility of waiter_state is
    // sequenced by two locks, both threads are guaranteed to see the correct
    // value.
    {
        BAIDU_SCOPED_LOCK(b->waiter_lock);
        if (b->value.load(butil::memory_order_relaxed) != bw->expected_value) {
            bw->waiter_state = WAITER_STATE_UNMATCHEDVALUE;
        } else if (bw->waiter_state == WAITER_STATE_READY/*1*/ &&
                   !bw->task_meta->interrupted) {
            if (args->prepend) {
                b->waiters.Prepend(bw);
            } else {
                b->waiters.Append(bw);
            }
            bw->container.store(b, butil::memory_order_relaxed);
#ifdef BRPC_BTHREAD_TRACER
            bw->control->_task_tracer.set_status(TASK_STATUS_SUSPENDED, bw->task_meta);
#endif // BRPC_BTHREAD_TRACER
            if (bw->abstime != NULL) {
                bw->sleep_id = get_global_timer_thread()->schedule(
                    erase_from_butex_and_wakeup, bw, *bw->abstime);
                if (!bw->sleep_id) {  // TimerThread stopped.
                    errno = ESTOP;
                    erase_from_butex_and_wakeup(bw);
                }
            }
            return;
        }
    }
    
    // b->container is NULL which makes erase_from_butex_and_wakeup() and
    // TaskGroup::interrupt() no-op, there's no race between following code and
    // the two functions. The on-stack ButexBthreadWaiter is safe to use and
    // bw->waiter_state will not change again.
    // unsleep_if_necessary(bw, get_global_timer_thread());
    tls_task_group->ready_to_run(bw->task_meta);
    // FIXME: jump back to original thread is buggy.
    
    // // Value unmatched or waiter is already woken up by TimerThread, jump
    // // back to original bthread.
    // TaskGroup* g = tls_task_group;
    // ReadyToRunArgs args = { g->current_tid(), false };
    // g->set_remained(TaskGroup::ready_to_run_in_worker, &args);
    // // 2: Don't run remained because we're already in a remained function
    // //    otherwise stack may overflow.
    // TaskGroup::sched_to(&g, bw->tid, false/*2*/);
}

static int butex_wait_from_pthread(TaskGroup* g, Butex* b, int expected_value,
                                   const timespec* abstime, bool prepend) {
    TaskMeta* task = NULL;
    ButexPthreadWaiter pw;
    pw.tid = 0;
    pw.sig.store(PTHREAD_NOT_SIGNALLED, butil::memory_order_relaxed);
    int rc = 0;
    
    if (g) {
        task = g->current_task();
        task->current_waiter.store(&pw, butil::memory_order_release);
    }
    b->waiter_lock.lock();
    if (b->value.load(butil::memory_order_relaxed) != expected_value) {
        b->waiter_lock.unlock();
        errno = EWOULDBLOCK;
        rc = -1;
    } else if (task != NULL && task->interrupted) {
        b->waiter_lock.unlock();
        // Race with set and may consume multiple interruptions, which are OK.
        task->interrupted = false;
        errno = EINTR;
        rc = -1;
    } else {
        if (prepend) {
            b->waiters.Prepend(&pw);
        } else {
            b->waiters.Append(&pw);
        }
        pw.container.store(b, butil::memory_order_relaxed);
        b->waiter_lock.unlock();

#ifdef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
        bvar::Adder<int64_t>& num_waiters = butex_waiter_count();
        num_waiters << 1;
#endif
        rc = wait_pthread(pw, abstime);
#ifdef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
        num_waiters << -1;
#endif
    }
    if (task) {
        // If current_waiter is NULL, TaskGroup::interrupt() is running and
        // using pw, spin until current_waiter != NULL.
        BT_LOOP_WHEN(task->current_waiter.exchange(
                         NULL, butil::memory_order_acquire) == NULL,
                     30/*nops before sched_yield*/);
        if (task->interrupted) {
            task->interrupted = false;
            if (rc == 0) {
                errno = EINTR;
                return -1;
            }
        }
    }
    return rc;
}

int butex_wait(void* arg, int expected_value, const timespec* abstime, bool prepend) {
    Butex* b = container_of(static_cast<butil::atomic<int>*>(arg), Butex, value);
    if (b->value.load(butil::memory_order_relaxed) != expected_value) {
        errno = EWOULDBLOCK;
        // Sometimes we may take actions immediately after unmatched butex,
        // this fence makes sure that we see changes before changing butex.
        butil::atomic_thread_fence(butil::memory_order_acquire);
        return -1;
    }
    TaskGroup* g = tls_task_group;
    if (NULL == g || g->is_current_pthread_task()) {
        return butex_wait_from_pthread(g, b, expected_value, abstime, prepend);
    }
    ButexBthreadWaiter bbw;
    // tid is 0 iff the thread is non-bthread
    bbw.tid = g->current_tid();
    bbw.container.store(NULL, butil::memory_order_relaxed);
    bbw.task_meta = g->current_task();
    bbw.sleep_id = 0;
    bbw.waiter_state = WAITER_STATE_READY;
    bbw.expected_value = expected_value;
    bbw.initial_butex = b;
    bbw.control = g->control();
    bbw.abstime = abstime;
    bbw.tag = g->tag();

    if (abstime != NULL) {
        // Schedule timer before queueing. If the timer is triggered before
        // queueing, cancel queueing. This is a kind of optimistic locking.
        if (butil::timespec_to_microseconds(*abstime) <
            (butil::gettimeofday_us() + MIN_SLEEP_US)) {
            // Already timed out.
            errno = ETIMEDOUT;
            return -1;
        }
    }
#ifdef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
    bvar::Adder<int64_t>& num_waiters = butex_waiter_count();
    num_waiters << 1;
#endif

    // release fence matches with acquire fence in interrupt_and_consume_waiters
    // in task_group.cpp to guarantee visibility of `interrupted'.
    bbw.task_meta->current_waiter.store(&bbw, butil::memory_order_release);
    WaitForButexArgs args{ &bbw, prepend };
    g->set_remained(wait_for_butex, &args);
    TaskGroup::sched(&g);

    // erase_from_butex_and_wakeup (called by TimerThread) is possibly still
    // running and using bbw. The chance is small, just spin until it's done.
    BT_LOOP_WHEN(unsleep_if_necessary(&bbw, get_global_timer_thread()) < 0,
                 30/*nops before sched_yield*/);
    
    // If current_waiter is NULL, TaskGroup::interrupt() is running and using bbw.
    // Spin until current_waiter != NULL.
    BT_LOOP_WHEN(bbw.task_meta->current_waiter.exchange(
                     NULL, butil::memory_order_acquire) == NULL,
                 30/*nops before sched_yield*/);
#ifdef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
    num_waiters << -1;
#endif

    bool is_interrupted = false;
    if (bbw.task_meta->interrupted) {
        // Race with set and may consume multiple interruptions, which are OK.
        bbw.task_meta->interrupted = false;
        is_interrupted = true;
    }
    // If timed out as well as value unmatched, return ETIMEDOUT.
    if (WAITER_STATE_TIMEDOUT == bbw.waiter_state) {
        errno = ETIMEDOUT;
        return -1;
    } else if (WAITER_STATE_UNMATCHEDVALUE == bbw.waiter_state) {
        errno = EWOULDBLOCK;
        return -1;
    } else if (is_interrupted) {
        errno = EINTR;
        return -1;
    }
    return 0;
}

}  // namespace bthread

namespace butil {
template <> struct ObjectPoolBlockMaxItem<bthread::Butex> {
    static const size_t value = 128;
};
}
