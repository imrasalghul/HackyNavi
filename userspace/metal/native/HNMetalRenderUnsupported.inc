// SPDX-License-Identifier: MIT
// Exact typed unsupported MTLRenderCommandEncoder methods from the captured
// macOS 26.2 SDK header. All availability annotations are retained.
// Header SHA-256: 8fbac9b5ab95dcb000a189d165176ac027284c6686633e711098ee45b8d930db
// Supported selectors and inherited MTLCommandEncoder methods live in
// HNMetalCommand.mm. Every method below raises via its noreturn helper.
// Match the captured protocol's enclosing nonnull-default region.
NS_ASSUME_NONNULL_BEGIN

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index API_AVAILABLE(macos(10.11), ios(8.3)) {
    (void)bytes;
    (void)length;
    (void)index;
    unsupported(_cmd);
}

- (void)setVertexBuffers:(const id <MTLBuffer> __nullable [__nonnull])buffers offsets:(const NSUInteger [__nonnull])offsets withRange:(NSRange)range {
    (void)buffers;
    (void)offsets;
    (void)range;
    unsupported(_cmd);
}

- (void) setVertexBuffer:(nullable id<MTLBuffer>)buffer offset:(NSUInteger)offset attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index API_AVAILABLE(macos(14.0), ios(17.0)) {
    (void)buffer;
    (void)offset;
    (void)stride;
    (void)index;
    unsupported(_cmd);
}

- (void) setVertexBuffers:(id<MTLBuffer> const __nullable [__nonnull])buffers offsets:(NSUInteger const [__nonnull])offsets attributeStrides:(NSUInteger const [__nonnull])strides withRange:(NSRange)range API_AVAILABLE(macos(14.0), ios(17.0)) {
    (void)buffers;
    (void)offsets;
    (void)strides;
    (void)range;
    unsupported(_cmd);
}

- (void) setVertexBufferOffset:(NSUInteger)offset attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index API_AVAILABLE(macos(14.0), ios(17.0)) {
    (void)offset;
    (void)stride;
    (void)index;
    unsupported(_cmd);
}

- (void) setVertexBytes:(void const *)bytes length:(NSUInteger)length attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index API_AVAILABLE(macos(14.0), ios(17.0)) {
    (void)bytes;
    (void)length;
    (void)stride;
    (void)index;
    unsupported(_cmd);
}

- (void)setVertexTexture:(nullable id <MTLTexture>)texture atIndex:(NSUInteger)index {
    (void)texture;
    (void)index;
    unsupported(_cmd);
}

- (void)setVertexTextures:(const id <MTLTexture> __nullable [__nonnull])textures withRange:(NSRange)range {
    (void)textures;
    (void)range;
    unsupported(_cmd);
}

- (void)setVertexSamplerState:(nullable id <MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    (void)sampler;
    (void)index;
    unsupported(_cmd);
}

- (void)setVertexSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers withRange:(NSRange)range {
    (void)samplers;
    (void)range;
    unsupported(_cmd);
}

- (void)setVertexSamplerState:(nullable id <MTLSamplerState>)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index {
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    unsupported(_cmd);
}

- (void)setVertexSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers lodMinClamps:(const float [__nonnull])lodMinClamps lodMaxClamps:(const float [__nonnull])lodMaxClamps withRange:(NSRange)range {
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    unsupported(_cmd);
}

- (void)setVertexVisibleFunctionTable:(nullable id <MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)functionTable;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setVertexVisibleFunctionTables:(const id <MTLVisibleFunctionTable> __nullable [__nonnull])functionTables withBufferRange:(NSRange)range API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)functionTables;
    (void)range;
    unsupported(_cmd);
}

- (void)setVertexIntersectionFunctionTable:(nullable id <MTLIntersectionFunctionTable>)intersectionFunctionTable atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)intersectionFunctionTable;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setVertexIntersectionFunctionTables:(const id <MTLIntersectionFunctionTable> __nullable [__nonnull])intersectionFunctionTables withBufferRange:(NSRange)range API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)intersectionFunctionTables;
    (void)range;
    unsupported(_cmd);
}

- (void)setVertexAccelerationStructure:(nullable id <MTLAccelerationStructure>)accelerationStructure atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)accelerationStructure;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(nullable const MTLVertexAmplificationViewMapping *)viewMappings API_AVAILABLE(macos(10.15.4), ios(13.0), macCatalyst(13.4), tvos(16.0)) {
    (void)count;
    (void)viewMappings;
    unsupported(_cmd);
}

- (void)setDepthTestMinBound:(float)minBound maxBound:(float)maxBound API_AVAILABLE(macos(26.0), ios(26.0)) {
    (void)minBound;
    (void)maxBound;
    unsupported(_cmd);
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index API_AVAILABLE(macos(10.11), ios(8.3)) {
    (void)bytes;
    (void)length;
    (void)index;
    unsupported(_cmd);
}

- (void)setFragmentBuffers:(const id <MTLBuffer> __nullable [__nonnull])buffers offsets:(const NSUInteger [__nonnull])offsets withRange:(NSRange)range {
    (void)buffers;
    (void)offsets;
    (void)range;
    unsupported(_cmd);
}

- (void)setFragmentTexture:(nullable id <MTLTexture>)texture atIndex:(NSUInteger)index {
    (void)texture;
    (void)index;
    unsupported(_cmd);
}

- (void)setFragmentTextures:(const id <MTLTexture> __nullable [__nonnull])textures withRange:(NSRange)range {
    (void)textures;
    (void)range;
    unsupported(_cmd);
}

- (void)setFragmentSamplerState:(nullable id <MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    (void)sampler;
    (void)index;
    unsupported(_cmd);
}

- (void)setFragmentSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers withRange:(NSRange)range {
    (void)samplers;
    (void)range;
    unsupported(_cmd);
}

- (void)setFragmentSamplerState:(nullable id <MTLSamplerState>)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index {
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    unsupported(_cmd);
}

- (void)setFragmentSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers lodMinClamps:(const float [__nonnull])lodMinClamps lodMaxClamps:(const float [__nonnull])lodMaxClamps withRange:(NSRange)range {
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    unsupported(_cmd);
}

- (void)setFragmentVisibleFunctionTable:(nullable id <MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)functionTable;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setFragmentVisibleFunctionTables:(const id <MTLVisibleFunctionTable> __nullable [__nonnull])functionTables withBufferRange:(NSRange)range API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)functionTables;
    (void)range;
    unsupported(_cmd);
}

- (void)setFragmentIntersectionFunctionTable:(nullable id <MTLIntersectionFunctionTable>)intersectionFunctionTable atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)intersectionFunctionTable;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setFragmentIntersectionFunctionTables:(const id <MTLIntersectionFunctionTable> __nullable [__nonnull])intersectionFunctionTables withBufferRange:(NSRange)range API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)intersectionFunctionTables;
    (void)range;
    unsupported(_cmd);
}

- (void)setFragmentAccelerationStructure:(nullable id <MTLAccelerationStructure>)accelerationStructure atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)accelerationStructure;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setDepthStoreAction:(MTLStoreAction)storeAction API_AVAILABLE(macos(10.12), ios(10.0)) {
    (void)storeAction;
    unsupported(_cmd);
}

- (void)setStencilStoreAction:(MTLStoreAction)storeAction API_AVAILABLE(macos(10.12), ios(10.0)) {
    (void)storeAction;
    unsupported(_cmd);
}

- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)storeActionOptions API_AVAILABLE(macos(10.13), ios(11.0)) {
    (void)storeActionOptions;
    unsupported(_cmd);
}

- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)storeActionOptions API_AVAILABLE(macos(10.13), ios(11.0)) {
    (void)storeActionOptions;
    unsupported(_cmd);
}

- (void)setObjectBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)bytes;
    (void)length;
    (void)index;
    unsupported(_cmd);
}

- (void)setObjectBuffer:(nullable id <MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)buffer;
    (void)offset;
    (void)index;
    unsupported(_cmd);
}

- (void)setObjectBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)offset;
    (void)index;
    unsupported(_cmd);
}

- (void)setObjectBuffers:(const id <MTLBuffer> __nullable [__nonnull])buffers offsets:(const NSUInteger [__nonnull])offsets withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)buffers;
    (void)offsets;
    (void)range;
    unsupported(_cmd);
}

- (void)setObjectTexture:(nullable id <MTLTexture>)texture atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)texture;
    (void)index;
    unsupported(_cmd);
}

- (void)setObjectTextures:(const id <MTLTexture> __nullable [__nonnull])textures withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)textures;
    (void)range;
    unsupported(_cmd);
}

- (void)setObjectSamplerState:(nullable id <MTLSamplerState>)sampler atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)sampler;
    (void)index;
    unsupported(_cmd);
}

- (void)setObjectSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)samplers;
    (void)range;
    unsupported(_cmd);
}

- (void)setObjectSamplerState:(nullable id <MTLSamplerState>)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    unsupported(_cmd);
}

- (void)setObjectSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers lodMinClamps:(const float [__nonnull])lodMinClamps lodMaxClamps:(const float [__nonnull])lodMaxClamps withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    unsupported(_cmd);
}

- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)length;
    (void)index;
    unsupported(_cmd);
}

- (void)setMeshBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)bytes;
    (void)length;
    (void)index;
    unsupported(_cmd);
}

- (void)setMeshBuffer:(nullable id <MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)buffer;
    (void)offset;
    (void)index;
    unsupported(_cmd);
}

- (void)setMeshBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)offset;
    (void)index;
    unsupported(_cmd);
}

- (void)setMeshBuffers:(const id <MTLBuffer> __nullable [__nonnull])buffers offsets:(const NSUInteger [__nonnull])offsets withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)buffers;
    (void)offsets;
    (void)range;
    unsupported(_cmd);
}

- (void)setMeshTexture:(nullable id <MTLTexture>)texture atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)texture;
    (void)index;
    unsupported(_cmd);
}

- (void)setMeshTextures:(const id <MTLTexture> __nullable [__nonnull])textures withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)textures;
    (void)range;
    unsupported(_cmd);
}

- (void)setMeshSamplerState:(nullable id <MTLSamplerState>)sampler atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)sampler;
    (void)index;
    unsupported(_cmd);
}

- (void)setMeshSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)samplers;
    (void)range;
    unsupported(_cmd);
}

- (void)setMeshSamplerState:(nullable id <MTLSamplerState>)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    unsupported(_cmd);
}

- (void)setMeshSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers lodMinClamps:(const float [__nonnull])lodMinClamps lodMaxClamps:(const float [__nonnull])lodMaxClamps withRange:(NSRange)range API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    unsupported(_cmd);
}

- (void)drawMeshThreadgroups:(MTLSize)threadgroupsPerGrid threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)threadgroupsPerGrid;
    (void)threadsPerObjectThreadgroup;
    (void)threadsPerMeshThreadgroup;
    unsupported(_cmd);
}

- (void) drawMeshThreads:(MTLSize)threadsPerGrid threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)threadsPerGrid;
    (void)threadsPerObjectThreadgroup;
    (void)threadsPerMeshThreadgroup;
    unsupported(_cmd);
}

- (void)drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup API_AVAILABLE(macos(13.0), ios(16.0)) {
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    (void)threadsPerObjectThreadgroup;
    (void)threadsPerMeshThreadgroup;
    unsupported(_cmd);
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType indexBuffer:(id <MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount {
    (void)primitiveType;
    (void)indexCount;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    (void)instanceCount;
    unsupported(_cmd);
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType indexBuffer:(id <MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset {
    (void)primitiveType;
    (void)indexCount;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    unsupported(_cmd);
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount indexType:(MTLIndexType)indexType indexBuffer:(id <MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance API_AVAILABLE(macos(10.11), ios(9.0)) {
    (void)primitiveType;
    (void)indexCount;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    (void)instanceCount;
    (void)baseVertex;
    (void)baseInstance;
    unsupported(_cmd);
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType indirectBuffer:(id <MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset API_AVAILABLE(macos(10.11), ios(9.0)) {
    (void)primitiveType;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    unsupported(_cmd);
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexType:(MTLIndexType)indexType indexBuffer:(id <MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)indexBufferOffset indirectBuffer:(id <MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset API_AVAILABLE(macos(10.11), ios(9.0)) {
    (void)primitiveType;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    unsupported(_cmd);
}

- (void)textureBarrier API_DEPRECATED_WITH_REPLACEMENT("memoryBarrierWithScope:MTLBarrierScopeRenderTargets", macos(10.11, 10.14)) API_UNAVAILABLE(ios) {
    unsupported(_cmd);
}

- (void)updateFence:(id <MTLFence>)fence afterStages:(MTLRenderStages)stages API_AVAILABLE(macos(10.13), ios(10.0)) {
    (void)fence;
    (void)stages;
    unsupported(_cmd);
}

- (void)waitForFence:(id <MTLFence>)fence beforeStages:(MTLRenderStages)stages API_AVAILABLE(macos(10.13), ios(10.0)) {
    (void)fence;
    (void)stages;
    unsupported(_cmd);
}

-(void)setTessellationFactorBuffer:(nullable id <MTLBuffer>)buffer offset:(NSUInteger)offset instanceStride:(NSUInteger)instanceStride API_AVAILABLE(macos(10.12), ios(10.0)) {
    (void)buffer;
    (void)offset;
    (void)instanceStride;
    unsupported(_cmd);
}

-(void)setTessellationFactorScale:(float)scale API_AVAILABLE(macos(10.12), ios(10.0)) {
    (void)scale;
    unsupported(_cmd);
}

-(void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(nullable id <MTLBuffer>)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance API_AVAILABLE(macos(10.12), ios(10.0)) {
    (void)numberOfPatchControlPoints;
    (void)patchStart;
    (void)patchCount;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)instanceCount;
    (void)baseInstance;
    unsupported(_cmd);
}

-(void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(nullable id <MTLBuffer>)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset indirectBuffer:(id <MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset API_AVAILABLE(macos(10.12), ios(12.0), tvos(14.5)) {
    (void)numberOfPatchControlPoints;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    unsupported(_cmd);
}

-(void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(nullable id <MTLBuffer>)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id <MTLBuffer>)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance API_AVAILABLE(macos(10.12), ios(10.0)) {
    (void)numberOfPatchControlPoints;
    (void)patchStart;
    (void)patchCount;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)controlPointIndexBuffer;
    (void)controlPointIndexBufferOffset;
    (void)instanceCount;
    (void)baseInstance;
    unsupported(_cmd);
}

-(void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(nullable id <MTLBuffer>)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id <MTLBuffer>)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset indirectBuffer:(id <MTLBuffer>)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset API_AVAILABLE(macos(10.12), ios(12.0), tvos(14.5)) {
    (void)numberOfPatchControlPoints;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)controlPointIndexBuffer;
    (void)controlPointIndexBufferOffset;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    unsupported(_cmd);
}

- (void)setTileBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)bytes;
    (void)length;
    (void)index;
    unsupported(_cmd);
}

- (void)setTileBuffer:(nullable id <MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)buffer;
    (void)offset;
    (void)index;
    unsupported(_cmd);
}

- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)offset;
    (void)index;
    unsupported(_cmd);
}

- (void)setTileBuffers:(const id <MTLBuffer> __nullable [__nonnull])buffers offsets:(const NSUInteger [__nonnull])offsets withRange:(NSRange)range API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)buffers;
    (void)offsets;
    (void)range;
    unsupported(_cmd);
}

- (void)setTileTexture:(nullable id <MTLTexture>)texture atIndex:(NSUInteger)index API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)texture;
    (void)index;
    unsupported(_cmd);
}

- (void)setTileTextures:(const id <MTLTexture> __nullable [__nonnull])textures withRange:(NSRange)range API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)textures;
    (void)range;
    unsupported(_cmd);
}

- (void)setTileSamplerState:(nullable id <MTLSamplerState>)sampler atIndex:(NSUInteger)index API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)sampler;
    (void)index;
    unsupported(_cmd);
}

- (void)setTileSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers withRange:(NSRange)range API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)samplers;
    (void)range;
    unsupported(_cmd);
}

- (void)setTileSamplerState:(nullable id <MTLSamplerState>)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    unsupported(_cmd);
}

- (void)setTileSamplerStates:(const id <MTLSamplerState> __nullable [__nonnull])samplers lodMinClamps:(const float [__nonnull])lodMinClamps lodMaxClamps:(const float [__nonnull])lodMaxClamps withRange:(NSRange)range API_AVAILABLE(ios(11.0), tvos(14.5), macos(11.0), macCatalyst(14.0)) {
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    unsupported(_cmd);
}

- (void)setTileVisibleFunctionTable:(nullable id <MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)functionTable;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setTileVisibleFunctionTables:(const id <MTLVisibleFunctionTable> __nullable [__nonnull])functionTables withBufferRange:(NSRange)range API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)functionTables;
    (void)range;
    unsupported(_cmd);
}

- (void)setTileIntersectionFunctionTable:(nullable id <MTLIntersectionFunctionTable>)intersectionFunctionTable atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)intersectionFunctionTable;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)setTileIntersectionFunctionTables:(const id <MTLIntersectionFunctionTable> __nullable [__nonnull])intersectionFunctionTables withBufferRange:(NSRange)range API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)intersectionFunctionTables;
    (void)range;
    unsupported(_cmd);
}

- (void)setTileAccelerationStructure:(nullable id <MTLAccelerationStructure>)accelerationStructure atBufferIndex:(NSUInteger)bufferIndex API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) {
    (void)accelerationStructure;
    (void)bufferIndex;
    unsupported(_cmd);
}

- (void)dispatchThreadsPerTile:(MTLSize)threadsPerTile API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)threadsPerTile;
    unsupported(_cmd);
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length offset:(NSUInteger)offset atIndex:(NSUInteger)index API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    (void)length;
    (void)offset;
    (void)index;
    unsupported(_cmd);
}

- (void)useResource:(id <MTLResource>)resource usage:(MTLResourceUsage)usage API_DEPRECATED_WITH_REPLACEMENT("Use useResource:usage:stages: instead", macos(10.13, 13.0), ios(11.0, 16.0)) {
    (void)resource;
    (void)usage;
    unsupported(_cmd);
}

- (void)useResources:(const id <MTLResource> __nonnull[__nonnull])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage API_DEPRECATED_WITH_REPLACEMENT("Use useResources:count:usage:stages: instead", macos(10.13, 13.0), ios(11.0, 16.0)) {
    (void)resources;
    (void)count;
    (void)usage;
    unsupported(_cmd);
}

- (void)useResource:(id <MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages) stages API_AVAILABLE(macos(10.15), ios(13.0)) {
    (void)resource;
    (void)usage;
    (void)stages;
    unsupported(_cmd);
}

- (void)useResources:(const id <MTLResource> __nonnull[__nonnull])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages API_AVAILABLE(macos(10.15), ios(13.0)) {
    (void)resources;
    (void)count;
    (void)usage;
    (void)stages;
    unsupported(_cmd);
}

- (void)useHeap:(id <MTLHeap>)heap API_DEPRECATED_WITH_REPLACEMENT("Use useHeap:stages: instead", macos(10.13, 13.0), ios(11.0, 16.0)) {
    (void)heap;
    unsupported(_cmd);
}

- (void)useHeaps:(const id <MTLHeap> __nonnull[__nonnull])heaps count:(NSUInteger)count API_DEPRECATED_WITH_REPLACEMENT("Use useHeaps:count:stages: instead", macos(10.13, 13.0), ios(11.0, 16.0)) {
    (void)heaps;
    (void)count;
    unsupported(_cmd);
}

- (void)useHeap:(id <MTLHeap>)heap stages:(MTLRenderStages)stages API_AVAILABLE(macos(10.15), ios(13.0)) {
    (void)heap;
    (void)stages;
    unsupported(_cmd);
}

- (void)useHeaps:(const id <MTLHeap> __nonnull[__nonnull])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages API_AVAILABLE(macos(10.15), ios(13.0)) {
    (void)heaps;
    (void)count;
    (void)stages;
    unsupported(_cmd);
}

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange API_AVAILABLE(macos(10.14), ios(12.0)) {
    (void)indirectCommandBuffer;
    (void)executionRange;
    unsupported(_cmd);
}

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandbuffer indirectBuffer:(id<MTLBuffer>)indirectRangeBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset API_AVAILABLE(macos(10.14), macCatalyst(13.0), ios(13.0)) {
    (void)indirectCommandbuffer;
    (void)indirectRangeBuffer;
    (void)indirectBufferOffset;
    unsupported(_cmd);
}

-(void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before API_AVAILABLE(macos(10.14), macCatalyst(13.0), ios(16.0)) {
    (void)scope;
    (void)after;
    (void)before;
    unsupported(_cmd);
}

-(void)memoryBarrierWithResources:(const id<MTLResource> __nonnull[__nonnull])resources count:(NSUInteger)count afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before API_AVAILABLE(macos(10.14), macCatalyst(13.0), ios(16.0)) {
    (void)resources;
    (void)count;
    (void)after;
    (void)before;
    unsupported(_cmd);
}

-(void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer atSampleIndex:(NSUInteger)sampleIndex withBarrier:(BOOL)barrier API_AVAILABLE(macos(10.15), ios(14.0)) {
    (void)sampleBuffer;
    (void)sampleIndex;
    (void)barrier;
    unsupported(_cmd);
}

- (void)setColorAttachmentMap:(nullable MTLLogicalToPhysicalColorAttachmentMap *)mapping API_AVAILABLE(macos(26.0), ios(26.0)) {
    (void)mapping;
    unsupported(_cmd);
}

- (NSUInteger)tileWidth API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    unsupported(_cmd);
}

- (NSUInteger)tileHeight API_AVAILABLE(macos(11.0), macCatalyst(14.0), ios(11.0), tvos(14.5)) {
    unsupported(_cmd);
}
NS_ASSUME_NONNULL_END
