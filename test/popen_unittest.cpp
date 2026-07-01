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

// Date: 2017/11/06 10:57:08

#include "butil/popen.h"
#include "butil/errno.h"
#include "butil/atomicops.h"
#include "butil/strings/string_piece.h"
#include "butil/build_config.h"
#include "butil/compiler_specific.h"
#include <memory>
#include <gtest/gtest.h>

namespace butil {
extern int read_command_output_through_clone(std::ostream&, const char*);
extern int read_command_output_through_popen(std::ostream&, const char*);
}

namespace {

class PopenTest : public testing::Test {
};

TEST(PopenTest, posix_popen) {
    std::ostringstream oss;
    int rc = butil::read_command_output_through_popen(oss, "echo \"Hello World\"");
    ASSERT_EQ(0, rc) << berror(errno);
    ASSERT_EQ("Hello World\n", oss.str());

    oss.str("");
    rc = butil::read_command_output_through_popen(oss, "exit 1");
    EXPECT_EQ(1, rc) << berror(errno);
    ASSERT_TRUE(oss.str().empty()) << oss.str();
    oss.str("");
    rc = butil::read_command_output_through_popen(oss, "kill -9 $$");
    ASSERT_EQ(-1, rc);
    ASSERT_EQ(errno, ECHILD);
    ASSERT_TRUE(butil::StringPiece(oss.str()).ends_with("was killed by signal 9"));
    oss.str("");
    rc = butil::read_command_output_through_popen(oss, "kill -15 $$");
    ASSERT_EQ(-1, rc);
    ASSERT_EQ(errno, ECHILD);
    ASSERT_TRUE(butil::StringPiece(oss.str()).ends_with("was killed by signal 15"));

    // TODO(zhujiashun): Fix this in macos
    /*
    oss.str("");
     ASSERT_EQ(0, butil::read_command_output_through_popen(oss, "for i in `seq 1 100000`; do echo -n '=' ; done"));
    ASSERT_EQ(100000u, oss.str().length());
    std::string expected;
    expected.resize(100000, '=');
    ASSERT_EQ(expected, oss.str());
    */
}

#if defined(OS_LINUX)

#if !defined(BUTIL_USE_TSAN)
    // read_command_output_through_clone() spawns the child with
    // clone(CLONE_VM), so the child shares the address space with the parent.
    // ThreadSanitizer's fork/clone interception (ForkChildAfter) is
    // incompatible with CLONE_VM children: it hits an internal
    // "CHECK failed: ((!thr->slot)) != (0)" and aborts the child with TSan's
    // default exit code (66), which then surfaces here as a bogus rc==66 /
    // ECHILD failure. This is a TSan runtime limitation rather than a defect in
    // the code under test, so skip the test when running under TSan.
TEST(PopenTest, clone) {
    std::ostringstream oss;
    int rc = butil::read_command_output_through_clone(oss, "echo \"Hello World\"");
    ASSERT_EQ(0, rc) << berror(errno);
    ASSERT_EQ("Hello World\n", oss.str());

    oss.str("");
    rc = butil::read_command_output_through_clone(oss, "exit 1");
    ASSERT_EQ(1, rc) << berror(errno);
    ASSERT_TRUE(oss.str().empty()) << oss.str();
    oss.str("");
    rc = butil::read_command_output_through_clone(oss, "kill -9 $$");
    ASSERT_EQ(-1, rc);
    ASSERT_EQ(errno, ECHILD);
    ASSERT_TRUE(butil::StringPiece(oss.str()).ends_with("was killed by signal 9"));
    oss.str("");
    rc = butil::read_command_output_through_clone(oss, "kill -15 $$");
    ASSERT_EQ(-1, rc);
    ASSERT_EQ(errno, ECHILD);
    ASSERT_TRUE(butil::StringPiece(oss.str()).ends_with("was killed by signal 15"));

    oss.str("");
    ASSERT_EQ(0, butil::read_command_output_through_clone(oss, "for i in `seq 1 100000`; do echo -n '=' ; done"));
    ASSERT_EQ(100000u, oss.str().length());
    std::string expected;
    expected.resize(100000, '=');
    ASSERT_EQ(expected, oss.str());
}
#endif  // BUTIL_USE_TSAN

struct CounterArg {
    butil::atomic<int64_t> counter;
    butil::atomic<bool> stop;
};

static void* counter_thread(void* args) {
    CounterArg* ca = (CounterArg*)args;
    while (!ca->stop.load(std::memory_order_relaxed)) {
        ca->counter.fetch_add(1, std::memory_order_relaxed);
    }
    return NULL;
}

static int fork_thread(void* arg) {
    usleep(100 * 1000);
    _exit(0);
}

const int CHILD_STACK_SIZE = 64 * 1024;

TEST(PopenTest, does_vfork_suspend_all_threads) {
    pthread_t tid;
    // Allocate the shared CounterArg on the heap rather than on the main
    // thread's stack. If it lived on the stack, ThreadSanitizer could report a
    // false data race: a previous test's stack object (e.g. the
    // std::ostringstream in PopenTest.posix_popen) that occupied the same
    // reused stack address leaves behind a non-atomic write in TSan's shadow
    // memory, which then conflicts with the atomic reads performed by
    // counter_thread on the reused address.
    std::unique_ptr<CounterArg> ca(new CounterArg);
    ca->counter.store(0, std::memory_order_relaxed);
    ca->stop.store(false, std::memory_order_relaxed);
    ASSERT_EQ(0, pthread_create(&tid, NULL, counter_thread, ca.get()));
    usleep(100 * 1000);
    char* child_stack_mem = (char*)malloc(CHILD_STACK_SIZE);
    void* child_stack = child_stack_mem + CHILD_STACK_SIZE;
    const int64_t counter_before_fork = ca->counter.load(std::memory_order_relaxed);
    pid_t cpid = clone(fork_thread, child_stack, CLONE_VFORK, NULL);
    const int64_t counter_after_fork = ca->counter.load(std::memory_order_relaxed);
    usleep(100 * 1000);
    const int64_t counter_after_sleep = ca->counter.load(std::memory_order_relaxed);
    int ws;
    ca->stop.store(true, std::memory_order_relaxed);
    pthread_join(tid, NULL);
    std::cout << "bc=" << counter_before_fork << " ac=" << counter_after_fork
              << " as=" << counter_after_sleep
              << std::endl;
    ASSERT_EQ(cpid, waitpid(cpid, &ws, __WALL));
    free(child_stack_mem);
}

#endif  // OS_LINUX

}
