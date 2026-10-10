// SPDX-License-Identifier: MIT
#include "HackyNaviVMCompletion.hpp"
#include "HackyNaviGraphicsQueue.hpp"
#include "HackyNaviVMContext.hpp"
OSDefineMetaClassAndStructors(HackyNaviVMCompletion, OSObject)
bool HackyNaviVMCompletion::init() {
    queue_ = nullptr; context_ = nullptr; epoch_ = offset_ = bytes_ = 0;
    captureGeneration_ = 0;
    id_ = generation_ = sequence_ = producer_ = 0; vmid_ = 0; valid_ = false;
    return OSObject::init();
}
HackyNaviVMCompletion* HackyNaviVMCompletion::withOwners(HackyNaviGraphicsQueue* queue,
    HackyNaviVMContext* context) {
    auto* completion = new HackyNaviVMCompletion;
    if (!completion) return nullptr;
    if (!completion->init()) { completion->release(); return nullptr; }
    completion->queue_ = queue; queue->retain();
    completion->context_ = context; context->retain();
    return completion;
}
IOReturn HackyNaviVMCompletion::readDestination(uint64_t offset, void* output, size_t bytes) {
    if (!queue_ || !context_ || !valid_) return kIOReturnNotReady;
    return queue_->readCompletion(*this, offset, output, bytes);
}
IOReturn HackyNaviVMCompletion::retireContext(const hackynavi::vmcontext::Poll& poll) {
    if (!queue_ || !context_ || !valid_) return kIOReturnNotReady;
    return queue_->retireCompletion(*this, poll);
}
IOReturn HackyNaviVMCompletion::consumeJob(const hackynavi::vmcontext::Poll& poll) {
    if (!queue_ || !context_ || !valid_) return kIOReturnNotReady;
    return queue_->consumeCompletion(*this, poll);
}
void HackyNaviVMCompletion::free() {
    if (context_) { context_->release(); context_ = nullptr; }
    if (queue_) { queue_->release(); queue_ = nullptr; }
    OSObject::free();
}
