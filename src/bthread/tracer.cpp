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

#ifdef BRPC_BTHREAD_TRACER

#include <signal.h>
#include "butil/debug/stack_trace.h"
#include "bthread/tracer.h"
#include "bthread/task_group.h"
#include "bthread/processor.h"

namespace bthread {

DEFINE_bool(enable_fast_unwind, true, "whether to enable fast unwind");
DEFINE_uint32(max_sigqueue_try, 10, "max try times for sigqueue");
DEFINE_uint32(signal_trace_timeout_ms, 50, "timeout for signal trace in ms");

void TaskTracer::set_status(TaskStatus s, TaskMeta* m) {
    CHECK_NE(TASK_STATUS_RUNNING, s) << "Use `set_running_status' instead";
    CHECK_NE(TASK_STATUS_END, s) << "Use `set_end_status_unsafe' instead";

    bool tracing;
    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (TASK_STATUS_UNKNOWN == m->status && TASK_STATUS_JUMPING == s) {
            // Do not update status for jumping when bthread is ending.
            return;
        }

        tracing = m->traced;
        // bthread is scheduled for the first time.
        if (TASK_STATUS_READY == s || NULL == m->stack) {
            m->status = TASK_STATUS_FIRST_READY;
        } else {
            m->status = s;
        }
    }

    if (tracing && TASK_STATUS_JUMPING == s) {
        WaitForTracing(m);
    }
}

void TaskTracer::set_running_status(pid_t worker_tid, TaskMeta* m) {
    BAIDU_SCOPED_LOCK(m->version_lock);
    m->worker_tid = worker_tid;
    m->status = TASK_STATUS_RUNNING;
}

bool TaskTracer::set_end_status_unsafe(TaskMeta* m) {
    m->status = TASK_STATUS_END;
    return m->traced;
}

std::string TaskTracer::Trace(bthread_t tid) {
    Result result = TraceImpl(tid);
    return result.error ? result.err_msg : ToString(result);
}

void TaskTracer::Trace(std::ostream& os, bthread_t tid) {
    Result result = TraceImpl(tid);
    if (result.error) {
        os << result.err_msg;
    } else {
        ToString(result, os);
    }
}

TaskTracer::Result TaskTracer::TraceImpl(bthread_t tid) {
    if (tid == bthread_self()) {
        return Result::MakeErrorResult("Can not trace self=%d", tid);
    }

    // Only one bthread can be traced at a time.
    BAIDU_SCOPED_LOCK(_trace_request_mutex);

    TaskMeta* m = TaskGroup::address_meta(tid);
    if (NULL == m) {
        return Result::MakeErrorResult("bthread=%d never existed", tid);
    }

    BAIDU_SCOPED_LOCK(_mutex);
    TaskStatus status;
    pid_t worker_tid;
    const uint32_t given_version = get_version(tid);
    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_version == *m->version_butex) {
            // Start tracing.
            m->traced = true;
            worker_tid = m->worker_tid;
            status = m->status;
        } else {
            return Result::MakeErrorResult("bthread=%d not exist now", tid);
        }
    }

    if (TASK_STATUS_UNKNOWN == status) {
        return Result::MakeErrorResult("bthread=%d not exist now", tid);
    } else if (TASK_STATUS_CREATED == status) {
        return Result::MakeErrorResult("bthread=%d has just been created", tid);
    } else if (TASK_STATUS_FIRST_READY == status) {
        return Result::MakeErrorResult("bthread=%d is scheduled for the first time", tid);
    } else if (TASK_STATUS_END == status) {
        return Result::MakeErrorResult("bthread=%d has ended", tid);
    } else if (TASK_STATUS_JUMPING == status) {
        // Wait for jumping completion.
        status = WaitForJumping(m);
    }

    // After jumping, the status may be RUNNING, SUSPENDED, or READY, which is traceable.

    Result result{};
    if (TASK_STATUS_RUNNING == status) {
        result = SignalTrace(worker_tid);
    } else if (TASK_STATUS_SUSPENDED == status || TASK_STATUS_READY == status) {
        result = ContextTrace(m->stack->context);
    }

    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        // If m->status is BTHREAD_STATUS_END, the bthread also waits for tracing completion,
        // so given_version != *m->version_butex is OK.
        m->traced = false;
    }
    // Wake up the waiting worker thread to jump.
    _cond.Signal();

    return result;
}

void TaskTracer::SignalSafeUsleep(unsigned int microseconds) {
    ErrnoGuard guard;
    struct timespec sleep_time{};
    sleep_time.tv_sec = microseconds / 1000000;
    sleep_time.tv_nsec = (microseconds % 1000000) * 1000;
    // On Linux, sleep() is implemented via nanosleep(2).
    // sleep() is async-signal-safety, so nanosleep() is considered as async-signal-safety on Linux.
    // abseil-cpp and folly also use nanosleep() in signal handler. For details, see:
    // 1. abseil-cpp: https://github.com/abseil/abseil-cpp/blob/27a0c7308f04e4560fabe5a7beca837e8f3f2c5b/absl/debugging/failure_signal_handler.cc#L314
    // 2. folly: https://github.com/facebook/folly/blob/479de0144d3acb6aa4b3483affa23cf4f49f07ee/folly/debugging/symbolizer/SignalHandler.cpp#L446
    while (nanosleep(&sleep_time, &sleep_time) == -1 && EINTR == errno);
}

bool TaskTracer::RegisterSignalHandler(pid_t tid) {
    {
        BAIDU_SCOPED_LOCK(_worker_mutex);
        _worker_tids.push_back(tid);
    }
    // Set up the signal handler.
    struct sigaction sa{};
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SignalHandler;
    sa.sa_flags = SA_SIGINFO;
    sigfillset(&sa.sa_mask);

    if (sigaction(SIGURG, &sa, nullptr) == -1) {
        LOG(ERROR) << "Failed to sigaction";
        return false;
    }

    return true;
}

void TaskTracer::WaitForTracing(TaskMeta* m) {
    BAIDU_SCOPED_LOCK(_mutex);
    while (m->traced) {
        _cond.Wait();
    }
}

void TaskTracer::StopAndInterruptWorker() {
    if (_stop.exchange(true, butil::memory_order_relaxed)) {
        return;
    }

    BAIDU_SCOPED_LOCK(_worker_mutex);
    for (auto worker_tid : _worker_tids) {
        union sigval value{};
        value.sival_ptr = this;
        while (sigqueue(worker_tid, SIGURG, value) != 0) {
            PLOG(ERROR) << "Fail to sigqueue";
        }
    }
    _worker_tids.clear();
}

TaskStatus TaskTracer::WaitForJumping(TaskMeta* m) {
    // Reasons for not using locks here:
    // 1. It is necessary to lock before jump_stack, unlock after jump_stack,
    //    which involves two different bthread and is prone to errors.
    // 2. jump_stack is fast.
    int i = 0;
    do {
        // The bthread is jumping now, spin until it finishes.
        if (i++ < 30) {
            cpu_relax();
        } else {
            sched_yield();
        }

        BAIDU_SCOPED_LOCK(m->version_lock);
        if (TASK_STATUS_JUMPING != m->status) {
            return m->status;
        }
    } while (true);
}

TaskTracer::Result TaskTracer::ContextTrace(bthread_fcontext_t fcontext) {
    unw_cursor_t cursor = MakeCursor(fcontext);
    return TraceCore(cursor);
}

void TaskTracer::SignalHandler(int, siginfo_t* info, void* context) {
    ErrnoGuard guard;
    static_cast<TaskTracer*>(info->si_value.sival_ptr)
        ->SignalTraceHandler(static_cast<unw_context_t*>(context));
}

// Caution: This function is called in signal handler, so it should be async-signal-safety.
void TaskTracer::SignalTraceHandler(unw_context_t* context) {
    if (_stop.load(butil::memory_order_relaxed)) {
        // Only wakeup the worker thread.
        return;
    }

    // The signal is not from BthreadTracer, ignore.
    if (SIGNAL_TRACE_STATUS_START != _signal_handler_flag.load(butil::memory_order_acquire)) {
        return;
    }

    _signal_context = context;
    _signal_handler_flag.store(SIGNAL_TRACE_STATUS_TRACING, butil::memory_order_seq_cst);
    butil::Timer timer(butil::Timer::STARTED);
    while (SIGNAL_TRACE_STATUS_TRACING ==
           _signal_handler_flag.load(butil::memory_order_seq_cst)) {
        SignalSafeUsleep(50); // 50us
        // Timeout to avoid deadlock of libunwind.
        timer.stop();
        if (timer.m_elapsed() > FLAGS_signal_trace_timeout_ms) {
            break;
        }
    }
}

TaskTracer::Result TaskTracer::SignalTrace(pid_t tid) {
    _signal_context = NULL;
    // Use memory_order_seq_cst to ensure the flag is set before sending signal.
    _signal_handler_flag.store(SIGNAL_TRACE_STATUS_START, butil::memory_order_seq_cst);

    // CAUTION:
    // The signal handler will wait for the backtrace to complete.
    // If the worker thread is interrupted when holding a resource(lock, etc),
    // and this function waits for the resource during capturing backtraces,
    // it may cause a deadlock.
    //
    // https://github.com/gperftools/gperftools/wiki/gperftools'-stacktrace-capturing-methods-and-their-issues#libunwind
    // Generally, libunwind promises async-signal-safety for capturing backtraces.
    // But in practice, it is only partially async-signal-safe due to reliance on
    // dl_iterate_phdr API, which is used to enumerate all loaded ELF modules
    // (.so files and main executable binary). No libc offers dl_iterate_pdhr that
    // is async-signal-safe. In practice, the issue may happen if we take tracing
    // signal during an existing dl_iterate_phdr call (such as when the program
    // throws an exception) or during dlopen/dlclose-ing some .so module.
    // Deadlock call stack:
    // #0  __lll_lock_wait (futex=futex@entry=0x7f0d3d7f0990 <_rtld_global+2352>, private=0) at lowlevellock.c:52
    // #1  0x00007f0d3a73c131 in __GI___pthread_mutex_lock (mutex=0x7f0d3d7f0990 <_rtld_global+2352>) at ../nptl/pthread_mutex_lock.c:115
    // #2  0x00007f0d38eb0231 in __GI___dl_iterate_phdr (callback=callback@entry=0x7f0d38c456a0 <_ULx86_64_dwarf_callback>, data=data@entry=0x7f0d07defad0) at dl-iteratephdr.c:40
    // #3  0x00007f0d38c45d79 in _ULx86_64_dwarf_find_proc_info (as=0x7f0d38c4f340 <local_addr_space>, ip=ip@entry=139694791966897, pi=pi@entry=0x7f0d07df0498, need_unwind_info=need_unwind_info@entry=1, arg=0x7f0
    // d07df0340) at dwarf/Gfind_proc_info-lsb.c:759
    // #4  0x00007f0d38c43260 in fetch_proc_info (c=c@entry=0x7f0d07df0340, ip=139694791966897) at dwarf/Gparser.c:461
    // #5  0x00007f0d38c44e46 in find_reg_state (sr=0x7f0d07defd10, c=0x7f0d07df0340) at dwarf/Gparser.c:925
    // #6  _ULx86_64_dwarf_step (c=c@entry=0x7f0d07df0340) at dwarf/Gparser.c:972
    // #7  0x00007f0d38c40c14 in _ULx86_64_step (cursor=cursor@entry=0x7f0d07df0340) at x86_64/Gstep.c:71
    // #8  0x00007f0d399ed8f6 in GetStackTraceWithContext_libunwind (result=<optimized out>, max_depth=63, skip_count=132054887, ucp=<optimized out>) at src/stacktrace_libunwind-inl.h:138
    // #9  0x00007f0d399ee083 in GetStackTraceWithContext (result=0x7f0d07df07b8, max_depth=63, skip_count=3, uc=0x7f0d07df0a40) at src/stacktrace.cc:305
    // #10 0x00007f0d399ea992 in CpuProfiler::prof_handler (signal_ucontext=<optimized out>, cpu_profiler=0x7f0d399f6600, sig=<optimized out>) at src/profiler.cc:359
    // #11 0x00007f0d399eb633 in ProfileHandler::SignalHandler (sig=27, sinfo=0x7f0d07df0b70, ucontext=0x7f0d07df0a40) at src/profile-handler.cc:530
    // #12 <signal handler called>
    // #13 0x00007f0d3a73c0b1 in __GI___pthread_mutex_lock (mutex=0x7f0d3d7f0990 <_rtld_global+2352>) at ../nptl/pthread_mutex_lock.c:115
    // #14 0x00007f0d38eb0231 in __GI___dl_iterate_phdr (callback=0x7f0d38f525f0, data=0x7f0d07df10c0) at dl-iteratephdr.c:40
    // #15 0x00007f0d38f536c1 in _Unwind_Find_FDE () from /lib/x86_64-linux-gnu/libgcc_s.so.1
    // #16 0x00007f0d38f4f868 in ?? () from /lib/x86_64-linux-gnu/libgcc_s.so.1
    // #17 0x00007f0d38f50a20 in ?? () from /lib/x86_64-linux-gnu/libgcc_s.so.1
    // #18 0x00007f0d38f50f99 in _Unwind_RaiseException () from /lib/x86_64-linux-gnu/libgcc_s.so.1
    // #19 0x00007f0d390088dc in __cxa_throw () from /lib/x86_64-linux-gnu/libstdc++.so.6
    // #20 0x00007f0d3b5b2245 in __cxxabiv1::__cxa_throw (thrownException=0x7f0d114ea8c0, type=0x7f0d3d6dd830 <typeinfo for rockset::GRPCError>, destructor=<optimized out>) at /src/folly/folly/experimental/exception_tracer/ExceptionTracerLib.cpp:107
    //
    // Therefore, we do not capture backtracks in the signal handler to avoid mutex
    // reentry and deadlock. Instead, we capture backtracks in this function and
    // ends the signal handler after capturing backtraces is complete.
    // Even so, there is still a deadlock problem:
    // the worker thread is interrupted when during an existing dl_iterate_phdr call,
    // and wait for the capturing backtraces to complete. This function capture
    // backtracks with dl_iterate_phdr. We introduce a timeout mechanism in signal
    // handler to avoid deadlock.

    union sigval value{};
    value.sival_ptr = this;
    size_t sigqueue_try = 0;
    while (sigqueue(tid, SIGURG, value) != 0) {
        if (errno != EAGAIN || sigqueue_try++ >= FLAGS_max_sigqueue_try) {
            return Result::MakeErrorResult("Fail to sigqueue: %s", berror());
        }
    }

    butil::Timer timer(butil::Timer::STARTED);
    // Use memory_order_seq_cst to ensure the signal is sent and the flag is set before checking.
    for (int i = 0;
         SIGNAL_TRACE_STATUS_START == _signal_handler_flag.load(butil::memory_order_seq_cst);
         ++i) {
        if (i < 30) {
            sched_yield();
        } else {
            SignalSafeUsleep(5); // 5us
        }

        // Timeout to avoid dead loop if SIGURG is covered.
        timer.stop();
        if (timer.m_elapsed() > FLAGS_signal_trace_timeout_ms) {
            return Result::MakeErrorResult(
                "Timeout exceed %dms", FLAGS_signal_trace_timeout_ms);

        }
    }

    unw_cursor_t cursor;
    int  rc = unw_init_local(&cursor, _signal_context);
    if (0 != rc) {
        return Result::MakeErrorResult("Failed to init local, rc=%d", rc);
    }

    Result result = TraceCore(cursor);

    // Use memory_order_seq_cst to ensure the flag is set after tracing.
    _signal_handler_flag.store(SIGNAL_TRACE_STATUS_UNKNOWN, butil::memory_order_seq_cst);

    return result;
}

unw_cursor_t TaskTracer::MakeCursor(bthread_fcontext_t fcontext) {
    unw_cursor_t cursor;
    unw_init_local(&cursor, &_context);
    auto regs = reinterpret_cast<uintptr_t*>(fcontext);

    // Only need RBP, RIP, RSP.
    // The base pointer (RBP).
    if (unw_set_reg(&cursor, UNW_X86_64_RBP, regs[6]) != 0) {
        LOG(ERROR) << "Fail to set RBP";
    }
    // The instruction pointer (RIP).
    if (unw_set_reg(&cursor, UNW_REG_IP, regs[7]) != 0) {
        LOG(ERROR) << "Fail to set RIP";
    }
#if UNW_VERSION_MAJOR >= 1 && UNW_VERSION_MINOR >= 7
    // The stack pointer (RSP).
    if (unw_set_reg(&cursor, UNW_REG_SP, regs[8]) != 0) {
        LOG(ERROR) << "Fail to set RSP";
    }
#endif

    return cursor;
}

TaskTracer::Result TaskTracer::TraceCore(unw_cursor_t& cursor) {
    Result result{};
    result.fast_unwind = FLAGS_enable_fast_unwind;
    for (result.frame_count = 0; result.frame_count < arraysize(result.ips); ++result.frame_count) {
        int rc = unw_step(&cursor);
        if (0 == rc) {
            break;
        } else if (rc < 0) {
            return Result::MakeErrorResult("unw_step rc=%d", rc);
        }

        unw_word_t ip = 0;
        // Fast unwind do not care about the return value.
        rc = unw_get_reg(&cursor, UNW_REG_IP, &ip);
        result.ips[result.frame_count] = ip;

        if (result.fast_unwind) {
            continue;
        }

        if (0 != rc) {
            butil::strings::SafeSPrintf(result.mangled[result.frame_count], "\0");
            continue;
        }

        rc = unw_get_proc_name(
            &cursor, result.mangled[result.frame_count], sizeof(result.mangled[result.frame_count]), NULL);
        if (0 != rc && UNW_ENOMEM != rc) {
            butil::strings::SafeSPrintf(result.mangled[result.frame_count], "\0");
        }
    }

    return result;
}

std::string TaskTracer::ToString(const Result& result) {
    if (result.frame_count == 0) {
        return "No frame";
    }

    if (result.fast_unwind) {
        butil::debug::StackTrace stack_trace((void**)&result.ips, result.frame_count);
        return stack_trace.ToString();
    }

    std::string result_str;
    result_str.reserve(2048);
    for (size_t i = 0; i < result.frame_count; ++i) {
        butil::string_appendf(&result_str, "#%zu 0x%016lx ", i, result.ips[i]);
        if (strlen(result.mangled[i]) == 0) {
            result_str.append("<unknown>");
        } else {
            result_str.append(butil::demangle(result.mangled[i]));
        }
        if (i + 1 < result.frame_count) {
            result_str.push_back('\n');
        }
    }
    return result_str;
}

void TaskTracer::ToString(const Result& result, std::ostream& out_os) {
    if (result.frame_count == 0) {
        out_os << "No frame";
        return;
    }

    if (result.fast_unwind) {
        butil::debug::StackTrace stack_trace((void**)&result.ips, result.frame_count);
        stack_trace.OutputToStream(&out_os);
        return;
    }

    for (size_t i = 0; i < result.frame_count; ++i) {
        out_os << "# " << i << " 0x" << std::hex << result.ips[i] << std::dec << " ";
        if (strlen(result.mangled[i]) == 0) {
            out_os << "<unknown>";
        } else {
            out_os << butil::demangle(result.mangled[i]);
        }
        if (i + 1 < result.frame_count) {
            out_os << '\n';
        }
    }
}

} // namespace bthread

#endif // BRPC_BTHREAD_TRACER

