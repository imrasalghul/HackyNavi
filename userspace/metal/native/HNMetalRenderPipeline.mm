// SPDX-License-Identifier: MIT
#import "HNMetalRenderPipeline.hpp"
#include "../../runtime/graphics_build.hpp"
#include <new>

namespace {
using namespace hackynavi::compute;
BOOL fail(NSError* __autoreleasing* error,NSString* message,Result result={Status::Unsupported,0}) {
    if (error) *error=[NSError errorWithDomain:@"org.hackynavi.Metal.RenderPipeline" code:NSInteger(result.status)
        userInfo:@{NSLocalizedDescriptionKey:message,@"IOReturn":@(result.iokit)}];
    return NO;
}
[[noreturn]] void unsupported(NSString* message) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:message userInfo:nil];
}
bool noLinkedFunctions(MTLLinkedFunctions* functions) {
    return !functions || (!functions.functions.count && !functions.binaryFunctions.count &&
        !functions.groups.count && !functions.privateFunctions.count);
}
BOOL descriptorSupported(MTLRenderPipelineDescriptor* d,NSError* __autoreleasing* error) {
    if (!d || d.vertexDescriptor || d.rasterSampleCount!=1 || d.alphaToCoverageEnabled ||
        d.alphaToOneEnabled || !d.rasterizationEnabled || d.maxVertexAmplificationCount!=1 ||
        d.depthAttachmentPixelFormat!=MTLPixelFormatInvalid || d.stencilAttachmentPixelFormat!=MTLPixelFormatInvalid ||
        (d.inputPrimitiveTopology!=MTLPrimitiveTopologyClassUnspecified && d.inputPrimitiveTopology!=MTLPrimitiveTopologyClassTriangle) ||
        d.supportIndirectCommandBuffers || d.binaryArchives.count || d.vertexPreloadedLibraries.count ||
        d.fragmentPreloadedLibraries.count || !noLinkedFunctions(d.vertexLinkedFunctions) ||
        !noLinkedFunctions(d.fragmentLinkedFunctions) || d.supportAddingVertexBinaryFunctions ||
        d.supportAddingFragmentBinaryFunctions || d.maxVertexCallStackDepth!=1 || d.maxFragmentCallStackDepth!=1 ||
        (d.shaderValidation!=MTLShaderValidationDefault && d.shaderValidation!=MTLShaderValidationDisabled))
        return fail(error,@"The render descriptor exceeds the single-sample triangle profile.");
    MTLRenderPipelineDescriptor* defaults=[MTLRenderPipelineDescriptor new];
    if (!defaults) return fail(error,@"Unable to obtain descriptor defaults.",{Status::OutOfMemory,0});
    // Even inactive tessellation state must retain its real SDK default.
    if (d.tessellationPartitionMode!=defaults.tessellationPartitionMode ||
        d.maxTessellationFactor!=defaults.maxTessellationFactor ||
        d.tessellationFactorScaleEnabled!=defaults.tessellationFactorScaleEnabled ||
        d.tessellationFactorFormat!=defaults.tessellationFactorFormat ||
        d.tessellationControlPointIndexType!=defaults.tessellationControlPointIndexType ||
        d.tessellationFactorStepFunction!=defaults.tessellationFactorStepFunction ||
        d.tessellationOutputWindingOrder!=defaults.tessellationOutputWindingOrder)
        return fail(error,@"Tessellation descriptor state is not implemented.");
    if (!d.colorAttachments || !d.vertexBuffers || !d.fragmentBuffers)
        return fail(error,@"The render descriptor is incomplete.",{Status::Invalid,0});
    for (NSUInteger index=0;index<8;++index) {
        auto* c=d.colorAttachments[index];
        if (!c || c.pixelFormat!=(index ? MTLPixelFormatInvalid : MTLPixelFormatRGBA8Unorm) ||
            c.blendingEnabled || c.writeMask!=MTLColorWriteMaskAll ||
            c.sourceRGBBlendFactor!=MTLBlendFactorOne || c.sourceAlphaBlendFactor!=MTLBlendFactorOne ||
            c.destinationRGBBlendFactor!=MTLBlendFactorZero || c.destinationAlphaBlendFactor!=MTLBlendFactorZero ||
            c.rgbBlendOperation!=MTLBlendOperationAdd || c.alphaBlendOperation!=MTLBlendOperationAdd)
            return fail(error,@"Only color attachment zero, RGBA8Unorm and full unblended writes are implemented.");
    }
    for (NSUInteger index=0;index<31;++index)
        if (d.vertexBuffers[index].mutability!=MTLMutabilityDefault ||
            d.fragmentBuffers[index].mutability!=MTLMutabilityDefault)
            return fail(error,@"Custom render buffer mutability is not implemented.");
    return YES;
}
}
@implementation HNMetalRenderPipeline {
    id<HNMetalContextOwner> _device;
    NSString* _label;
    HNMetalFunction* _vertex;
    HNMetalFunction* _fragment;
    std::shared_ptr<Context> _context;
    std::shared_ptr<const GraphicsPipeline> _pipeline;
    std::shared_ptr<const FragmentBufferGraphicsPipeline> _bufferPipeline;
    std::shared_ptr<const VertexBufferGraphicsPipeline> _vertexPipeline;
    std::shared_ptr<const VertexFragmentBufferGraphicsPipeline> _vertexFragmentPipeline;
}
- (instancetype)init { return nil; }
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                             descriptor:(MTLRenderPipelineDescriptor*)descriptor
                                  error:(NSError* _Nullable __autoreleasing* _Nullable)error {
    if (error) *error=nil;
    self=[super init]; if (!self) return nil;
    MTLRenderPipelineDescriptor* d=[descriptor copy];
    if (!device || ![device respondsToSelector:@selector(nativeComputeContext)] ||
        !descriptorSupported(d,error)) return nil;
    if (![d.vertexFunction isMemberOfClass:[HNMetalFunction class]] ||
        ![d.fragmentFunction isMemberOfClass:[HNMetalFunction class]] ||
        d.vertexFunction.device!=device || d.fragmentFunction.device!=device ||
        d.vertexFunction.functionType!=MTLFunctionTypeVertex || d.fragmentFunction.functionType!=MTLFunctionTypeFragment) {
        fail(error,@"Expected actual vertex and fragment functions from this exact device.",{Status::Invalid,0}); return nil;
    }
    try {
        _context=[device nativeComputeContext];
        HNColorTopology topology{};
        auto live=_context ? _context->graphicsInformation(topology) : Result{Status::Invalid,0};
        if (!live) { fail(error,@"A live native graphics context is required.",live); return nil; }
        _vertex=(HNMetalFunction*)d.vertexFunction; _fragment=(HNMetalFunction*)d.fragmentFunction;
        const auto vertex=[_vertex nativeGraphicsShader],fragment=[_fragment nativeGraphicsShader];
        if (!vertex || !fragment) { fail(error,@"The functions contain no actual graphics shader.",{Status::Invalid,0}); return nil; }
        if (vertex->vertexBufferBinding() || fragment->fragmentBufferBinding()) {
            hackynavi::nativeabi::HelloReply information{};
            live=_context->information(information);
            const bool vertexProfile=information.header.minor==hackynavi::nativeabi::kVertexBufferedGraphicsMinor &&
                information.capabilities==hackynavi::nativeabi::kVertexBufferedGraphicsCapabilities;
            const bool fragmentProfile=information.header.minor==hackynavi::nativeabi::kBufferedGraphicsMinor &&
                information.capabilities==hackynavi::nativeabi::kBufferedGraphicsCapabilities;
            if (!live || (!vertexProfile && (vertex->vertexBufferBinding() || !fragmentProfile))) {
                fail(error,@"Buffer shaders require the matching native graphics profile.",
                    live ? Result{Status::Unsupported,0} : live); return nil;
            }
        }
        std::string diagnostic;
        // The actual owned fragment reflection selects the compiler route.
        // A descriptor or caller flag cannot opt arbitrary code into it.
        const auto built=vertex->vertexBufferBinding() ? (fragment->fragmentBufferBinding() ?
            buildVertexFragmentBufferGraphicsPipeline(*vertex,*fragment,_vertexFragmentPipeline,diagnostic) :
            buildVertexBufferGraphicsPipeline(*vertex,*fragment,_vertexPipeline,diagnostic)) : fragment->fragmentBufferBinding() ?
            buildFragmentBufferGraphicsPipeline(*vertex,*fragment,_bufferPipeline,diagnostic) :
            buildGraphicsPipeline(*vertex,*fragment,_pipeline,diagnostic);
        if (!built) {
            NSString* message=[[NSString alloc] initWithBytes:diagnostic.data() length:diagnostic.size() encoding:NSUTF8StringEncoding];
            fail(error,message.length ? message : @"The actual graphics compiler failed.",built); return nil;
        }
        live=_context->graphicsInformation(topology);
        if (!live || [device nativeComputeContext]!=_context) {
            fail(error,@"The native owner changed or closed during graphics compilation.",live ? Result{Status::Invalid,0} : live); return nil;
        }
        _label=[d.label copy]; _device=device; return self;
    } catch (const std::bad_alloc&) { fail(error,@"Unable to retain graphics pipeline storage.",{Status::OutOfMemory,0}); return nil; }
}
- (id<MTLDevice>)device { return _device; }
- (nullable NSString*)label { return _label; }
- (std::shared_ptr<Context>)nativeContext { return _context; }
- (std::shared_ptr<const GraphicsPipeline>)nativePipeline { return _pipeline; }
- (std::shared_ptr<const FragmentBufferGraphicsPipeline>)nativeBufferPipeline { return _bufferPipeline; }
- (std::shared_ptr<const VertexBufferGraphicsPipeline>)nativeVertexBufferPipeline { return _vertexPipeline; }
- (std::shared_ptr<const VertexFragmentBufferGraphicsPipeline>)nativeVertexFragmentBufferPipeline { return _vertexFragmentPipeline; }
- (NSUInteger)allocatedSize { return 0; } // CPU code only; draw owns actual GPU allocations.
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (MTLShaderValidation)shaderValidation { return MTLShaderValidationDisabled; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 0; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (NSUInteger)maxTotalThreadsPerObjectThreadgroup { return 0; }
- (NSUInteger)maxTotalThreadsPerMeshThreadgroup { return 0; }
- (NSUInteger)objectThreadExecutionWidth { return 0; }
- (NSUInteger)meshThreadExecutionWidth { return 0; }
- (NSUInteger)maxTotalThreadgroupsPerMeshGrid { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions { (void)dimensions; unsupported(@"Imageblocks are not implemented."); }
- (MTLResourceID)gpuResourceID { unsupported(@"Pipeline argument-buffer IDs are not implemented."); }
- (nullable id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage { (void)function; (void)stage; return nil; }
- (nullable id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor*)descriptor stage:(MTLRenderStages)stage { (void)descriptor; (void)stage; return nil; }
- (nullable id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor*)descriptor stage:(MTLRenderStages)stage { (void)descriptor; (void)stage; return nil; }
- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithAdditionalBinaryFunctions:(MTLRenderPipelineFunctionsDescriptor*)functions error:(NSError* __autoreleasing*)error {
    (void)functions; fail(error,@"Adding binary functions is not implemented."); return nil;
}
- (nullable MTLRenderPipelineReflection*)reflection API_AVAILABLE(macos(26.0)) { return nil; }
- (MTLSize)requiredThreadsPerTileThreadgroup API_AVAILABLE(macos(26.0)) { return MTLSizeMake(0,0,0); }
- (MTLSize)requiredThreadsPerObjectThreadgroup API_AVAILABLE(macos(26.0)) { return MTLSizeMake(0,0,0); }
- (MTLSize)requiredThreadsPerMeshThreadgroup API_AVAILABLE(macos(26.0)) { return MTLSizeMake(0,0,0); }
- (nullable id<MTLFunctionHandle>)functionHandleWithName:(NSString*)name stage:(MTLRenderStages)stage API_AVAILABLE(macos(26.0)) { (void)name; (void)stage; return nil; }
- (nullable id<MTLFunctionHandle>)functionHandleWithBinaryFunction:(id<MTL4BinaryFunction>)function stage:(MTLRenderStages)stage API_AVAILABLE(macos(26.0)) { (void)function; (void)stage; return nil; }
- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithBinaryFunctions:(MTL4RenderPipelineBinaryFunctionsDescriptor*)functions error:(NSError* __autoreleasing*)error API_AVAILABLE(macos(26.0)) {
    (void)functions; fail(error,@"Metal 4 linking is not implemented."); return nil;
}
- (MTL4PipelineDescriptor*)newRenderPipelineDescriptorForSpecialization API_AVAILABLE(macos(26.0)) { unsupported(@"Metal 4 specialization is not implemented."); }
@end
