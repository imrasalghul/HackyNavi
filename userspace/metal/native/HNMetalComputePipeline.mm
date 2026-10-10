// SPDX-License-Identifier: MIT
#import "HNMetalComputePipeline.hpp"
#import <CoreFoundation/CoreFoundation.h>
#include "hackynavi/gfx11_compute_product.hpp"
#include "hackynavi/gfx11_occupancy.hpp"
#include <cstring>
#include <new>

namespace {
using hackynavi::compute::Pipeline;
using hackynavi::compute::Result;
using hackynavi::compute::Status;

NSError* pipelineError(NSString* description, Result result) {
    return [NSError errorWithDomain:@"org.hackynavi.Metal.ComputePipeline" code:NSInteger(result.status)
        userInfo:@{NSLocalizedDescriptionKey: description, @"IOReturn": @(result.iokit)}];
}
BOOL fail(NSError * __autoreleasing * error, NSString* description, Result result) {
    if (error) *error = pipelineError(description, result);
    return NO;
}
NSString* compilerText(const std::string& diagnostic) {
    NSString* decoded = [[NSString alloc] initWithBytes:diagnostic.data() length:diagnostic.size()
                                              encoding:NSUTF8StringEncoding];
    return decoded.length ? decoded : @"The actual pipeline compiler returned no UTF-8 diagnostic.";
}
[[noreturn]] void unsupported(NSString* reason) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:reason userInfo:nil];
}
bool emptyArray(NSDictionary* reflection, NSString* key) {
    id value = reflection[key];
    return [value isKindOfClass:[NSArray class]] && [value count] == 0;
}
// JSON booleans, fractions, negatives and out-of-range values are not AIR
// thread limits. Compare the original NSNumber to its bounded integer copy.
bool positiveU32(id value, uint32_t& out) {
    if (![value isKindOfClass:[NSNumber class]] ||
        CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ||
        [value compare:@1] == NSOrderedAscending ||
        [value compare:@(UINT32_MAX)] == NSOrderedDescending) return false;
    const uint32_t integer = [value unsignedIntValue];
    if ([value compare:@(integer)] != NSOrderedSame) return false;
    out = integer; return true;
}
bool reflectionLimit(const hackynavi::compute::Shader& shader, uint32_t& maximum) {
    const auto& json = shader.reflectionJson();
    NSData* data = [NSData dataWithBytes:json.data() length:json.size()];
    id parsed = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nullptr] : nil;
    NSDictionary* reflection = [parsed isKindOfClass:[NSDictionary class]] ? parsed : nil;
    NSString* entry = [[NSString alloc] initWithBytes:shader.name().data() length:shader.name().size()
                                          encoding:NSUTF8StringEncoding];
    if (!reflection || !entry || ![reflection[@"stage"] isEqual:@"Kernel"] ||
        ![reflection[@"entry_point"] isEqual:entry] ||
        !emptyArray(reflection, @"function_constants") ||
        !emptyArray(reflection, @"argument_buffer_fields") ||
        !emptyArray(reflection, @"imageblock_layouts") ||
        !emptyArray(reflection, @"implicit_imageblock_attachments") ||
        !emptyArray(reflection, @"runtime_sampler_specializations") ||
        !emptyArray(reflection, @"runtime_storage_image_specializations") ||
        reflection[@"fragment_imageblock"] != [NSNull null]) return false;
    id bindings = reflection[@"bindings"];
    if (![bindings isKindOfClass:[NSArray class]] || ![bindings count] || [bindings count] > 31) return false;
    for (id binding in bindings) {
        // Threadgroup, image, sampler and other resource kinds are not part of
        // this zero-LDS ordinary-buffer profile. Actual code is checked below.
        if (![binding isKindOfClass:[NSDictionary class]] || ![binding[@"kind"] isEqual:@"Buffer"] ||
            binding[@"stage_input_location"] != [NSNull null]) return false;
    }
    id airLimit = reflection[@"max_work_group_size"];
    uint32_t actual = hackynavi::gfx11occupancy::kAllAdmittedVariantsMaxThreads;
    if (airLimit != [NSNull null]) {
        uint32_t bounded = 0;
        if (!positiveU32(airLimit, bounded)) return false;
        if (bounded < actual) actual = bounded;
    }
    maximum = actual; return true;
}
bool dimensions(MTLSize grid, MTLSize local, uint32_t maximum, uint32_t* g, uint32_t* l) {
    const NSUInteger gridValues[3] = {grid.width, grid.height, grid.depth};
    const NSUInteger localValues[3] = {local.width, local.height, local.depth};
    uint64_t gridThreads = 1, groupThreads = 1;
    for (unsigned d = 0; d < 3; ++d) {
        if (!gridValues[d] || gridValues[d] > UINT32_MAX || !localValues[d] ||
            localValues[d] > maximum || gridThreads > UINT64_MAX / gridValues[d]) return false;
        gridThreads *= gridValues[d];
        // Each factor <=640, so even the three-dimensional product fits u64.
        groupThreads *= localValues[d];
        g[d] = uint32_t(gridValues[d]); l[d] = uint32_t(localValues[d]);
    }
    return groupThreads <= maximum;
}
BOOL validateProduct(const Pipeline& pipeline, uint32_t maximum,
                     NSError * __autoreleasing * error) {
    using namespace hackynavi;
    const auto& resources = pipeline.resources();
    if (!pipeline.variantCount() || pipeline.variantCount() > nativeabi::kMaxProductVariants ||
        resources.empty() || resources.size() > gfx11product::kMaxBindings)
        return fail(error, @"Compiled pipeline has an invalid variant/resource count.", {Status::Protocol, 0});
    uint32_t decodedReads = 0, decodedWrites = 0;
    for (uint32_t index = 0; index < pipeline.variantCount(); ++index) {
        HNPipelineVariant v{};
        const auto metadata = pipeline.variantInfo(index, v);
        if (!metadata) return fail(error, @"Unable to read the owned compiler variant.", metadata);
        uint64_t threads = 1;
        for (unsigned d = 0; d < 3; ++d) {
            if (!v.local_size[d] || v.local_size[d] > maximum)
                return fail(error, @"Compiled local dimensions exceed the pipeline limit.", {Status::Unsupported, 0});
            threads *= v.local_size[d];
        }
        if (threads > maximum || !gfx11occupancy::fits(v.num_vgprs, uint32_t(threads)) ||
            v.wave_size != gfx11occupancy::kWaveSize || !v.code_bytes ||
            v.code_bytes > gfx11product::kMaxCodeBytes ||
            v.descriptor_binding_mask != (v.descriptor_read_mask | v.descriptor_write_mask) ||
            v.dispatch_initiator_required_bits != (1U << 15) ||
            v.push_constant_bytes != gfx11product::kPushBytes || !v.descriptor_table_bytes ||
            v.descriptor_table_bytes > gfx11product::kMaxBindings * gfx11product::kDescriptorBytes ||
            v.descriptor_table_bytes % gfx11product::kDescriptorBytes)
            return fail(error, @"Compiled variant is outside the admitted wave32 buffer profile.", {Status::Unsupported, 0});
        std::vector<uint8_t> code(v.code_bytes);
        const auto copied = pipeline.copyCode(index, code.data(), code.size());
        if (!copied) return fail(error, @"Unable to copy the immutable compiler ISA.", copied);
        gfx11product::Request request{code.data(), code.size(), v.executable_bytes,
            {v.local_size[0], v.local_size[1], v.local_size[2]}, v.wave_size, v.num_sgprs, v.num_vgprs};
        gfx11product::Report report{};
        const auto admitted = gfx11product::validate(request, report);
        if (admitted != gfx11product::Status::Ok) {
            NSString* reason = [NSString stringWithFormat:@"Actual ISA variant %u rejected at byte %u: %s.",
                index, report.byteOffset, gfx11product::statusString(admitted)];
            return fail(error, reason, {Status::Unsupported, 0});
        }
        // The derived launch state proves WGP mode and zero LDS/scratch/shared
        // VGPRs for this exact profile. Compiler metadata never grants access.
        if (report.rsrc1 != v.compute_pgm_rsrc[0] || report.rsrc2 != v.compute_pgm_rsrc[1] ||
            report.rsrc3 != v.compute_pgm_rsrc[2] || report.readMask != v.descriptor_read_mask ||
            report.writeMask != v.descriptor_write_mask)
            return fail(error, @"Actual ISA launch/access requirements disagree with compiler metadata.", {Status::Unsupported, 0});
        for (uint32_t binding = 0; binding < gfx11product::kMaxBindings; ++binding)
            if (((report.readMask | report.writeMask) & (1U << binding)) &&
                (binding + 1) * gfx11product::kDescriptorBytes > v.descriptor_table_bytes)
                return fail(error, @"Actual ISA references a descriptor outside its owned table.", {Status::Protocol, 0});
        decodedReads |= report.readMask; decodedWrites |= report.writeMask;
    }
    uint32_t resourceMask = 0, metalMask = 0;
    for (const auto& resource : resources) {
        if (resource.descriptor_set || resource.binding >= 32 || resource.metal_index >= 31 ||
            (resourceMask & (1U << resource.binding)) || (metalMask & (1U << resource.metal_index)))
            return fail(error, @"Invalid or duplicate compiled buffer binding.", {Status::Protocol, 0});
        const uint32_t bit = 1U << resource.binding;
        const uint32_t access = ((decodedReads & bit) ? 1U : 0U) | ((decodedWrites & bit) ? 2U : 0U);
        if (!access || access != resource.access || (resource.reflected_access & ~3U) ||
            (access & ~resource.reflected_access))
            return fail(error, @"Actual ISA buffer access disagrees with the reflected resource contract.", {Status::Unsupported, 0});
        resourceMask |= bit; metalMask |= 1U << resource.metal_index;
    }
    if (!decodedWrites || resourceMask != (decodedReads | decodedWrites))
        return fail(error, @"Actual ISA and compiled resources do not form the same complete binding set.", {Status::Unsupported, 0});
    return YES;
}
}

@implementation HNMetalComputePipeline {
    id<HNMetalContextOwner> _device;
    HNMetalFunction* _function;
    std::shared_ptr<hackynavi::compute::Context> _context;
    std::shared_ptr<const hackynavi::compute::Shader> _shader;
    std::shared_ptr<const Pipeline> _prototype;
    std::string _acoPath;
    NSString* _label;
    uint32_t _maximum;
}
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                               function:(HNMetalFunction*)function
                                acoPath:(NSString*)absolutePath
          maxTotalThreadsPerThreadgroup:(NSUInteger)cap
                                  label:(nullable NSString*)label
                                  error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    self = [super init];
    if (!self) return nil;
    if (!device || ![device respondsToSelector:@selector(nativeComputeContext)] ||
        ![function isMemberOfClass:[HNMetalFunction class]] || function.device != device ||
        ![absolutePath isKindOfClass:[NSString class]] || (label && ![label isKindOfClass:[NSString class]])) {
        fail(error, @"Expected the actual compute function and its exact existing device.", {Status::Invalid, 0});
        return nil;
    }
    try {
        // Snapshot mutable application strings before starting compiler work.
        NSString* path = [absolutePath copy];
        NSData* pathBytes = [path dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
        if (!path.isAbsolutePath || !pathBytes.length || pathBytes.length > 4096 ||
            std::memchr(pathBytes.bytes, 0, pathBytes.length)) {
            fail(error, @"A bounded absolute UTF-8 path to the real ACO compiler is required.", {Status::Invalid, 0});
            return nil;
        }
        _acoPath.assign(static_cast<const char*>(pathBytes.bytes), pathBytes.length);
        _label = [label copy];
        _context = [device nativeComputeContext]; _shader = [function nativeShader];
        hackynavi::nativeabi::HelloReply info{};
        const auto live = _context ? _context->information(info) : Result{Status::Invalid, 0};
        if (!live || !_shader) {
            fail(error, @"The function needs a live existing native context and actual compiled shader.",
                 live ? Result{Status::Invalid, 0} : live);
            return nil;
        }
        if (!reflectionLimit(*_shader, _maximum)) {
            fail(error, @"Actual AIR reflection is malformed or requires unsupported compute resources.", {Status::Unsupported, 0});
            return nil;
        }
        if (cap && cap < _maximum) _maximum = uint32_t(cap);
        const uint32_t one[3] = {1, 1, 1};
        std::string diagnostic;
        const auto built = _shader->build(one, one, _acoPath, _prototype, diagnostic);
        if (!built) {
            fail(error, compilerText(diagnostic), built); return nil;
        }
        if (!_prototype) {
            fail(error, @"The compiler returned no owned prototype.", {Status::Protocol, 0}); return nil;
        }
        if (!validateProduct(*_prototype, _maximum, error)) return nil;
        // Compilation can take time; do not publish a pipeline bound to a
        // replaced/closed context even though no GPU allocation happened here.
        const auto stillLive = _context->information(info);
        if (!stillLive || [device nativeComputeContext] != _context) {
            fail(error, @"The native context changed or closed during pipeline compilation.",
                 stillLive ? Result{Status::Invalid, 0} : stillLive);
            return nil;
        }
        _device = device; _function = function;
        return self;
    } catch (const std::bad_alloc&) {
        fail(error, @"Unable to retain the actual compiled pipeline.", {Status::OutOfMemory, 0}); return nil;
    }
}
- (BOOL)buildForGrid:(MTLSize)grid
 threadsPerThreadgroup:(MTLSize)local
              pipeline:(std::shared_ptr<const Pipeline>&)out
                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    uint32_t g[3]{}, l[3]{};
    if (out || !dimensions(grid, local, _maximum, g, l))
        return fail(error, @"Expected an empty output and nonzero, bounded dispatch dimensions.", {Status::Invalid, 0});
    hackynavi::nativeabi::HelloReply info{};
    auto live = _context->information(info);
    if (!live || [_device nativeComputeContext] != _context)
        return fail(error, @"The pipeline's exact native context is no longer live.", live ? Result{Status::Invalid, 0} : live);
    try {
        std::shared_ptr<const Pipeline> actual;
        std::string diagnostic;
        const auto built = _shader->build(g, l, _acoPath, actual, diagnostic);
        if (!built) return fail(error, compilerText(diagnostic), built);
        if (!actual) return fail(error, @"The compiler returned no owned product.", {Status::Protocol, 0});
        if (!validateProduct(*actual, _maximum, error)) return NO;
        live = _context->information(info);
        if (!live || [_device nativeComputeContext] != _context)
            return fail(error, @"The native context changed or closed during dispatch compilation.",
                        live ? Result{Status::Invalid, 0} : live);
        out = std::move(actual); return YES;
    } catch (const std::bad_alloc&) {
        return fail(error, @"Unable to retain the actual dispatch pipeline.", {Status::OutOfMemory, 0});
    }
}
- (id<MTLDevice>)device { return _device; }
- (nullable NSString*)label { return _label; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _maximum; }
- (NSUInteger)threadExecutionWidth { return hackynavi::gfx11occupancy::kWaveSize; }
- (NSUInteger)staticThreadgroupMemoryLength { return hackynavi::gfx11occupancy::kStaticThreadgroupMemoryBytes; }
// The retained prototype and future products are CPU-owned bytes. This object
// has zero persistent GPU allocation; real dispatch code/control allocations
// belong to the native submission and cannot be placed in a residency set here.
- (NSUInteger)allocatedSize { return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (MTLShaderValidation)shaderValidation { return MTLShaderValidationDisabled; }
- (MTLResourceID)gpuResourceID { unsupported(@"Pipeline argument-buffer GPU resource IDs are not implemented."); }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions {
    (void)dimensions; unsupported(@"Imageblock compute pipelines are not supported.");
}
- (nullable id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function {
    (void)function; return nil;
}
- (nullable id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor*)descriptor {
    (void)descriptor; return nil;
}
- (nullable id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor*)descriptor {
    (void)descriptor; return nil;
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithAdditionalBinaryFunctions:(NSArray<id<MTLFunction>>*)functions
                                                                                     error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    (void)functions;
    fail(error, @"Adding binary functions is not supported by this compute profile.", {Status::Unsupported, 0}); return nil;
}
- (nullable MTLComputePipelineReflection*)reflection API_AVAILABLE(macos(26.0)) { return nil; }
- (MTLSize)requiredThreadsPerThreadgroup API_AVAILABLE(macos(26.0)) { return MTLSizeMake(0, 0, 0); }
- (nullable id<MTLFunctionHandle>)functionHandleWithName:(NSString*)name API_AVAILABLE(macos(26.0)) {
    (void)name; return nil;
}
- (nullable id<MTLFunctionHandle>)functionHandleWithBinaryFunction:(id<MTL4BinaryFunction>)function API_AVAILABLE(macos(26.0)) {
    (void)function; return nil;
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithBinaryFunctions:(NSArray<id<MTL4BinaryFunction>>*)functions
                                                                          error:(NSError * _Nullable __autoreleasing * _Nullable)error API_AVAILABLE(macos(26.0)) {
    (void)functions;
    fail(error, @"Metal 4 binary-function linking is not supported.", {Status::Unsupported, 0}); return nil;
}
@end
