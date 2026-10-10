// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_ACCELERATOR_PROTOCOL_HPP
#define HACKYNAVI_ACCELERATOR_PROTOCOL_HPP
#include <IOKit/IOService.h>
#include <stdint.h>

// Kernel-to-kernel service protocol. This is not a user-client selector or an
// exported HackyNaviDevice C++ ABI. Both participants use public IOService
// dispatch so the boot core has no hard link to the SystemKC IOAccelerator.
namespace hackynavi { namespace acceleratorbridge {
static constexpr uint32_t kVersion = 1;
static constexpr const char* kPublish = "org.hackynavi.accelerator.publish.v1";
static constexpr const char* kWithdraw = "org.hackynavi.accelerator.withdraw.v1";
static constexpr const char* kOpen = "org.hackynavi.accelerator.open-native.v1";
static constexpr const char* kReady = "HackyNaviAcceleratorReady";
struct Request {
    uint32_t bytes;
    uint32_t version;
    IOService* child;
    uint64_t ownerRegistryID;
    // Output observation only. Neither this number nor the registry hint
    // grants hardware access; the core's current normal Operation does.
    uint64_t generation;
};
static_assert(sizeof(Request) == 32, "Intel kernel accelerator request ABI");
struct OpenRequest {
    Request identity;
    task_t task;
    void* securityID;
    OSDictionary* properties;
    IOUserClient* client;
    uint32_t type;
    uint32_t reserved;
};
static_assert(sizeof(OpenRequest) == 72, "Intel kernel accelerator open ABI");
} }
#endif
