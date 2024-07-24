// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BUTIL_DEBUG_ADDRESS_ANNOTATIONS_H_
#define BUTIL_DEBUG_ADDRESS_ANNOTATIONS_H_

#include "butil/macros.h"

// Public LSan API from <sanitizer/asan_interface.h>.
extern "C" {
void __asan_poison_memory_region(void const volatile*, size_t);
void __asan_unpoison_memory_region(void const volatile*, size_t);
void __sanitizer_start_switch_fiber(void**, const void*, size_t);
void __sanitizer_finish_switch_fiber(void*, const void**, size_t*);
void *__asan_get_current_fake_stack(void);
void __sanitizer_print_stack_trace(void);
int __asan_update_allocation_context(void* addr);
} // extern "C"

namespace butil {
namespace debug {

BUTIL_FORCE_INLINE void
ASanPoisonMemoryRegion(void const volatile* addr, size_t size)  {
    __asan_poison_memory_region(addr, size);
}
BUTIL_FORCE_INLINE void
ASanUnpoisonMemoryRegion(void const volatile* addr, size_t size) {
    __asan_unpoison_memory_region(addr, size);
}

BUTIL_FORCE_INLINE void
ASanStartSwitchFiber(void** fake_stack_save,
                     const void* bottom,
                     size_t size) {
    __sanitizer_start_switch_fiber(fake_stack_save, bottom, size);
}
BUTIL_FORCE_INLINE void
ASanFinishSwitchFiber(void *fake_stack_save,
                      const void **bottom_old,
                      size_t *size_old) {
    __sanitizer_finish_switch_fiber(fake_stack_save, bottom_old, size_old);
}

} // namespace debug
} // namespace butil

#endif  // BUTIL_DEBUG_ADDRESS_ANNOTATIONS_H_
