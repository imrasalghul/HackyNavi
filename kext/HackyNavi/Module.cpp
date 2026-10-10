// SPDX-License-Identifier: MIT
#include <mach/kmod.h>

// Use the platform kmod entry points and preserve C++ initializer sections.
// On x86_64 the kernel handles C++ initialization; older architectures used
// libkmodc++ wrappers. Do not replace _start/_stop with no-op definitions.
extern "C" {
kern_return_t _start(kmod_info_t*, void*);
kern_return_t _stop(kmod_info_t*, void*);
KMOD_EXPLICIT_DECL(org.hackynavi.driver.HackyNavi, "0.1.0", _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t* _realmain = nullptr;
__attribute__((visibility("hidden"))) kmod_stop_func_t* _antimain = nullptr;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
