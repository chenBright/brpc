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

// Date: Tue Jul 10 17:40:58 CST 2012

#include <sys/types.h>
#include <stddef.h>                         // size_t
#include <gflags/gflags.h>
#include "butil/compat.h"                   // OS_MACOSX
#include "butil/macros.h"                   // ARRAY_SIZE
#include "butil/scoped_lock.h"              // BAIDU_SCOPED_LOCK
#include "butil/fast_rand.h"
#include "butil/unique_ptr.h"
#include "butil/third_party/murmurhash3/murmurhash3.h" // fmix64
#include "butil/reloadable_flags.h"
#include "bthread/errno.h"                  // ESTOP
#include "bthread/butex.h"                  // butex_*
#include "bthread/sys_futex.h"              // futex_wake_private
#include "bthread/processor.h"              // cpu_relax
#include "bthread/task_control.h"
#include "bthread/task_group.h"
#include "bthread/timer_thread.h"

#ifdef __x86_64__
#include <x86intrin.h>
#endif // __x86_64__

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

namespace bthread {

static const bthread_attr_t BTHREAD_ATTR_TASKGROUP = {
    BTHREAD_STACKTYPE_UNKNOWN, 0, NULL, BTHREAD_TAG_INVALID };

DEFINE_bool(show_bthread_creation_in_vars, false, "When this flags is on, The time "
            "from bthread creation to first run will be recorded and shown in /vars");
BUTIL_VALIDATE_GFLAG(show_bthread_creation_in_vars, butil::PassValidate);

DEFINE_bool(show_per_worker_usage_in_vars, false,
            "Show per-worker usage in /vars/bthread_per_worker_usage_<tid>");
BUTIL_VALIDATE_GFLAG(show_per_worker_usage_in_vars, butil::PassValidate);

DEFINE_bool(bthread_enable_cpu_clock_stat, false,
            "Enable CPU clock statistics for bthread");
BUTIL_VALIDATE_GFLAG(bthread_enable_cpu_clock_stat, butil::PassValidate);

BAIDU_VOLATILE_THREAD_LOCAL(TaskGroup*, tls_task_group, NULL);
// Sync with TaskMeta::local_storage when a bthread is created or destroyed.
// During running, the two fields may be inconsistent, use tls_bls as the
// groundtruth.
BAIDU_VOLATILE_THREAD_LOCAL(LocalStorage, tls_bls, BTHREAD_LOCAL_STORAGE_INITIALIZER);

// defined in bthread/key.cpp
extern void return_keytable(bthread_keytable_pool_t*, KeyTable*);

// [Hacky] This is a special TLS set by bthread-rpc privately... to save
// overhead of creation keytable, may be removed later.
BAIDU_VOLATILE_THREAD_LOCAL(void*, tls_unique_user_ptr, NULL);

const TaskStatistics EMPTY_STAT = { 0, 0, 0 };

void* (*g_create_span_func)() = NULL;

void* run_create_span_func() {
    if (g_create_span_func) {
        return g_create_span_func();
    }
    return BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_bls).rpcz_parent_span;
}

AtomicInteger128::Value AtomicInteger128::load() const {
#if __x86_64__ || __ARM_NEON
    // Supress compiler warning.
    (void)_mutex;
#endif // __x86_64__ || __ARM_NEON

#if __x86_64__ || __ARM_NEON
#ifdef __x86_64__
    __m128i value = _mm_load_si128(reinterpret_cast<const __m128i*>(&_value));
#else // __ARM_NEON
    int64x2_t value = vld1q_s64(reinterpret_cast<const int64_t*>(&_value));
#endif // __x86_64__
    return {value[0], value[1]};
#else // __x86_64__ || __ARM_NEON
    BAIDU_SCOPED_LOCK(_mutex);
    return _value;
#endif // __x86_64__ || __ARM_NEON
}

void AtomicInteger128::store(Value value) {
#if __x86_64__
    __m128i v = _mm_load_si128(reinterpret_cast<__m128i*>(&value));
    _mm_store_si128(reinterpret_cast<__m128i*>(&_value), v);
#elif __ARM_NEON
    int64x2_t v = vld1q_s64(reinterpret_cast<int64_t*>(&value));
    vst1q_s64(reinterpret_cast<int64_t*>(&_value), v);
#else
    BAIDU_SCOPED_LOCK(_mutex);
    _value = value;
#endif // __x86_64__ || __ARM_NEON
}


int TaskGroup::get_attr(bthread_t tid, bthread_attr_t* out) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            *out = m->attr;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

void TaskGroup::set_stopped(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            m->stop = true;
        }
    }
}

bool TaskGroup::is_stopped(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            return m->stop;
        }
    }
    // If the tid does not exist or version does not match, it's intuitive
    // to treat the thread as "stopped".
    return true;
}

bool TaskGroup::wait_task(bthread_t* tid) {
    do {
#ifndef BTHREAD_DONT_SAVE_PARKING_STATE
        if (_last_pl_state.stopped()) {
            return false;
        }
        _pl->wait(_last_pl_state);
        if (steal_task(tid)) {
            return true;
        }
#else
        const ParkingLot::State st = _pl->get_state();
        if (st.stopped()) {
            return false;
        }
        if (steal_task(tid)) {
            return true;
        }
        _pl->wait(st);
#endif
    } while (true);
}

static double get_cumulated_cputime_from_this(void* arg) {
    return static_cast<TaskGroup*>(arg)->cumulated_cputime_ns() / 1000000000.0;
}

int64_t TaskGroup::cumulated_cputime_ns() const {
    CPUTimeStat cpu_time_stat = _cpu_time_stat.load();
    // Add the elapsed time of running bthread.
    int64_t cumulated_cputime_ns = cpu_time_stat.cumulated_cputime_ns();
    if (!cpu_time_stat.is_main_task()) {
        cumulated_cputime_ns += butil::cpuwide_time_ns() - cpu_time_stat.last_run_ns();
    }
    return cumulated_cputime_ns;
}

void TaskGroup::run_main_task() {
    bvar::PassiveStatus<double> cumulated_cputime(
        get_cumulated_cputime_from_this, this);
    std::unique_ptr<bvar::PerSecond<bvar::PassiveStatus<double> > > usage_bvar;

    TaskGroup* dummy = this;
    bthread_t tid;
    while (wait_task(&tid)) {
        sched_to(&dummy, tid);
        DCHECK_EQ(this, dummy);
        DCHECK_EQ(_cur_meta->stack, _main_stack);
        if (_cur_meta->tid != _main_tid) {
            task_runner(1/*skip remained*/);
        }
        if (FLAGS_show_per_worker_usage_in_vars && !usage_bvar) {
            char name[32];
#if defined(OS_MACOSX)
            snprintf(name, sizeof(name), "bthread_worker_usage_%" PRIu64,
                     pthread_numeric_id());
#else
            snprintf(name, sizeof(name), "bthread_worker_usage_%ld",
                     (long)syscall(SYS_gettid));
#endif
            usage_bvar.reset(new bvar::PerSecond<bvar::PassiveStatus<double> >
                             (name, &cumulated_cputime, 1));
        }
    }
    // Don't forget to add elapse of last wait_task.
    current_task()->stat.cputime_ns +=
        butil::cpuwide_time_ns() - _cpu_time_stat.load_unsafe().last_run_ns();
}

TaskGroup::TaskGroup(TaskControl* c)
    :  _control(c) {
    CHECK(c);
}

TaskGroup::~TaskGroup() {
    if (_main_tid) {
        TaskMeta* m = address_meta(_main_tid);
        CHECK(_main_stack == m->stack);
#ifdef BUTIL_USE_ASAN
        _main_stack->storage.bottom = NULL;
        _main_stack->storage.stacksize = 0;
#endif // BUTIL_USE_ASAN
        return_stack(m->release_stack());
        return_resource(get_slot(_main_tid));
        _main_tid = 0;
    }
}

#ifdef BUTIL_USE_ASAN
int PthreadAttrGetStack(void*& stack_addr, size_t& stack_size) {
#if defined(OS_MACOSX)
    stack_addr = pthread_get_stackaddr_np(pthread_self());
    stack_size = pthread_get_stacksize_np(pthread_self());
    return 0;
#else
    pthread_attr_t attr;
    int rc = pthread_getattr_np(pthread_self(), &attr);
    if (0 != rc) {
        LOG(ERROR) << "Fail to get pthread attributes: " << berror(rc);
        return rc;
    }
    rc = pthread_attr_getstack(&attr, &stack_addr, &stack_size);
    if (0 != rc) {
        LOG(ERROR) << "Fail to get pthread stack: " << berror(rc);
    }
    pthread_attr_destroy(&attr);
    return rc;
#endif // OS_MACOSX
}
#endif // BUTIL_USE_ASAN

int TaskGroup::init(size_t runqueue_capacity) {
    if (_rq.init(runqueue_capacity) != 0) {
        LOG(FATAL) << "Fail to init _rq";
        return -1;
    }
    if (_remote_rq.init(runqueue_capacity / 2) != 0) {
        LOG(FATAL) << "Fail to init _remote_rq";
        return -1;
    }

#ifdef BUTIL_USE_ASAN
    void* stack_addr = NULL;
    size_t stack_size = 0;
    if (0 != PthreadAttrGetStack(stack_addr, stack_size)) {
        return -1;
    }
#endif // BUTIL_USE_ASAN

    ContextualStack* stk = get_stack(STACK_TYPE_MAIN, NULL);
    if (NULL == stk) {
        LOG(FATAL) << "Fail to get main stack container";
        return -1;
    }
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource<TaskMeta>(&slot);
    if (NULL == m) {
        LOG(FATAL) << "Fail to get TaskMeta";
        return -1;
    }
    m->sleep_failed = false;
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = NULL;
    m->arg = NULL;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = butil::cpuwide_time_ns();
    m->stat = EMPTY_STAT;
    m->attr = BTHREAD_ATTR_TASKGROUP;
    m->tid = make_tid(*m->version_butex, slot);
    m->set_stack(stk);

#ifdef BUTIL_USE_ASAN
    stk->storage.bottom = stack_addr;
    stk->storage.stacksize = stack_size;
    // No guard size required for ASan.
#endif // BUTIL_USE_ASAN

    _cur_meta = m;
    _main_tid = m->tid;
    _main_stack = stk;

    CPUTimeStat cpu_time_stat;
    cpu_time_stat.set_last_run_ns(m->cpuwide_start_ns, true);
    _cpu_time_stat.store(cpu_time_stat);
    _last_cpu_clock_ns = 0;

    return 0;
}

#ifdef BUTIL_USE_ASAN
void TaskGroup::asan_task_runner(intptr_t) {
    // This is a new thread, and it doesn't have the fake stack yet. ASan will
    // create it lazily, for now just pass NULL.
    internal::FinishSwitchFiber(NULL);
    task_runner(0);
}
#endif // BUTIL_USE_ASAN

void TaskGroup::task_runner(intptr_t skip_remained) {
    // NOTE: tls_task_group is volatile since tasks are moved around
    //       different groups.
    TaskGroup* g = tls_task_group;
#ifdef BRPC_BTHREAD_TRACER
    TaskTracer::set_running_status(g->tid(), g->_cur_meta);
#endif // BRPC_BTHREAD_TRACER

    if (!skip_remained) {
        while (g->_last_context_remained) {
            RemainedFn fn = g->_last_context_remained;
            g->_last_context_remained = NULL;
            fn(g->_last_context_remained_arg);
            g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
        }

#ifndef NDEBUG
        --g->_sched_recursive_guard;
#endif
    }

    do {
        // A task can be stopped before it gets running, in which case
        // we may skip user function, but that may confuse user:
        // Most tasks have variables to remember running result of the task,
        // which is often initialized to values indicating success. If an
        // user function is never called, the variables will be unchanged
        // however they'd better reflect failures because the task is stopped
        // abnormally.

        // Meta and identifier of the task is persistent in this run.
        TaskMeta* const m = g->_cur_meta;

        if (FLAGS_show_bthread_creation_in_vars) {
            // NOTE: the thread triggering exposure of pending time may spend
            // considerable time because a single bvar::LatencyRecorder
            // contains many bvar.
            g->_control->exposed_pending_time() <<
                (butil::cpuwide_time_ns() - m->cpuwide_start_ns) / 1000L;
        }

        // Not catch exceptions except ExitException which is for implementing
        // bthread_exit(). User code is intended to crash when an exception is
        // not caught explicitly. This is consistent with other threading
        // libraries.
        void* thread_return;
        try {
            thread_return = m->fn(m->arg);
        } catch (ExitException& e) {
            thread_return = e.value();
        }

        // TODO: Save thread_return
        (void)thread_return;

        // Logging must be done before returning the keytable, since the logging lib
        // use bthread local storage internally, or will cause memory leak.
        // FIXME: the time from quiting fn to here is not counted into cputime
        if (m->attr.flags & BTHREAD_LOG_START_AND_FINISH) {
            LOG(INFO) << "Finished bthread " << m->tid << ", cputime="
                      << m->stat.cputime_ns / 1000000.0 << "ms";
        }

        // Clean tls variables, must be done before changing version_butex
        // otherwise another thread just joined this thread may not see side
        // effects of destructing tls variables.
        LocalStorage* tls_bls_ptr = BAIDU_GET_PTR_VOLATILE_THREAD_LOCAL(tls_bls);
        KeyTable* kt = tls_bls_ptr->keytable;
        if (kt != NULL) {
            return_keytable(m->attr.keytable_pool, kt);
            // After deletion: tls may be set during deletion.
            tls_bls_ptr = BAIDU_GET_PTR_VOLATILE_THREAD_LOCAL(tls_bls);
            tls_bls_ptr->keytable = NULL;
            m->local_storage.keytable = NULL; // optional
        }

        // During running the function in TaskMeta and deleting the KeyTable in
        // return_KeyTable, the group is probably changed.
        g =  BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);

        // Increase the version and wake up all joiners, if resulting version
        // is 0, change it to 1 to make bthread_t never be 0. Any access
        // or join to the bthread after changing version will be rejected.
        // The spinlock is for visibility of TaskGroup::get_attr.
#ifdef BRPC_BTHREAD_TRACER
        bool tracing = false;
#endif // BRPC_BTHREAD_TRACER
        {
            BAIDU_SCOPED_LOCK(m->version_lock);
#ifdef BRPC_BTHREAD_TRACER
            tracing = TaskTracer::set_end_status_unsafe(m);
#endif // BRPC_BTHREAD_TRACER
            if (0 == ++*m->version_butex) {
                ++*m->version_butex;
            }
        }
        butex_wake_except(m->version_butex, 0);

#ifdef BRPC_BTHREAD_TRACER
        if (tracing) {
            // Wait for tracing completion.
            g->_control->_task_tracer.WaitForTracing(m);
        }
        g->_control->_task_tracer.set_status(TASK_STATUS_UNKNOWN, m);
#endif // BRPC_BTHREAD_TRACER

        g->_control->_nbthreads << -1;
        g->_control->tag_nbthreads(g->tag()) << -1;
        g->set_remained(_release_last_context, m);
        ending_sched(&g);

    } while (g->_cur_meta->tid != g->_main_tid);

    // Was called from a pthread and we don't have BTHREAD_STACKTYPE_PTHREAD
    // tasks to run, quit for more tasks.
}

void TaskGroup::_release_last_context(void* arg) {
    TaskMeta* m = static_cast<TaskMeta*>(arg);
    if (m->stack_type() != STACK_TYPE_PTHREAD) {
        return_stack(m->release_stack()/*may be NULL*/);
    } else {
        // it's _main_stack, don't return.
        m->set_stack(NULL);
    }
    return_resource(get_slot(m->tid));
}

int TaskGroup::start_foreground(TaskGroup** pg,
                                bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (BAIDU_UNLIKELY(NULL == m)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->sleep_failed = false;
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    if (using_attr.flags & BTHREAD_INHERIT_SPAN) {
        m->local_storage.rpcz_parent_span = run_create_span_func();
    }
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }

    TaskGroup* g = *pg;
    g->_control->_nbthreads << 1;
    g->_control->tag_nbthreads(g->tag()) << 1;
#ifdef BRPC_BTHREAD_TRACER
    g->_control->_task_tracer.set_status(TASK_STATUS_CREATED, m);
#endif // BRPC_BTHREAD_TRACER
    if (g->is_current_pthread_task()) {
        // never create foreground task in pthread.
        g->ready_to_run(m, using_attr.flags & BTHREAD_NOSIGNAL);
    } else {
        // NOSIGNAL affects current task, not the new task.
        RemainedFn fn = NULL;
        auto& cur_attr = g->_cur_meta->attr;
        if (cur_attr.flags & BTHREAD_GLOBAL_PRIORITY) {
            fn = priority_to_run;
        } else if (g->current_task()->about_to_quit) {
            fn = ready_to_run_in_worker_ignoresignal;
        } else {
            fn = ready_to_run_in_worker;
        }
        ReadyToRunArgs args = {
            g->tag(), g->_cur_meta, (bool)(using_attr.flags & BTHREAD_NOSIGNAL)
        };
        g->set_remained(fn, &args);
        sched_to(pg, m->tid);
    }
    return 0;
}

template <bool REMOTE>
int TaskGroup::start_background(bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (BAIDU_UNLIKELY(NULL == m)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->sleep_failed = false;
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    if (using_attr.flags & BTHREAD_INHERIT_SPAN) {
        m->local_storage.rpcz_parent_span = run_create_span_func();
    }
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }
    _control->_nbthreads << 1;
    _control->tag_nbthreads(tag()) << 1;
#ifdef BRPC_BTHREAD_TRACER
    _control->_task_tracer.set_status(TASK_STATUS_CREATED, m);
#endif // BRPC_BTHREAD_TRACER
    if (REMOTE) {
        ready_to_run_remote(m, (using_attr.flags & BTHREAD_NOSIGNAL));
    } else {
        ready_to_run(m, (using_attr.flags & BTHREAD_NOSIGNAL));
    }
    return 0;
}

// Explicit instantiations.
template int
TaskGroup::start_background<true>(bthread_t* __restrict th,
                                  const bthread_attr_t* __restrict attr,
                                  void * (*fn)(void*),
                                  void* __restrict arg);
template int
TaskGroup::start_background<false>(bthread_t* __restrict th,
                                   const bthread_attr_t* __restrict attr,
                                   void * (*fn)(void*),
                                   void* __restrict arg);

int TaskGroup::join(bthread_t tid, void** return_value) {
    if (__builtin_expect(!tid, 0)) {  // tid of bthread is never 0.
        return EINVAL;
    }
    TaskMeta* m = address_meta(tid);
    if (BAIDU_UNLIKELY(NULL == m)) {
        // The bthread is not created yet, this join is definitely wrong.
        return EINVAL;
    }
    TaskGroup* g = tls_task_group;
    if (g != NULL && g->current_tid() == tid) {
        // joining self causes indefinite waiting.
        return EINVAL;
    }
    const uint32_t expected_version = get_version(tid);
    while (*m->version_butex == expected_version) {
        if (butex_wait(m->version_butex, expected_version, NULL) < 0 &&
            errno != EWOULDBLOCK && errno != EINTR) {
            return errno;
        }
    }
    if (return_value) {
        *return_value = NULL;
    }
    return 0;
}

bool TaskGroup::exists(bthread_t tid) {
    if (tid != 0) {  // tid of bthread is never 0.
        TaskMeta* m = address_meta(tid);
        if (m != NULL) {
            return (*m->version_butex == get_version(tid));
        }
    }
    return false;
}

TaskStatistics TaskGroup::main_stat() const {
    TaskMeta* m = address_meta(_main_tid);
    return m ? m->stat : EMPTY_STAT;
}

void TaskGroup::ending_sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
#ifndef BTHREAD_FAIR_WSQ
    // When BTHREAD_FAIR_WSQ is defined, profiling shows that cpu cost of
    // WSQ::steal() in example/multi_threaded_echo_c++ changes from 1.9%
    // to 2.9%
    const bool popped = g->_rq.pop(&next_tid);
#else
    const bool popped = g->_rq.steal(&next_tid);
#endif
    if (!popped && !g->steal_task(&next_tid)) {
        // Jump to main task if there's no task to run.
        next_tid = g->_main_tid;
    }

    TaskMeta* const cur_meta = g->_cur_meta;
    TaskMeta* next_meta = address_meta(next_tid);
    if (next_meta->stack == NULL) {
        if (next_meta->stack_type() == cur_meta->stack_type()) {
            // Reuse the stack of the current ending task.
            //
            // also works with pthread_task scheduling to pthread_task, the
            // transfered stack is just _main_stack.
            next_meta->set_stack(cur_meta->release_stack());
        } else {
#ifdef BUTIL_USE_ASAN
            ContextualStack* stk = get_stack(
                next_meta->stack_type(), asan_task_runner);
#else
            ContextualStack* stk = get_stack(next_meta->stack_type(), task_runner);
#endif // BUTIL_USE_ASAN
            if (stk) {
                next_meta->set_stack(stk);
            } else {
                // stack_type is BTHREAD_STACKTYPE_PTHREAD or out of memory,
                // In latter case, attr is forced to be BTHREAD_STACKTYPE_PTHREAD.
                // This basically means that if we can't allocate stack, run
                // the task in pthread directly.
                next_meta->attr.stack_type = BTHREAD_STACKTYPE_PTHREAD;
                next_meta->set_stack(g->_main_stack);
            }
        }
    }
    sched_to(pg, next_meta, true);
}

void TaskGroup::sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
#ifndef BTHREAD_FAIR_WSQ
    const bool popped = g->_rq.pop(&next_tid);
#else
    const bool popped = g->_rq.steal(&next_tid);
#endif
    if (!popped && !g->steal_task(&next_tid)) {
        // Jump to main task if there's no task to run.
        next_tid = g->_main_tid;
    }
    sched_to(pg, next_tid);
}

extern void CheckBthreadScheSafety();

void TaskGroup::sched_to(TaskGroup** pg, TaskMeta* next_meta, bool cur_ending) {
    TaskGroup* g = *pg;
#ifndef NDEBUG
    if ((++g->_sched_recursive_guard) > 1) {
        LOG(FATAL) << "Recursively(" << g->_sched_recursive_guard - 1
                   << ") call sched_to(" << g << ")";
    }
#endif
    // Save errno so that errno is bthread-specific.
    int saved_errno = errno;
    void* saved_unique_user_ptr = tls_unique_user_ptr;

    TaskMeta* const cur_meta = g->_cur_meta;
    int64_t now = butil::cpuwide_time_ns();
    CPUTimeStat cpu_time_stat = g->_cpu_time_stat.load_unsafe();
    int64_t elp_ns = now - cpu_time_stat.last_run_ns();
    cur_meta->stat.cputime_ns += elp_ns;
    // Update cpu_time_stat.
    cpu_time_stat.set_last_run_ns(now, is_main_task(g, next_meta->tid));
    cpu_time_stat.add_cumulated_cputime_ns(elp_ns, is_main_task(g, cur_meta->tid));
    g->_cpu_time_stat.store(cpu_time_stat);

    if (FLAGS_bthread_enable_cpu_clock_stat) {
        const int64_t cpu_thread_time = butil::cputhread_time_ns();
        if (g->_last_cpu_clock_ns != 0) {
            cur_meta->stat.cpu_usage_ns += cpu_thread_time - g->_last_cpu_clock_ns;
        }
        g->_last_cpu_clock_ns = cpu_thread_time;
    } else {
        g->_last_cpu_clock_ns = 0;
    }

    ++cur_meta->stat.nswitch;
    ++ g->_nswitch;
    // Switch to the task
    if (__builtin_expect(next_meta != cur_meta, 1)) {
        g->_cur_meta = next_meta;
        // Switch tls_bls
        cur_meta->local_storage = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_bls);
        BAIDU_SET_VOLATILE_THREAD_LOCAL(tls_bls, next_meta->local_storage);

        // Logging must be done after switching the local storage, since the logging lib
        // use bthread local storage internally, or will cause memory leak.
        if ((cur_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH) ||
            (next_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH)) {
            LOG(INFO) << "Switch bthread: " << cur_meta->tid << " -> "
                      << next_meta->tid;
        }

        if (cur_meta->stack != NULL) {
            if (next_meta->stack != cur_meta->stack) {
                CheckBthreadScheSafety();
#ifdef BRPC_BTHREAD_TRACER
                g->_control->_task_tracer.set_status(TASK_STATUS_JUMPING, cur_meta);
                g->_control->_task_tracer.set_status(TASK_STATUS_JUMPING, next_meta);
#endif // BRPC_BTHREAD_TRACER
                {
                    BTHREAD_SCOPED_ASAN_FIBER_SWITCHER(next_meta->stack->storage);
                    jump_stack(cur_meta->stack, next_meta->stack);
                }
                // probably went to another group, need to assign g again.
                g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
#ifdef BRPC_BTHREAD_TRACER
                TaskTracer::set_running_status(g->tid(), g->_cur_meta);
#endif // BRPC_BTHREAD_TRACER
            }
#ifndef NDEBUG
            else {
                // else pthread_task is switching to another pthread_task, sc
                // can only equal when they're both _main_stack
                CHECK(cur_meta->stack == g->_main_stack);
            }
#endif
        } /* else because of ending_sched(including pthread_task->pthread_task). */
#ifdef BRPC_BTHREAD_TRACER
        else {
            // _cur_meta: TASK_STATUS_FIRST_READY -> TASK_STATUS_RUNNING.
            TaskTracer::set_running_status(g->tid(), g->_cur_meta);
        }
#endif // BRPC_BTHREAD_TRACER
    } else {
        LOG(FATAL) << "bthread=" << g->current_tid() << " sched_to itself!";
    }

    while (g->_last_context_remained) {
        RemainedFn fn = g->_last_context_remained;
        g->_last_context_remained = NULL;
        fn(g->_last_context_remained_arg);
        g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
    }

    // Restore errno
    errno = saved_errno;
    // tls_unique_user_ptr probably changed.
    BAIDU_SET_VOLATILE_THREAD_LOCAL(tls_unique_user_ptr, saved_unique_user_ptr);

#ifndef NDEBUG
    --g->_sched_recursive_guard;
#endif
    *pg = g;
}

void TaskGroup::destroy_self() {
    if (_control) {
        _control->_destroy_group(this);
        _control = NULL;
    } else {
        CHECK(false);
    }
}


void TaskGroup::ready_to_run(TaskMeta* meta, bool nosignal) {
#ifdef BRPC_BTHREAD_TRACER
    _control->_task_tracer.set_status(TASK_STATUS_READY, meta);
#endif // BRPC_BTHREAD_TRACER
    push_rq(meta->tid);
    if (nosignal) {
        ++_num_nosignal;
    } else {
        const int additional_signal = _num_nosignal;
        _num_nosignal = 0;
        _nsignaled += 1 + additional_signal;
        _control->signal_task(1 + additional_signal, _tag);
    }
}

void TaskGroup::flush_nosignal_tasks() {
    const int val = _num_nosignal;
    if (val) {
        _num_nosignal = 0;
        _nsignaled += val;
        _control->signal_task(val, _tag);
    }
}

void TaskGroup::ready_to_run_remote(TaskMeta* meta, bool nosignal) {
#ifdef BRPC_BTHREAD_TRACER
    _control->_task_tracer.set_status(TASK_STATUS_READY, meta);
#endif // BRPC_BTHREAD_TRACER
    _remote_rq._mutex.lock();
    while (!_remote_rq.push_locked(meta->tid)) {
        flush_nosignal_tasks_remote_locked(_remote_rq._mutex);
        LOG_EVERY_SECOND(ERROR) << "_remote_rq is full, capacity="
                                << _remote_rq.capacity();
        ::usleep(1000);
        _remote_rq._mutex.lock();
    }
    if (nosignal) {
        ++_remote_num_nosignal;
        _remote_rq._mutex.unlock();
    } else {
        const int additional_signal = _remote_num_nosignal;
        _remote_num_nosignal = 0;
        _remote_nsignaled += 1 + additional_signal;
        _remote_rq._mutex.unlock();
        _control->signal_task(1 + additional_signal, _tag);
    }
}

void TaskGroup::flush_nosignal_tasks_remote_locked(butil::Mutex& locked_mutex) {
    const int val = _remote_num_nosignal;
    if (!val) {
        locked_mutex.unlock();
        return;
    }
    _remote_num_nosignal = 0;
    _remote_nsignaled += val;
    locked_mutex.unlock();
    _control->signal_task(val, _tag);
}

void TaskGroup::ready_to_run_general(TaskMeta* meta, bool nosignal) {
    if (tls_task_group == this) {
        return ready_to_run(meta, nosignal);
    }
    return ready_to_run_remote(meta, nosignal);
}

void TaskGroup::flush_nosignal_tasks_general() {
    if (tls_task_group == this) {
        return flush_nosignal_tasks();
    }
    return flush_nosignal_tasks_remote();
}

void TaskGroup::ready_to_run_in_worker(void* args_in) {
    ReadyToRunArgs* args = static_cast<ReadyToRunArgs*>(args_in);
    return tls_task_group->ready_to_run(args->meta, args->nosignal);
}

void TaskGroup::ready_to_run_in_worker_ignoresignal(void* args_in) {
    ReadyToRunArgs* args = static_cast<ReadyToRunArgs*>(args_in);
#ifdef BRPC_BTHREAD_TRACER
    tls_task_group->_control->_task_tracer.set_status(
        TASK_STATUS_READY, args->meta);
#endif // BRPC_BTHREAD_TRACER
    return tls_task_group->push_rq(args->meta->tid);
}

void TaskGroup::priority_to_run(void* args_in) {
    ReadyToRunArgs* args = static_cast<ReadyToRunArgs*>(args_in);
#ifdef BRPC_BTHREAD_TRACER
    tls_task_group->_control->_task_tracer.set_status(
        TASK_STATUS_READY, args->meta);
#endif // BRPC_BTHREAD_TRACER
    return tls_task_group->control()->push_priority_queue(args->tag, args->meta->tid);
}

struct SleepArgs {
    uint64_t timeout_us;
    bthread_t tid;
    TaskMeta* meta;
    TaskGroup* group;
};

static void ready_to_run_from_timer_thread(void* arg) {
    CHECK(tls_task_group == NULL);
    const SleepArgs* e = static_cast<const SleepArgs*>(arg);
    auto g = e->group;
    auto tag = g->tag();
    g->control()->choose_one_group(tag)->ready_to_run_remote(e->meta);
}

void TaskGroup::_add_sleep_event(void* void_args) {
    // Must copy SleepArgs. After calling TimerThread::schedule(), previous
    // thread may be stolen by a worker immediately and the on-stack SleepArgs
    // will be gone.
    SleepArgs e = *static_cast<SleepArgs*>(void_args);
    TaskGroup* g = e.group;
#ifdef BRPC_BTHREAD_TRACER
    g->_control->_task_tracer.set_status(TASK_STATUS_SUSPENDED, e.meta);
#endif // BRPC_BTHREAD_TRACER

    TimerThread::TaskId sleep_id;
    sleep_id = get_global_timer_thread()->schedule(
        ready_to_run_from_timer_thread, void_args,
        butil::microseconds_from_now(e.timeout_us));

    if (!sleep_id) {
        e.meta->sleep_failed = true;
        // Fail to schedule timer, go back to previous thread.
        g->ready_to_run(e.meta);
        return;
    }

    // Set TaskMeta::current_sleep which is for interruption.
    const uint32_t given_ver = get_version(e.tid);
    {
        BAIDU_SCOPED_LOCK(e.meta->version_lock);
        if (given_ver == *e.meta->version_butex && !e.meta->interrupted) {
            e.meta->current_sleep = sleep_id;
            return;
        }
    }
    // The thread is stopped or interrupted.
    // interrupt() always sees that current_sleep == 0. It will not schedule
    // the calling thread. The race is between current thread and timer thread.
    if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
        // added to timer, previous thread may be already woken up by timer and
        // even stopped. It's safe to schedule previous thread when unschedule()
        // returns 0 which means "the not-run-yet sleep_id is removed". If the
        // sleep_id is running(returns 1), ready_to_run_in_worker() will
        // schedule previous thread as well. If sleep_id does not exist,
        // previous thread is scheduled by timer thread before and we don't
        // have to do it again.
        g->ready_to_run(e.meta);
    }
}

// To be consistent with sys_usleep, set errno and return -1 on error.
int TaskGroup::usleep(TaskGroup** pg, uint64_t timeout_us) {
    if (0 == timeout_us) {
        yield(pg);
        return 0;
    }
    TaskGroup* g = *pg;
    // We have to schedule timer after we switched to next bthread otherwise
    // the timer may wake up(jump to) current still-running context.
    SleepArgs e = { timeout_us, g->current_tid(), g->current_task(), g };
    g->set_remained(_add_sleep_event, &e);
    sched(pg);
    g = *pg;
    if (e.meta->sleep_failed) {
        // Fail to schedule timer, return error.
        e.meta->sleep_failed = false;
        errno = ESTOP;
        return -1;
    }
    e.meta->current_sleep = 0;
    if (e.meta->interrupted) {
        // Race with set and may consume multiple interruptions, which are OK.
        e.meta->interrupted = false;
        // NOTE: setting errno to ESTOP is not necessary from bthread's
        // pespective, however many RPC code expects bthread_usleep to set
        // errno to ESTOP when the thread is stopping, and print FATAL
        // otherwise. To make smooth transitions, ESTOP is still set instead
        // of EINTR when the thread is stopping.
        errno = (e.meta->stop ? ESTOP : EINTR);
        return -1;
    }
    return 0;
}

// Defined in butex.cpp
bool erase_from_butex_because_of_interruption(ButexWaiter* bw);

static int interrupt_and_consume_waiters(
    bthread_t tid, ButexWaiter** pw, uint64_t* sleep_id) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m == NULL) {
        return EINVAL;
    }
    const uint32_t given_ver = get_version(tid);
    BAIDU_SCOPED_LOCK(m->version_lock);
    if (given_ver == *m->version_butex) {
        *pw = m->current_waiter.exchange(NULL, butil::memory_order_acquire);
        *sleep_id = m->current_sleep;
        m->current_sleep = 0;  // only one stopper gets the sleep_id
        m->interrupted = true;
        return 0;
    }
    return EINVAL;
}

static int set_butex_waiter(bthread_t tid, ButexWaiter* w) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            // Release fence makes m->interrupted visible to butex_wait
            m->current_waiter.store(w, butil::memory_order_release);
            return 0;
        }
    }
    return EINVAL;
}

// The interruption is "persistent" compared to the ones caused by signals,
// namely if a bthread is interrupted when it's not blocked, the interruption
// is still remembered and will be checked at next blocking. This designing
// choice simplifies the implementation and reduces notification loss caused
// by race conditions.
// TODO: bthreads created by BTHREAD_ATTR_PTHREAD blocking on bthread_usleep()
// can't be interrupted.
int TaskGroup::interrupt(bthread_t tid, TaskControl* c, bthread_tag_t tag) {
    // Consume current_waiter in the TaskMeta, wake it up then set it back.
    ButexWaiter* w = NULL;
    uint64_t sleep_id = 0;
    int rc = interrupt_and_consume_waiters(tid, &w, &sleep_id);
    if (rc) {
        return rc;
    }
    // a bthread cannot wait on a butex and be sleepy at the same time.
    CHECK(!sleep_id || !w);
    if (w != NULL) {
        erase_from_butex_because_of_interruption(w);
        // If butex_wait() already wakes up before we set current_waiter back,
        // the function will spin until current_waiter becomes non-NULL.
        rc = set_butex_waiter(tid, w);
        if (rc) {
            LOG(FATAL) << "butex_wait should spin until setting back waiter";
            return rc;
        }
    } else if (sleep_id != 0) {
        if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
            TaskGroup* g = tls_task_group;
            TaskMeta* m = address_meta(tid);
            if (g) {
                g->ready_to_run(m);
            } else {
                if (!c) {
                    return EINVAL;
                }
                c->choose_one_group(tag)->ready_to_run_remote(m);
            }
        }
    }
    return 0;
}

void TaskGroup::yield(TaskGroup** pg) {
    TaskGroup* g = *pg;
    ReadyToRunArgs args = { g->tag(), g->_cur_meta, false };
    g->set_remained(ready_to_run_in_worker, &args);
    sched(pg);
}

void print_task(std::ostream& os, bthread_t tid) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m == NULL) {
        os << "bthread=" << tid << " : never existed";
        return;
    }
    const uint32_t given_ver = get_version(tid);
    bool matched = false;
    bool stop = false;
    bool interrupted = false;
    bool about_to_quit = false;
    void* (*fn)(void*) = NULL;
    void* arg = NULL;
    bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
    bool has_tls = false;
    int64_t cpuwide_start_ns = 0;
    TaskStatistics stat = {0, 0, 0};
    TaskStatus status = TASK_STATUS_UNKNOWN;
    bool traced = false;
    pid_t worker_tid = 0;
    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            matched = true;
            stop = m->stop;
            interrupted = m->interrupted;
            about_to_quit = m->about_to_quit;
            fn = m->fn;
            arg = m->arg;
            attr = m->attr;
            has_tls = m->local_storage.keytable;
            cpuwide_start_ns = m->cpuwide_start_ns;
            stat = m->stat;
            status = m->status;
            traced = m->traced;
            worker_tid = m->worker_tid;
        }
    }
    if (!matched) {
        os << "bthread=" << tid << " : not exist now";
    } else {
        os << "bthread=" << tid << " :\nstop=" << stop
           << "\ninterrupted=" << interrupted
           << "\nabout_to_quit=" << about_to_quit
           << "\nfn=" << (void*)fn
           << "\narg=" << (void*)arg
           << "\nattr={stack_type=" << attr.stack_type
           << " flags=" << attr.flags
           << " keytable_pool=" << attr.keytable_pool
           << "}\nhas_tls=" << has_tls
           << "\nuptime_ns=" << butil::cpuwide_time_ns() - cpuwide_start_ns
           << "\ncputime_ns=" << stat.cputime_ns
           << "\nnswitch=" << stat.nswitch
#ifdef BRPC_BTHREAD_TRACER
           << "\nstatus=" << status
           << "\ntraced=" << traced
           << "\nworker_tid=" << worker_tid;
#else
           ;
           (void)status;(void)traced;(void)worker_tid;
#endif // BRPC_BTHREAD_TRACER
    }
}

}  // namespace bthread
