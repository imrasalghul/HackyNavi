// SPDX-License-Identifier: MIT
#include <mach/kmod.h>
extern "C" {
kern_return_t _start(kmod_info_t*, void*);
kern_return_t _stop(kmod_info_t*, void*);
KMOD_EXPLICIT_DECL(org.hackynavi.driver.HackyNaviAccelerator, "0.1.0", _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t* _realmain = nullptr;
__attribute__((visibility("hidden"))) kmod_stop_func_t* _antimain = nullptr;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
