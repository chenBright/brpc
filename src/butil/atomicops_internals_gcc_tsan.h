// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is an internal atomic implementation for builds compiled with
// ThreadSanitizer (-fsanitize=thread) using compilers whose runtime does NOT
// ship <sanitizer/tsan_interface_atomic.h> (notably GCC's libsanitizer; that
// header is Clang/compiler-rt only). Instead of the __tsan_atomic* interface
// used by atomicops_internals_tsan.h, this file is implemented purely with the
// standard __atomic builtins. Both GCC and Clang instrument these builtins
// under -fsanitize=thread, so the sanitizer correctly models the acquire/
// release ordering of every subtle::Atomic* operation (e.g. the double-checked
// locking in Singleton<>::get()), eliminating false data races.
//
// Use butil/atomicops.h instead of including this directly.

#ifndef BUTIL_ATOMICOPS_INTERNALS_GCC_TSAN_H_
#define BUTIL_ATOMICOPS_INTERNALS_GCC_TSAN_H_

namespace butil {
namespace subtle {

inline Atomic32 NoBarrier_CompareAndSwap(volatile Atomic32* ptr,
                                         Atomic32 old_value,
                                         Atomic32 new_value) {
  Atomic32 cmp = old_value;
  __atomic_compare_exchange_n(ptr, &cmp, new_value, false,
      __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  return cmp;
}

inline Atomic32 NoBarrier_AtomicExchange(volatile Atomic32* ptr,
                                         Atomic32 new_value) {
  return __atomic_exchange_n(ptr, new_value, __ATOMIC_RELAXED);
}

inline Atomic32 Acquire_AtomicExchange(volatile Atomic32* ptr,
                                       Atomic32 new_value) {
  return __atomic_exchange_n(ptr, new_value, __ATOMIC_ACQUIRE);
}

inline Atomic32 Release_AtomicExchange(volatile Atomic32* ptr,
                                       Atomic32 new_value) {
  return __atomic_exchange_n(ptr, new_value, __ATOMIC_RELEASE);
}

inline Atomic32 NoBarrier_AtomicIncrement(volatile Atomic32* ptr,
                                          Atomic32 increment) {
  return __atomic_add_fetch(ptr, increment, __ATOMIC_RELAXED);
}

inline Atomic32 Barrier_AtomicIncrement(volatile Atomic32* ptr,
                                        Atomic32 increment) {
  return __atomic_add_fetch(ptr, increment, __ATOMIC_SEQ_CST);
}

inline Atomic32 Acquire_CompareAndSwap(volatile Atomic32* ptr,
                                       Atomic32 old_value,
                                       Atomic32 new_value) {
  Atomic32 cmp = old_value;
  __atomic_compare_exchange_n(ptr, &cmp, new_value, false,
      __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
  return cmp;
}

inline Atomic32 Release_CompareAndSwap(volatile Atomic32* ptr,
                                       Atomic32 old_value,
                                       Atomic32 new_value) {
  Atomic32 cmp = old_value;
  __atomic_compare_exchange_n(ptr, &cmp, new_value, false,
      __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  return cmp;
}

inline void NoBarrier_Store(volatile Atomic32* ptr, Atomic32 value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELAXED);
}

inline void Acquire_Store(volatile Atomic32* ptr, Atomic32 value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

inline void Release_Store(volatile Atomic32* ptr, Atomic32 value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

inline Atomic32 NoBarrier_Load(volatile const Atomic32* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

inline Atomic32 Acquire_Load(volatile const Atomic32* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

inline Atomic32 Release_Load(volatile const Atomic32* ptr) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

inline Atomic64 NoBarrier_CompareAndSwap(volatile Atomic64* ptr,
                                         Atomic64 old_value,
                                         Atomic64 new_value) {
  Atomic64 cmp = old_value;
  __atomic_compare_exchange_n(ptr, &cmp, new_value, false,
      __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  return cmp;
}

inline Atomic64 NoBarrier_AtomicExchange(volatile Atomic64* ptr,
                                         Atomic64 new_value) {
  return __atomic_exchange_n(ptr, new_value, __ATOMIC_RELAXED);
}

inline Atomic64 Acquire_AtomicExchange(volatile Atomic64* ptr,
                                       Atomic64 new_value) {
  return __atomic_exchange_n(ptr, new_value, __ATOMIC_ACQUIRE);
}

inline Atomic64 Release_AtomicExchange(volatile Atomic64* ptr,
                                       Atomic64 new_value) {
  return __atomic_exchange_n(ptr, new_value, __ATOMIC_RELEASE);
}

inline Atomic64 NoBarrier_AtomicIncrement(volatile Atomic64* ptr,
                                          Atomic64 increment) {
  return __atomic_add_fetch(ptr, increment, __ATOMIC_RELAXED);
}

inline Atomic64 Barrier_AtomicIncrement(volatile Atomic64* ptr,
                                        Atomic64 increment) {
  return __atomic_add_fetch(ptr, increment, __ATOMIC_SEQ_CST);
}

inline void NoBarrier_Store(volatile Atomic64* ptr, Atomic64 value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELAXED);
}

inline void Acquire_Store(volatile Atomic64* ptr, Atomic64 value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

inline void Release_Store(volatile Atomic64* ptr, Atomic64 value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

inline Atomic64 NoBarrier_Load(volatile const Atomic64* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

inline Atomic64 Acquire_Load(volatile const Atomic64* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

inline Atomic64 Release_Load(volatile const Atomic64* ptr) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

inline Atomic64 Acquire_CompareAndSwap(volatile Atomic64* ptr,
                                       Atomic64 old_value,
                                       Atomic64 new_value) {
  Atomic64 cmp = old_value;
  __atomic_compare_exchange_n(ptr, &cmp, new_value, false,
      __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
  return cmp;
}

inline Atomic64 Release_CompareAndSwap(volatile Atomic64* ptr,
                                       Atomic64 old_value,
                                       Atomic64 new_value) {
  Atomic64 cmp = old_value;
  __atomic_compare_exchange_n(ptr, &cmp, new_value, false,
      __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  return cmp;
}

inline void MemoryBarrier() {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

}  // namespace subtle
}  // namespace butil

#endif  // BUTIL_ATOMICOPS_INTERNALS_GCC_TSAN_H_
