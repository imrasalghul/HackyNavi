// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOReturn.h>
#include <libkern/c++/OSObject.h>
#include <stddef.h>
#include <stdint.h>
#include "hackynavi/vm_context.hpp"
class HackyNaviGraphicsQueue;
class HackyNaviVMContext;

// Private-constructor, queue-issued observation of a real GPU fence and RPTR.
// Retains both native owners. Bounded readback precedes one-shot retirement.
// Retirement revalidates the sealed sole producer, actual fence/RPTR/CP idle,
// then invalidates and disables both VM hubs before freeing any DMA storage.
// Any subsequent queue producer advance, fault, reset or context retirement
// invalidates readback. Caller releases the object outside driver mutexes.
class HackyNaviVMCompletion : public OSObject {
    OSDeclareDefaultStructors(HackyNaviVMCompletion)
public:
    IOReturn readDestination(uint64_t relativeOffset, void* kernelOutput, size_t bytes);
    // Caller installs real GART::Access from Operation or RetirementScope.
    // No readback is possible after retirement begins; failures retain exposed
    // storage for verified device recovery. Poll is bounded to100ms per hub.
    IOReturn retireContext(const hackynavi::vmcontext::Poll&);
    // Normal Operation authority only. After actual fence/RPTR/idle proof,
    // detach both hubs and release this job's private controls. Preserve the
    // real data BOs, tree and VMID for another capture. Consumes this token;
    // all later read/consume/retire attempts fail. Not a userspace selector.
    IOReturn consumeJob(const hackynavi::vmcontext::Poll&);
    uint64_t job() const { return valid_ ? sequence_ : 0; }
    // Historical Session identity must be copied before consuming this token.
    // A number alone never authorizes new GPU work or storage retirement.
    uint64_t captureGeneration() const { return valid_ ? captureGeneration_ : 0; }
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviGraphicsQueue;
    static HackyNaviVMCompletion* withOwners(HackyNaviGraphicsQueue*, HackyNaviVMContext*);
    HackyNaviGraphicsQueue* queue_;
    HackyNaviVMContext* context_;
    uint64_t epoch_, offset_, bytes_, captureGeneration_;
    uint32_t id_, generation_, sequence_, producer_;
    uint8_t vmid_;
    bool valid_;
};
