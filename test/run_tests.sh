#!/usr/bin/env bash

# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# turn on coredumps
ulimit -c unlimited
# In many CI containers the kernel core_pattern pipes cores to an external
# handler (apport/systemd-coredump) instead of writing them to CWD, so no
# "core.*" file ever appears here. Try to redirect cores to the current
# directory; this is best-effort because /proc/sys/kernel/core_pattern is
# usually read-only inside containers.
if [ -w /proc/sys/kernel/core_pattern ]; then
    echo "core.%e.%p" > /proc/sys/kernel/core_pattern 2>/dev/null || true
fi
rm -f core.*

test_num=0
failed_test=""
rc=0
test_bins="test_butil test_bvar bthread*unittest brpc*unittest"
export ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1"
for test_bin in $test_bins; do
    test_num=$((test_num + 1))
    >&2 echo "[runtest] $test_bin"
    ./$test_bin
    # If ASan abort without detailed call stack of new/delete,
    # try to disable fast_unwind_on_malloc, which would be a performance killer.
    # ASAN_OPTIONS="fast_unwind_on_malloc=0:detect_leaks=0" ./$test_bin
    rc=$?
    if [ $rc -ne 0 ]; then
        failed_test="$test_bin"
        break;
    fi
done
if [ $test_num -eq 0 ]; then
    >&2 echo "[runtest] Cannot find any tests"
    exit 1
fi

print_bt () {
    local prog="$1"
    # find newest core file
    COREFILE=$(find . -name "core*" -type f -printf "%T@ %p\n" 2>/dev/null | sort -k 1 -n | cut -d' ' -f 2- | tail -n 1)
    if [ ! -z "$COREFILE" ]; then
        >&2 echo "corefile=$COREFILE prog=$prog"
        gdb -c "$COREFILE" "$prog" -ex "bt" -ex "thread apply all bt" -ex "set pagination 0" -batch;
        return
    fi

    # No core file. This is the common case in CI containers (core_pattern
    # pipes cores away) and for sanitizer builds (disable_coredump=1 by
    # default), so the crash site would otherwise be lost. Fall back to
    # re-running the failed test under gdb to capture the backtrace directly,
    # without needing a core dump.
    >&2 echo "[runtest] no core file found (core_pattern=$(cat /proc/sys/kernel/core_pattern 2>/dev/null))"
    if [ "$rc" -gt 128 ] && command -v gdb >/dev/null 2>&1; then
        >&2 echo "[runtest] '$prog' was killed by signal $((rc - 128)); re-running under gdb to capture backtrace"
        gdb "./$prog" -batch \
            -ex "set pagination 0" \
            -ex "handle SIGSEGV stop print" \
            -ex "run" \
            -ex "bt" \
            -ex "thread apply all bt"
    fi
}

if [ -z "$failed_test" ]; then
    >&2 echo "[runtest] $test_num succeeded"
elif [ $test_num -gt 1 ]; then
    print_bt $failed_test
    >&2 echo "[runtest] '$failed_test' failed, $((test_num-1)) succeeded"
else
    print_bt $failed_test
    >&2 echo "[runtest] '$failed_test' failed"
fi
exit $rc
