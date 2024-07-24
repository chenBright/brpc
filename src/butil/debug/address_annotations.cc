#include "butil/debug/address_annotations.h"
#include "butil/debug/stack_trace.h"
#include "butil/logging.h"

extern "C" {

void BAIDU_WEAK __asan_poison_memory_region(void const volatile*, size_t) {}

void BAIDU_WEAK __asan_unpoison_memory_region(void const volatile*, size_t) {}

void BAIDU_WEAK __sanitizer_start_switch_fiber(void**, const void*, size_t) {
    LOG_ONCE(ERROR) << "Address sanitizer does not support fibers";
}

void BAIDU_WEAK __sanitizer_finish_switch_fiber(void*, const void**, size_t*) {
    LOG_ONCE(ERROR) << "Address sanitizer does not support fibers";
}

void* BAIDU_WEAK __asan_get_current_fake_stack(void) {
    return reinterpret_cast<void*>(0x1111111);
}

// void __asan_on_error(void) {
//     LOG(INFO) << 1111111;
//     __sanitizer_print_stack_trace();
// }

void BAIDU_WEAK __sanitizer_print_stack_trace(void) {
    LOG(INFO) << 22222;
}

int BAIDU_WEAK __asan_update_allocation_context(void* addr) {
    return 0;
}

} // extern "C"

namespace butil {
namespace debug {

} // namespace debug
} // namespace butil