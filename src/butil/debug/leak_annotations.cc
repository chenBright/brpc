// todo apache liscence
#include "butil/debug/leak_annotations.h"
#include "butil/macros.h"

extern "C" {
void BAIDU_WEAK __lsan_disable() {}
void BAIDU_WEAK __lsan_enable() {}
void BAIDU_WEAK __lsan_ignore_object(const void*) {}

// Invoke leak detection immediately. If leaks are found, the process will exit.
void BAIDU_WEAK __lsan_do_leak_check() {}
}  // extern "C"

namespace butil {
namespace debug {

} // namespace debug
} // namespace butil

