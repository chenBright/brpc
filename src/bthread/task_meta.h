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

#ifndef BTHREAD_TASK_META_H
#define BTHREAD_TASK_META_H

#include <pthread.h>                 // pthread_spin_init
#include "bthread/butex.h"           // butex_construct/destruct
#include "butil/atomicops.h"          // butil::atomic
#include "bthread/types.h"           // bthread_attr_t
#include "bthread/stack.h"           // ContextualStack
#include "bthread/timer_thread.h"
#include "butil/thread_local.h"
#include "butil/debug/thread_annotations.h" // BUTIL_TSAN_ANNOTATE_BENIGN_RACE_SIZED

namespace bthread {

struct TaskStatistics {
    int64_t cputime_ns;
    int64_t nswitch;
    int64_t cpu_usage_ns;
};

class KeyTable;
struct ButexWaiter;

struct LocalStorage {
    KeyTable* keytable;
    void* assigned_data;
    void* rpcz_parent_span;  // Points to std::weak_ptr<brpc::Span>* (managed by brpc)
};

#define BTHREAD_LOCAL_STORAGE_INITIALIZER { NULL, NULL, NULL }

const static LocalStorage LOCAL_STORAGE_INIT = BTHREAD_LOCAL_STORAGE_INITIALIZER;

EXTERN_BAIDU_VOLATILE_THREAD_LOCAL(LocalStorage, tls_bls);

enum TaskStatus {
    TASK_STATUS_UNKNOWN,
    TASK_STATUS_CREATED,
    TASK_STATUS_FIRST_READY,
    TASK_STATUS_READY,
    TASK_STATUS_JUMPING,
    TASK_STATUS_RUNNING,
    TASK_STATUS_SUSPENDED,
    TASK_STATUS_END,
};

struct TaskMeta {
    // [Not Reset]
    butil::atomic<ButexWaiter*> current_waiter{NULL};
    uint64_t current_sleep{TimerThread::INVALID_TASK_ID};

    // A flag to mark if the Timer scheduling failed.
    bool sleep_failed{false};

    // A builtin flag to mark if the thread is stopping.
    bool stop{false};

    // The thread is interrupted and should wake up from some blocking ops.
    bool interrupted{false};

    // Scheduling of the thread can be delayed.
    bool about_to_quit{false};
    
    // [Not Reset] guarantee visibility of version_butex.
    pthread_spinlock_t version_lock{};
    
    // [Not Reset] only modified by one bthread at any time, no need to be atomic
    uint32_t* version_butex{NULL};

    // The identifier. It does not have to be here, however many code is
    // simplified if they can get tid from TaskMeta.
    bthread_t tid{INVALID_BTHREAD};

    int priority_index{-1};

    // User function and argument
    void* (*fn)(void*){NULL};
    void* arg{NULL};

    // Stack of this task.
    ContextualStack* stack{NULL};

    // Attributes creating this task
    bthread_attr_t attr{BTHREAD_ATTR_NORMAL};
    
    // Statistics
    int64_t cpuwide_start_ns{0};
    TaskStatistics stat{};

    // bthread local storage, sync with tls_bls (defined in task_group.cpp)
    // when the bthread is created or destroyed.
    // DO NOT use this field directly, use tls_bls instead.
    LocalStorage local_storage{};

    // Only used when TaskTracer is enabled.
    // Bthread status.
    TaskStatus status{TASK_STATUS_UNKNOWN};
    // Whether bthread is traced？
    bool traced{false};
    // [Not Reset] guarantee tracing completion before jumping.
    pthread_mutex_t trace_lock{};
    // Worker thread id.
    pthread_t worker_tid{};

public:
    // Only initialize [Not Reset] fields, other fields will be reset in
    // bthread_start* functions
    TaskMeta() {
        pthread_spin_init(&version_lock, 0);
        version_butex = butex_create_checked<uint32_t>();
        *version_butex = 1;
        pthread_mutex_init(&trace_lock, NULL);
        // `interrupted` is intentionally raced on: TaskGroup::interrupt()
        // (interrupt_and_consume_waiters) writes it under version_lock, while
        // butex_wait_from_pthread()/wait_for_butex() read and clear it under
        // the butex's waiter_lock -- two different locks on purpose. An
        // interruption is "persistent", so consuming it zero or multiple times
        // is harmless (see the "Race with set ... which are OK" comment in
        // butex_wait_from_pthread). The TaskMeta object lives in a ResourcePool
        // and is never freed, so annotating it once at construction covers its
        // whole lifetime. Mark it benign so TSan stops reporting this design.
        BUTIL_TSAN_ANNOTATE_BENIGN_RACE_SIZED(
            &interrupted, sizeof(interrupted),
            "benign TaskMeta::interrupted (version_lock vs waiter_lock)");
        // `stat` is intentionally raced on: TaskGroup::sched_to() writes
        // stat.cputime_ns, stat.cpu_usage_ns, and stat.nswitch without holding
        // any lock (only the current worker thread can write to its own task's
        // stat). Meanwhile, print_task() reads stat under version_lock. This is
        // benign because: (1) stat is only written by the task's own worker
        // thread in sched_to(), (2) reads in print_task() are protected by
        // version_lock to ensure version consistency, (3) occasional stale reads
        // of stat fields are harmless for diagnostic purposes. The TaskMeta
        // object lives in a ResourcePool and is never freed, so annotating it
        // once at construction covers its whole lifetime.
        BUTIL_TSAN_ANNOTATE_BENIGN_RACE_SIZED(
            &stat, sizeof(stat),
            "benign TaskMeta::stat (sched_to write vs print_task read)");
        // `fn` is intentionally raced on: TaskGroup::init() and the bthread
        // creation path write it without holding any lock (the TaskMeta is
        // freshly fetched from the ResourcePool and initialized by a single
        // worker thread). Meanwhile, the diagnostic interfaces
        // get_living_bthreads()/print_task() scan the whole ResourcePool and
        // read `fn` (to filter out internal main-task bthreads) without any
        // lock. This is benign because the read is best-effort for diagnostics
        // only: an occasional stale/half-initialized value merely affects
        // whether a bthread shows up once in the listing, which is harmless.
        // The TaskMeta object lives in a ResourcePool and is never freed, so
        // annotating it once at construction covers its whole lifetime.
        BUTIL_TSAN_ANNOTATE_BENIGN_RACE_SIZED(
            &fn, sizeof(fn),
            "benign TaskMeta::fn (init write vs get_living_bthreads read)");
    }
        
    ~TaskMeta() {
        pthread_mutex_destroy(&trace_lock);
        butex_destroy(version_butex);
        version_butex = NULL;
        pthread_spin_destroy(&version_lock);
    }

    void set_stack(ContextualStack* s) {
        stack = s;
    }

    ContextualStack* release_stack() {
        ContextualStack* tmp = stack;
        stack = NULL;
        return tmp;
    }

    StackType stack_type() const {
        return static_cast<StackType>(attr.stack_type);
    }
};

// Global callback for creating a new bthread span when creating a new bthread.
// This is set by brpc layer. When a bthread is created with BTHREAD_INHERIT_SPAN,
// this callback is invoked to create a new span for the bthread.
// The returned void* points to a heap-allocated weak_ptr<Span>* managed by brpc layer.
// Returns NULL if span creation is disabled or fails.
extern void* (*g_create_bthread_span)();

// Global destructor callback for rpcz_parent_span.
// This is set by brpc layer to clean up the heap-allocated weak_ptr.
// bthread layer doesn't know the concrete type, it just calls this function
// with the void* pointer when cleaning up LocalStorage.
extern void (*g_rpcz_parent_span_dtor)(void*);

// Global callback invoked when a bthread ends (used by higher layers to
// observe and react to bthread end events, e.g., to finish spans). This
// pointer is set by the upper layer during initialization.
extern void (*g_end_bthread_span)();

}  // namespace bthread

#endif  // BTHREAD_TASK_META_H
