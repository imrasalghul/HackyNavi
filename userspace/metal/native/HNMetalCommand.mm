// SPDX-License-Identifier: MIT
#import "HNMetalCommand.hpp"
#import <dispatch/dispatch.h>
#include "../../runtime/command_lifecycle.hpp"
#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace {
using namespace hackynavi::compute;
namespace command = hackynavi::compute::command;
// Public Metal table indices are 0..30. The independent native ABI can still
// carry 32 resources; it is not the public Metal argument-index contract.
constexpr size_t kSlots = 64, kBindings = 31, kCommands = 64, kHandlers = 64;
static_assert(uint32_t(command::Phase::NotEnqueued) == MTLCommandBufferStatusNotEnqueued &&
              uint32_t(command::Phase::Enqueued) == MTLCommandBufferStatusEnqueued &&
              uint32_t(command::Phase::Committed) == MTLCommandBufferStatusCommitted &&
              uint32_t(command::Phase::Scheduled) == MTLCommandBufferStatusScheduled &&
              uint32_t(command::Phase::Completed) == MTLCommandBufferStatusCompleted &&
              uint32_t(command::Phase::Error) == MTLCommandBufferStatusError, "Metal command status ABI");
thread_local const void* callbackOwner = nullptr;
thread_local const void* workerOwner = nullptr;

[[noreturn]] void invalid(NSString* reason) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
}
[[noreturn]] void unsupported(SEL selector) {
    invalid([NSString stringWithFormat:@"%@ is not implemented by the native compute command profile.",
             NSStringFromSelector(selector)]);
}
NSString* labelCopy(NSString* value) {
    if (value && (![value isKindOfClass:[NSString class]] || value.length > 4096))
        invalid(@"Expected a string of at most 4096 characters.");
    return [value copy];
}
NSError* commandError(NSString* text, Result result, NSError* cause = nil) {
    MTLCommandBufferError code = MTLCommandBufferErrorInternal;
    switch (result.status) {
        case Status::Invalid: case Status::NotFound: code = MTLCommandBufferErrorInvalidResource; break;
        case Status::OutOfMemory: code = MTLCommandBufferErrorOutOfMemory; break;
        case Status::Timeout: code = MTLCommandBufferErrorTimeout; break;
        case Status::DeviceLost: code = MTLCommandBufferErrorDeviceRemoved; break;
        default: break;
    }
    NSMutableDictionary* info = [@{NSLocalizedDescriptionKey:text,
        @"HackyNaviStatus":@(uint32_t(result.status)), @"IOReturn":@(result.iokit)} mutableCopy];
    if (cause) info[NSUnderlyingErrorKey] = cause;
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:code userInfo:info];
}
bool span(const void* p, size_t bytes) {
    return !bytes || (p && uintptr_t(p) <= UINTPTR_MAX - (bytes - 1));
}
struct BoundBuffer {
    HNMetalBuffer* __strong object = nil;
    std::shared_ptr<Buffer> native;
    uint64_t offset = 0;
};
bool validGraphicsBuffer(const BoundBuffer& bound, id<MTLDevice> device,
                         const std::shared_ptr<Context>& context,uint64_t bytes) {
    if (!bound.object || ![bound.object isMemberOfClass:[HNMetalBuffer class]] ||
        bound.object.device != device || !context || [bound.object nativeContext] != context ||
        !bound.native || [bound.object nativeBuffer] != bound.native ||
        bound.object.length != bound.native->length() || (bound.offset & 15)) return false;
    const uint64_t length = bound.native->length();
    return bound.offset <= length && length - bound.offset >= bytes;
}
bool validFragmentBuffer(const BoundBuffer& bound,id<MTLDevice> device,const std::shared_ptr<Context>& context) {
    return validGraphicsBuffer(bound,device,context,16);
}
bool validVertexBuffer(const BoundBuffer& bound,id<MTLDevice> device,const std::shared_ptr<Context>& context) {
    return validGraphicsBuffer(bound,device,context,48);
}
enum class CommandKind { Compute, SynchronizeManaged, Draw, SynchronizeTexture };
struct RecordedCommand {
    CommandKind kind = CommandKind::Compute;
    HNMetalComputePipeline* __strong pipeline = nil;
    std::array<BoundBuffer, kBindings> buffers;
    BoundBuffer synchronized;
    HNMetalRenderPipeline* __strong renderPipeline = nil;
    BoundBuffer fragment;
    BoundBuffer vertex;
    HNMetalTexture* __strong texture = nil;
    MTLSize grid{}, local{};
};
struct PreparedCommand {
    CommandKind kind = CommandKind::Compute;
    std::shared_ptr<const Pipeline> pipeline;
    std::array<Binding, kBindings> bindings;
    BoundBuffer synchronized;
    std::shared_ptr<const GraphicsPipeline> graphics;
    std::shared_ptr<const FragmentBufferGraphicsPipeline> bufferGraphics;
    std::shared_ptr<const VertexBufferGraphicsPipeline> vertexGraphics;
    std::shared_ptr<const VertexFragmentBufferGraphicsPipeline> vertexFragmentGraphics;
    BoundBuffer fragment;
    BoundBuffer vertex;
    HNMetalTexture* __strong texture = nil;
    size_t count = 0;
};
struct CommandState {
    command::Lifecycle life;
    command::Ticket ticket;
    bool encoderActive = false;
    NSString* __strong label = nil;
    NSError* __strong error = nil;
    NSMutableArray<MTLCommandBufferHandler>* __strong scheduled = [NSMutableArray new];
    NSMutableArray<MTLCommandBufferHandler>* __strong completed = [NSMutableArray new];
    NSMutableArray<NSString*>* __strong debug = [NSMutableArray new];
    std::vector<RecordedCommand> commands;
};
struct QueueState {
    explicit QueueState(uint32_t limit) : scheduler(limit) {}
    std::mutex mutex;
    std::condition_variable changed;
    command::Scheduler scheduler;
    std::array<HNMetalCommandBuffer* __strong, kSlots> committed{};
    bool draining = false;
};
void mutableCommand(const CommandState& state) {
    if (!state.life.mutableEncoding()) invalid(@"The command buffer has already been committed.");
}
void validateShape(MTLSize grid, MTLSize local, NSUInteger maximum) {
    const NSUInteger l[3] = {local.width, local.height, local.depth};
    const NSUInteger g[3] = {grid.width, grid.height, grid.depth};
    uint64_t product = 1, total = 1;
    for (unsigned n = 0; n != 3; ++n) {
        if (!l[n] || l[n] > UINT32_MAX || product > maximum / l[n] || g[n] > UINT32_MAX)
            invalid(@"Dispatch dimensions exceed the actual pipeline's compute limits.");
        product *= l[n];
        if (g[n] && total > UINT64_MAX / g[n]) invalid(@"Dispatch thread count overflows 64 bits.");
        total *= g[n];
    }
}
bool emptyAttachment(MTLRenderPassAttachmentDescriptor* a,MTLRenderPassAttachmentDescriptor* defaults) {
    return a && defaults && !a.texture && !a.resolveTexture && !a.level && !a.slice && !a.depthPlane &&
        !a.resolveLevel && !a.resolveSlice && !a.resolveDepthPlane && a.loadAction==defaults.loadAction &&
        a.storeAction==defaults.storeAction && a.storeActionOptions==defaults.storeActionOptions;
}
HNMetalTexture* renderTarget(MTLRenderPassDescriptor* supplied,id<MTLDevice> device,
                            const std::shared_ptr<Context>& context) {
    MTLRenderPassDescriptor* d=[supplied copy];
    MTLRenderPassDescriptor* defaults=[MTLRenderPassDescriptor renderPassDescriptor];
    if (!d || !defaults || !d.colorAttachments || !d.sampleBufferAttachments || d.visibilityResultBuffer ||
        (d.renderTargetArrayLength!=0 && d.renderTargetArrayLength!=1) || d.imageblockSampleLength ||
        d.threadgroupMemoryLength || d.tileWidth || d.tileHeight ||
        d.defaultRasterSampleCount!=defaults.defaultRasterSampleCount ||
        d.rasterizationRateMap || [d getSamplePositions:nullptr count:0] ||
        !emptyAttachment(d.depthAttachment,defaults.depthAttachment) ||
        !emptyAttachment(d.stencilAttachment,defaults.stencilAttachment)) return nil;
    if (@available(macOS 26.0,*))
        if (d.supportColorAttachmentMapping || d.visibilityResultType!=defaults.visibilityResultType) return nil;
    for (NSUInteger n=0;n<4;++n) {
        auto* a=d.sampleBufferAttachments[n]; auto* base=defaults.sampleBufferAttachments[n];
        if (a.sampleBuffer || a.startOfVertexSampleIndex!=base.startOfVertexSampleIndex ||
            a.endOfVertexSampleIndex!=base.endOfVertexSampleIndex ||
            a.startOfFragmentSampleIndex!=base.startOfFragmentSampleIndex ||
            a.endOfFragmentSampleIndex!=base.endOfFragmentSampleIndex) return nil;
    }
    for (NSUInteger n=1;n<8;++n)
        if (!emptyAttachment(d.colorAttachments[n],defaults.colorAttachments[n])) return nil;
    auto* a=d.colorAttachments[0];
    if (!a || ![a.texture isMemberOfClass:[HNMetalTexture class]] || a.texture.device!=device ||
        a.level || a.slice || a.depthPlane || a.resolveTexture || a.resolveLevel || a.resolveSlice ||
        a.resolveDepthPlane || (a.loadAction!=MTLLoadActionLoad && a.loadAction!=MTLLoadActionDontCare) ||
        a.storeAction!=MTLStoreActionStore || a.storeActionOptions!=MTLStoreActionOptionNone) return nil;
    HNMetalTexture* texture=(HNMetalTexture*)a.texture;
    if ([texture nativeContext]!=context || (d.renderTargetWidth && d.renderTargetWidth!=texture.width) ||
        (d.renderTargetHeight && d.renderTargetHeight!=texture.height)) return nil;
    return texture;
}
}

@interface HNMetalCommandQueue ()
- (void)kick;
- (void)drain;
- (std::shared_ptr<QueueState>)queueState;
- (std::shared_ptr<Context>)context;
@end


@interface HNMetalCommandBuffer ()
- (instancetype)initWithQueue:(HNMetalCommandQueue*)queue state:(std::shared_ptr<CommandState>)state;
- (std::shared_ptr<CommandState>)commandState;
- (void)executeNative;
- (void)invokeHandlers:(BOOL)success;
@end
@interface HNMetalCommandEncoder ()
- (instancetype)initWithBuffer:(HNMetalCommandBuffer*)buffer;
@end
@interface HNMetalBlitEncoder ()
- (instancetype)initWithBuffer:(HNMetalCommandBuffer*)buffer;
@end
@interface HNMetalRenderEncoder ()
- (instancetype)initWithBuffer:(HNMetalCommandBuffer*)buffer target:(HNMetalTexture*)target;
@end

@implementation HNMetalCommandQueue {
    id<HNMetalContextOwner> _device;
    std::shared_ptr<Context> _context;
    std::shared_ptr<QueueState> _state;
    dispatch_queue_t _worker;
    NSString* _label;
}
- (instancetype)init { return nil; }
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                 maxCommandBufferCount:(NSUInteger)limit
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    self = [super init]; if (!self) return nil;
    if (!device || ![device respondsToSelector:@selector(nativeComputeContext)] || !limit || limit > kSlots) {
        if (error) *error = commandError(@"Expected an existing native device and a queue capacity of 1..64.", {Status::Invalid, 0});
        return nil;
    }
    try {
        _context = [device nativeComputeContext];
        hackynavi::nativeabi::HelloReply info{};
        auto live = _context ? _context->information(info) : Result{Status::Invalid, 0};
        if (live && [device nativeComputeContext] != _context) live = {Status::Invalid, 0};
        if (!live) {
            if (error) *error = commandError(@"The command queue requires the device's live native context.", live);
            return nil;
        }
        _state = std::make_shared<QueueState>(uint32_t(limit));
        _worker = dispatch_queue_create("org.hackynavi.Metal.compute", DISPATCH_QUEUE_SERIAL);
        if (!_state->scheduler.valid() || !_worker) {
            if (error) *error = commandError(@"Unable to allocate command queue state.", {Status::OutOfMemory, 0});
            return nil;
        }
        _device = device; return self;
    } catch (const std::bad_alloc&) {
        if (error) *error = commandError(@"Unable to allocate command queue state.", {Status::OutOfMemory, 0});
        return nil;
    }
}
- (id<MTLDevice>)device { return _device; }
- (std::shared_ptr<Context>)context { return _context; }
- (std::shared_ptr<QueueState>)queueState { return _state; }
- (nullable NSString*)label { @synchronized(self) { return _label; } }
- (void)setLabel:(nullable NSString*)value { NSString* copy = labelCopy(value); @synchronized(self) { _label = copy; } }
- (nullable id<MTLCommandBuffer>)commandBuffer {
    try {
        auto state = std::make_shared<CommandState>();
        if (!state->scheduled || !state->completed || !state->debug) return nil;
        HNMetalCommandBuffer* buffer = [[HNMetalCommandBuffer alloc] initWithQueue:self state:state];
        if (!buffer) return nil;
        {
            std::unique_lock<std::mutex> lock(_state->mutex);
            if (_state->scheduler.usedSlots() == _state->scheduler.capacity() &&
                workerOwner == (__bridge const void*)self)
                invalid(@"The native worker cannot block waiting for its own queue's capacity.");
            _state->changed.wait(lock, [&]{ return _state->scheduler.usedSlots() < _state->scheduler.capacity(); });
            if (!_state->scheduler.reserve(state->life, state->ticket)) return nil;
        }
        return buffer;
    } catch (const std::bad_alloc&) { return nil; }
}
- (nullable id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor*)descriptor {
    if (!descriptor || !descriptor.retainedReferences || descriptor.errorOptions != MTLCommandBufferErrorOptionNone || descriptor.logState)
        return nil;
    return [self commandBuffer];
}
- (nullable id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return nil; }
- (void)kick {
    bool start = false;
    { std::lock_guard<std::mutex> lock(_state->mutex);
      if (!_state->draining) { _state->draining = true; start = true; } }
    if (start) dispatch_async(_worker, ^{ [self drain]; });
}
- (void)drain {
    const void* previous = workerOwner; workerOwner = (__bridge const void*)self;
    @try {
        for (;;) {
            HNMetalCommandBuffer* buffer = nil;
            {
                std::lock_guard<std::mutex> lock(_state->mutex);
                command::Ticket ticket;
                if (!_state->scheduler.takeReady(ticket)) { _state->draining = false; break; }
                buffer = _state->committed[ticket.slot];
            }
            @autoreleasepool { [buffer executeNative]; }
        }
    } @finally { workerOwner = previous; }
}
- (void)insertDebugCaptureBoundary { unsupported(_cmd); }
- (void)addResidencySet:(id<MTLResidencySet>)set { (void)set; unsupported(_cmd); }
- (void)addResidencySets:(const id<MTLResidencySet>[])sets count:(NSUInteger)count { (void)sets; (void)count; unsupported(_cmd); }
- (void)removeResidencySet:(id<MTLResidencySet>)set { (void)set; unsupported(_cmd); }
- (void)removeResidencySets:(const id<MTLResidencySet>[])sets count:(NSUInteger)count { (void)sets; (void)count; unsupported(_cmd); }
@end

@implementation HNMetalCommandBuffer {
    HNMetalCommandQueue* _queue;
    std::shared_ptr<QueueState> _q;
    std::shared_ptr<CommandState> _state;
}
- (instancetype)init { return nil; }
- (instancetype)initWithQueue:(HNMetalCommandQueue*)queue state:(std::shared_ptr<CommandState>)state {
    self = [super init]; if (self) { _queue = queue; _q = [queue queueState]; _state = std::move(state); } return self;
}
- (void)dealloc {
    if (_q && _state) {
        bool canceled;
        { std::lock_guard<std::mutex> lock(_q->mutex); canceled = _q->scheduler.cancel(_state->ticket); }
        if (canceled) { _q->changed.notify_all(); [_queue kick]; }
    }
}
- (std::shared_ptr<CommandState>)commandState { return _state; }
- (id<MTLDevice>)device { return _queue.device; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferErrorOption)errorOptions { return MTLCommandBufferErrorOptionNone; }
- (nullable NSString*)label { std::lock_guard<std::mutex> lock(_q->mutex); return _state->label; }
- (void)setLabel:(nullable NSString*)value {
    NSString* copy = labelCopy(value); std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state); _state->label = copy;
}
- (MTLCommandBufferStatus)status { std::lock_guard<std::mutex> lock(_q->mutex); return MTLCommandBufferStatus(_state->life.phase()); }
- (nullable NSError*)error { std::lock_guard<std::mutex> lock(_q->mutex); return _state->error; }
- (void)enqueue {
    std::lock_guard<std::mutex> lock(_q->mutex);
    if (!_q->scheduler.enqueue(_state->ticket)) invalid(@"A command buffer can be enqueued only once before commit.");
}
- (void)commit {
    {
        std::lock_guard<std::mutex> lock(_q->mutex);
        if (_state->encoderActive || _state->debug.count) invalid(@"End the active encoder and close command debug groups before commit.");
        if (!_q->scheduler.commit(_state->ticket)) invalid(@"A command buffer can be committed only once.");
        _q->committed[_state->ticket.slot] = self;
    }
    [_queue kick];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block {
    if (!block) invalid(@"A scheduled handler is required.");
    MTLCommandBufferHandler copied = [block copy];
    std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state);
    if (_state->scheduled.count >= kHandlers) invalid(@"A command buffer supports at most 64 scheduled handlers.");
    [_state->scheduled addObject:copied];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {
    if (!block) invalid(@"A completion handler is required.");
    MTLCommandBufferHandler copied = [block copy];
    std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state);
    if (_state->completed.count >= kHandlers) invalid(@"A command buffer supports at most 64 completion handlers.");
    [_state->completed addObject:copied];
}
- (void)waitUntilScheduled {
    if (workerOwner == (__bridge const void*)_queue) invalid(@"The native command worker cannot wait on its own queue.");
    std::unique_lock<std::mutex> lock(_q->mutex);
    if (_state->life.mutableEncoding()) invalid(@"Commit the command buffer before waiting.");
    if (callbackOwner == (__bridge const void*)self && !_state->life.scheduledReady())
        invalid(@"A scheduled handler cannot wait for its own scheduling handlers to return.");
    _q->changed.wait(lock, [&]{ return _state->life.scheduledReady(); });
}
- (void)waitUntilCompleted {
    if (callbackOwner == (__bridge const void*)self || workerOwner == (__bridge const void*)_queue)
        invalid(@"A callback cannot wait for itself, and the native worker cannot wait on its own queue.");
    std::unique_lock<std::mutex> lock(_q->mutex);
    if (_state->life.mutableEncoding()) invalid(@"Commit the command buffer before waiting.");
    _q->changed.wait(lock, [&]{ return _state->life.completedReady(); });
}
- (void)executeNative {
    NSError* error = nil;
    auto context = [_queue context];
    bool nativeEntered = false;
    @try {
        try {
            if ([(id<HNMetalContextOwner>)self.device nativeComputeContext] != context)
                error = commandError(@"The command queue's native context was replaced.", {Status::Invalid, 0});
            hackynavi::nativeabi::HelloReply info{};
            const auto live = context->information(info);
            if (!live) error = commandError(@"The command queue's actual native context is no longer live.", live);
            std::vector<PreparedCommand> prepared;
            prepared.reserve(_state->commands.size());
            // Compile every actual shape and validate every binding before any
            // GPU call; a later compiler error cannot leave an earlier command
            // unexpectedly submitted. Native runtime admission remains final.
            for (const auto& record : _state->commands) {
                if (error) break;
                PreparedCommand dispatch;
                dispatch.kind = record.kind;
                if (record.kind == CommandKind::Draw || record.kind == CommandKind::SynchronizeTexture) {
                    if (![record.texture isMemberOfClass:[HNMetalTexture class]] ||
                        record.texture.device != self.device || [record.texture nativeContext] != context ||
                        (record.kind == CommandKind::SynchronizeTexture && record.texture.storageMode != MTLStorageModeManaged)) {
                        error = commandError(@"The recorded texture does not belong to this live native owner.", {Status::Invalid,0}); break;
                    }
                    dispatch.texture = record.texture;
                    if (record.kind == CommandKind::Draw) {
                        if (![record.renderPipeline isMemberOfClass:[HNMetalRenderPipeline class]] ||
                            record.renderPipeline.device != self.device || [record.renderPipeline nativeContext] != context) {
                            error = commandError(@"The recorded graphics pipeline does not belong to this native owner.", {Status::Invalid,0}); break;
                        }
                        dispatch.graphics = [record.renderPipeline nativePipeline];
                        dispatch.bufferGraphics = [record.renderPipeline nativeBufferPipeline];
                        dispatch.vertexGraphics = [record.renderPipeline nativeVertexBufferPipeline];
                        dispatch.vertexFragmentGraphics = [record.renderPipeline nativeVertexFragmentBufferPipeline];
                        if (unsigned(bool(dispatch.graphics))+unsigned(bool(dispatch.bufferGraphics))+
                            unsigned(bool(dispatch.vertexGraphics))+unsigned(bool(dispatch.vertexFragmentGraphics))!=1) {
                            error = commandError(@"The graphics pipeline has no unique admitted machine-code profile.", {Status::Invalid,0}); break;
                        }
                        if (dispatch.bufferGraphics || dispatch.vertexFragmentGraphics) {
                            if (!validFragmentBuffer(record.fragment,self.device,context)) {
                                error = commandError(@"The fragment shader needs this owner's retained buffer0 and an aligned 16-byte range.", {Status::Invalid,0}); break;
                            }
                            dispatch.fragment = record.fragment;
                        }
                        if (dispatch.vertexGraphics || dispatch.vertexFragmentGraphics) {
                            if (!validVertexBuffer(record.vertex,self.device,context)) {
                                error = commandError(@"The vertex shader needs this owner's retained buffer0 and an aligned 48-byte range.", {Status::Invalid,0}); break;
                            }
                            dispatch.vertex = record.vertex;
                        }
                        if (dispatch.vertexFragmentGraphics && dispatch.vertex.native==dispatch.fragment.native) {
                            error = commandError(@"Vertex and fragment resources require distinct native allocations.", {Status::Invalid,0}); break;
                        }
                    }
                    prepared.push_back(std::move(dispatch)); continue;
                }
                if (record.kind == CommandKind::SynchronizeManaged) {
                    const auto& bound = record.synchronized;
                    if (!bound.object || ![bound.object isMemberOfClass:[HNMetalBuffer class]] ||
                        bound.object.device != self.device || bound.object.storageMode != MTLStorageModeManaged ||
                        !bound.native || [bound.object nativeBuffer] != bound.native ||
                        !bound.native->length() || bound.object.length != bound.native->length()) {
                        error = commandError(@"The synchronized resource is not this device's actual Managed buffer.", {Status::Invalid, 0});
                        break;
                    }
                    dispatch.synchronized = bound;
                    prepared.push_back(std::move(dispatch));
                    continue;
                }
                NSError* compileError = nil;
                if (![record.pipeline buildForGrid:record.grid threadsPerThreadgroup:record.local
                    pipeline:dispatch.pipeline error:&compileError]) {
                    error = commandError(@"The actual dispatch compiler rejected this command.", {Status::Unsupported, 0}, compileError); break;
                }
                const auto& resources = dispatch.pipeline->resources();
                if (resources.empty() || resources.size() > kBindings) {
                    error = commandError(@"The actual pipeline has an unsupported resource layout.", {Status::Unsupported, 0}); break;
                }
                for (const auto& resource : resources) {
                    if (resource.metal_index >= kBindings) { error = commandError(@"The reflected buffer index is outside the native profile.", {Status::Invalid, 0}); break; }
                    const auto& bound = record.buffers[resource.metal_index];
                    if (!bound.object || bound.object.device != self.device || !bound.native ||
                        bound.offset >= bound.native->length()) {
                        error = commandError(@"A reflected buffer argument is missing or belongs to another device.", {Status::Invalid, 0}); break;
                    }
                    const uint64_t bytes = bound.native->length() - bound.offset;
                    if ((bound.offset | bytes) & 3 || bytes > UINT32_MAX) {
                        error = commandError(@"Buffer argument offsets and remaining ranges must fit the native four-byte descriptor contract.", {Status::Invalid, 0}); break;
                    }
                    for (size_t n = 0; n < dispatch.count; ++n)
                        if (dispatch.bindings[n].buffer == bound.native || dispatch.bindings[n].binding == resource.binding)
                            error = commandError(@"Aliased or duplicate native buffer bindings are not supported.", {Status::Unsupported, 0});
                    if (error) break;
                    dispatch.bindings[dispatch.count++] = {resource.binding, bound.native, bound.offset, bytes};
                }
                if (!error) prepared.push_back(std::move(dispatch));
            }
            Completion previous{};
            for (const auto& dispatch : prepared) {
                if (error) break;
                if ([(id<HNMetalContextOwner>)self.device nativeComputeContext] != context) {
                    error = commandError(@"The command queue's native context changed before dispatch.", {Status::Invalid, 0}); break;
                }
                if (dispatch.kind == CommandKind::SynchronizeManaged || dispatch.kind == CommandKind::SynchronizeTexture) {
                    // Prior compute calls have really completed and consumed
                    // their jobs. This actual native download follows them in
                    // FIFO order; it creates no GPU completion identity.
                    nativeEntered = true;
                    const auto result = dispatch.kind == CommandKind::SynchronizeTexture ?
                        [dispatch.texture synchronizeManagedContents] : [dispatch.synchronized.object synchronizeManagedContents];
                    nativeEntered = false;
                    if (!result) error = commandError(@"The actual Managed buffer readback failed.", result);
                    continue;
                }
                Completion actual{};
                nativeEntered = true;
                const auto result = dispatch.kind == CommandKind::Draw ?
                    (dispatch.vertexFragmentGraphics ?
                        [dispatch.texture drawWithVertexFragmentBufferPipeline:dispatch.vertexFragmentGraphics
                            vertex:dispatch.vertex.native vertexOffset:dispatch.vertex.offset
                            fragment:dispatch.fragment.native fragmentOffset:dispatch.fragment.offset completion:actual] :
                    dispatch.vertexGraphics ?
                        [dispatch.texture drawWithVertexBufferPipeline:dispatch.vertexGraphics
                            buffer:dispatch.vertex.native offset:dispatch.vertex.offset completion:actual] :
                    dispatch.bufferGraphics ?
                        [dispatch.texture drawWithBufferPipeline:dispatch.bufferGraphics
                            buffer:dispatch.fragment.native offset:dispatch.fragment.offset completion:actual] :
                        [dispatch.texture drawWithPipeline:dispatch.graphics completion:actual]) :
                    context->dispatch(dispatch.pipeline, dispatch.bindings.data(), dispatch.count, actual);
                nativeEntered = false;
                if (!result) error = commandError(@"The actual native submission or retirement failed; work may have executed.", result);
                else if (!actual.epoch || !actual.sequence || (previous.sequence &&
                         (actual.epoch != previous.epoch || actual.sequence <= previous.sequence))) {
                    error = commandError(@"The native dispatch returned an invalid completion identity.", {Status::Protocol, 0});
                    const auto closed = context->close();
                    if (!closed) error = commandError(@"Closing the native context after an invalid completion also failed.", closed, error);
                }
                previous = actual;
            }
        } catch (const std::bad_alloc&) { error = commandError(@"Unable to retain command compilation data.", {Status::OutOfMemory, 0}); }
          catch (...) { error = commandError(@"An unexpected native command exception occurred.", {Status::Protocol, 0}); }
    } @catch (NSException* exception) {
        error = commandError(exception.reason ? exception.reason : @"An Objective-C command exception occurred.", {Status::Protocol, 0});
    }
    if (nativeEntered) {
        // An exception inside a mutating native call has an unknown exposure
        // point. Close through the actual owner's recovery path; never retry.
        try {
            const auto closed = context->close();
            if (!closed) error = commandError(@"Native recovery after an interrupted command failed.", closed, error);
        } catch (...) { error = commandError(@"Native recovery raised an exception; ownership remains unresolved.", {Status::Protocol, 0}, error); }
    }
    const BOOL success = error == nil;
    {
        std::lock_guard<std::mutex> lock(_q->mutex);
        _state->error = error;
        // The native ProductCompute call is synchronous through fence wait and
        // job consumption. Only this actual successful return proves scheduling.
        // Readback-only buffers perform actual copied I/O without a synthetic
        // GPU sequence. Empty buffers complete without a GPU-work claim.
        if (success) _q->scheduler.markScheduled(_state->ticket);
        _q->scheduler.finish(_state->ticket, success);
        // Public commandBuffer capacity is released after execution, not after
        // callbacks. Otherwise a capacity-one handler creating its next buffer
        // would deadlock. The worker and callback block retain this object;
        // its lifecycle/handler wait state outlives this reusable ticket slot.
        _q->scheduler.release(_state->ticket);
        _q->committed[_state->ticket.slot] = nil;
    }
    _q->changed.notify_all();
    // A concurrent executor permits A's handler to wait for B, while the serial
    // GPU worker advances B. A serial callback queue would deadlock that case.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ @autoreleasepool { [self invokeHandlers:success]; } });
}
- (void)invokeHandlers:(BOOL)success {
    const void* previous = callbackOwner; callbackOwner = (__bridge const void*)self;
    @try {
        const auto invoke = [](NSArray<MTLCommandBufferHandler>* list, id<MTLCommandBuffer> buffer) {
            for (MTLCommandBufferHandler handler in list) {
                @try { try { handler(buffer); } catch (...) { NSLog(@"HackyNavi command handler threw a C++ exception."); } }
                @catch (NSException* exception) { NSLog(@"HackyNavi command handler raised %@.", exception.name); }
            }
        };
        if (success) {
            invoke(_state->scheduled, self);
            { std::lock_guard<std::mutex> lock(_q->mutex); _q->scheduler.markScheduledCallbacksDone(_state->life); }
            _q->changed.notify_all();
        }
        invoke(_state->completed, self);
    } @finally {
        // Destroy captured application resources/blocks outside our mutex.
        _state->commands.clear(); [_state->scheduled removeAllObjects]; [_state->completed removeAllObjects];
        {
            std::lock_guard<std::mutex> lock(_q->mutex);
            _q->scheduler.markCallbacksDone(_state->life);
        }
        _q->changed.notify_all();
        callbackOwner = previous;
    }
}
- (nullable id<MTLComputeCommandEncoder>)computeCommandEncoder {
    std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state);
    if (_state->encoderActive) invalid(@"End the current encoder before creating another.");
    HNMetalCommandEncoder* encoder = [[HNMetalCommandEncoder alloc] initWithBuffer:self];
    if (encoder) _state->encoderActive = true;
    return encoder;
}
- (nullable id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
    if (type != MTLDispatchTypeSerial) return nil;
    return [self computeCommandEncoder];
}
- (nullable id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor*)descriptor {
    // Sample attachment-array extent is not exposed by the public descriptor.
    // Until the whole descriptor contract is implemented, reject this path
    // explicitly instead of silently discarding counter requests.
    (void)descriptor; return nil;
}
- (void)pushDebugGroup:(NSString*)string {
    NSString* copy = labelCopy(string); if (!copy) invalid(@"A debug group name is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state);
    if (_state->debug.count >= 64) invalid(@"The debug group nesting limit is 64.");
    [_state->debug addObject:copy];
}
- (void)popDebugGroup {
    std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state);
    if (!_state->debug.count) invalid(@"No command-buffer debug group is open.");
    [_state->debug removeLastObject];
}
- (CFTimeInterval)kernelStartTime { unsupported(_cmd); }
- (CFTimeInterval)kernelEndTime { unsupported(_cmd); }
- (CFTimeInterval)GPUStartTime { unsupported(_cmd); }
- (CFTimeInterval)GPUEndTime { unsupported(_cmd); }
- (id<MTLLogContainer>)logs { unsupported(_cmd); }
- (nullable id<MTLBlitCommandEncoder>)blitCommandEncoder {
    std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state);
    if (_state->encoderActive) invalid(@"End the current encoder before creating another.");
    HNMetalBlitEncoder* encoder = [[HNMetalBlitEncoder alloc] initWithBuffer:self];
    if (encoder) _state->encoderActive = true;
    return encoder;
}
- (nullable id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor*)d { (void)d; return nil; }
- (nullable id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)d {
    HNMetalTexture* target=renderTarget(d,self.device,[_queue context]);
    if (!target) return nil;
    std::lock_guard<std::mutex> lock(_q->mutex); mutableCommand(*_state);
    if (_state->encoderActive) invalid(@"End the current encoder before creating another.");
    auto* encoder=[[HNMetalRenderEncoder alloc] initWithBuffer:self target:target];
    if (encoder) _state->encoderActive=true;
    return encoder;
}
- (nullable id<MTLParallelRenderCommandEncoder>)parallelRenderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)d { (void)d; return nil; }
- (nullable id<MTLResourceStateCommandEncoder>)resourceStateCommandEncoder { return nil; }
- (nullable id<MTLResourceStateCommandEncoder>)resourceStateCommandEncoderWithDescriptor:(MTLResourceStatePassDescriptor*)d { (void)d; return nil; }
- (nullable id<MTLAccelerationStructureCommandEncoder>)accelerationStructureCommandEncoder { return nil; }
- (id<MTLAccelerationStructureCommandEncoder>)accelerationStructureCommandEncoderWithDescriptor:(MTLAccelerationStructurePassDescriptor*)d { (void)d; unsupported(_cmd); }
- (void)presentDrawable:(id<MTLDrawable>)drawable { (void)drawable; unsupported(_cmd); }
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time { (void)drawable; (void)time; unsupported(_cmd); }
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)time { (void)drawable; (void)time; unsupported(_cmd); }
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value { (void)event; (void)value; unsupported(_cmd); }
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value { (void)event; (void)value; unsupported(_cmd); }
- (void)useResidencySet:(id<MTLResidencySet>)set { (void)set; unsupported(_cmd); }
- (void)useResidencySets:(const id<MTLResidencySet>[])sets count:(NSUInteger)count { (void)sets; (void)count; unsupported(_cmd); }
@end

@implementation HNMetalCommandEncoder {
    HNMetalCommandBuffer* _buffer;
    std::shared_ptr<QueueState> _q;
    std::shared_ptr<CommandState> _state;
    bool _ended;
    HNMetalComputePipeline* _pipeline;
    std::array<BoundBuffer, kBindings> _bindings;
    NSString* _label;
    NSMutableArray<NSString*>* _debug;
    NSMutableArray<NSString*>* _signposts;
}
- (instancetype)init { return nil; }
- (instancetype)initWithBuffer:(HNMetalCommandBuffer*)buffer {
    self = [super init];
    if (self) {
        _buffer = buffer; _state = [buffer commandState];
        _q = [(HNMetalCommandQueue*)buffer.commandQueue queueState];
        _debug = [NSMutableArray new]; _signposts = [NSMutableArray new];
        if (!_debug || !_signposts) return nil;
    }
    return self;
}
- (void)checkEncoding {
    mutableCommand(*_state);
    if (_ended || !_state->encoderActive) invalid(@"This compute encoder has ended.");
}
- (id<MTLDevice>)device { return _buffer.device; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }
- (nullable NSString*)label { std::lock_guard<std::mutex> lock(_q->mutex); return _label; }
- (void)setLabel:(nullable NSString*)value {
    NSString* copy = labelCopy(value); std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; _label = copy;
}
- (void)endEncoding {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_debug.count) invalid(@"Close compute debug groups before ending the encoder.");
    _ended = true; _state->encoderActive = false;
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline {
    if (![pipeline isMemberOfClass:[HNMetalComputePipeline class]] || pipeline.device != self.device)
        invalid(@"An actual HackyNavi compute pipeline from this exact device is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; _pipeline = (HNMetalComputePipeline*)pipeline;
}
- (BoundBuffer)checkedBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset {
    BoundBuffer bound;
    if (!buffer) { if (offset) invalid(@"An unbound buffer requires a zero offset."); return bound; }
    if (![buffer isMemberOfClass:[HNMetalBuffer class]] || buffer.device != self.device ||
        (offset & 3) || offset >= buffer.length)
        invalid(@"Expected this device's actual native buffer and an in-range four-byte-aligned offset.");
    bound.object = (HNMetalBuffer*)buffer; bound.native = [bound.object nativeBuffer]; bound.offset = offset;
    if (!bound.native || bound.native->length() != buffer.length)
        invalid(@"The Metal buffer has no matching native allocation owner.");
    return bound;
}
- (void)setBuffer:(nullable id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= kBindings) invalid(@"The Metal compute table has 31 buffer binding slots.");
    BoundBuffer bound = [self checkedBuffer:buffer offset:offset];
    // Move the old retained owner out so its final native release never runs
    // while holding the queue mutex.
    { std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; std::swap(_bindings[index], bound); }
}
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= kBindings || (offset & 3)) invalid(@"Expected a supported buffer index and aligned offset.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!_bindings[index].native || offset >= _bindings[index].native->length()) invalid(@"The bound buffer does not contain this offset.");
    _bindings[index].offset = offset;
}
- (void)setBuffers:(const id<MTLBuffer>[])buffers offsets:(const NSUInteger[])offsets withRange:(NSRange)range {
    if (range.location > kBindings || range.length > kBindings - range.location ||
        !span(buffers, range.length * sizeof(*buffers)) || !span(offsets, range.length * sizeof(*offsets)))
        invalid(@"The buffer array range is outside the 31 Metal binding slots.");
    std::array<BoundBuffer, kBindings> replacement;
    for (NSUInteger n = 0; n < range.length; ++n) replacement[n] = [self checkedBuffer:buffers[n] offset:offsets[n]];
    { std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
      for (NSUInteger n = 0; n < range.length; ++n) std::swap(_bindings[range.location + n], replacement[n]); }
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)local {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!_pipeline) invalid(@"Set an actual compute pipeline before dispatch.");
    validateShape(grid, local, _pipeline.maxTotalThreadsPerThreadgroup);
    if (!grid.width || !grid.height || !grid.depth) return; // Actual zero-work command, no submission or synthetic result.
    if (_state->commands.size() >= kCommands) invalid(@"A command buffer supports at most 64 actual compute dispatches.");
    try {
        RecordedCommand record; record.pipeline = _pipeline; record.grid = grid; record.local = local; record.buffers = _bindings;
        _state->commands.push_back(std::move(record));
    } catch (const std::bad_alloc&) { invalid(@"Unable to retain the dispatch and its native resource owners."); }
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)local {
    const NSUInteger g[3] = {groups.width, groups.height, groups.depth}, l[3] = {local.width, local.height, local.depth};
    NSUInteger result[3]{};
    for (unsigned n = 0; n < 3; ++n) {
        if (!l[n] || l[n] > UINT32_MAX || g[n] > UINT32_MAX || (g[n] && l[n] > UINT32_MAX / g[n]))
            invalid(@"Threadgroup multiplication exceeds the native dispatch dimensions.");
        result[n] = g[n] * l[n];
    }
    [self dispatchThreads:MTLSizeMake(result[0], result[1], result[2]) threadsPerThreadgroup:local];
}
- (void)insertDebugSignpost:(NSString*)string {
    NSString* copy = labelCopy(string); if (!copy) invalid(@"A debug signpost string is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_signposts.count >= 64) invalid(@"The encoder stores at most 64 debug signposts.");
    [_signposts addObject:copy];
}
- (void)pushDebugGroup:(NSString*)string {
    NSString* copy = labelCopy(string); if (!copy) invalid(@"A debug group name is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_debug.count >= 64) invalid(@"The debug group nesting limit is 64.");
    [_debug addObject:copy];
}
- (void)popDebugGroup {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!_debug.count) invalid(@"No compute debug group is open.");
    [_debug removeLastObject];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope {
    if (scope != MTLBarrierScopeBuffers) invalid(@"This serial compute encoder supports only buffer barriers.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    // The SDK explicitly permits ignoring barriers on serial encoders. Every
    // actual Context dispatch here completes before the following dispatch.
}
- (void)memoryBarrierWithResources:(const id<MTLResource>[])resources count:(NSUInteger)count {
    if (count > kBindings || !span(resources, count * sizeof(*resources))) invalid(@"Invalid barrier resource list.");
    for (NSUInteger n = 0; n < count; ++n)
        if (![resources[n] isMemberOfClass:[HNMetalBuffer class]] || resources[n].device != self.device)
            invalid(@"A serial buffer barrier requires this device's actual buffers.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
}
- (void)barrierAfterQueueStages:(MTLStages)after beforeStages:(MTLStages)before API_AVAILABLE(macos(26.0)) {
    if (after != MTLStageDispatch || before != MTLStageDispatch) unsupported(_cmd);
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    // All prior compute dispatches in this serial queue have actually finished
    // before this pass reaches the native Context. No asynchronous barrier is
    // being represented or emulated.
}
- (void)setBytes:(const void*)bytes length:(NSUInteger)length atIndex:(NSUInteger)index { (void)bytes; (void)length; (void)index; unsupported(_cmd); }
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index { (void)buffer; (void)offset; (void)stride; (void)index; unsupported(_cmd); }
- (void)setBuffers:(const id<MTLBuffer>[])buffers offsets:(const NSUInteger[])offsets attributeStrides:(const NSUInteger[])strides withRange:(NSRange)range { (void)buffers; (void)offsets; (void)strides; (void)range; unsupported(_cmd); }
- (void)setBufferOffset:(NSUInteger)offset attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index { (void)offset; (void)stride; (void)index; unsupported(_cmd); }
- (void)setBytes:(const void*)bytes length:(NSUInteger)length attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index { (void)bytes; (void)length; (void)stride; (void)index; unsupported(_cmd); }
- (void)setVisibleFunctionTable:(nullable id<MTLVisibleFunctionTable>)table atBufferIndex:(NSUInteger)index { (void)table; (void)index; unsupported(_cmd); }
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable>[])tables withBufferRange:(NSRange)range { (void)tables; (void)range; unsupported(_cmd); }
- (void)setIntersectionFunctionTable:(nullable id<MTLIntersectionFunctionTable>)table atBufferIndex:(NSUInteger)index { (void)table; (void)index; unsupported(_cmd); }
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable>[])tables withBufferRange:(NSRange)range { (void)tables; (void)range; unsupported(_cmd); }
- (void)setAccelerationStructure:(nullable id<MTLAccelerationStructure>)structure atBufferIndex:(NSUInteger)index { (void)structure; (void)index; unsupported(_cmd); }
- (void)setTexture:(nullable id<MTLTexture>)texture atIndex:(NSUInteger)index { (void)texture; (void)index; unsupported(_cmd); }
- (void)setTextures:(const id<MTLTexture>[])textures withRange:(NSRange)range { (void)textures; (void)range; unsupported(_cmd); }
- (void)setSamplerState:(nullable id<MTLSamplerState>)sampler atIndex:(NSUInteger)index { (void)sampler; (void)index; unsupported(_cmd); }
- (void)setSamplerStates:(const id<MTLSamplerState>[])samplers withRange:(NSRange)range { (void)samplers; (void)range; unsupported(_cmd); }
- (void)setSamplerState:(nullable id<MTLSamplerState>)sampler lodMinClamp:(float)minimum lodMaxClamp:(float)maximum atIndex:(NSUInteger)index { (void)sampler; (void)minimum; (void)maximum; (void)index; unsupported(_cmd); }
- (void)setSamplerStates:(const id<MTLSamplerState>[])samplers lodMinClamps:(const float[])minimum lodMaxClamps:(const float[])maximum withRange:(NSRange)range { (void)samplers; (void)minimum; (void)maximum; (void)range; unsupported(_cmd); }
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index { (void)length; (void)index; unsupported(_cmd); }
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height { (void)width; (void)height; unsupported(_cmd); }
- (void)setStageInRegion:(MTLRegion)region { (void)region; unsupported(_cmd); }
- (void)setStageInRegionWithIndirectBuffer:(id<MTLBuffer>)buffer indirectBufferOffset:(NSUInteger)offset { (void)buffer; (void)offset; unsupported(_cmd); }
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)buffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)local { (void)buffer; (void)offset; (void)local; unsupported(_cmd); }
- (void)updateFence:(id<MTLFence>)fence { (void)fence; unsupported(_cmd); }
- (void)waitForFence:(id<MTLFence>)fence { (void)fence; unsupported(_cmd); }
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage { (void)resource; (void)usage; unsupported(_cmd); }
- (void)useResources:(const id<MTLResource>[])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage { (void)resources; (void)count; (void)usage; unsupported(_cmd); }
- (void)useHeap:(id<MTLHeap>)heap { (void)heap; unsupported(_cmd); }
- (void)useHeaps:(const id<MTLHeap>[])heaps count:(NSUInteger)count { (void)heaps; (void)count; unsupported(_cmd); }
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range { (void)buffer; (void)range; unsupported(_cmd); }
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer indirectBuffer:(id<MTLBuffer>)indirect indirectBufferOffset:(NSUInteger)offset { (void)buffer; (void)indirect; (void)offset; unsupported(_cmd); }
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)buffer atSampleIndex:(NSUInteger)index withBarrier:(BOOL)barrier { (void)buffer; (void)index; (void)barrier; unsupported(_cmd); }
@end

@implementation HNMetalBlitEncoder {
    HNMetalCommandBuffer* _buffer;
    std::shared_ptr<QueueState> _q;
    std::shared_ptr<CommandState> _state;
    bool _ended;
    NSString* _label;
    NSMutableArray<NSString*>* _debug;
    NSMutableArray<NSString*>* _signposts;
}
- (instancetype)init { return nil; }
- (instancetype)initWithBuffer:(HNMetalCommandBuffer*)buffer {
    self = [super init];
    if (self) {
        _buffer = buffer; _state = [buffer commandState];
        _q = [(HNMetalCommandQueue*)buffer.commandQueue queueState];
        _debug = [NSMutableArray new]; _signposts = [NSMutableArray new];
        if (!_debug || !_signposts) return nil;
    }
    return self;
}
- (void)checkEncoding {
    mutableCommand(*_state);
    if (_ended || !_state->encoderActive) invalid(@"This blit encoder has ended.");
}
- (id<MTLDevice>)device { return _buffer.device; }
- (nullable NSString*)label { std::lock_guard<std::mutex> lock(_q->mutex); return _label; }
- (void)setLabel:(nullable NSString*)value {
    NSString* copy = labelCopy(value);
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; _label = copy;
}
- (void)endEncoding {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_debug.count) invalid(@"Close blit debug groups before ending the encoder.");
    _ended = true; _state->encoderActive = false;
}
- (void)synchronizeResource:(id<MTLResource>)resource {
    if ([resource isMemberOfClass:[HNMetalTexture class]]) {
        [self synchronizeTexture:(HNMetalTexture*)resource slice:0 level:0]; return;
    }
    if (![resource isMemberOfClass:[HNMetalBuffer class]] || resource.device != self.device ||
        resource.storageMode != MTLStorageModeManaged)
        invalid(@"synchronizeResource requires this device's actual Managed buffer.");
    BoundBuffer bound;
    bound.object = (HNMetalBuffer*)resource;
    bound.native = [bound.object nativeBuffer];
    if (!bound.native || !bound.native->length() || bound.object.length != bound.native->length())
        invalid(@"The Managed buffer has no matching native allocation owner.");
    // Construct the retained record before the lock. If vector allocation
    // fails, its last resource release runs only after unlocking the queue.
    RecordedCommand record;
    record.kind = CommandKind::SynchronizeManaged;
    record.synchronized = std::move(bound);
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_state->commands.size() >= kCommands) invalid(@"A command buffer supports at most 64 ordered compute/synchronization operations.");
    try {
        _state->commands.push_back(std::move(record));
    } catch (const std::bad_alloc&) { invalid(@"Unable to retain the Managed buffer synchronization."); }
}
- (void)insertDebugSignpost:(NSString*)string {
    NSString* copy = labelCopy(string); if (!copy) invalid(@"A debug signpost string is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_signposts.count >= 64) invalid(@"The encoder stores at most 64 debug signposts.");
    [_signposts addObject:copy];
}
- (void)pushDebugGroup:(NSString*)string {
    NSString* copy = labelCopy(string); if (!copy) invalid(@"A debug group name is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_debug.count >= 64) invalid(@"The debug group nesting limit is 64.");
    [_debug addObject:copy];
}
- (void)popDebugGroup {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!_debug.count) invalid(@"No blit debug group is open.");
    [_debug removeLastObject];
}
- (void)synchronizeTexture:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level API_AVAILABLE(macos(10.11), macCatalyst(13.0)) API_UNAVAILABLE(ios) {
    if (slice || level || ![texture isMemberOfClass:[HNMetalTexture class]] ||
        texture.device!=self.device || texture.storageMode!=MTLStorageModeManaged ||
        [(HNMetalTexture*)texture nativeContext]!=[(HNMetalCommandQueue*)_buffer.commandQueue context])
        invalid(@"Texture synchronization requires this device's exact Managed texture, level zero and slice zero.");
    RecordedCommand record; record.kind=CommandKind::SynchronizeTexture; record.texture=(HNMetalTexture*)texture;
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_state->commands.size()>=kCommands) invalid(@"A command buffer supports at most 64 ordered operations.");
    try { _state->commands.push_back(std::move(record)); }
    catch (const std::bad_alloc&) { invalid(@"Unable to retain the texture synchronization."); }
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toTexture:(id<MTLTexture>)destinationTexture destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel destinationOrigin:(MTLOrigin)destinationOrigin {
    (void)sourceTexture;
    (void)sourceSlice;
    (void)sourceLevel;
    (void)sourceOrigin;
    (void)sourceSize;
    (void)destinationTexture;
    (void)destinationSlice;
    (void)destinationLevel;
    (void)destinationOrigin;
    unsupported(_cmd);
}
- (void)copyFromBuffer:(id<MTLBuffer>)sourceBuffer sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)sourceBytesPerRow sourceBytesPerImage:(NSUInteger)sourceBytesPerImage sourceSize:(MTLSize)sourceSize toTexture:(id<MTLTexture>)destinationTexture destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel destinationOrigin:(MTLOrigin)destinationOrigin {
    (void)sourceBuffer;
    (void)sourceOffset;
    (void)sourceBytesPerRow;
    (void)sourceBytesPerImage;
    (void)sourceSize;
    (void)destinationTexture;
    (void)destinationSlice;
    (void)destinationLevel;
    (void)destinationOrigin;
    unsupported(_cmd);
}
- (void)copyFromBuffer:(id<MTLBuffer>)sourceBuffer sourceOffset:(NSUInteger)sourceOffset sourceBytesPerRow:(NSUInteger)sourceBytesPerRow sourceBytesPerImage:(NSUInteger)sourceBytesPerImage sourceSize:(MTLSize)sourceSize toTexture:(id<MTLTexture>)destinationTexture destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel destinationOrigin:(MTLOrigin)destinationOrigin options:(MTLBlitOption)options API_AVAILABLE(macos(10.11), ios(9.0)) {
    (void)sourceBuffer;
    (void)sourceOffset;
    (void)sourceBytesPerRow;
    (void)sourceBytesPerImage;
    (void)sourceSize;
    (void)destinationTexture;
    (void)destinationSlice;
    (void)destinationLevel;
    (void)destinationOrigin;
    (void)options;
    unsupported(_cmd);
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toBuffer:(id<MTLBuffer>)destinationBuffer destinationOffset:(NSUInteger)destinationOffset destinationBytesPerRow:(NSUInteger)destinationBytesPerRow destinationBytesPerImage:(NSUInteger)destinationBytesPerImage {
    (void)sourceTexture;
    (void)sourceSlice;
    (void)sourceLevel;
    (void)sourceOrigin;
    (void)sourceSize;
    (void)destinationBuffer;
    (void)destinationOffset;
    (void)destinationBytesPerRow;
    (void)destinationBytesPerImage;
    unsupported(_cmd);
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize toBuffer:(id<MTLBuffer>)destinationBuffer destinationOffset:(NSUInteger)destinationOffset destinationBytesPerRow:(NSUInteger)destinationBytesPerRow destinationBytesPerImage:(NSUInteger)destinationBytesPerImage options:(MTLBlitOption)options API_AVAILABLE(macos(10.11), ios(9.0)) {
    (void)sourceTexture;
    (void)sourceSlice;
    (void)sourceLevel;
    (void)sourceOrigin;
    (void)sourceSize;
    (void)destinationBuffer;
    (void)destinationOffset;
    (void)destinationBytesPerRow;
    (void)destinationBytesPerImage;
    (void)options;
    unsupported(_cmd);
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)texture {
    (void)texture;
    unsupported(_cmd);
}
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value {
    (void)buffer;
    (void)range;
    (void)value;
    unsupported(_cmd);
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
              toTexture:(id<MTLTexture>)destinationTexture destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
             sliceCount:(NSUInteger)sliceCount levelCount:(NSUInteger)levelCount API_AVAILABLE(macos(10.15), ios(13.0)) {
    (void)sourceTexture;
    (void)sourceSlice;
    (void)sourceLevel;
    (void)destinationTexture;
    (void)destinationSlice;
    (void)destinationLevel;
    (void)sliceCount;
    (void)levelCount;
    unsupported(_cmd);
}
- (void)copyFromTexture:(id<MTLTexture>)sourceTexture toTexture:(id<MTLTexture>)destinationTexture API_AVAILABLE(macos(10.15), ios(13.0)) {
    (void)sourceTexture;
    (void)destinationTexture;
    unsupported(_cmd);
}
- (void)copyFromBuffer:(id <MTLBuffer>)sourceBuffer sourceOffset:(NSUInteger)sourceOffset toBuffer:(id <MTLBuffer>)destinationBuffer destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size {
    (void)sourceBuffer;
    (void)sourceOffset;
    (void)destinationBuffer;
    (void)destinationOffset;
    (void)size;
    unsupported(_cmd);
}
- (void)updateFence:(id <MTLFence>)fence API_AVAILABLE(macos(10.13), ios(10.0)) {
    (void)fence;
    unsupported(_cmd);
}
- (void)waitForFence:(id <MTLFence>)fence API_AVAILABLE(macos(10.13), ios(10.0)) {
    (void)fence;
    unsupported(_cmd);
}
-(void) getTextureAccessCounters:(id<MTLTexture>)texture
                          region:(MTLRegion)region
                        mipLevel:(NSUInteger)mipLevel
                           slice:(NSUInteger)slice
                   resetCounters:(BOOL)resetCounters
                  countersBuffer:(id<MTLBuffer>)countersBuffer
            countersBufferOffset:(NSUInteger)countersBufferOffset API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(13.0), tvos(16.0)) {
    (void)texture;
    (void)region;
    (void)mipLevel;
    (void)slice;
    (void)resetCounters;
    (void)countersBuffer;
    (void)countersBufferOffset;
    unsupported(_cmd);
}
-(void) resetTextureAccessCounters:(id<MTLTexture>)texture
                            region:(MTLRegion)region
                          mipLevel:(NSUInteger)mipLevel
                             slice:(NSUInteger)slice API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(13.0), tvos(16.0)) {
    (void)texture;
    (void)region;
    (void)mipLevel;
    (void)slice;
    unsupported(_cmd);
}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)texture;
    unsupported(_cmd);
}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)texture;
    (void)slice;
    (void)level;
    unsupported(_cmd);
}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)texture API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)texture;
    unsupported(_cmd);
}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)texture;
    (void)slice;
    (void)level;
    unsupported(_cmd);
}
- (void) resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)buffer;
    (void)range;
    unsupported(_cmd);
}
- (void)copyIndirectCommandBuffer:(id <MTLIndirectCommandBuffer>)source sourceRange:(NSRange)sourceRange
                      destination:(id <MTLIndirectCommandBuffer>)destination destinationIndex:(NSUInteger)destinationIndex API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)source;
    (void)sourceRange;
    (void)destination;
    (void)destinationIndex;
    unsupported(_cmd);
}
- (void)optimizeIndirectCommandBuffer:(id <MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)range API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)indirectCommandBuffer;
    (void)range;
    unsupported(_cmd);
}
-(void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                atSampleIndex:(NSUInteger)sampleIndex
                  withBarrier:(BOOL)barrier API_AVAILABLE(macos(10.15), ios(14.0)) {
    (void)sampleBuffer;
    (void)sampleIndex;
    (void)barrier;
    unsupported(_cmd);
}
-(void)resolveCounters:(id<MTLCounterSampleBuffer>)sampleBuffer
               inRange:(NSRange)range
     destinationBuffer:(id<MTLBuffer>)destinationBuffer
     destinationOffset:(NSUInteger)destinationOffset API_AVAILABLE(macos(10.15), ios(14.0)) {
    (void)sampleBuffer;
    (void)range;
    (void)destinationBuffer;
    (void)destinationOffset;
    unsupported(_cmd);
}
- (void)copyFromTensor:(id<MTLTensor>)sourceTensor
          sourceOrigin:(MTLTensorExtents *)sourceOrigin
      sourceDimensions:(MTLTensorExtents *)sourceDimensions
              toTensor:(id<MTLTensor>)destinationTensor
     destinationOrigin:(MTLTensorExtents *)destinationOrigin
 destinationDimensions:(MTLTensorExtents *)destinationDimensions API_AVAILABLE(macos(26.0), ios(26.0)) {
    (void)sourceTensor;
    (void)sourceOrigin;
    (void)sourceDimensions;
    (void)destinationTensor;
    (void)destinationOrigin;
    (void)destinationDimensions;
    unsupported(_cmd);
}
- (void)barrierAfterQueueStages:(MTLStages)after beforeStages:(MTLStages)before API_AVAILABLE(macos(26.0)) {
    (void)after; (void)before; unsupported(_cmd);
}
@end
#include "HNMetalRenderEncoder.inc"
