// SPDX-License-Identifier: MIT
#import "HNMetalDevice.hpp"
#include "HNMetalCapabilities.hpp"
#import "HNMetalArchitecture.hpp"
#import "HNMetalTexture.hpp"
#import "HNMetalRenderPipeline.hpp"
#include "HNMetalDeviceContract.hpp"
#import <objc/runtime.h>
#include <os/log.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <atomic>

// These exact measured encodings are used only by typed unsupported methods.
// No FP/profile/architecture values or private compiler objects are created.
typedef struct { unsigned int first, second; } HNMetalPairABI;
struct IndirectArgumentBufferCapabilities {
    unsigned int first:1, second:1, third:1, reserved:29;
};
struct MTLTargetDeviceArch {
    unsigned long long architecture;
    unsigned int version;
    char* name;
};
struct MTLCompilerConnectionManager;

namespace {
using hackynavi::compute::Context;
using hackynavi::compute::Result;
using hackynavi::compute::Status;
constexpr NSUInteger kLibraryBytes = 64U * 1024U * 1024U;

NSError* deviceError(NSString* text, Result result = {Status::Unsupported, 0}) {
    return [NSError errorWithDomain:@"org.hackynavi.Metal.Device" code:NSInteger(result.status)
        userInfo:@{NSLocalizedDescriptionKey: text, @"IOReturn": @(result.iokit)}];
}
BOOL fail(NSError* __autoreleasing* error, NSString* text, Result result = {Status::Unsupported, 0}) {
    if (error) *error = deviceError(text, result);
    return NO;
}
// Discovery callers do not receive NSError from the principal initializer.
// Preserve the first actual failure there, with bounded process-wide output.
// Successful admission and ordinary unsupported API calls produce no logs.
void logAdmissionFailure(const char* stage, io_service_t accelerator, NSError* error) {
    static std::atomic<unsigned> reports{0};
    unsigned index = reports.load(std::memory_order_relaxed);
    while (index < 32 && !reports.compare_exchange_weak(index, index + 1,
                                                       std::memory_order_relaxed)) {}
    if (index >= 32) return;
    const char* reason = error.localizedDescription.UTF8String;
    const auto iokit = [error.userInfo[@"IOReturn"] unsignedIntValue];
    char message[2048]{};
    std::snprintf(message, sizeof(message),
        "stage=%.64s accelerator-port=0x%x error-present=%u status=%lld IOReturn=0x%x reason=%.1720s%s",
        stage, unsigned(accelerator), unsigned(error != nil), static_cast<long long>(error.code), unsigned(iokit),
        reason ? reason : "No error detail was returned.", index == 31 ? " [report limit reached]" : "");
    std::fprintf(stderr, "HackyNavi Metal admission: %s\n", message);
    os_log_error(OS_LOG_DEFAULT, "HackyNavi Metal admission: %{public}s", message);
}
NSString* abiFailure(const char* selector, const char* expected, const char* actual,
                     const char* implementation = nullptr) {
    char detail[1984]{};
    std::snprintf(detail, sizeof(detail),
        "Metal ABI mismatch: selector=%.160s expected-bytes=%zu expected=%.512s "
        "framework-bytes=%zu framework=%.512s implementation-bytes=%zu implementation=%.512s",
        selector ? selector : "<missing>", expected ? std::strlen(expected) : 0,
        expected ? expected : "<no matching required selector>", actual ? std::strlen(actual) : 0,
        actual ? actual : "<missing>", implementation ? std::strlen(implementation) : 0,
        implementation ? implementation : "<missing>");
    return [NSString stringWithUTF8String:detail];
}
[[noreturn]] void unsupported(NSString* text) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException reason:text userInfo:nil];
}
struct Port {
    io_object_t value = IO_OBJECT_NULL;
    ~Port() { if (value) IOObjectRelease(value); }
    Port() = default;
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;
    io_object_t take() { const auto result = value; value = IO_OBJECT_NULL; return result; }
};
struct File {
    int value = -1;
    ~File() { if (value >= 0) ::close(value); }
};
BOOL exactClass(io_object_t object, const char* name) {
    io_name_t actual{};
    return object && IOObjectGetClass(object, actual) == KERN_SUCCESS && !std::strcmp(actual, name);
}
BOOL relationship(io_service_t accelerator, Port& provider, uint64_t& acceleratorID,
                  uint64_t& ownerID, NSError* __autoreleasing* error) {
    if (!exactClass(accelerator, "HackyNaviAccelerator") ||
        !IOObjectConformsTo(accelerator, "IOAccelerator") ||
        IOObjectConformsTo(accelerator, "IOServiceCompatibility"))
        return fail(error, @"Expected the actual HackyNavi IOAccelerator service.", {Status::Invalid, 0});
    const auto parentStatus = IORegistryEntryGetParentEntry(accelerator, kIOServicePlane, &provider.value);
    if (parentStatus != KERN_SUCCESS || !exactClass(provider.value, "HackyNaviDevice"))
        return fail(error, @"The immediate IOService provider must be HackyNaviDevice.", {Status::Invalid, parentStatus});
    const auto acceleratorStatus = IORegistryEntryGetRegistryEntryID(accelerator, &acceleratorID);
    const auto ownerStatus = IORegistryEntryGetRegistryEntryID(provider.value, &ownerID);
    if (acceleratorStatus != KERN_SUCCESS || ownerStatus != KERN_SUCCESS ||
        !acceleratorID || !ownerID || acceleratorID == ownerID)
        return fail(error, @"Accelerator and native owner must have distinct real registry identities.",
                    {Status::Invalid, acceleratorStatus != KERN_SUCCESS ? acceleratorStatus : ownerStatus});
    CFTypeRef property = IORegistryEntryCreateCFProperty(accelerator, CFSTR("HackyNaviOwnerRegistryID"),
                                                        kCFAllocatorDefault, 0);
    int64_t claimed = 0;
    const bool exact = property && CFGetTypeID(property) == CFNumberGetTypeID() &&
        !CFNumberIsFloatType(static_cast<CFNumberRef>(property)) &&
        CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberSInt64Type, &claimed) &&
        claimed > 0 && uint64_t(claimed) == ownerID;
    if (property) CFRelease(property);
    return exact ? YES : fail(error, @"HackyNaviOwnerRegistryID does not identify the immediate provider.",
                               {Status::Invalid, 0});
}
BOOL nativeMacOSProcess(NSError* __autoreleasing* error) {
    // Actual dyld metadata API, declared uint32_t(void) by Apple's dyld_priv.h.
    // The measured CF _CFMZEnabled returns true for IOS(2)/MACCATALYST(6).
    // Admit only native MACOS(1), so discovery cannot request the unsupported
    // allowLibrariesFromOtherPlatforms policy after opening our native owner.
    using ActivePlatform = uint32_t (*)(void);
    void* symbol = dlsym(RTLD_DEFAULT, "dyld_get_active_platform");
    const uint32_t platform = symbol ? reinterpret_cast<ActivePlatform>(symbol)() : 0;
    if (!symbol || platform != 1U)
        return fail(error, [NSString stringWithFormat:@"Native macOS process required: dyld platform API present=%u expected=1 actual=%u.", unsigned(symbol != nullptr), platform]);
    return YES;
}
BOOL frameworkLimitsContract(NSError* __autoreleasing* error) {
    Class base = objc_getClass("_MTLDevice");
    Method method = base ? class_getInstanceMethod(base, sel_registerName("limits")) : nullptr;
    const char* actual = method ? method_getTypeEncoding(method) : nullptr;
    char expected[512]{};
    const auto length = std::snprintf(expected, sizeof(expected), "%s16@0:8", @encode(const HNMetalDeviceLimits*));
    if (length <= 0 || size_t(length) >= sizeof(expected))
        return fail(error, [NSString stringWithFormat:@"Metal limits expected full encoding length=%d exceeds admission capacity=%zu. %@", length, sizeof(expected), abiFailure("limits", expected, actual)]);
    if (!actual || std::strcmp(actual, expected))
        return fail(error, abiFailure("limits", expected, actual));
    return YES;
}
BOOL adoptFrameworkProtocol(NSError* __autoreleasing* error) {
    static dispatch_once_t once;
    static BOOL ready = NO;
    static NSString* firstFailure = nil;
    dispatch_once(&once, ^{
        Class type = [HNMetalDevice class];
        Protocol* spi = NSProtocolFromString(@"MTLDeviceSPI");
        Protocol* publicProtocol = NSProtocolFromString(@"MTLDevice");
        if (!spi || !publicProtocol || class_getSuperclass(type) != objc_getClass("_MTLDevice") ||
            !class_conformsToProtocol(type, publicProtocol)) {
            firstFailure = [NSString stringWithFormat:@"Metal protocol prerequisites failed: SPI=%u public=%u actual-superclass=%s expected-superclass=_MTLDevice public-conformance=%u.",
                unsigned(spi != nil), unsigned(publicProtocol != nil),
                class_getSuperclass(type) ? class_getName(class_getSuperclass(type)) : "<missing>",
                unsigned(publicProtocol && class_conformsToProtocol(type, publicProtocol))];
            return;
        }
        unsigned int count = 0;
        auto* actual = protocol_copyMethodDescriptionList(spi, YES, YES, &count);
        constexpr size_t expectedCount = sizeof(kHNMetalSPIContract) / sizeof(kHNMetalSPIContract[0]);
        bool valid = actual && count == expectedCount;
        if (!valid) firstFailure = [NSString stringWithFormat:@"Metal MTLDeviceSPI required instance method count mismatch: expected=%zu actual=%u list-present=%u.",
            expectedCount, count, unsigned(actual != nullptr)];
        bool seen[expectedCount]{};
        for (unsigned int n = 0; valid && n < count; ++n) {
            const char* name = actual[n].name ? sel_getName(actual[n].name) : nullptr;
            const HNMetalMethodContract* expected = nullptr;
            for (size_t index = 0; index < expectedCount; ++index) {
                const auto& entry = kHNMetalSPIContract[index];
                if (name && !std::strcmp(name, entry.selector)) {
                    if (!seen[index]) { expected = &entry; seen[index] = true; }
                    break;
                }
            }
            Method implementation = actual[n].name ? class_getInstanceMethod(type, actual[n].name) : nullptr;
            const char* encoding = implementation ? method_getTypeEncoding(implementation) : nullptr;
            valid = expected && actual[n].types && !std::strcmp(expected->encoding, actual[n].types) &&
                    encoding && !std::strcmp(encoding, expected->encoding);
            if (!valid) firstFailure = abiFailure(name, expected ? expected->encoding : nullptr,
                                                 actual[n].types, encoding);
        }
        std::free(actual);
        unsigned int classCount = 0;
        auto* requiredClassMethods = protocol_copyMethodDescriptionList(spi, YES, NO, &classCount);
        std::free(requiredClassMethods);
        if (!valid || classCount) {
            if (!firstFailure) firstFailure = [NSString stringWithFormat:@"Metal MTLDeviceSPI required class method count mismatch: expected=0 actual=%u.", classCount];
            return;
        }
        // Adopt the real existing protocol only after every required method is
        // implemented with its exact measured ABI. No empty substitute protocol
        // or generic success-returning implementation is created here.
        if (!class_conformsToProtocol(type, spi) && !class_addProtocol(type, spi)) {
            firstFailure = @"Metal MTLDeviceSPI exact ABI matched, but Objective-C runtime protocol adoption failed.";
            return;
        }
        ready = class_conformsToProtocol(type, spi);
        if (!ready) firstFailure = @"Metal MTLDeviceSPI conformance is absent after runtime protocol adoption.";
    });
    return ready ? YES : fail(error, firstFailure ? firstFailure : @"Metal MTLDeviceSPI admission failed without method metadata.");
}
NSString* executablePath(NSString* input, NSError* __autoreleasing* error) {
    if (![input isKindOfClass:[NSString class]] || !input.isAbsolutePath || !input.length ||
        [input rangeOfString:[NSString stringWithFormat:@"%C", unichar(0)]].location != NSNotFound) {
        fail(error, @"Expected an absolute compiler-helper executable path.", {Status::Invalid, 0}); return nil;
    }
    NSString* path = input.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    const char* fs = path.fileSystemRepresentation;
    struct stat info{};
    if (!fs || std::strlen(fs) > 4096 || ::stat(fs, &info) || !S_ISREG(info.st_mode) || ::access(fs, X_OK)) {
        fail(error, @"Each compiler helper must be an existing executable regular file.", {Status::Invalid, 0}); return nil;
    }
    return [path copy];
}
BOOL compilerTools(const hackynavi::compute::Shader::Tools& input,
                   hackynavi::compute::Shader::Tools& output, NSError* __autoreleasing* error) {
    const auto path = [&](const std::string& value) -> NSString* {
        if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos) {
            fail(error, @"A compiler helper has an invalid path.", {Status::Invalid, 0}); return nil;
        }
        NSString* string = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
        return executablePath(string, error);
    };
    NSString* air = path(input.airDisassembler);
    if (!air || !air.UTF8String) return fail(error,
        [NSString stringWithFormat:@"apple-air-dis helper rejected: %@", error && *error ? (*error).localizedDescription : @"no UTF-8 path"], {Status::Invalid, 0});
    NSString* spirv = path(input.spirvValidator);
    if (!spirv || !spirv.UTF8String) return fail(error,
        [NSString stringWithFormat:@"spirv-val helper rejected: %@", error && *error ? (*error).localizedDescription : @"no UTF-8 path"], {Status::Invalid, 0});
    try {
        hackynavi::compute::Shader::Tools candidate{air.UTF8String, spirv.UTF8String};
        output = std::move(candidate);
        return YES;
    } catch (const std::bad_alloc&) {
        return fail(error, @"Unable to retain compiler-helper paths.", {Status::OutOfMemory, 0});
    }
}
NSData* readLibrary(NSURL* url, NSError* __autoreleasing* error) {
    if (![url isKindOfClass:[NSURL class]] || !url.isFileURL || !url.path.length) {
        fail(error, @"A metallib URL must identify a local file.", {Status::Invalid, 0}); return nil;
    }
    const char* path = url.fileSystemRepresentation;
    if (!path) { fail(error, @"The metallib URL has no filesystem representation.", {Status::Invalid, 0}); return nil; }
    File file;
    // Avoid blocking on a FIFO before fstat can reject nonregular input.
    file.value = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat info{};
    if (file.value < 0 || ::fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        uint64_t(info.st_size) > kLibraryBytes) {
        fail(error, @"The metallib must be a nonempty regular file of at most 64 MiB.", {Status::Invalid, 0}); return nil;
    }
    NSMutableData* copy = [NSMutableData dataWithLength:NSUInteger(info.st_size)];
    if (!copy) { fail(error, @"Unable to allocate the metallib snapshot.", {Status::OutOfMemory, 0}); return nil; }
    size_t offset = 0;
    while (offset < copy.length) {
        const auto count = ::read(file.value, static_cast<uint8_t*>(copy.mutableBytes) + offset, copy.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { fail(error, @"The metallib file could not be read completely.", {Status::Invalid, 0}); return nil; }
        offset += size_t(count);
    }
    return copy;
}
// Catch failures in asynchronous creation, not exceptions from the caller's
// completion handler. The handler is invoked once, outside this boundary.
id<MTLComputePipelineState> createAsyncPipeline(
    id<MTLComputePipelineState> (^create)(NSError* __autoreleasing*), NSError* __autoreleasing* error) {
    @try {
        try { return create(error); }
        catch (const std::bad_alloc&) { fail(error, @"Unable to allocate pipeline metadata.", {Status::OutOfMemory, 0}); }
        catch (const std::exception&) { fail(error, @"Pipeline construction raised a native compiler exception."); }
    } @catch (NSException* exception) {
        (void)exception;
        fail(error, @"Pipeline construction rejected the descriptor or function.", {Status::Invalid, 0});
    }
    return nil;
}
}

@implementation HNMetalDevice {
    std::shared_ptr<Context> _context;
    io_service_t _accelerator;
    io_service_t _owner;
    uint64_t _acceleratorID;
    uint64_t _ownerID;
    hackynavi::metal::Capabilities _capabilities;
    HNMetalArchitecture* _architecture;
    HNMetalDeviceLimits _limits;
    bool _limitsInitialized;
    hackynavi::metal::CapabilityQueries _featureQueries;
    bool _featureQueriesInitialized;
    NSString* _acoPath;
    hackynavi::compute::Shader::Tools _compilerTools;
    id _acceleratorService;
}
// NS_UNAVAILABLE alone does not stop runtime init/new messages.
- (instancetype)init { return nil; }
- (nullable instancetype)initWithAcceleratorPort:(io_service_t)accelerator {
    NSBundle* bundle = [NSBundle bundleForClass:[HNMetalDevice class]];
    if (![bundle.bundleURL.lastPathComponent isEqualToString:@"HackyNaviMetal.bundle"]) {
        logAdmissionFailure("bundle-identity", accelerator,
            deviceError(@"The principal class was not loaded from HackyNaviMetal.bundle.", {Status::Invalid, 0}));
        return nil;
    }
    NSURL* contents = [bundle.bundleURL URLByAppendingPathComponent:@"Contents" isDirectory:YES];
    // Resolving symlinks must not escape the plugin. No user environment path
    // or executable from another bundle is accepted by the loader entry point.
    NSString* root = [contents.path.stringByStandardizingPath.stringByResolvingSymlinksInPath
                      stringByAppendingString:@"/"];
    NSError* error = nil;
    const auto bundled = [&](NSString* name) -> NSString* {
        NSURL* helper = [contents URLByAppendingPathComponent:[@"Helpers/" stringByAppendingString:name] isDirectory:NO];
        NSString* checked = executablePath(helper.path, &error);
        if (!checked) return nil;
        if (!root.length || ![checked hasPrefix:root]) {
            fail(&error, @"A resolved compiler helper escapes the actual bundle Contents directory.", {Status::Invalid, 0});
            return nil;
        }
        return checked;
    };
    NSString* aco = bundled(@"hackynavi-aco");
    if (!aco) { logAdmissionFailure("bundled-hackynavi-aco", accelerator, error); return nil; }
    NSString* air = bundled(@"apple-air-dis");
    if (!air || !air.UTF8String) { logAdmissionFailure("bundled-apple-air-dis", accelerator, error); return nil; }
    NSString* spirv = bundled(@"spirv-val");
    if (!spirv || !spirv.UTF8String) { logAdmissionFailure("bundled-spirv-val", accelerator, error); return nil; }
    try {
        hackynavi::compute::Shader::Tools tools{air.UTF8String, spirv.UTF8String};
        return [self initWithAcceleratorPort:accelerator acoPath:aco compilerTools:tools error:&error];
    } catch (const std::bad_alloc&) {
        logAdmissionFailure("bundled-compiler-tools", accelerator,
            deviceError(@"Unable to retain bundled compiler helper paths.", {Status::OutOfMemory, 0}));
        return nil;
    }
}
- (nullable instancetype)initWithAcceleratorPort:(io_service_t)accelerator
                                        acoPath:(NSString*)absolutePath
                                  compilerTools:(const hackynavi::compute::Shader::Tools&)tools
                                          error:(NSError* __autoreleasing*)error {
    NSError* __autoreleasing localError = nil;
    if (!error) error = &localError;
    if (error) *error = nil;
    const auto rejected = [&](const char* stage) -> HNMetalDevice* {
        logAdmissionFailure(stage, accelerator, *error); return nil;
    };
    NSString* helper = executablePath(absolutePath, error);
    hackynavi::compute::Shader::Tools checkedTools;
    if (!helper) return rejected("compiler-aco-path");
    if (!compilerTools(tools, checkedTools, error)) return rejected("compiler-tools");
    if (!frameworkLimitsContract(error)) return rejected("framework-limits");
    if (!nativeMacOSProcess(error)) return rejected("native-platform");
    Port provider;
    uint64_t acceleratorID = 0, ownerID = 0;
    if (!relationship(accelerator, provider, acceleratorID, ownerID, error)) return rejected("accelerator-relationship");
    // MTLAddDevice requires the actual private protocol. Its required methods
    // and exact encodings must all match before the native owner can be opened.
    if (!adoptFrameworkProtocol(error)) return rejected("framework-device-spi");
    self = [super init];
    if (!self) { fail(error, @"The actual Metal superclass could not initialize.", {Status::OutOfMemory, 0}); return rejected("superclass-init"); }
    const auto retained = IOObjectRetain(accelerator);
    if (retained != KERN_SUCCESS) { fail(error, @"Unable to retain the real accelerator service.", {Status::Transport, retained}); return rejected("accelerator-retain"); }
    _accelerator = accelerator;
    _owner = provider.take();
    auto result = Context::openVertexBufferedGraphicsAccelerator(accelerator, acceleratorID, ownerID, _context);
    if (!result) { fail(error, @"The exact accelerator could not open its native owner's reusable buffer-graphics profile.", result); return rejected("native-context-open"); }
    result = hackynavi::metal::Capabilities::capture(_context, ownerID, _capabilities);
    if (!result) { fail(error, @"The opened native context did not admit the required buffer-graphics capabilities.", result); return rejected("native-capabilities"); }
    _architecture = [[HNMetalArchitecture alloc] initWithCapabilities:_capabilities];
    if (!_architecture) { fail(error, @"Unable to retain the actual admitted GPU architecture.", {Status::OutOfMemory, 0}); return rejected("native-architecture"); }
    // Recheck the immediate relationship after the potentially blocking open.
    Port again;
    uint64_t observedAccelerator = 0, observedOwner = 0;
    if (!relationship(accelerator, again, observedAccelerator, observedOwner, error)) return rejected("relationship-recheck");
    if (!IOObjectIsEqualTo(_owner, again.value) || observedAccelerator != acceleratorID || observedOwner != ownerID) {
        fail(error, @"The accelerator/provider identity changed while opening the native owner.", {Status::DeviceLost, 0});
        return rejected("identity-recheck");
    }
    _acceleratorID = acceleratorID;
    _ownerID = ownerID;
    _acoPath = helper;
    _compilerTools = std::move(checkedTools);
    return self;
}
- (void)dealloc {
    if (_context) (void)_context->close();
    if (_owner) IOObjectRelease(_owner);
    if (_accelerator) IOObjectRelease(_accelerator);
}
- (std::shared_ptr<Context>)nativeComputeContext {
    hackynavi::nativeabi::HelloReply current{};
    if (!_context || !_context->information(current) || current.registryId != _ownerID ||
        current.header.epoch != _capabilities.epoch()) return {};
    return _context;
}
- (void)initLimits {
    @synchronized(self) {
        if (_limitsInitialized) return; // The admitted snapshot is immutable.
        HNMetalDeviceLimits actual{};
        if (!_capabilities.makeLimits(actual)) unsupported(@"Actual native limits are unavailable.");
        _limits = actual;
        _limitsInitialized = true;
    }
}
- (void)initFeatureQueries {
    @synchronized(self) {
        if (_featureQueriesInitialized) return;
        hackynavi::metal::CapabilityQueries actual{};
        if (!_capabilities.makeFeatureQueries(actual)) unsupported(@"Actual native feature queries are unavailable.");
        _featureQueries = actual;
        _featureQueriesInitialized = true;
    }
}
- (BOOL)supportsReadWriteBufferArguments {
    @synchronized(self) {
        if (!_featureQueriesInitialized) unsupported(@"Initialize native feature queries before using them.");
        return _featureQueries.readWriteBufferArguments;
    }
}
- (BOOL)supportsNonUniformThreadgroupSize {
    @synchronized(self) {
        if (!_featureQueriesInitialized) unsupported(@"Initialize native feature queries before using them.");
        return _featureQueries.nonUniformThreadgroupSize;
    }
}
- (const HNMetalDeviceLimits*)limits {
    @synchronized(self) {
        if (!_limitsInitialized) unsupported(@"Initialize the actual native limits before querying them.");
        return &_limits;
    }
}
- (void)_setAcceleratorService:(id)service {
    if (!service) unsupported(@"The actual accelerator service wrapper is required.");
    @synchronized(self) {
        if (_acceleratorService && _acceleratorService != service)
            unsupported(@"Replacing the accelerator service wrapper is not supported.");
        _acceleratorService = service; // Actual wrapper weakly holds this device.
    }
}
- (id)_deviceWrapper { return self; } // No tooling wrapper has been constructed.
- (void)_wasRemoved {
    if (_context) (void)_context->close();
    [super _wasRemoved]; // Actual Metal removal helper, not fabricated notification.
}
- (NSString*)name { return @"AMD Navi 31 (HackyNavi compute)"; }
- (uint64_t)registryID { return _acceleratorID; }
- (NSUInteger)maxBufferLength { return NSUInteger(_capabilities.maxBufferLength()); }
- (uint64_t)recommendedMaxWorkingSetSize { return _capabilities.systemBudget(); }
- (MTLSize)maxThreadsPerThreadgroup {
    const auto maximum = _capabilities.maxThreadsPerThreadgroup();
    return MTLSizeMake(maximum, maximum, maximum);
}
- (NSUInteger)maxThreadgroupMemoryLength { return 0; } // Actual admitted zero-LDS profile.
- (BOOL)hasUnifiedMemory { return NO; } // Actual admitted discrete Navi31 device.
- (BOOL)isLowPower { return NO; }
- (nullable id<MTLCommandQueue>)newCommandQueue { return [self newCommandQueueWithMaxCommandBufferCount:64]; }
- (nullable id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {
    return [[HNMetalCommandQueue alloc] initWithDevice:self maxCommandBufferCount:count error:nullptr];
}
- (nullable id<MTLCommandQueue>)newCommandQueueWithDescriptor:(MTLCommandQueueDescriptor*)descriptor {
    if (!descriptor) return nil;
    MTLCommandQueueDescriptor* copy = [descriptor copy];
    if (!copy || copy.logState) return nil;
    return [self newCommandQueueWithMaxCommandBufferCount:copy.maxCommandBufferCount];
}
- (nullable id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    return [[HNMetalBuffer alloc] initWithDevice:self context:_context length:length options:options error:nullptr];
}
- (nullable id<MTLLibrary>)newDefaultLibrary { return [self newDefaultLibraryWithBundle:NSBundle.mainBundle error:nullptr]; }
- (nullable id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle*)bundle error:(NSError* __autoreleasing*)error {
    if (error) *error = nil;
    NSURL* url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    return [self newLibraryWithURL:url error:error];
}
- (nullable id<MTLLibrary>)newLibraryWithFile:(NSString*)path error:(NSError* __autoreleasing*)error {
    if (![path isKindOfClass:[NSString class]] || !path.length) {
        fail(error, @"Expected a metallib file path.", {Status::Invalid, 0}); return nil;
    }
    return [self newLibraryWithURL:[NSURL fileURLWithPath:path] error:error];
}
- (nullable id<MTLLibrary>)newLibraryWithURL:(NSURL*)url error:(NSError* __autoreleasing*)error {
    if (error) *error = nil;
    NSData* copy = readLibrary(url, error);
    return copy ? [[HNMetalLibrary alloc] initWithDevice:self metallibData:copy compilerTools:_compilerTools error:error] : nil;
}
- (nullable id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError* __autoreleasing*)error {
    if (error) *error = nil;
    if (!data || !dispatch_data_get_size(data) || dispatch_data_get_size(data) > kLibraryBytes) {
        fail(error, @"Expected nonempty metallib data of at most 64 MiB.", {Status::Invalid, 0}); return nil;
    }
    const void* bytes = nullptr;
    size_t count = 0;
    __attribute__((objc_precise_lifetime)) dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &count);
    if (!mapped || !bytes || !count || count > kLibraryBytes) {
        fail(error, @"Unable to map the supplied metallib data.", {Status::OutOfMemory, 0}); return nil;
    }
    NSData* snapshot = [NSData dataWithBytes:bytes length:count];
    if (!snapshot) { fail(error, @"Unable to allocate the metallib snapshot.", {Status::OutOfMemory, 0}); return nil; }
    return [[HNMetalLibrary alloc] initWithDevice:self metallibData:snapshot compilerTools:_compilerTools error:error];
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                                                     error:(NSError* __autoreleasing*)error {
    if (error) *error = nil;
    if (![function isMemberOfClass:[HNMetalFunction class]] || function.device != self) {
        fail(error, @"Expected an actual compute function from this device.", {Status::Invalid, 0}); return nil;
    }
    return [[HNMetalComputePipeline alloc] initWithDevice:self function:(HNMetalFunction*)function
        acoPath:_acoPath maxTotalThreadsPerThreadgroup:0 label:nil error:error];
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
    options:(MTLPipelineOption)options reflection:(MTLAutoreleasedComputePipelineReflection*)reflection
    error:(NSError* __autoreleasing*)error {
    if (reflection) *reflection = nil;
    if (options != MTLPipelineOptionNone) {
        fail(error, @"Pipeline reflection and optional creation flags are not implemented."); return nil;
    }
    return [self newComputePipelineStateWithFunction:function error:error];
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                         completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler {
    if (!handler) unsupported(@"A pipeline completion handler is required.");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSError* error = nil;
            id<MTLComputePipelineState> pipeline = createAsyncPipeline(^id<MTLComputePipelineState>(NSError* __autoreleasing* issue) {
                return [self newComputePipelineStateWithFunction:function error:issue];
            }, &error);
            handler(pipeline, error);
        }
    });
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
    completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
    if (!handler) unsupported(@"A pipeline completion handler is required.");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSError* error = nil;
            id<MTLComputePipelineState> pipeline = createAsyncPipeline(^id<MTLComputePipelineState>(NSError* __autoreleasing* issue) {
                return [self newComputePipelineStateWithFunction:function options:options reflection:nullptr error:issue];
            }, &error);
            handler(pipeline, nil, error);
        }
    });
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor*)descriptor
    options:(MTLPipelineOption)options reflection:(MTLAutoreleasedComputePipelineReflection*)reflection
    error:(NSError* __autoreleasing*)error {
    if (error) *error = nil;
    if (reflection) *reflection = nil;
    MTLComputePipelineDescriptor* copy = [descriptor copy];
    if (!copy || options != MTLPipelineOptionNone || copy.stageInputDescriptor ||
        copy.threadGroupSizeIsMultipleOfThreadExecutionWidth || copy.supportIndirectCommandBuffers ||
        copy.preloadedLibraries.count || copy.binaryArchives.count || copy.linkedFunctions ||
        copy.supportAddingBinaryFunctions || copy.maxCallStackDepth != 1 ||
        (copy.shaderValidation != MTLShaderValidationDefault && copy.shaderValidation != MTLShaderValidationDisabled)) {
        fail(error, @"The compute descriptor requests unsupported pipeline features."); return nil;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // Validate the legacy property too; do not assume an undocumented alias.
    const BOOL hasInsertedLibraries = copy.insertLibraries.count != 0;
#pragma clang diagnostic pop
    if (hasInsertedLibraries) { fail(error, @"Inserted dynamic libraries are not implemented."); return nil; }
    if (@available(macOS 26.0, *)) {
        const auto required = copy.requiredThreadsPerThreadgroup;
        if (required.width || required.height || required.depth) {
            fail(error, @"Fixed threadgroup descriptor requirements are not implemented."); return nil;
        }
    }
    // Actual MTLPipelineBufferDescriptorArrayInternal subscript implementation
    // in build/metal-device-hooks/final-disassembly.json checks index <31.
    // Check the entire valid array, not merely the function's used bindings.
    if (!copy.buffers) { fail(error, @"The compute buffer descriptor array is missing.", {Status::Invalid, 0}); return nil; }
    for (NSUInteger index = 0; index < 31; ++index) {
        MTLPipelineBufferDescriptor* buffer = copy.buffers[index];
        if (buffer && buffer.mutability != MTLMutabilityDefault) {
            fail(error, @"Custom buffer mutability is not implemented."); return nil;
        }
    }
    if (![copy.computeFunction isMemberOfClass:[HNMetalFunction class]] || copy.computeFunction.device != self) {
        fail(error, @"Expected an actual compute function from this device.", {Status::Invalid, 0}); return nil;
    }
    return [[HNMetalComputePipeline alloc] initWithDevice:self function:(HNMetalFunction*)copy.computeFunction
        acoPath:_acoPath maxTotalThreadsPerThreadgroup:copy.maxTotalThreadsPerThreadgroup label:copy.label error:error];
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor*)descriptor options:(MTLPipelineOption)options
    completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
    if (!handler) unsupported(@"A pipeline completion handler is required.");
    MTLComputePipelineDescriptor* copy = [descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSError* error = nil;
            id<MTLComputePipelineState> pipeline = createAsyncPipeline(^id<MTLComputePipelineState>(NSError* __autoreleasing* issue) {
                return [self newComputePipelineStateWithDescriptor:copy options:options reflection:nullptr error:issue];
            }, &error);
            handler(pipeline, nil, error);
        }
    });
}
// Unsupported factories and capability queries follow below. A value of zero
// is used only where it means an actually unsupported capability; unknown
// architecture/profile/timing/resource-accounting values are never invented.
- (nullable id <MTLLogState>) newLogStateWithDescriptor:(MTLLogStateDescriptor* _Nonnull) descriptor error:(NSError** )error {
    (void)descriptor;
    fail(error, @"newLogStateWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)desc {
    (void)desc;
    unsupported(@"heapTextureSizeAndAlignWithDescriptor: is not implemented by the native compute profile.");
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    (void)length;
    (void)options;
    unsupported(@"heapBufferSizeAndAlignWithLength:options: is not implemented by the native compute profile.");
}
- (nullable id <MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor {
    (void)descriptor;
    return nil;
}
- (nullable id <MTLBuffer>)newBufferWithBytes:(const void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options {
    return [[HNMetalBuffer alloc] initWithDevice:self context:_context bytes:pointer
                                         length:length options:options error:nullptr];
}
- (nullable id <MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^ NS_SWIFT_SENDABLE __nullable)(void *pointer, NSUInteger length))deallocator {
    (void)pointer;
    (void)length;
    (void)options;
    (void)deallocator;
    return nil;
}
- (nullable id <MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor {
    (void)descriptor;
    return nil;
}
- (nullable id <MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    return [[HNMetalTexture alloc] initWithDevice:self context:_context descriptor:descriptor error:nullptr];
}
- (nullable id <MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)iosurface plane:(NSUInteger)plane {
    (void)descriptor;
    (void)iosurface;
    (void)plane;
    return nil;
}
- (nullable id <MTLTexture>)newSharedTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    (void)descriptor;
    return nil;
}
- (nullable id <MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle *)sharedHandle {
    (void)sharedHandle;
    return nil;
}
- (nullable id <MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor {
    (void)descriptor;
    return nil;
}
- (nullable id <MTLLibrary>)newLibraryWithSource:(NSString *)source options:(nullable MTLCompileOptions *)options error:(__autoreleasing NSError **)error {
    (void)source;
    (void)options;
    fail(error, @"newLibraryWithSource:options:error: is not implemented by the native compute profile.");
    return nil;
}
- (void)newLibraryWithSource:(NSString *)source options:(nullable MTLCompileOptions *)options completionHandler:(MTLNewLibraryCompletionHandler)completionHandler {
    (void)source;
    (void)options;
    if (!completionHandler) unsupported(@"A completion handler is required.");
    NSError* failure = deviceError(@"newLibraryWithSource:options:completionHandler: is not implemented by the native compute profile.");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { completionHandler(nil, failure); }
    });
}
- (nullable id <MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor error:(__autoreleasing NSError **)error {
    (void)descriptor;
    fail(error, @"newLibraryWithStitchedDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (void)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor completionHandler:(MTLNewLibraryCompletionHandler)completionHandler {
    (void)descriptor;
    if (!completionHandler) unsupported(@"A completion handler is required.");
    NSError* failure = deviceError(@"newLibraryWithStitchedDescriptor:completionHandler: is not implemented by the native compute profile.");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { completionHandler(nil, failure); }
    });
}
- (nullable id <MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(__autoreleasing NSError **)error {
    return [[HNMetalRenderPipeline alloc] initWithDevice:self descriptor:descriptor error:error];
}
- (nullable id <MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options reflection:(MTLAutoreleasedRenderPipelineReflection * __nullable)reflection error:(__autoreleasing NSError **)error {
    if (reflection) *reflection = nil;
    if (options != MTLPipelineOptionNone) { fail(error,@"Render reflection and archive options are not implemented."); return nil; }
    return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor completionHandler:(MTLNewRenderPipelineStateCompletionHandler)completionHandler {
    if (!completionHandler) unsupported(@"A completion handler is required.");
    MTLRenderPipelineDescriptor* snapshot = [descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSError* failure = nil;
            auto pipeline = [self newRenderPipelineStateWithDescriptor:snapshot error:&failure];
            completionHandler(pipeline, failure);
        }
    });
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)completionHandler {
    if (!completionHandler) unsupported(@"A completion handler is required.");
    MTLRenderPipelineDescriptor* snapshot = [descriptor copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSError* failure = nil;
            auto pipeline = [self newRenderPipelineStateWithDescriptor:snapshot options:options reflection:nullptr error:&failure];
            completionHandler(pipeline, nil, failure);
        }
    });
}
- (nullable id <MTLFence>)newFence {
    return nil;
}
// Implement the required legacy selector using its exact deprecated SDK type.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet {
    (void)featureSet;
    return NO;
}
#pragma clang diagnostic pop
- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily {
    (void)gpuFamily;
    return NO;
}
- (BOOL)supportsTextureSampleCount:(NSUInteger)sampleCount {
    return sampleCount == 1;
}
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format {
    (void)format;
    unsupported(@"minimumLinearTextureAlignmentForPixelFormat: is not implemented by the native compute profile.");
}
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format {
    (void)format;
    unsupported(@"minimumTextureBufferAlignmentForPixelFormat: is not implemented by the native compute profile.");
}
- (nullable id <MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor*)descriptor options:(MTLPipelineOption)options reflection:(MTLAutoreleasedRenderPipelineReflection * __nullable)reflection error:(__autoreleasing NSError **)error {
    (void)descriptor;
    (void)options;
    if (reflection) *reflection = nil;
    fail(error, @"newRenderPipelineStateWithTileDescriptor:options:reflection:error: is not implemented by the native compute profile.");
    return nil;
}
- (void)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)completionHandler {
    (void)descriptor;
    (void)options;
    if (!completionHandler) unsupported(@"A completion handler is required.");
    NSError* failure = deviceError(@"newRenderPipelineStateWithTileDescriptor:options:completionHandler: is not implemented by the native compute profile.");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { completionHandler(nil, nil, failure); }
    });
}
- (nullable id <MTLRenderPipelineState>)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor*)descriptor
                                                                         options:(MTLPipelineOption)options
                                                                      reflection:(MTLAutoreleasedRenderPipelineReflection * __nullable)reflection
                                                                           error:(__autoreleasing NSError **)error {
    (void)descriptor;
    (void)options;
    if (reflection) *reflection = nil;
    fail(error, @"newRenderPipelineStateWithMeshDescriptor:options:reflection:error: is not implemented by the native compute profile.");
    return nil;
}
- (void)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)descriptor
                                         options:(MTLPipelineOption)options
                               completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)completionHandler {
    (void)descriptor;
    (void)options;
    if (!completionHandler) unsupported(@"A completion handler is required.");
    NSError* failure = deviceError(@"newRenderPipelineStateWithMeshDescriptor:options:completionHandler: is not implemented by the native compute profile.");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { completionHandler(nil, nil, failure); }
    });
}
- (void)getDefaultSamplePositions:(MTLSamplePosition *)positions count:(NSUInteger)count {
    (void)positions;
    (void)count;
    unsupported(@"getDefaultSamplePositions:count: is not implemented by the native compute profile.");
}
- (nullable id <MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray <MTLArgumentDescriptor *> *)arguments {
    (void)arguments;
    return nil;
}
-(BOOL)supportsRasterizationRateMapWithLayerCount:(NSUInteger)layerCount {
    (void)layerCount;
    return NO;
}
-(nullable id<MTLRasterizationRateMap>)newRasterizationRateMapWithDescriptor:(MTLRasterizationRateMapDescriptor*)descriptor {
    (void)descriptor;
    return nil;
}
- (nullable id <MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor*)descriptor maxCommandCount:(NSUInteger)maxCount options:(MTLResourceOptions)options {
    (void)descriptor;
    (void)maxCount;
    (void)options;
    return nil;
}
- (nullable id <MTLEvent>)newEvent {
    return nil;
}
- (nullable id <MTLSharedEvent>)newSharedEvent {
    return nil;
}
- (nullable id <MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle *)sharedEventHandle {
    (void)sharedEventHandle;
    return nil;
}
-(nullable id<MTLIOFileHandle>)newIOHandleWithURL:(NSURL *)url
                                            error:(NSError **)error {
    (void)url;
    fail(error, @"newIOHandleWithURL:error: is not implemented by the native compute profile.");
    return nil;
}
-(nullable id<MTLIOCommandQueue>)newIOCommandQueueWithDescriptor:(MTLIOCommandQueueDescriptor*)descriptor
                                                           error:(NSError **)error {
    (void)descriptor;
    fail(error, @"newIOCommandQueueWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
-(nullable id<MTLIOFileHandle>)newIOHandleWithURL:(NSURL *)url
                                compressionMethod:(MTLIOCompressionMethod)compressionMethod
                                            error:(NSError **)error {
    (void)url;
    (void)compressionMethod;
    fail(error, @"newIOHandleWithURL:compressionMethod:error: is not implemented by the native compute profile.");
    return nil;
}
-(nullable id<MTLIOFileHandle>)newIOFileHandleWithURL:(NSURL *)url
                                                error:(NSError **)error {
    (void)url;
    fail(error, @"newIOFileHandleWithURL:error: is not implemented by the native compute profile.");
    return nil;
}
-(nullable id<MTLIOFileHandle>)newIOFileHandleWithURL:(NSURL *)url
                                    compressionMethod:(MTLIOCompressionMethod)compressionMethod
                                                error:(NSError **)error {
    (void)url;
    (void)compressionMethod;
    fail(error, @"newIOFileHandleWithURL:compressionMethod:error: is not implemented by the native compute profile.");
    return nil;
}
-(MTLSize) sparseTileSizeWithTextureType:(MTLTextureType)textureType
                             pixelFormat:(MTLPixelFormat)pixelFormat
                             sampleCount:(NSUInteger)sampleCount {
    (void)textureType;
    (void)pixelFormat;
    (void)sampleCount;
    unsupported(@"sparseTileSizeWithTextureType:pixelFormat:sampleCount: is not implemented by the native compute profile.");
}
-(void) convertSparsePixelRegions:(const MTLRegion[_Nonnull])pixelRegions
                    toTileRegions:(MTLRegion[_Nonnull])tileRegions
                     withTileSize:(MTLSize)tileSize
                    alignmentMode:(MTLSparseTextureRegionAlignmentMode)mode
                       numRegions:(NSUInteger)numRegions {
    (void)pixelRegions;
    (void)tileRegions;
    (void)tileSize;
    (void)mode;
    (void)numRegions;
    unsupported(@"convertSparsePixelRegions:toTileRegions:withTileSize:alignmentMode:numRegions: is not implemented by the native compute profile.");
}
-(void) convertSparseTileRegions:(const MTLRegion[_Nonnull])tileRegions
                  toPixelRegions:(MTLRegion[_Nonnull])pixelRegions
                    withTileSize:(MTLSize)tileSize
                      numRegions:(NSUInteger)numRegions {
    (void)tileRegions;
    (void)pixelRegions;
    (void)tileSize;
    (void)numRegions;
    unsupported(@"convertSparseTileRegions:toPixelRegions:withTileSize:numRegions: is not implemented by the native compute profile.");
}
- (NSUInteger) sparseTileSizeInBytesForSparsePageSize:(MTLSparsePageSize)sparsePageSize {
    (void)sparsePageSize;
    unsupported(@"sparseTileSizeInBytesForSparsePageSize: is not implemented by the native compute profile.");
}
-(MTLSize) sparseTileSizeWithTextureType:(MTLTextureType)textureType
                             pixelFormat:(MTLPixelFormat)pixelFormat
                             sampleCount:(NSUInteger)sampleCount
                          sparsePageSize:(MTLSparsePageSize)sparsePageSize {
    (void)textureType;
    (void)pixelFormat;
    (void)sampleCount;
    (void)sparsePageSize;
    unsupported(@"sparseTileSizeWithTextureType:pixelFormat:sampleCount:sparsePageSize: is not implemented by the native compute profile.");
}
- (nullable id<MTLCounterSampleBuffer>) newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor*)descriptor
                                                              error:(NSError**)error {
    (void)descriptor;
    fail(error, @"newCounterSampleBufferWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
-(void)sampleTimestamps:(MTLTimestamp *)cpuTimestamp
           gpuTimestamp:(MTLTimestamp *)gpuTimestamp {
    (void)cpuTimestamp;
    (void)gpuTimestamp;
    unsupported(@"sampleTimestamps:gpuTimestamp: is not implemented by the native compute profile.");
}
-(id<MTLArgumentEncoder>)newArgumentEncoderWithBufferBinding:(id<MTLBufferBinding>)bufferBinding {
    (void)bufferBinding;
    unsupported(@"newArgumentEncoderWithBufferBinding: is not implemented by the native compute profile.");
}
- (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)samplingPoint {
    (void)samplingPoint;
    return NO;
}
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count {
    (void)count;
    return NO;
}
- (nullable id<MTLDynamicLibrary>) newDynamicLibrary:(id<MTLLibrary>)library error:(NSError **) error {
    (void)library;
    fail(error, @"newDynamicLibrary:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTLDynamicLibrary>) newDynamicLibraryWithURL:(NSURL *)url error:(NSError **) error {
    (void)url;
    fail(error, @"newDynamicLibraryWithURL:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTLBinaryArchive>) newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor*)descriptor
                                                           error:(NSError**)error {
    (void)descriptor;
    fail(error, @"newBinaryArchiveWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor *)descriptor {
    (void)descriptor;
    unsupported(@"accelerationStructureSizesWithDescriptor: is not implemented by the native compute profile.");
}
- (nullable id <MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size {
    (void)size;
    return nil;
}
- (nullable id <MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)descriptor {
    (void)descriptor;
    return nil;
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)size {
    (void)size;
    unsupported(@"heapAccelerationStructureSizeAndAlignWithSize: is not implemented by the native compute profile.");
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithDescriptor:(MTLAccelerationStructureDescriptor *)descriptor {
    (void)descriptor;
    unsupported(@"heapAccelerationStructureSizeAndAlignWithDescriptor: is not implemented by the native compute profile.");
}
- (nullable id<MTLResidencySet>) newResidencySetWithDescriptor:(MTLResidencySetDescriptor *)desc
                                            error:(NSError *__nullable*)error {
    (void)desc;
    fail(error, @"newResidencySetWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (MTLSizeAndAlign)tensorSizeAndAlignWithDescriptor:(MTLTensorDescriptor *)descriptor API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    unsupported(@"tensorSizeAndAlignWithDescriptor: is not implemented by the native compute profile.");
}
- (nullable id <MTLTensor>)newTensorWithDescriptor:(MTLTensorDescriptor *)descriptor
                                             error:(__autoreleasing NSError * _Nullable * _Nullable)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    fail(error, @"newTensorWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function API_AVAILABLE(macos(26.0)) {
    (void)function;
    return nil;
}
- (nullable id<MTL4CommandAllocator>)newCommandAllocator API_AVAILABLE(macos(26.0)) {
    return nil;
}
- (nullable id<MTL4CommandAllocator>)newCommandAllocatorWithDescriptor:(MTL4CommandAllocatorDescriptor *)descriptor
                                                                 error:(NSError**)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    fail(error, @"newCommandAllocatorWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTL4CommandQueue>)newMTL4CommandQueue API_AVAILABLE(macos(26.0)) {
    return nil;
}
- (nullable id<MTL4CommandQueue>)newMTL4CommandQueueWithDescriptor:(MTL4CommandQueueDescriptor *)descriptor
                                                             error:(__autoreleasing NSError * _Nullable * _Nullable)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    fail(error, @"newMTL4CommandQueueWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTL4CommandBuffer>)newCommandBuffer API_AVAILABLE(macos(26.0)) {
    return nil;
}
- (nullable id<MTL4ArgumentTable>)newArgumentTableWithDescriptor:(MTL4ArgumentTableDescriptor *)descriptor
                                                           error:(NSError * _Nullable *)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    fail(error, @"newArgumentTableWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTLTextureViewPool>)newTextureViewPoolWithDescriptor:(MTLResourceViewPoolDescriptor *)descriptor
                                                              error:(NSError * _Nullable *)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    fail(error, @"newTextureViewPoolWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTL4Compiler>)newCompilerWithDescriptor:(MTL4CompilerDescriptor *)descriptor
                                                 error:(NSError **)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    fail(error, @"newCompilerWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (nullable id<MTL4Archive>)newArchiveWithURL:(NSURL *)url
                                        error:(NSError **)error API_AVAILABLE(macos(26.0)) {
    (void)url;
    fail(error, @"newArchiveWithURL:error: is not implemented by the native compute profile.");
    return nil;
}
- (id<MTL4PipelineDataSetSerializer>)newPipelineDataSetSerializerWithDescriptor:
    (MTL4PipelineDataSetSerializerDescriptor *)descriptor API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    unsupported(@"newPipelineDataSetSerializerWithDescriptor: is not implemented by the native compute profile.");
}
- (nullable id<MTLBuffer>)newBufferWithLength:(NSUInteger)length
                                      options:(MTLResourceOptions)options
                      placementSparsePageSize:(MTLSparsePageSize)placementSparsePageSize API_AVAILABLE(macos(26.0)) {
    (void)length;
    (void)options;
    (void)placementSparsePageSize;
    return nil;
}
- (nullable id<MTL4CounterHeap>)newCounterHeapWithDescriptor:(MTL4CounterHeapDescriptor *)descriptor
                                                       error:(NSError *__nullable*)error API_AVAILABLE(macos(26.0)) {
    (void)descriptor;
    fail(error, @"newCounterHeapWithDescriptor:error: is not implemented by the native compute profile.");
    return nil;
}
- (NSUInteger)sizeOfCounterHeapEntry:(MTL4CounterHeapType)type API_AVAILABLE(macos(26.0)) {
    (void)type;
    unsupported(@"sizeOfCounterHeapEntry: is not implemented by the native compute profile.");
}
- (uint64_t)queryTimestampFrequency API_AVAILABLE(macos(26.0)) {
    unsupported(@"queryTimestampFrequency is not implemented by the native compute profile.");
}
- (nullable id<MTLFunctionHandle>)functionHandleWithBinaryFunction:(id<MTL4BinaryFunction>)function API_AVAILABLE(macos(26.0)) {
    (void)function;
    return nil;
}
- (MTLArchitecture *)architecture { return _architecture; }
- (BOOL)isHeadless { unsupported(@"isHeadless has no implemented native compute contract."); }
- (BOOL)isRemovable { unsupported(@"isRemovable has no implemented native compute contract."); }
- (MTLDeviceLocation)location { unsupported(@"location has no implemented native compute contract."); }
- (NSUInteger)locationNumber { unsupported(@"locationNumber has no implemented native compute contract."); }
- (uint64_t)maxTransferRate { unsupported(@"maxTransferRate has no implemented native compute contract."); }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTierNone; }
- (MTLArgumentBuffersTier)argumentBuffersSupport { unsupported(@"argumentBuffersSupport has no implemented native compute contract."); }
- (BOOL)areRasterOrderGroupsSupported { return NO; }
- (BOOL)supports32BitFloatFiltering { return NO; }
- (BOOL)supports32BitMSAA { return NO; }
- (BOOL)supportsQueryTextureLOD { return NO; }
- (BOOL)supportsBCTextureCompression { return NO; }
- (BOOL)supportsPullModelInterpolation { return NO; }
- (BOOL)areBarycentricCoordsSupported { return NO; }
- (BOOL)supportsShaderBarycentricCoordinates { return NO; }
- (NSUInteger)currentAllocatedSize {
    uint64_t bytes = 0;
    const auto result = _context ? _context->allocatedSize(bytes) : Result{Status::NotOpen, 0};
    if (!result || bytes > NSUIntegerMax) unsupported(@"Native allocation accounting is unavailable for this context.");
    return NSUInteger(bytes);
}
- (NSUInteger)maxArgumentBufferSamplerCount { return 0; }
- (BOOL)areProgrammableSamplePositionsSupported { return NO; }
- (uint64_t)peerGroupID { unsupported(@"peerGroupID has no implemented native compute contract."); }
- (uint32_t)peerIndex { unsupported(@"peerIndex has no implemented native compute contract."); }
- (uint32_t)peerCount { unsupported(@"peerCount has no implemented native compute contract."); }
- (NSUInteger)sparseTileSizeInBytes { unsupported(@"sparseTileSizeInBytes has no implemented native compute contract."); }
- (NSArray<id<MTLCounterSet>>*)counterSets { return @[]; }
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsRenderDynamicLibraries { return NO; }
- (BOOL)supportsRaytracing { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsFunctionPointersFromRender { return NO; }
- (BOOL)supportsRaytracingFromRender { return NO; }
- (BOOL)supportsPrimitiveMotionBlur { return NO; }
- (BOOL)shouldMaximizeConcurrentCompilation { unsupported(@"shouldMaximizeConcurrentCompilation has no implemented native compute contract."); }
- (void)setShouldMaximizeConcurrentCompilation:(BOOL)value { (void)value; unsupported(@"shouldMaximizeConcurrentCompilation is not implemented."); }
- (NSUInteger)maximumConcurrentCompilationTaskCount { unsupported(@"maximumConcurrentCompilationTaskCount has no implemented native compute contract."); }
#include "HNMetalDeviceSPI.inc"
@end
