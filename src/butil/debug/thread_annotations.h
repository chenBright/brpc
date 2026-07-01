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

#ifndef BUTIL_DEBUG_THREAD_ANNOTATIONS_H_
#define BUTIL_DEBUG_THREAD_ANNOTATIONS_H_

#include "butil/compiler_specific.h"

// It provides ThreadSanitizer fiber annotations for bthread. TSan cannot
// observe the user-space context switches performed by bthread (implemented
// in assembly via bthread_jump_fcontext). These annotations let TSan track
// each bthread stack as an independent fiber, so that the happens-before
// relations established by bthread synchronization primitives (which rely on
// butil::atomic, natively understood by TSan) are attributed to the correct
// execution streams instead of being mixed across bthreads sharing a worker
// pthread.
// See <sanitizer/tsan_interface.h> for detail of these annotations.

#ifdef BUTIL_USE_TSAN

#include <sanitizer/tsan_interface.h>
#include <cstddef> // size_t

// Returns the fiber handle of the current thread/fiber. The handle of a
// worker pthread is managed by TSan itself and must NOT be destroyed.
#define BUTIL_TSAN_GET_CURRENT_FIBER() \
    __tsan_get_current_fiber()

// Creates a new fiber handle for a freshly allocated bthread stack.
#define BUTIL_TSAN_CREATE_FIBER(flags) \
    __tsan_create_fiber(flags)

// Destroys a fiber handle previously created by BUTIL_TSAN_CREATE_FIBER.
#define BUTIL_TSAN_DESTROY_FIBER(fiber) \
    __tsan_destroy_fiber(fiber)

// Must be called immediately before switching to `fiber`, i.e. right before
// the actual stack jump. With flags == 0 a happens-before relation is
// established between the current fiber and the target fiber.
#define BUTIL_TSAN_SWITCH_TO_FIBER(fiber, flags) \
    __tsan_switch_to_fiber(fiber, flags)

// Sets a human readable name for `fiber`, shown in TSan reports.
#define BUTIL_TSAN_SET_FIBER_NAME(fiber, name) \
    __tsan_set_fiber_name(fiber, name)

// Marks the memory range [addr, addr+size) as a benign data race so that TSan
// stops reporting races on it. It is implemented by the TSan runtime, which
// exports AnnotateBenignRaceSized through its dynamic-annotations compatibility
// layer. We declare the symbol ourselves so this header does not depend on the
// third-party dynamic_annotations header; the signature is kept identical to
// that declaration to stay compatible if both are ever visible in one TU.
extern "C" void AnnotateBenignRaceSized(
    const char* file, int line, const volatile void* mem, size_t size,
    const char* description);

#define BUTIL_TSAN_ANNOTATE_BENIGN_RACE_SIZED(addr, size, desc) \
    AnnotateBenignRaceSized(__FILE__, __LINE__, (addr), (size), (desc))

// Establish a happens-before edge carried by `addr` without requiring `addr`
// itself to be a real atomic. BUTIL_TSAN_RELEASE() publishes the calling
// fiber/thread's current shadow state onto `addr`; a subsequent
// BUTIL_TSAN_ACQUIRE() of the same `addr` makes every memory access that
// happened before the release visible to the acquirer. This is used to teach
// TSan about synchronization that is actually carried by a lock-protected,
// non-atomic variable (e.g. bthread's version_butex bumped under version_lock),
// whose change may be observed by a reader through a lock-free fast path that
// never goes through butex_wait()/futex.
#define BUTIL_TSAN_ACQUIRE(addr) __tsan_acquire((void*)(addr))
#define BUTIL_TSAN_RELEASE(addr) __tsan_release((void*)(addr))

#else
// If TSan is not used, these annotations are no-ops.
#define BUTIL_TSAN_GET_CURRENT_FIBER() (NULL)
#define BUTIL_TSAN_CREATE_FIBER(flags) ((void)(flags), (void*)NULL)
#define BUTIL_TSAN_DESTROY_FIBER(fiber) ((void)(fiber))
#define BUTIL_TSAN_SWITCH_TO_FIBER(fiber, flags) ((void)(fiber), (void)(flags))
#define BUTIL_TSAN_SET_FIBER_NAME(fiber, name) ((void)(fiber), (void)(name))
#define BUTIL_TSAN_ANNOTATE_BENIGN_RACE_SIZED(addr, size, desc) \
    ((void)(addr), (void)(size), (void)(desc))
#define BUTIL_TSAN_ACQUIRE(addr) ((void)(addr))
#define BUTIL_TSAN_RELEASE(addr) ((void)(addr))
#endif // BUTIL_USE_TSAN

#endif  // BUTIL_DEBUG_THREAD_ANNOTATIONS_H_
