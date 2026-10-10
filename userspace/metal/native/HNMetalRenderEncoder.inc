// SPDX-License-Identifier: MIT
// Included by HNMetalCommand.mm so render commands share its real FIFO and
// completion lifecycle. No alternate queue or success-only backend exists.
NS_ASSUME_NONNULL_BEGIN
@implementation HNMetalRenderEncoder {
    HNMetalCommandBuffer* _buffer;
    HNMetalTexture* _target;
    HNMetalRenderPipeline* _pipeline;
    BoundBuffer _fragment;
    BoundBuffer _vertex;
    std::shared_ptr<QueueState> _q;
    std::shared_ptr<CommandState> _state;
    bool _ended;
    NSString* _label;
    NSMutableArray<NSString*>* _debug;
    NSMutableArray<NSString*>* _signposts;
}
- (instancetype)init { return nil; }
- (instancetype)initWithBuffer:(HNMetalCommandBuffer*)buffer target:(HNMetalTexture*)target {
    self=[super init];
    if (self) {
        _buffer=buffer; _target=target; _state=[buffer commandState];
        _q=[(HNMetalCommandQueue*)buffer.commandQueue queueState];
        _debug=[NSMutableArray new]; _signposts=[NSMutableArray new];
        if (!_debug || !_signposts) return nil;
    }
    return self;
}
- (void)checkEncoding {
    mutableCommand(*_state);
    if (_ended || !_state->encoderActive) invalid(@"This render encoder has ended.");
}
- (id<MTLDevice>)device { return _buffer.device; }
- (nullable NSString*)label { std::lock_guard<std::mutex> lock(_q->mutex); return _label; }
- (void)setLabel:(nullable NSString*)value {
    NSString* copy=labelCopy(value); std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; _label=copy;
}
- (void)endEncoding {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_debug.count) invalid(@"Close render debug groups before ending the encoder.");
    _ended=true; _state->encoderActive=false;
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline {
    if (![pipeline isMemberOfClass:[HNMetalRenderPipeline class]] || pipeline.device!=self.device ||
        [(HNMetalRenderPipeline*)pipeline nativeContext]!=[_target nativeContext])
        invalid(@"Expected this device's actual graphics pipeline and exact texture owner.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; _pipeline=(HNMetalRenderPipeline*)pipeline;
}
- (void)setFragmentBuffer:(nullable id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index) invalid(@"The fragment profile implements only buffer index zero.");
    BoundBuffer replacement;
    if (buffer) {
        if (![buffer isMemberOfClass:[HNMetalBuffer class]]) invalid(@"Expected an actual HackyNavi buffer.");
        replacement.object=(HNMetalBuffer*)buffer;
        replacement.native=[replacement.object nativeBuffer]; replacement.offset=offset;
        if (!validFragmentBuffer(replacement,self.device,[_target nativeContext]))
            invalid(@"Expected the exact texture owner's buffer with an aligned, logical 16-byte range.");
    } else if (offset) invalid(@"Clearing buffer zero requires a zero offset.");
    // Drop a replaced BO only after releasing the queue mutex; final native
    // release may block. Recorded draws retain their own independent binding.
    { std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; std::swap(_fragment,replacement); }
}
- (void)setVertexBuffer:(nullable id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index) invalid(@"The vertex profile implements only buffer index zero.");
    BoundBuffer replacement;
    if (buffer) {
        if (![buffer isMemberOfClass:[HNMetalBuffer class]]) invalid(@"Expected an actual HackyNavi buffer.");
        replacement.object=(HNMetalBuffer*)buffer;
        replacement.native=[replacement.object nativeBuffer]; replacement.offset=offset;
        if (!validVertexBuffer(replacement,self.device,[_target nativeContext]))
            invalid(@"Expected the exact texture owner's buffer with an aligned, logical 48-byte range.");
    } else if (offset) invalid(@"Clearing buffer zero requires a zero offset.");
    { std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding]; std::swap(_vertex,replacement); }
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(10.11), ios(8.3)) {
    if (index || (offset & 15)) invalid(@"Expected vertex buffer index zero and a 16-byte-aligned offset.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!validVertexBuffer(_vertex,self.device,[_target nativeContext]) ||
        offset>_vertex.native->length() || _vertex.native->length()-offset<48)
        invalid(@"The bound vertex buffer does not contain the complete 48-byte range.");
    _vertex.offset=offset;
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(10.11), ios(8.3)) {
    if (index || (offset & 15)) invalid(@"Expected fragment buffer index zero and a 16-byte-aligned offset.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!validFragmentBuffer(_fragment,self.device,[_target nativeContext]) ||
        offset > _fragment.native->length() || _fragment.native->length()-offset < 16)
        invalid(@"The bound fragment buffer does not contain the complete 16-byte range.");
    _fragment.offset=offset;
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)base {
    RecordedCommand record;
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!_pipeline || type!=MTLPrimitiveTypeTriangle || start || count!=3 || instances!=1 || base)
        invalid(@"This graphics profile requires a pipeline and one triangle of three vertices starting at zero.");
    if (_state->commands.size()>=kCommands) invalid(@"A command buffer supports at most 64 ordered operations.");
    record.kind=CommandKind::Draw; record.renderPipeline=_pipeline; record.texture=_target;
    if ([_pipeline nativeBufferPipeline] || [_pipeline nativeVertexFragmentBufferPipeline]) {
        if (!validFragmentBuffer(_fragment,self.device,[_target nativeContext]))
            invalid(@"The actual fragment pipeline requires a bound readonly buffer0/16-byte range.");
        record.fragment=_fragment;
    }
    if ([_pipeline nativeVertexBufferPipeline] || [_pipeline nativeVertexFragmentBufferPipeline]) {
        if (!validVertexBuffer(_vertex,self.device,[_target nativeContext]))
            invalid(@"The actual vertex pipeline requires a bound readonly buffer0/48-byte range.");
        record.vertex=_vertex;
    }
    if ([_pipeline nativeVertexFragmentBufferPipeline] && record.vertex.native==record.fragment.native)
        invalid(@"Vertex and fragment resources require distinct native allocations.");
    try { _state->commands.push_back(std::move(record)); }
    catch (const std::bad_alloc&) { invalid(@"Unable to retain the actual draw resources."); }
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances {
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
}
- (void)setViewport:(MTLViewport)v {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (v.originX!=0 || v.originY!=0 || v.width!=double(_target.width) || v.height!=double(_target.height) || v.znear!=0 || v.zfar!=1)
        invalid(@"Only the full target viewport with depth range zero to one is implemented.");
}
- (void)setViewports:(const MTLViewport[_Nonnull])values count:(NSUInteger)count {
    if (count!=1 || !span(values,sizeof(*values))) invalid(@"Exactly one full target viewport is supported.");
    [self setViewport:values[0]];
}
- (void)setScissorRect:(MTLScissorRect)r {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (r.x || r.y || r.width!=_target.width || r.height!=_target.height)
        invalid(@"Only the full target scissor is implemented.");
}
- (void)setScissorRects:(const MTLScissorRect[_Nonnull])values count:(NSUInteger)count {
    if (count!=1 || !span(values,sizeof(*values))) invalid(@"Exactly one full target scissor is supported.");
    [self setScissorRect:values[0]];
}
- (void)setFrontFacingWinding:(MTLWinding)value {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (value!=MTLWindingClockwise) unsupported(_cmd);
}
- (void)setCullMode:(MTLCullMode)value {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (value!=MTLCullModeNone) unsupported(_cmd);
}
- (void)setDepthClipMode:(MTLDepthClipMode)value {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (value!=MTLDepthClipModeClip) unsupported(_cmd);
}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (bias!=0 || slope!=0 || clamp!=0) unsupported(_cmd);
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)value {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (value!=MTLTriangleFillModeFill) unsupported(_cmd);
}
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (red!=0 || green!=0 || blue!=0 || alpha!=0) unsupported(_cmd);
}
- (void)setDepthStencilState:(nullable id<MTLDepthStencilState>)state {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (state) unsupported(_cmd);
}
- (void)setStencilReferenceValue:(uint32_t)value {
    [self setStencilFrontReferenceValue:value backReferenceValue:value];
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (front || back) unsupported(_cmd);
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (mode!=MTLVisibilityResultModeDisabled || offset) unsupported(_cmd);
}
- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (index || action!=MTLStoreActionStore) unsupported(_cmd);
}
- (void)setColorStoreActionOptions:(MTLStoreActionOptions)options atIndex:(NSUInteger)index {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (index || options!=MTLStoreActionOptionNone) unsupported(_cmd);
}
- (void)insertDebugSignpost:(NSString*)string {
    NSString* copy=labelCopy(string); if (!copy) invalid(@"A debug signpost is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_signposts.count>=64) invalid(@"At most 64 signposts can be retained."); [_signposts addObject:copy];
}
- (void)pushDebugGroup:(NSString*)string {
    NSString* copy=labelCopy(string); if (!copy) invalid(@"A debug group is required.");
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (_debug.count>=64) invalid(@"The debug group nesting limit is 64."); [_debug addObject:copy];
}
- (void)popDebugGroup {
    std::lock_guard<std::mutex> lock(_q->mutex); [self checkEncoding];
    if (!_debug.count) invalid(@"No render debug group is open."); [_debug removeLastObject];
}
- (void)barrierAfterQueueStages:(MTLStages)after beforeStages:(MTLStages)before API_AVAILABLE(macos(26.0)) {
    (void)after; (void)before; unsupported(_cmd);
}
NS_ASSUME_NONNULL_END
#include "HNMetalRenderUnsupported.inc"
@end
