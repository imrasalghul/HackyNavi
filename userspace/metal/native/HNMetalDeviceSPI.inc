// SPDX-License-Identifier: MIT
// Exact required macOS 15.8 SPI declarations from spi-decisions.json.
// Included only inside HNMetalDevice implementation. Unsupported contracts
// raise explicit exceptions before using arguments or taking ownership.

- (id)vendorName {
    return @"AMD";
}
- (id)GPUBVHBuilder {
    unsupported(@"GPUBVHBuilder is not implemented by the native compute profile.");
}
- (BOOL)isBCTextureCompressionSupported { return NO; }
- (NSUInteger)maxTileSamplers { return [self limits]->maxTileSamplers; }
- (NSUInteger)minConstantBufferAlignmentBytes { return [self limits]->minConstantBufferAlignmentBytes; }
- (id)newDynamicLibraryFromURL:(id)argument0 error:(id __autoreleasing*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newDynamicLibraryFromURL:error: is not implemented by the native compute profile.");
}
- (BOOL)supportsBGR10A2 { return NO; }
- (BOOL)supportsInvariantVertexPosition { return NO; }
- (BOOL)supportsMemorylessRenderTargets { return NO; }
- (BOOL)supportsStreamingCodecSignaling { return NO; }
- (BOOL)supportsViewportAndScissorArray { return NO; }
- (uint32_t)acceleratorPort {
    return _accelerator;
}
- (NSInteger)defaultTextureWriteRoundingMode {
    unsupported(@"defaultTextureWriteRoundingMode is not implemented by the native compute profile.");
}
- (NSUInteger)maxInterpolants { return [self limits]->maxInterpolants; }
- (NSUInteger)maxTileTextures { return [self limits]->maxTileTextures; }
- (BOOL)supportsBufferWithAddressRanges { return NO; }
- (BOOL)supportsIndirectDrawAndDispatch { return NO; }
- (BOOL)supportsLargeFramebufferConfigs { return NO; }
- (BOOL)supportsTextureOutOfBoundsReads { return NO; }
- (BOOL)supportsWritableArrayOfTextures { return NO; }
- (void)_removeRequested {
    unsupported(@"_removeRequested is not implemented by the native compute profile.");
}
- (void)_setDeviceWrapper:(id)argument0 {
    (void)argument0;
    unsupported(@"_setDeviceWrapper: is not implemented by the native compute profile.");
}
- (void)allowLibrariesFromOtherPlatforms {
    unsupported(@"allowLibrariesFromOtherPlatforms is not implemented by the native compute profile.");
}
- (BOOL)areGPUAssertionsEnabled { return NO; }
- (BOOL)areWritableHeapsEnabled { return NO; }
- (NSUInteger)bufferRobustnessSupport {
    unsupported(@"bufferRobustnessSupport is not implemented by the native compute profile.");
}
- (NSUInteger)commandBufferErrorOptions {
    unsupported(@"commandBufferErrorOptions is not implemented by the native compute profile.");
}
- (NSUInteger)commandQueueLimit {
    unsupported(@"commandQueueLimit is not implemented by the native compute profile.");
}
- (void)compilerPropagatesThreadPriority:(bool)argument0 {
    (void)argument0;
    unsupported(@"compilerPropagatesThreadPriority: is not implemented by the native compute profile.");
}
- (NSUInteger)currentCommandQueueCount {
    unsupported(@"currentCommandQueueCount is not implemented by the native compute profile.");
}
- (NSInteger)currentPerformanceState {
    unsupported(@"currentPerformanceState is not implemented by the native compute profile.");
}
- (NSUInteger)dedicatedMemorySize {
    return _capabilities.vramBytes();
}
- (void)deserializeInstanceAccelerationStructure:(id)argument0 fromBytes:(const void*)argument1 primitiveAccelerationStructures:(id)argument2 withDescriptor:(id)argument3 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    unsupported(@"deserializeInstanceAccelerationStructure:fromBytes:primitiveAccelerationStructures:withDescriptor: is not implemented by the native compute profile.");
}
- (id)deserializeInstanceAccelerationStructureFromBytes:(void*)argument0 primitiveAccelerationStructures:(id)argument1 withDescriptor:(id)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"deserializeInstanceAccelerationStructureFromBytes:primitiveAccelerationStructures:withDescriptor: is not implemented by the native compute profile.");
}
- (void)deserializePrimitiveAccelerationStructure:(id)argument0 fromBytes:(const void*)argument1 withDescriptor:(id)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"deserializePrimitiveAccelerationStructure:fromBytes:withDescriptor: is not implemented by the native compute profile.");
}
- (id)deserializePrimitiveAccelerationStructureFromBytes:(void*)argument0 withDescriptor:(id)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"deserializePrimitiveAccelerationStructureFromBytes:withDescriptor: is not implemented by the native compute profile.");
}
- (NSUInteger)deviceCreationFlags {
    unsupported(@"deviceCreationFlags is not implemented by the native compute profile.");
}
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes { return [self limits]->deviceLinearReadOnlyTextureAlignmentBytes; }
- (NSUInteger)deviceLinearTextureAlignmentBytes { return [self limits]->deviceLinearTextureAlignmentBytes; }
- (BOOL)deviceOrFeatureProfileSupportsFeatureSet:(NSUInteger)argument0 {
    (void)argument0;
    return NO;
}
- (BOOL)deviceSupportsFeatureSet:(NSUInteger)argument0 {
    (void)argument0;
    return NO;
}
- (NSUInteger)doubleFPConfig {
    unsupported(@"doubleFPConfig is not implemented by the native compute profile.");
}
- (id)familyName {
    return @"Navi 31";
}
- (NSUInteger)featureProfile {
    unsupported(@"featureProfile is not implemented by the native compute profile.");
}
- (MTLCompilerConnectionManager*)getCompilerConnectionManager:(int32_t)argument0 {
    (void)argument0;
    unsupported(@"getCompilerConnectionManager: is not implemented by the native compute profile.");
}
- (id)getMostCompatibleArchitecture:(id)argument0 {
    (void)argument0;
    unsupported(@"getMostCompatibleArchitecture: is not implemented by the native compute profile.");
}
- (NSUInteger)halfFPConfig {
    unsupported(@"halfFPConfig is not implemented by the native compute profile.");
}
- (IndirectArgumentBufferCapabilities)indirectArgumentBufferCapabilities {
    unsupported(@"indirectArgumentBufferCapabilities is not implemented by the native compute profile.");
}
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes { return [self limits]->iosurfaceReadOnlyTextureAlignmentBytes; }
- (NSUInteger)iosurfaceTextureAlignmentBytes { return [self limits]->iosurfaceTextureAlignmentBytes; }
- (BOOL)isAnisoSampleFixSupported { return NO; }
- (BOOL)isCompatibleWithAccelerationStructure:(HNMetalPairABI)argument0 {
    (void)argument0;
    return NO;
}
- (BOOL)isFixedLinePointFillDepthGradientSupported { return NO; }
- (BOOL)isFloat32FilteringSupported { return NO; }
- (BOOL)isLargeMRTSupported { return NO; }
- (BOOL)isMagicMipmapSupported { return NO; }
- (BOOL)isMsaa32bSupported { return NO; }
- (BOOL)isPlacementHeapSupported { return NO; }
- (BOOL)isRTZRoundingSupported { return NO; }
- (BOOL)isVendorSliceCompatibleWithDeploymentTarget:(uint32_t)argument0 platform:(uint32_t)argument1 sdkVersion:(uint32_t)argument2 compilerPluginVersion:(uint32_t)argument3 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    unsupported(@"isVendorSliceCompatibleWithDeploymentTarget:platform:sdkVersion:compilerPluginVersion: is not implemented by the native compute profile.");
}
- (NSUInteger)latestSupportedGenericBVHVersion {
    unsupported(@"latestSupportedGenericBVHVersion is not implemented by the native compute profile.");
}
- (HNMetalPairABI)libraryCacheStats {
    unsupported(@"libraryCacheStats is not implemented by the native compute profile.");
}
- (NSUInteger)linearTextureAlignmentBytes { return [self limits]->linearTextureAlignmentBytes; }
- (NSUInteger)linearTextureArrayAlignmentBytes { return [self limits]->linearTextureArrayAlignmentBytes; }
- (NSUInteger)linearTextureArrayAlignmentSlice { return [self limits]->linearTextureArrayAlignmentSlice; }
- (id)loadDynamicLibrariesForComputeDescriptor:(id)argument0 error:(id __autoreleasing*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"loadDynamicLibrariesForComputeDescriptor:error: is not implemented by the native compute profile.");
}
- (id)loadDynamicLibrariesForComputeDescriptor:(id)argument0 options:(NSUInteger)argument1 error:(id __autoreleasing*)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"loadDynamicLibrariesForComputeDescriptor:options:error: is not implemented by the native compute profile.");
}
- (id)loadDynamicLibrariesForFunction:(id)argument0 insertLibraries:(id)argument1 error:(id __autoreleasing*)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"loadDynamicLibrariesForFunction:insertLibraries:error: is not implemented by the native compute profile.");
}
- (id)loadDynamicLibrariesForFunction:(id)argument0 insertLibraries:(id)argument1 options:(NSUInteger)argument2 error:(id __autoreleasing*)argument3 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    unsupported(@"loadDynamicLibrariesForFunction:insertLibraries:options:error: is not implemented by the native compute profile.");
}
- (NSUInteger)maxAccelerationStructureLevels { return [self limits]->maxAccelerationStructureLevels; }
- (NSUInteger)maxAccelerationStructureTraversalDepth {
    unsupported(@"maxAccelerationStructureTraversalDepth is not implemented by the native compute profile.");
}
- (NSUInteger)maxColorAttachments { return [self limits]->maxColorAttachments; }
- (NSUInteger)maxComputeAttributes { return [self limits]->maxComputeAttributes; }
- (NSUInteger)maxComputeBuffers { return [self limits]->maxComputeBuffers; }
- (NSUInteger)maxComputeInlineDataSize { return [self limits]->maxComputeInlineDataSize; }
- (NSUInteger)maxComputeLocalMemorySizes { return [self limits]->maxComputeLocalMemorySizes; }
- (NSUInteger)maxComputeSamplers { return [self limits]->maxComputeSamplers; }
- (NSUInteger)maxComputeTextures { return [self limits]->maxComputeTextures; }
- (NSUInteger)maxComputeThreadgroupMemory { return [self limits]->maxComputeThreadgroupMemory; }
- (NSUInteger)maxComputeThreadgroupMemoryAlignmentBytes { return [self limits]->maxComputeThreadgroupMemoryAlignmentBytes; }
- (NSUInteger)maxConcurrentExecutingCompilationTasks {
    unsupported(@"maxConcurrentExecutingCompilationTasks is not implemented by the native compute profile.");
}
- (NSUInteger)maxConstantBufferArguments { return [self limits]->maxConstantBufferArguments; }
- (NSUInteger)maxCustomSamplePositions { return [self limits]->maxCustomSamplePositions; }
- (NSUInteger)maxFenceInstances { return [self limits]->maxFenceInstances; }
- (NSUInteger)maxFragmentBuffers { return [self limits]->maxFragmentBuffers; }
- (NSUInteger)maxFragmentInlineDataSize { return [self limits]->maxFragmentInlineDataSize; }
- (NSUInteger)maxFragmentSamplers { return [self limits]->maxFragmentSamplers; }
- (NSUInteger)maxFragmentTextures { return [self limits]->maxFragmentTextures; }
- (NSUInteger)maxFramebufferStorageBits { return [self limits]->maxFramebufferStorageBits; }
- (NSUInteger)maxFunctionConstantIndices { return [self limits]->maxFunctionConstantIndices; }
- (NSUInteger)maxIOCommandsInFlight { return [self limits]->maxIOCommandsInFlight; }
- (NSUInteger)maxIndirectBuffers { return [self limits]->maxIndirectBuffers; }
- (NSUInteger)maxIndirectSamplers { return [self limits]->maxIndirectSamplers; }
- (NSUInteger)maxIndirectSamplersPerDevice { return [self limits]->maxIndirectSamplersPerDevice; }
- (NSUInteger)maxIndirectTextures { return [self limits]->maxIndirectTextures; }
- (NSUInteger)maxInterpolatedComponents { return [self limits]->maxInterpolatedComponents; }
- (float)maxLineWidth { return [self limits]->maxLineWidth; }
- (float)maxPointSize { return [self limits]->maxPointSize; }
- (NSUInteger)maxPredicatedNestingDepth { return [self limits]->maxPredicatedNestingDepth; }
- (NSUInteger)maxRasterizationRateLayerCount {
    unsupported(@"maxRasterizationRateLayerCount is not implemented by the native compute profile.");
}
- (NSUInteger)maxTessellationFactor { return [self limits]->maxTessellationFactor; }
- (NSUInteger)maxTextureBufferWidth { return [self limits]->maxTextureBufferWidth; }
- (NSUInteger)maxTextureDepth3D { return [self limits]->maxTextureDepth3D; }
- (NSUInteger)maxTextureDimensionCube { return [self limits]->maxTextureDimensionCube; }
- (NSUInteger)maxTextureHeight2D { return [self limits]->maxTextureHeight2D; }
- (NSUInteger)maxTextureHeight3D { return [self limits]->maxTextureHeight3D; }
- (NSUInteger)maxTextureLayers { return [self limits]->maxTextureLayers; }
- (NSUInteger)maxTextureWidth1D { return [self limits]->maxTextureWidth1D; }
- (NSUInteger)maxTextureWidth2D { return [self limits]->maxTextureWidth2D; }
- (NSUInteger)maxTextureWidth3D { return [self limits]->maxTextureWidth3D; }
- (NSUInteger)maxTileBuffers { return [self limits]->maxTileBuffers; }
- (NSUInteger)maxTileInlineDataSize { return [self limits]->maxTileInlineDataSize; }
- (NSUInteger)maxTotalComputeThreadsPerThreadgroup { return [self limits]->maxTotalComputeThreadsPerThreadgroup; }
- (NSUInteger)maxVertexAmplificationCount { return [self limits]->maxVertexAmplificationCount; }
- (NSUInteger)maxVertexAmplificationFactor { return [self limits]->maxVertexAmplificationFactor; }
- (NSUInteger)maxVertexAttributes { return [self limits]->maxVertexAttributes; }
- (NSUInteger)maxVertexBuffers { return [self limits]->maxVertexBuffers; }
- (NSUInteger)maxVertexInlineDataSize { return [self limits]->maxVertexInlineDataSize; }
- (NSUInteger)maxVertexSamplers { return [self limits]->maxVertexSamplers; }
- (NSUInteger)maxVertexTextures { return [self limits]->maxVertexTextures; }
- (NSUInteger)maxViewportCount { return [self limits]->maxViewportCount; }
- (NSUInteger)maxVisibilityQueryOffset { return [self limits]->maxVisibilityQueryOffset; }
- (BOOL)metalAssertionsEnabled { return NO; }
- (NSUInteger)minBufferNoCopyAlignmentBytes { return [self limits]->minBufferNoCopyAlignmentBytes; }
- (NSUInteger)minLinearTextureAlignmentForPixelFormat:(NSUInteger)argument0 {
    (void)argument0;
    unsupported(@"minLinearTextureAlignmentForPixelFormat: is not implemented by the native compute profile.");
}
- (NSUInteger)minLinearTextureBaseAddressAlignmentForDescriptor:(id)argument0 {
    (void)argument0;
    unsupported(@"minLinearTextureBaseAddressAlignmentForDescriptor: is not implemented by the native compute profile.");
}
- (NSUInteger)minLinearTexturePitchAlignmentForDescriptor:(id)argument0 mustMatchExactly:(NSUInteger*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"minLinearTexturePitchAlignmentForDescriptor:mustMatchExactly: is not implemented by the native compute profile.");
}
- (NSUInteger)minTilePixels { return [self limits]->minTilePixels; }
- (id)motionEstimatorCapabilities {
    unsupported(@"motionEstimatorCapabilities is not implemented by the native compute profile.");
}
- (id)newAccelerationStructureWithBuffer:(id)argument0 offset:(NSUInteger)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newAccelerationStructureWithBuffer:offset: is not implemented by the native compute profile.");
}
- (id)newAccelerationStructureWithBuffer:(id)argument0 offset:(NSUInteger)argument1 resourceIndex:(NSUInteger)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"newAccelerationStructureWithBuffer:offset:resourceIndex: is not implemented by the native compute profile.");
}
- (id)newAccelerationStructureWithSize:(NSUInteger)argument0 resourceIndex:(NSUInteger)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newAccelerationStructureWithSize:resourceIndex: is not implemented by the native compute profile.");
}
- (id)newAccelerationStructureWithSize:(NSUInteger)argument0 withDescriptor:(id)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newAccelerationStructureWithSize:withDescriptor: is not implemented by the native compute profile.");
}
- (id)newArgumentEncoderWithLayout:(id)argument0 {
    (void)argument0;
    unsupported(@"newArgumentEncoderWithLayout: is not implemented by the native compute profile.");
}
- (id)newBinaryLibraryWithOptions:(NSUInteger)argument0 url:(id)argument1 error:(id __autoreleasing*)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"newBinaryLibraryWithOptions:url:error: is not implemented by the native compute profile.");
}
- (id)newDynamicLibrary:(id)argument0 computeDescriptor:(id)argument1 error:(id __autoreleasing*)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"newDynamicLibrary:computeDescriptor:error: is not implemented by the native compute profile.");
}
- (id)newDynamicLibraryWithDescriptor:(id)argument0 error:(id __autoreleasing*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newDynamicLibraryWithDescriptor:error: is not implemented by the native compute profile.");
}
- (id)newDynamicLibraryWithURL:(id)argument0 options:(NSUInteger)argument1 error:(id __autoreleasing*)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"newDynamicLibraryWithURL:options:error: is not implemented by the native compute profile.");
}
- (id)newEventWithOptions:(NSInteger)argument0 {
    (void)argument0;
    unsupported(@"newEventWithOptions: is not implemented by the native compute profile.");
}
- (id)newIndirectArgumentBufferLayoutWithStructType:(id)argument0 {
    (void)argument0;
    unsupported(@"newIndirectArgumentBufferLayoutWithStructType: is not implemented by the native compute profile.");
}
- (id)newIndirectCommandBufferWithDescriptor:(id)argument0 maxCount:(NSUInteger)argument1 options:(NSUInteger)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"newIndirectCommandBufferWithDescriptor:maxCount:options: is not implemented by the native compute profile.");
}
- (id)newIndirectComputeCommandEncoderWithBuffer:(id)argument0 {
    (void)argument0;
    unsupported(@"newIndirectComputeCommandEncoderWithBuffer: is not implemented by the native compute profile.");
}
- (id)newIndirectRenderCommandEncoderWithBuffer:(id)argument0 {
    (void)argument0;
    unsupported(@"newIndirectRenderCommandEncoderWithBuffer: is not implemented by the native compute profile.");
}
- (id)newIntersectionFunctionTableWithDescriptor:(id)argument0 {
    (void)argument0;
    unsupported(@"newIntersectionFunctionTableWithDescriptor: is not implemented by the native compute profile.");
}
- (id)newPerformanceStateAssertion:(NSInteger)argument0 error:(id __autoreleasing*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newPerformanceStateAssertion:error: is not implemented by the native compute profile.");
}
- (void)newPrecompiledComputePipelineStateWithDescriptor:(id)argument0 options:(NSUInteger)argument1 pipelineCache:(id)argument2 completionHandler:(void (^)(void))argument3 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    unsupported(@"newPrecompiledComputePipelineStateWithDescriptor:options:pipelineCache:completionHandler: is not implemented by the native compute profile.");
}
- (void)newPrecompiledRenderPipelineStateWithDescriptor:(id)argument0 options:(NSUInteger)argument1 pipelineCache:(id)argument2 completionHandler:(void (^)(void))argument3 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    unsupported(@"newPrecompiledRenderPipelineStateWithDescriptor:options:pipelineCache:completionHandler: is not implemented by the native compute profile.");
}
- (id)newProfileWithExecutionSize:(NSUInteger)argument0 {
    (void)argument0;
    unsupported(@"newProfileWithExecutionSize: is not implemented by the native compute profile.");
}
- (void)newRenderPipelineStateWithMeshDescriptor:(id)argument0 completionHandler:(void (^)(void))argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newRenderPipelineStateWithMeshDescriptor:completionHandler: is not implemented by the native compute profile.");
}
- (id)newRenderPipelineStateWithMeshDescriptor:(id)argument0 error:(id __autoreleasing*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newRenderPipelineStateWithMeshDescriptor:error: is not implemented by the native compute profile.");
}
- (id)newSharedEventWithMachPort:(uint32_t)argument0 {
    (void)argument0;
    unsupported(@"newSharedEventWithMachPort: is not implemented by the native compute profile.");
}
- (id)newSharedEventWithOptions:(NSInteger)argument0 {
    (void)argument0;
    unsupported(@"newSharedEventWithOptions: is not implemented by the native compute profile.");
}
- (id)newTextureWithDescriptor:(id)argument0 iosurface:(IOSurfaceRef)argument1 plane:(NSUInteger)argument2 slice:(NSUInteger)argument3 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    unsupported(@"newTextureWithDescriptor:iosurface:plane:slice: is not implemented by the native compute profile.");
}
- (id)newTextureWithSurface:(NSUInteger)argument0 buffer:(NSUInteger)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"newTextureWithSurface:buffer: is not implemented by the native compute profile.");
}
- (id)newTiledTextureWithBytesNoCopy:(void*)argument0 length:(NSUInteger)argument1 deallocator:(void (^)(void))argument2 descriptor:(id)argument3 offset:(NSUInteger)argument4 bytesPerRow:(NSUInteger)argument5 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    (void)argument4;
    (void)argument5;
    unsupported(@"newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow: is not implemented by the native compute profile.");
}
- (id)newTiledTextureWithBytesNoCopy:(void*)argument0 length:(NSUInteger)argument1 descriptor:(id)argument2 offset:(NSUInteger)argument3 bytesPerRow:(NSUInteger)argument4 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    (void)argument3;
    (void)argument4;
    unsupported(@"newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow: is not implemented by the native compute profile.");
}
- (id)newVisibleFunctionTableWithDescriptor:(id)argument0 {
    (void)argument0;
    unsupported(@"newVisibleFunctionTableWithDescriptor: is not implemented by the native compute profile.");
}
- (HNMetalPairABI)pipelineCacheStats {
    unsupported(@"pipelineCacheStats is not implemented by the native compute profile.");
}
- (id)pluginData {
    unsupported(@"pluginData is not implemented by the native compute profile.");
}
- (id)productName {
    return [self name];
}
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(id)argument0 {
    (void)argument0;
    unsupported(@"requiredLinearTextureBytesPerRowForDescriptor: is not implemented by the native compute profile.");
}
- (BOOL)requiresBFloat16Emulation {
    unsupported(@"requiresBFloat16Emulation is not implemented by the native compute profile.");
}
- (BOOL)requiresMeshShaderEmulation {
    unsupported(@"requiresMeshShaderEmulation is not implemented by the native compute profile.");
}
- (BOOL)requiresRaytracingEmulation {
    unsupported(@"requiresRaytracingEmulation is not implemented by the native compute profile.");
}
- (NSUInteger)samplerReductionModeSupport {
    unsupported(@"samplerReductionModeSupport is not implemented by the native compute profile.");
}
- (void)setCommandBufferErrorOptions:(NSUInteger)argument0 {
    (void)argument0;
    unsupported(@"setCommandBufferErrorOptions: is not implemented by the native compute profile.");
}
- (BOOL)setCompilerProcessesCount:(int32_t)argument0 {
    (void)argument0;
    unsupported(@"setCompilerProcessesCount: is not implemented by the native compute profile.");
}
- (void)setGPUAssertionsEnabled:(BOOL)argument0 {
    (void)argument0;
    if (argument0) unsupported(@"setGPUAssertionsEnabled: cannot enable an unimplemented native feature.");
}
- (void)setMetalAssertionsEnabled:(BOOL)argument0 {
    (void)argument0;
    if (argument0) unsupported(@"setMetalAssertionsEnabled: cannot enable an unimplemented native feature.");
}
- (void)setPluginData:(id)argument0 {
    (void)argument0;
    unsupported(@"setPluginData: is not implemented by the native compute profile.");
}
- (void)setWritableHeapsEnabled:(BOOL)argument0 {
    (void)argument0;
    if (argument0) unsupported(@"setWritableHeapsEnabled: cannot enable an unimplemented native feature.");
}
- (NSUInteger)sharedMemorySize {
    unsupported(@"sharedMemorySize is not implemented by the native compute profile.");
}
- (NSUInteger)singleFPConfig {
    unsupported(@"singleFPConfig is not implemented by the native compute profile.");
}
- (BOOL)supportPriorityBand { return NO; }
- (BOOL)supports2DLinearTexArraySPI { return NO; }
- (BOOL)supports32bpcMSAATextures { return NO; }
- (BOOL)supports3DASTCTextures { return NO; }
- (BOOL)supports3DBCTextures { return NO; }
- (BOOL)supportsASTCHDRTextureCompression { return NO; }
- (BOOL)supportsASTCTextureCompression { return NO; }
- (BOOL)supportsAlphaYUVFormats { return NO; }
- (BOOL)supportsAnisoSampleFix { return NO; }
- (BOOL)supportsArgumentBuffers { return NO; }
- (BOOL)supportsArgumentBuffersTier2 { return NO; }
- (BOOL)supportsArrayOfSamplers { return NO; }
- (BOOL)supportsArrayOfTextures { return NO; }
- (BOOL)supportsAtomicFloat { return NO; }
- (BOOL)supportsAtomicUlongVoidMinMax { return NO; }
- (BOOL)supportsBaseVertexInstanceDrawing { return NO; }
- (BOOL)supportsBfloat16Buffers { return NO; }
- (BOOL)supportsBfloat16Format { return NO; }
- (BOOL)supportsBinaryArchives { return NO; }
- (BOOL)supportsBinaryLibraries { return NO; }
- (BOOL)supportsBlackOrWhiteSamplerBorderColors { return NO; }
- (BOOL)supportsBufferBoundsChecking { return NO; }
- (BOOL)supportsBufferPrefetchStatistics { return NO; }
- (BOOL)supportsBufferWithIOSurface { return NO; }
- (BOOL)supportsBufferlessClientStorageTexture { return NO; }
- (BOOL)supportsCMPIndirectCommandBuffers { return NO; }
- (BOOL)supportsColorSpaceConversionMatrixSelection { return NO; }
- (BOOL)supportsCombinedMSAAStoreAndResolveAction { return NO; }
- (BOOL)supportsCommandBufferJump { return NO; }
- (BOOL)supportsCompressedTextureViewSPI { return NO; }
- (BOOL)supportsComputeCompressedTextureWrite { return NO; }
- (BOOL)supportsComputeMemoryBarrier { return NO; }
- (BOOL)supportsConcurrentComputeDispatch { return NO; }
- (BOOL)supportsConditionalLoadStore { return NO; }
- (BOOL)supportsCountingOcclusionQuery { return NO; }
- (BOOL)supportsCustomBorderColor { return NO; }
- (BOOL)supportsDeadlineProfile { return NO; }
- (BOOL)supportsDepthClipMode { return NO; }
- (BOOL)supportsDepthClipModeClampExtended { return NO; }
- (BOOL)supportsDeviceCoherency { return NO; }
- (BOOL)supportsDevicePartitioning { return NO; }
- (BOOL)supportsDynamicAttributeStride { return NO; }
- (BOOL)supportsDynamicControlPointCount { return NO; }
- (BOOL)supportsExplicitVisibilityGroups { return NO; }
- (BOOL)supportsExtendedSamplerLODBiasRange { return NO; }
- (BOOL)supportsExtendedVertexFormats { return NO; }
- (BOOL)supportsExtendedXR10Formats { return NO; }
- (BOOL)supportsExtendedYUVFormats { return NO; }
- (BOOL)supportsFP32TessFactors { return NO; }
- (BOOL)supportsFastMathInfNaNPropagation { return NO; }
- (BOOL)supportsFillTexture { return NO; }
- (BOOL)supportsFixedLinePointFillDepthGradient { return NO; }
- (BOOL)supportsFloat16BCubicFiltering { return NO; }
- (BOOL)supportsFloat16InfNanFiltering { return NO; }
- (BOOL)supportsForceSeamsOnCubemaps { return NO; }
- (BOOL)supportsForkJoin { return NO; }
- (BOOL)supportsFragmentBufferWrites { return NO; }
- (BOOL)supportsFragmentOnlyEncoders { return NO; }
- (BOOL)supportsFunctionPointersFromMesh { return NO; }
- (BOOL)supportsGFXIndirectCommandBuffers { return NO; }
- (BOOL)supportsGPUStatistics { return NO; }
- (BOOL)supportsGlobalVariableBindingInDylibs { return NO; }
- (BOOL)supportsGlobalVariableBindings { return NO; }
- (BOOL)supportsGlobalVariableRelocation { return NO; }
- (BOOL)supportsGlobalVariableRelocationCompute { return NO; }
- (BOOL)supportsGlobalVariableRelocationRender { return NO; }
- (BOOL)supportsHeapAccelerationStructureAllocation { return NO; }
- (BOOL)supportsHeapWithAddressRanges { return NO; }
- (BOOL)supportsIABHashForTools { return NO; }
- (BOOL)supportsImageBlockSampleCoverageControl { return NO; }
- (BOOL)supportsImageBlocks { return NO; }
- (BOOL)supportsIndirectStageInRegion { return NO; }
- (BOOL)supportsIndirectTessellation { return NO; }
- (BOOL)supportsIndirectTextures { return NO; }
- (BOOL)supportsIndirectWritableTextures { return NO; }
- (BOOL)supportsInt64 { return NO; }
- (BOOL)supportsInterchangeTiled { return NO; }
- (BOOL)supportsLateEvalEvent { return NO; }
- (BOOL)supportsLayeredRendering { return NO; }
- (BOOL)supportsLimitedYUVFormats { return NO; }
- (BOOL)supportsLinearTexture2DArray { return NO; }
- (BOOL)supportsLinearTextureFromSharedBuffer { return NO; }
- (BOOL)supportsLossyCompression { return NO; }
- (BOOL)supportsMSAADepthResolve { return NO; }
- (BOOL)supportsMSAADepthResolveFilter { return NO; }
- (BOOL)supportsMSAAStencilResolve { return NO; }
- (BOOL)supportsMSAAStencilResolveFilter { return NO; }
- (BOOL)supportsMemoryOrderAtomics { return NO; }
- (BOOL)supportsMeshRenderDynamicLibraries { return NO; }
- (BOOL)supportsMeshShaders { return NO; }
- (BOOL)supportsMeshShadersInICB { return NO; }
- (BOOL)supportsMipLevelsSmallerThanBlockSize { return NO; }
- (BOOL)supportsMirrorClampToEdgeSamplerMode { return NO; }
- (BOOL)supportsMutableTier1ArgumentBuffers { return NO; }
- (BOOL)supportsNativeHardwareFP16 { return NO; }
- (BOOL)supportsNonPrivateDepthStencilTextures { return NO; }
- (BOOL)supportsNonPrivateMSAATextures { return NO; }
- (BOOL)supportsNonSquareTileShaders { return NO; }
- (BOOL)supportsNonZeroTextureWriteLOD { return NO; }
- (BOOL)supportsNorm16BCubicFiltering { return NO; }
- (BOOL)supportsOpenCLTextureWriteSwizzles { return NO; }
- (BOOL)supportsPacked32TextureBufferWrites { return NO; }
- (BOOL)supportsPartialRenderMemoryBarrier { return NO; }
- (BOOL)supportsPerPlaneCompression { return NO; }
- (BOOL)supportsPerformanceStateAssertion { return NO; }
- (BOOL)supportsPipelineLibraries { return NO; }
- (BOOL)supportsPlacementHeaps { return NO; }
- (BOOL)supportsPostDepthCoverage { return NO; }
- (BOOL)supportsPrimitiveRestartOverride { return NO; }
- (BOOL)supportsProgrammableBlending { return NO; }
- (BOOL)supportsProgrammableSamplePositions { return NO; }
- (BOOL)supportsPublicXR10Formats { return NO; }
- (BOOL)supportsQuadGroup { return NO; }
- (BOOL)supportsQuadReduction { return NO; }
- (BOOL)supportsQuadShufflesAndBroadcast { return NO; }
- (BOOL)supportsRGBA10A2Gamma { return NO; }
- (BOOL)supportsRTZRounding { return NO; }
- (BOOL)supportsRasterOrderGroups { return NO; }
- (BOOL)supportsRasterOrderGroupsColorAttachment { return NO; }
- (BOOL)supportsRayTracingAccelerationStructureCPUDeserialization { return NO; }
- (BOOL)supportsRayTracingBuffersFromTables { return NO; }
- (BOOL)supportsRayTracingCurves { return NO; }
- (BOOL)supportsRayTracingDirectIntersectionResultAccess { return NO; }
- (BOOL)supportsRayTracingExtendedVertexFormats { return NO; }
- (BOOL)supportsRayTracingGPUTableUpdateBuffers { return NO; }
- (BOOL)supportsRayTracingICBs { return NO; }
- (BOOL)supportsRayTracingIndirectInstanceAccelerationStructureBuild { return NO; }
- (BOOL)supportsRayTracingMatrixLayout { return NO; }
- (BOOL)supportsRayTracingMultiLevelInstancing { return NO; }
- (BOOL)supportsRayTracingPerComponentMotionInterpolation { return NO; }
- (BOOL)supportsRayTracingPerPrimitiveData { return NO; }
- (BOOL)supportsRayTracingTraversalMetrics { return NO; }
- (BOOL)supportsReadWriteTextureArguments { return NO; }
- (BOOL)supportsReadWriteTextureArgumentsTier2 { return NO; }
- (BOOL)supportsReadWriteTextureCubeArguments { return NO; }
- (BOOL)supportsRelaxedTextureViewRequirements { return NO; }
- (BOOL)supportsRenderMemoryBarrier { return NO; }
- (BOOL)supportsRenderPassWithoutRenderTarget { return NO; }
- (BOOL)supportsRenderTargetTextureRotation { return NO; }
- (BOOL)supportsRenderTextureWrites { return NO; }
- (BOOL)supportsRenderToLinearTextures { return NO; }
- (BOOL)supportsResourceDetachBacking { return NO; }
- (BOOL)supportsResourceHeaps { return NO; }
- (BOOL)supportsSIMDGroup { return NO; }
- (BOOL)supportsSIMDGroupMatrix { return NO; }
- (BOOL)supportsSIMDReduction { return NO; }
- (BOOL)supportsSIMDShuffleAndFill { return NO; }
- (BOOL)supportsSIMDShufflesAndBroadcast { return NO; }
- (BOOL)supportsSRGBwrites { return NO; }
- (BOOL)supportsSampleCount:(NSUInteger)argument0 {
    (void)argument0;
    return NO;
}
- (BOOL)supportsSamplerAddressModeClampToHalfBorder { return NO; }
- (BOOL)supportsSamplerCompareFunction { return NO; }
- (BOOL)supportsSamplerReductionMode { return NO; }
- (BOOL)supportsSeparateDepthStencil { return NO; }
- (BOOL)supportsSeparateVisibilityAndShadingRate { return NO; }
- (BOOL)supportsSetThreadgroupPackingDisabled { return NO; }
- (BOOL)supportsShaderLODAverage { return NO; }
- (BOOL)supportsShaderMinLODClamp { return NO; }
- (BOOL)supportsSharedFunctionTables { return NO; }
- (BOOL)supportsSharedStorageHeapResources { return NO; }
- (BOOL)supportsSharedStorageTextures { return NO; }
- (BOOL)supportsSharedTextureHandles { return NO; }
- (BOOL)supportsSparseDepthAttachments { return NO; }
- (BOOL)supportsSparseHeaps { return NO; }
- (BOOL)supportsSparseTextures { return NO; }
- (BOOL)supportsStackOverflowErrorCode { return NO; }
- (BOOL)supportsStatefulDynamicLibraries { return NO; }
- (BOOL)supportsStencilFeedback { return NO; }
- (BOOL)supportsTLS { return NO; }
- (BOOL)supportsTessellation { return NO; }
- (BOOL)supportsTexture2DMultisampleArray { return NO; }
- (BOOL)supportsTextureCubeArray { return NO; }
- (BOOL)supportsTextureSwizzle { return NO; }
- (BOOL)supportsTextureWriteRoundingMode:(NSInteger)argument0 {
    (void)argument0;
    return NO;
}
- (BOOL)supportsTileShaders { return NO; }
- (BOOL)supportsUnalignedVertexFetch { return NO; }
- (BOOL)supportsVariableRateRasterization { return NO; }
- (BOOL)supportsVertexAmplification { return NO; }
- (BOOL)supportsVirtualSubstreams { return NO; }
- (BOOL)supportsYCBCRFormats { return NO; }
- (BOOL)supportsYCBCRFormats12 { return NO; }
- (BOOL)supportsYCBCRFormatsPQ { return NO; }
- (BOOL)supportsYCBCRFormatsXR { return NO; }
- (BOOL)supportsYCBCRPackedFormats12 { return NO; }
- (BOOL)supportsYCBCRPackedFormatsPQ { return NO; }
- (BOOL)supportsYCBCRPackedFormatsXR { return NO; }
- (id)targetDeviceArchitecture {
    unsupported(@"targetDeviceArchitecture is not implemented by the native compute profile.");
}
- (const MTLTargetDeviceArch*)targetDeviceInfo {
    unsupported(@"targetDeviceInfo is not implemented by the native compute profile.");
}
- (void)unloadShaderCaches {
    unsupported(@"unloadShaderCaches is not implemented by the native compute profile.");
}
- (BOOL)validateDynamicLibrary:(id)argument0 state:(BOOL)argument1 error:(id __autoreleasing*)argument2 {
    (void)argument0;
    (void)argument1;
    (void)argument2;
    unsupported(@"validateDynamicLibrary:state:error: is not implemented by the native compute profile.");
}
- (BOOL)validateDynamicLibraryDescriptor:(id)argument0 error:(id __autoreleasing*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"validateDynamicLibraryDescriptor:error: is not implemented by the native compute profile.");
}
- (BOOL)validateDynamicLibraryURL:(id)argument0 error:(id __autoreleasing*)argument1 {
    (void)argument0;
    (void)argument1;
    unsupported(@"validateDynamicLibraryURL:error: is not implemented by the native compute profile.");
}
