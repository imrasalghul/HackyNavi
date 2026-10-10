// SPDX-License-Identifier: MIT
#import "HNMetalLibrary.hpp"
#import <CoreFoundation/CoreFoundation.h>
#include "../include/hackynavi_metallib.h"
#include <new>

namespace {
NSError* libraryError(MTLLibraryError code, NSString* message) {
    return [NSError errorWithDomain:MTLLibraryErrorDomain code:code
                          userInfo:@{NSLocalizedDescriptionKey: message}];
}
NSString* text(const char* message) {
    if (!message) return @"Compiler returned no diagnostic.";
    NSString* decoded = [NSString stringWithUTF8String:message];
    return decoded ? decoded : @"Invalid compiler diagnostic encoding.";
}
void invalid(NSString* message) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:message userInfo:nil];
}
bool emptyArray(NSDictionary* reflection, NSString* key) {
    id value = reflection[key];
    return [value isKindOfClass:[NSArray class]] && [value count] == 0;
}
}

@interface HNMetalFunction ()
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                                shader:(std::shared_ptr<const hackynavi::compute::Shader>)shader
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                        graphicsShader:(std::shared_ptr<const hackynavi::compute::GraphicsShader>)shader
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error;
@end

@interface HNMetalLibrary ()
- (nullable HNMetalFunction*)newStageFunctionWithName:(NSString*)name
                                       computeOnly:(BOOL)computeOnly
                                             error:(NSError * _Nullable __autoreleasing * _Nullable)error;
@end

@implementation HNMetalFunction {
    id<HNMetalContextOwner> _device;
    std::shared_ptr<const hackynavi::compute::Shader> _shader;
    std::shared_ptr<const hackynavi::compute::GraphicsShader> _graphicsShader;
    MTLFunctionType _functionType;
    NSString* _name;
    NSString* _label;
}
- (instancetype)init { return nil; }
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                                shader:(std::shared_ptr<const hackynavi::compute::Shader>)shader
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    self = [super init];
    if (!self) return nil;
    if (!device || !shader) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Missing device or compiled shader.");
        return nil;
    }
    const auto& json = shader->reflectionJson();
    NSData* bytes = [NSData dataWithBytes:json.data() length:json.size()];
    id parsed = bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nullptr] : nil;
    NSDictionary* reflection = [parsed isKindOfClass:[NSDictionary class]] ? parsed : nil;
    // Only report an empty constants/attribute contract after checking actual
    // compiler reflection. The compiler separately rejects all unspecialized
    // function constants before lowering; metadata is not execution authority.
    if (!reflection || ![reflection[@"stage"] isEqual:@"Kernel"] ||
        !emptyArray(reflection, @"function_constants") || !emptyArray(reflection, @"vertex_attributes") ||
        !emptyArray(reflection, @"argument_buffer_fields") || reflection[@"tessellation"] != [NSNull null]) {
        if (error) *error = libraryError(MTLLibraryErrorUnsupported,
            @"This compute function requires unsupported constants, attributes or argument-buffer reflection.");
        return nil;
    }
    id bindings = reflection[@"bindings"];
    if (![bindings isKindOfClass:[NSArray class]]) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Compiler omitted resource reflection.");
        return nil;
    }
    uint32_t bufferIndices = 0;
    for (id binding in bindings) {
        if (![binding isKindOfClass:[NSDictionary class]] ||
            binding[@"stage_input_location"] != [NSNull null]) {
            if (error) *error = libraryError(MTLLibraryErrorUnsupported, @"Compute stage-input attributes are not implemented.");
            return nil;
        }
        if ([binding[@"kind"] isEqual:@"Buffer"]) {
            // Actual public buffer table has 31 entries (indices 0..30), as
            // independently measured in the framework descriptor subscript.
            id index = binding[@"metal_index"];
            if (![index isKindOfClass:[NSNumber class]] ||
                CFGetTypeID((__bridge CFTypeRef)index) == CFBooleanGetTypeID() ||
                [index compare:@0] == NSOrderedAscending || [index compare:@30] == NSOrderedDescending ||
                [index compare:@([index unsignedIntValue])] != NSOrderedSame ||
                (bufferIndices & (1U << [index unsignedIntValue]))) {
                if (error) *error = libraryError(MTLLibraryErrorUnsupported, @"Invalid or duplicate public Metal buffer index.");
                return nil;
            }
            bufferIndices |= 1U << [index unsignedIntValue];
        }
    }
    _name = [[NSString alloc] initWithBytes:shader->name().data() length:shader->name().size()
                                 encoding:NSUTF8StringEncoding];
    if (!_name || ![reflection[@"entry_point"] isEqual:_name]) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Compiled function name does not match reflection.");
        return nil;
    }
    _device = device; _shader = std::move(shader); _functionType = MTLFunctionTypeKernel;
    return self;
}
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                        graphicsShader:(std::shared_ptr<const hackynavi::compute::GraphicsShader>)shader
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    self = [super init];
    if (!self) return nil;
    if (!device || !shader || (shader->stage() != HN_SHADER_VERTEX && shader->stage() != HN_SHADER_FRAGMENT)) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Missing device or actual graphics stage.");
        return nil;
    }
    // GraphicsShader publishes only after actual compiler reflection verifies
    // the resource-free or exact readonly buffer0/16-byte fragment profile.
    _name = [[NSString alloc] initWithBytes:shader->name().data() length:shader->name().size()
                                 encoding:NSUTF8StringEncoding];
    if (!_name) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Invalid graphics function name encoding.");
        return nil;
    }
    _functionType = shader->stage() == HN_SHADER_VERTEX ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
    _device = device; _graphicsShader = std::move(shader);
    return self;
}
- (id<MTLDevice>)device { return _device; }
- (NSString*)name { return _name; }
- (MTLFunctionType)functionType { return _functionType; }
- (MTLPatchType)patchType { return MTLPatchTypeNone; }
- (NSInteger)patchControlPointCount { return -1; }
- (nullable NSArray<MTLVertexAttribute*>*)vertexAttributes { return nil; }
- (nullable NSArray<MTLAttribute*>*)stageInputAttributes { return nil; }
- (NSDictionary<NSString*, MTLFunctionConstant*>*)functionConstantsDictionary { return @{}; }
- (MTLFunctionOptions)options { return MTLFunctionOptionNone; }
- (nullable NSString*)label { @synchronized(self) { return _label; } }
- (void)setLabel:(nullable NSString*)value { @synchronized(self) { _label = [value copy]; } }
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index {
    (void)index; invalid(@"Argument buffers are not supported by this function."); return nil;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
                                                reflection:(MTLAutoreleasedArgument * _Nullable)reflection {
    if (reflection) *reflection = nil;
    return [self newArgumentEncoderWithBufferIndex:index];
}
#pragma clang diagnostic pop
- (std::shared_ptr<const hackynavi::compute::Shader>)nativeShader { return _shader; }
- (std::shared_ptr<const hackynavi::compute::GraphicsShader>)nativeGraphicsShader { return _graphicsShader; }
@end

@implementation HNMetalLibrary {
    id<HNMetalContextOwner> _device;
    std::shared_ptr<HNMetallibResult> _library;
    hackynavi::compute::Shader::Tools _compilerTools;
    NSDictionary<NSString*, NSNumber*>* _indices;
    NSArray<NSString*>* _names;
    NSString* _label;
}
- (instancetype)init { return nil; }
- (nullable instancetype)initWithDevice:(id<HNMetalContextOwner>)device
                          metallibData:(NSData*)data
                         compilerTools:(const hackynavi::compute::Shader::Tools&)tools
                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    self = [super init];
    if (!self) return nil;
    if (!device || ![device respondsToSelector:@selector(nativeComputeContext)] || !data.length ||
        data.length > 64U * 1024U * 1024U || hn_metallib_abi_version() != 1) {
        if (error) *error = libraryError(MTLLibraryErrorUnsupported, @"Invalid device, library size or container ABI.");
        return nil;
    }
    const auto validTool = [](const std::string& path) {
        return !path.empty() && path.front() == '/' && path.size() <= 4096 &&
               path.find('\0') == std::string::npos;
    };
    if (!validTool(tools.airDisassembler) || !validTool(tools.spirvValidator)) {
        if (error) *error = libraryError(MTLLibraryErrorUnsupported, @"Explicit absolute AIR and SPIR-V compiler helpers are required.");
        return nil;
    }
    const auto context = [device nativeComputeContext];
    hackynavi::nativeabi::HelloReply info{};
    if (!context || !context->information(info)) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Device has no live native compute context.");
        return nil;
    }
    try {
        _compilerTools = tools; // Independent immutable copy, never process-global configuration.
        _library = {hn_metallib_parse(static_cast<const uint8_t*>(data.bytes), data.length), hn_metallib_free};
        const char* message = hn_metallib_error(_library.get());
        if (!_library || !message || *message) {
            if (error) *error = libraryError(MTLLibraryErrorCompileFailure, text(message));
            return nil;
        }
        const uint32_t count = hn_metallib_function_count(_library.get());
        if (!count || count > 4096) {
            if (error) *error = libraryError(MTLLibraryErrorInternal, @"Invalid owned library function count.");
            return nil;
        }
        NSMutableArray<NSString*>* names = [[NSMutableArray alloc] initWithCapacity:count];
        NSMutableDictionary<NSString*, NSNumber*>* indices = [[NSMutableDictionary alloc] initWithCapacity:count];
        if (!names || !indices) {
            if (error) *error = libraryError(MTLLibraryErrorInternal, @"Unable to allocate library metadata.");
            return nil;
        }
        for (uint32_t n = 0; n < count; ++n) {
            HNMetallibFunctionInfo function{};
            if (hn_metallib_function_info(_library.get(), n, &function, sizeof(function)) != 1 ||
                function.struct_bytes != sizeof(function) || function.name_bytes < 2 || function.name_bytes > 1025 ||
                function.stage > HN_SHADER_FRAGMENT || function.reserved[0] || function.reserved[1]) {
                if (error) *error = libraryError(MTLLibraryErrorInternal, @"Invalid owned function metadata.");
                return nil;
            }
            std::vector<char> buffer(function.name_bytes);
            if (hn_metallib_copy_name(_library.get(), n, buffer.data(), buffer.size()) != 1 || buffer.back() != 0) {
                if (error) *error = libraryError(MTLLibraryErrorInternal, @"Unable to copy function name.");
                return nil;
            }
            NSString* name = [[NSString alloc] initWithBytes:buffer.data() length:buffer.size() - 1 encoding:NSUTF8StringEncoding];
            if (!name || indices[name]) {
                if (error) *error = libraryError(MTLLibraryErrorInternal, @"Invalid or duplicate function name.");
                return nil;
            }
            [names addObject:name]; indices[name] = @(n);
        }
        _names = [names copy]; _indices = [indices copy]; _device = device;
        if (!_names || !_indices) {
            if (error) *error = libraryError(MTLLibraryErrorInternal, @"Unable to retain library metadata.");
            return nil;
        }
        return self;
    } catch (const std::bad_alloc&) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Unable to allocate library storage.");
        return nil;
    }
}
- (id<MTLDevice>)device { return _device; }
- (NSArray<NSString*>*)functionNames { return _names; }
- (MTLLibraryType)type { return MTLLibraryTypeExecutable; }
- (nullable NSString*)installName { return nil; }
- (nullable NSString*)label { @synchronized(self) { return _label; } }
- (void)setLabel:(nullable NSString*)value { @synchronized(self) { _label = [value copy]; } }
- (nullable HNMetalFunction*)newComputeFunctionWithName:(NSString*)name
                                                 error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    return [self newStageFunctionWithName:name computeOnly:YES error:error];
}
- (nullable HNMetalFunction*)newStageFunctionWithName:(NSString*)name
                                             error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    return [self newStageFunctionWithName:name computeOnly:NO error:error];
}
- (nullable HNMetalFunction*)newStageFunctionWithName:(NSString*)name
                                       computeOnly:(BOOL)computeOnly
                                             error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    // Snapshot mutable NSString input before lookup or compiler invocation.
    NSString* requested = [name copy];
    NSNumber* index = requested ? _indices[requested] : nil;
    if (!index) {
        if (error) *error = libraryError(MTLLibraryErrorFunctionNotFound, @"Function is not present in this library.");
        return nil;
    }
    try {
        HNMetallibFunctionInfo function{};
        if (hn_metallib_function_info(_library.get(), index.unsignedIntValue, &function, sizeof(function)) != 1 ||
            !function.air_bytes || function.air_bytes > 64U * 1024U * 1024U) {
            if (error) *error = libraryError(MTLLibraryErrorInternal, @"Unable to read owned function metadata.");
            return nil;
        }
        if (function.stage > HN_SHADER_FRAGMENT || (computeOnly && function.stage != HN_SHADER_COMPUTE)) {
            if (error) *error = libraryError(MTLLibraryErrorUnsupported, @"Requested function is not a supported stage for this operation.");
            return nil;
        }
        std::vector<uint8_t> air(function.air_bytes);
        if (hn_metallib_copy_air(_library.get(), index.unsignedIntValue, air.data(), air.size()) != 1) {
            if (error) *error = libraryError(MTLLibraryErrorInternal, @"Unable to copy owned function AIR.");
            return nil;
        }
        const char* entry = requested.UTF8String;
        if (!entry) {
            if (error) *error = libraryError(MTLLibraryErrorInternal, @"Function name is not UTF-8.");
            return nil;
        }
        std::string diagnostic;
        if (function.stage != HN_SHADER_COMPUTE) {
            std::shared_ptr<const hackynavi::compute::GraphicsShader> graphics;
            auto result = hackynavi::compute::GraphicsShader::compile(air.data(), air.size(), HN_SHADER_AIR,
                static_cast<HNShaderStage>(function.stage), entry, _compilerTools, graphics, diagnostic);
            if (!result && result.status == hackynavi::compute::Status::Unsupported &&
                function.stage == HN_SHADER_VERTEX) {
                result = hackynavi::compute::GraphicsShader::compileVertexBuffer(
                    air.data(), air.size(), HN_SHADER_AIR, entry, _compilerTools, graphics, diagnostic);
            }
            if (!result && result.status == hackynavi::compute::Status::Unsupported &&
                function.stage == HN_SHADER_FRAGMENT) {
                // A second explicit admission path still invokes the actual
                // compiler and exact reflection checks; failure never relaxes
                // the old resource-free profile or fabricates a resource flag.
                result = hackynavi::compute::GraphicsShader::compileFragmentBuffer(
                    air.data(), air.size(), HN_SHADER_AIR, entry, _compilerTools, graphics, diagnostic);
            }
            if (!result) {
                if (error) *error = libraryError(MTLLibraryErrorCompileFailure,
                    diagnostic.empty() ? @"Graphics function compilation failed." : text(diagnostic.c_str()));
                return nil;
            }
            return [[HNMetalFunction alloc] initWithDevice:_device graphicsShader:std::move(graphics) error:error];
        }
        std::shared_ptr<const hackynavi::compute::Shader> shader;
        auto result = hackynavi::compute::Shader::compile(air.data(), air.size(), HN_SHADER_AIR, entry, _compilerTools, shader, diagnostic);
        if (!result) {
            if (error) *error = libraryError(MTLLibraryErrorCompileFailure,
                diagnostic.empty() ? @"Compute function compilation failed." : text(diagnostic.c_str()));
            return nil;
        }
        return [[HNMetalFunction alloc] initWithDevice:_device shader:std::move(shader) error:error];
    } catch (const std::bad_alloc&) {
        if (error) *error = libraryError(MTLLibraryErrorInternal, @"Unable to allocate function storage.");
        return nil;
    }
}
- (nullable id<MTLFunction>)newFunctionWithName:(NSString*)name {
    return [self newStageFunctionWithName:name error:nullptr];
}
- (nullable id<MTLFunction>)newFunctionWithName:(NSString*)name constantValues:(MTLFunctionConstantValues*)values
                                         error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (values) {
        if (error) *error = libraryError(MTLLibraryErrorUnsupported, @"Function-constant specialization is not implemented.");
        return nil;
    }
    return [self newStageFunctionWithName:name error:error];
}
- (void)newFunctionWithName:(NSString*)name constantValues:(MTLFunctionConstantValues*)values
         completionHandler:(void (^)(id<MTLFunction> _Nullable, NSError* _Nullable))handler {
    if (!handler) invalid(@"A function completion handler is required.");
    NSString* requested = [name copy];
    const BOOL hasValues = values != nil;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSError* error = nil;
            id<MTLFunction> function = nil;
            if (hasValues) error = libraryError(MTLLibraryErrorUnsupported, @"Function-constant specialization is not implemented.");
            else function = [self newStageFunctionWithName:requested error:&error];
            handler(function, error);
        }
    });
}
- (nullable id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor*)descriptor
                                               error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    if (error) *error = nil;
    MTLFunctionDescriptor* snapshot = [descriptor copy];
    if (!snapshot || [snapshot isKindOfClass:[MTLIntersectionFunctionDescriptor class]] ||
        snapshot.constantValues || snapshot.specializedName || snapshot.options != MTLFunctionOptionNone ||
        snapshot.binaryArchives.count) {
        if (error) *error = libraryError(MTLLibraryErrorUnsupported,
            @"Function specialization, binary archives, visible and intersection functions are not implemented.");
        return nil;
    }
    return [self newStageFunctionWithName:snapshot.name error:error];
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor*)descriptor
                completionHandler:(void (^)(id<MTLFunction> _Nullable, NSError* _Nullable))handler {
    if (!handler) invalid(@"A function completion handler is required.");
    MTLFunctionDescriptor* snapshot = [descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSError* error = nil;
            id<MTLFunction> function = [self newFunctionWithDescriptor:snapshot error:&error];
            handler(function, error);
        }
    });
}
- (nullable id<MTLFunction>)newIntersectionFunctionWithDescriptor:(MTLIntersectionFunctionDescriptor*)descriptor
                                                           error:(NSError * _Nullable __autoreleasing * _Nullable)error {
    (void)descriptor;
    if (error) *error = libraryError(MTLLibraryErrorUnsupported, @"Intersection functions are not implemented.");
    return nil;
}
- (void)newIntersectionFunctionWithDescriptor:(MTLIntersectionFunctionDescriptor*)descriptor
                            completionHandler:(void (^)(id<MTLFunction> _Nullable, NSError* _Nullable))handler {
    (void)descriptor;
    if (!handler) invalid(@"A function completion handler is required.");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { handler(nil, libraryError(MTLLibraryErrorUnsupported, @"Intersection functions are not implemented.")); }
    });
}
- (nullable MTLFunctionReflection*)reflectionForFunctionWithName:(NSString*)name API_AVAILABLE(macos(26.0)) {
    (void)name; return nil; // New SDK26 reflection objects are not advertised on macOS15.
}
@end
