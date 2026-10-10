// SPDX-License-Identifier: MIT
#include "HackyNaviGraphicsQueue.hpp"
#include "HackyNaviVMContext.hpp"
#include "HackyNaviVMCompletion.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

using namespace hackynavi;
namespace {
using VM = HackyNaviVMContext;
constexpr uint32_t kAllocationBytes = (gfxaco::kDataBytes + 4095) & ~uint32_t(4095);
constexpr uint32_t kAllocationWords = kAllocationBytes / sizeof(uint32_t);
constexpr uint64_t kSourceVa = 0x100000, kDestinationVa = 0x200000;
struct ReuseScratch {
    uint32_t upload[kAllocationWords], readback[kAllocationWords];
    VM::Handle data[3], firstControls[3];
    HackyNaviDMABuffer* owners[3];
    vmpt::Storage root;
    uint64_t epoch;
    uint32_t pages;
    uint8_t vmid;
};
uint32_t inputWord(uint32_t n) { return 0x9f715ba3U ^ (n * 0x01030d07U); }
uint32_t guardWord(unsigned buffer, uint32_t n) {
    return (buffer == 1 ? 0x6e09d2c5U : 0xd4031b79U) ^ (n * 0x05030201U);
}
}

const char* HackyNaviGraphicsQueue::reuseStepString(ReuseStep step) {
    switch (step) {
#define REUSE_NAME(x) case ReuseStep::x: return #x
    REUSE_NAME(None); REUSE_NAME(Allocate); REUSE_NAME(Upload); REUSE_NAME(MapFirst);
    REUSE_NAME(CaptureFirst); REUSE_NAME(IdentityFirst); REUSE_NAME(SubmitFirst);
    REUSE_NAME(ReadFirst); REUSE_NAME(ConsumeFirst); REUSE_NAME(IdentityConsumed);
    REUSE_NAME(StaleFirst); REUSE_NAME(RemapSecond); REUSE_NAME(CaptureSecond);
    REUSE_NAME(IdentitySecond); REUSE_NAME(StaleSecond); REUSE_NAME(SubmitSecond);
    REUSE_NAME(ReadSecond); REUSE_NAME(RetireSecond); REUSE_NAME(Complete);
    REUSE_NAME(StaleActiveSecond); REUSE_NAME(VerifyReleased);
#undef REUSE_NAME
    }
    return "Unknown";
}

IOReturn HackyNaviGraphicsQueue::runReuseTest(const HackyNaviFunctionGroup::Operation& scope,
    const gfxqueue::PollOptions& poll, ReuseTestResult& out) {
    out = {}; out.firstMismatch = UINT32_MAX;
    if (!gart_ || !gart_->normalAccessReady() ||
        !scope.matchesGraphics(gart_->device_, gart_->openOwner_) || !scope.generation())
        return out.result = kIOReturnOffline;
    if (!poll.maxPolls || poll.maxPolls > 1000000 || !poll.delayUs || poll.delayUs > 10 ||
        uint64_t(poll.maxPolls) * poll.delayUs > 1000000)
        return out.result = kIOReturnBadArgument;
    const vmcontext::Poll retirePoll = {10000, 10};
    auto* scratch = static_cast<ReuseScratch*>(IOMalloc(sizeof(ReuseScratch)));
    if (!scratch) return out.result = kIOReturnNoMemory;
    bzero(scratch, sizeof(*scratch));
    VM* context = nullptr;
    HackyNaviVMCompletion *first = nullptr, *second = nullptr;

    // Brief private observations only. Public VM/queue methods acquire their
    // own locks; never call them with Queue -> Firmware -> GART held here.
    const auto identity = [&](unsigned phase) -> bool {
        lockOwners();
        bool ok = gart_->normalAccessReady() && scope.matchesGraphics(gart_->device_, gart_->openOwner_) &&
            context && context->leased() && context->registryHold_ && context->tree_.ready();
        const auto* root = ok ? context->tree_.page(0) : nullptr;
        ok = ok && root;
        if (ok && phase == 1) {
            scratch->root = root->storage; scratch->pages = context->tree_.pageCount();
            scratch->vmid = context->vmid_; scratch->epoch = context->epoch_;
            out.vmid = context->vmid_; out.firstCapture = context->captureGeneration_;
        }
        if (ok) ok = root->storage.owner == scratch->root.owner && root->storage.bytes == scratch->root.bytes &&
            root->storage.dmaAddress == scratch->root.dmaAddress && context->tree_.pageCount() == scratch->pages &&
            context->vmid_ == scratch->vmid && context->epoch_ == scratch->epoch &&
            context->epoch_ == scope.generation();
        for (unsigned n = 0; n < 3 && ok; ++n) {
            auto* b = context->find(scratch->data[n]);
            ok = b && b->dma->size() == kAllocationBytes;
            if (ok && phase == 1) scratch->owners[n] = b->dma;
            if (ok) ok = b->dma == scratch->owners[n];
        }
        unsigned allocated = 0, internal = 0, mappings = 0;
        if (ok) {
            for (const auto& b : context->buffers_) if (b.dma) { ++allocated; if (b.internal) ++internal; }
            for (const auto& m : context->mappings_) if (m.bytes) ++mappings;
        }
        if (ok && phase == 0) {
            ok = context->state_ == VM::State::Prepared && context->reusableDetached_ && !context->hardwareHold_ &&
                context->mm_.phase() == vmcontext::Phase::Detached && context->gfx_.phase() == vmcontext::Phase::Detached &&
                !context->submittedQueue_ && !context->submittedCaptureGeneration_ &&
                context->captureGeneration_ == out.firstCapture && allocated == 3 && !internal && mappings == 2 &&
                context->residentBytes_ == 3 * kAllocationBytes;
            for (const auto& h : scratch->firstControls) if (ok) {
                const auto& b = context->buffers_[h.id - 1];
                ok = !b.dma && !b.internal && !b.mappings && b.generation == h.generation;
            }
        } else if (ok) {
            const VM::Handle controls[] = {context->capturedShader_, context->capturedIndirect_, context->capturedParameters_};
            uint64_t actualMask = 0, expectedMask = 0;
            ok = context->capturedClass_ == VM::ShaderClass::AppleAddOneGfx1100V1 &&
                context->captureValidLocked() && context->capturedControlMaskLocked(actualMask) &&
                allocated == 6 && internal == 3 && mappings == 5 &&
                context->residentBytes_ == 3 * kAllocationBytes + 3 * gfxaco::kControlBytes;
            for (unsigned n = 0; n < 3 && ok; ++n) {
                const auto h = controls[n];
                ok = h.id && h.id <= VM::kBufferSlots && h.generation;
                if (!ok) break;
                const uint64_t bit = UINT64_C(1) << (h.id - 1);
                ok = !(expectedMask & bit); expectedMask |= bit;
                if (phase == 1) scratch->firstControls[n] = h;
                else ok = ok && h.id == scratch->firstControls[n].id &&
                    scratch->firstControls[n].generation != UINT32_MAX &&
                    h.generation == scratch->firstControls[n].generation + 1;
            }
            ok = ok && actualMask == expectedMask;
            if (ok && phase != 1) {
                ok = out.firstCapture != UINT64_MAX && context->captureGeneration_ == out.firstCapture + 1;
                out.secondCapture = context->captureGeneration_;
            }
        }
        if (ok && phase != 1) { out.dataOwnersPreserved = true; out.rootPreserved = true; }
        if (ok && phase == 2) out.controlsReused = 3;
        finish(kIOReturnSuccess);
        return ok;
    };
    const auto compare = [&](unsigned job) -> bool {
        for (uint32_t n = 0; n < gfxaco::kThreads + gfxaco::kGuardWords; ++n) {
            const auto expected = n < gfxaco::kThreads ? inputWord(n) + job : guardWord(job, n);
            ++out.wordsCompared;
            if (n >= gfxaco::kThreads) ++out.guardWordsCompared;
            if (scratch->readback[n] != expected) {
                out.firstMismatch = n; out.expected = expected; out.observed = scratch->readback[n]; return false;
            }
        }
        return true;
    };
    const auto stale = [&]() -> bool {
        uint32_t untouched = 0x61dc7b2a;
        if (first->job() || first->readDestination(0, &untouched, sizeof(untouched)) != kIOReturnNotReady ||
            untouched != 0x61dc7b2a) return false;
        ++out.staleRejections;
        if (first->consumeJob(retirePoll) != kIOReturnNotReady) return false;
        ++out.staleRejections;
        if (first->retireContext(retirePoll) != kIOReturnNotReady) return false;
        ++out.staleRejections;
        return true;
    };
    const auto execute = [&]() -> IOReturn {
        IOReturn status = kIOReturnSuccess;
#define REUSE_STEP(name, expression) do { out.step = ReuseStep::name; status = (expression); \
        if (status != kIOReturnSuccess) return status; } while (false)
        out.step = ReuseStep::Allocate;
        for (uint8_t vmid = 1; vmid <= 7; ++vmid) {
            context = VM::withGART(gart_, vmid, scope.generation(), status);
            if (context || status != kIOReturnBusy) break;
        }
        if (!context) return status;
        for (auto& h : scratch->data) REUSE_STEP(Allocate, context->allocateBuffer(kAllocationBytes, 4096, h));
        // Upload initial A and sentinel-filled B/C only. No writeBuffer occurs
        // after the first job, including after readback of B for comparison.
        for (unsigned n = 0; n < 3; ++n) {
            for (uint32_t w = 0; w < kAllocationWords; ++w)
                scratch->upload[w] = n ? guardWord(n, w) : inputWord(w);
            REUSE_STEP(Upload, context->writeBuffer(scratch->data[n], 0, scratch->upload, kAllocationBytes));
        }
        REUSE_STEP(MapFirst, context->map(scratch->data[0], 0, kSourceVa, kAllocationBytes, VM::Read));
        REUSE_STEP(MapFirst, context->map(scratch->data[1], 0, kDestinationVa, kAllocationBytes, VM::Read | VM::Write));
        REUSE_STEP(CaptureFirst, context->captureCompilerCompute({scratch->data[0], scratch->data[1], 0, 0}));
        REUSE_STEP(IdentityFirst, identity(1) ? kIOReturnSuccess : kIOReturnInternalError);
        REUSE_STEP(SubmitFirst, submitIsolated(context, poll, first));
        if (!first) return kIOReturnInternalError;
        ++out.jobsCompleted;
        out.firstFence = first->sequence_; out.firstRptr = first->producer_;
        REUSE_STEP(ReadFirst, first->readDestination(0, scratch->readback, gfxaco::kDataBytes));
        REUSE_STEP(ReadFirst, compare(1) ? kIOReturnSuccess : kIOReturnIOError);
        REUSE_STEP(ConsumeFirst, first->consumeJob(retirePoll));
        REUSE_STEP(IdentityConsumed, identity(0) ? kIOReturnSuccess : kIOReturnInternalError);
        REUSE_STEP(StaleFirst, stale() ? kIOReturnSuccess : kIOReturnInternalError);
        REUSE_STEP(RemapSecond, context->unmap(scratch->data[0], kSourceVa, kAllocationBytes));
        REUSE_STEP(RemapSecond, context->unmap(scratch->data[1], kDestinationVa, kAllocationBytes));
        REUSE_STEP(RemapSecond, context->map(scratch->data[1], 0, kSourceVa, kAllocationBytes, VM::Read));
        REUSE_STEP(RemapSecond, context->map(scratch->data[2], 0, kDestinationVa, kAllocationBytes, VM::Read | VM::Write));
        REUSE_STEP(CaptureSecond, context->captureCompilerCompute({scratch->data[1], scratch->data[2], 0, 0}));
        REUSE_STEP(IdentitySecond, identity(2) ? kIOReturnSuccess : kIOReturnInternalError);
        REUSE_STEP(StaleSecond, stale() ? kIOReturnSuccess : kIOReturnInternalError);
        REUSE_STEP(SubmitSecond, submitIsolated(context, poll, second));
        if (!second) return kIOReturnInternalError;
        ++out.jobsCompleted;
        out.secondFence = second->sequence_; out.secondRptr = second->producer_;
        REUSE_STEP(SubmitSecond, out.secondFence > out.firstFence && out.secondRptr > out.firstRptr && identity(3)
            ? kIOReturnSuccess : kIOReturnInternalError);
        REUSE_STEP(StaleActiveSecond, stale() ? kIOReturnSuccess : kIOReturnInternalError);
        REUSE_STEP(ReadSecond, second->readDestination(0, scratch->readback, gfxaco::kDataBytes));
        REUSE_STEP(ReadSecond, compare(2) ? kIOReturnSuccess : kIOReturnIOError);
        REUSE_STEP(RetireSecond, second->retireContext(retirePoll));
        out.step = ReuseStep::VerifyReleased;
        lockOwners();
        bool released = context->state_ == VM::State::Retired && !context->hardwareHold_ &&
            !context->registryHold_ && !context->tree_.ready() && !context->residentBytes_ &&
            gart_->vmOwners_[scratch->vmid - 1] != context;
        for (const auto& b : context->buffers_) if (b.dma || b.mappings) released = false;
        for (const auto& m : context->mappings_) if (m.bytes) released = false;
        finish(kIOReturnSuccess);
        if (!released) return kIOReturnInternalError;
        out.step = ReuseStep::Complete; out.completed = true;
        return kIOReturnSuccess;
#undef REUSE_STEP
    };
    out.result = execute();
    // No descendant locks are held here. An ambiguous exposed context keeps
    // both its hardware/registry references for Engine's actual reset owner.
    // Only genuinely unpublished or proven consumed-idle storage can use the
    // CPU cleanup path; that method rechecks the live hardware hold itself.
    if (context && out.result != kIOReturnSuccess) {
        lockOwners();
        const bool exposed = context->hardwareHold_;
        out.retainedOnFailure = exposed && context->registryHold_;
        finish(kIOReturnSuccess);
        if (!exposed) out.cleanupResult = context->releaseBeforeExposure();
    }
    if (second) second->release();
    if (first) first->release();
    if (context) context->release();
    IOFree(scratch, sizeof(*scratch));
    return out.result;
}
