// SPDX-License-Identifier: MIT
// TARGET ONLY: the build host compiles this executable but NEVER runs it.
// Public Metal path only; no injected device, native Context or GPU fallback.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr NSUInteger Side = 64, RowBytes = Side * 4, ReadPitch = RowBytes + 16, HostGuard = 16;
constexpr size_t LibraryBytes = 6354;
constexpr const char* LibrarySha = "b99ae2fa65e9cfda2e13c146434fee1f0c0ef0cd2956beeba1cec01750641197";
struct Port { io_object_t value = IO_OBJECT_NULL; ~Port() { if (value) IOObjectRelease(value); } };
struct File { int value = -1; ~File() { if (value >= 0) ::close(value); } };
struct Counters { std::atomic<unsigned> scheduled{0}, completed{0}; };
void setPhase(NSMutableDictionary* report, NSString* phase) {
    report[@"phase"]=phase;
    std::fprintf(stderr,"PHASE: %s\n",phase.UTF8String);std::fflush(stderr);
}
bool fail(NSMutableDictionary* report, NSString* reason) {
    if (!reason) reason = @"An operation failed without a diagnostic.";
    report[@"error"] = reason; std::fprintf(stderr,"FAIL: %s\n",reason.UTF8String); return false;
}
NSString* errorText(NSError* error, NSString* fallback) {
    NSString* text = error.description; return text ? text : fallback;
}
std::string sha256(const void* bytes, size_t count) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(bytes, static_cast<CC_LONG>(count), digest.data());
    const char hex[] = "0123456789abcdef"; std::string text;
    for (unsigned char byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; }
    return text;
}
bool exactClass(io_object_t object, const char* expected) {
    io_name_t name{};
    return object && IOObjectGetClass(object,name) == KERN_SUCCESS && !std::strcmp(name,expected);
}
bool pciWord(io_registry_entry_t pci, CFStringRef name, uint32_t& value) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(pci,name,kCFAllocatorDefault,0);
    bool valid = property && CFGetTypeID(property) == CFDataGetTypeID() &&
        CFDataGetLength(static_cast<CFDataRef>(property)) == 4;
    if (valid) {
        const UInt8* bytes = CFDataGetBytePtr(static_cast<CFDataRef>(property));
        value = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
    }
    if (property) CFRelease(property); return valid;
}
bool physicalTarget(uint64_t registry, NSMutableDictionary* report) {
    // Read-only identity lookup. Never opens a service/user client directly.
    CFMutableDictionaryRef matching = IORegistryEntryIDMatching(registry);
    if (!matching) return fail(report,@"Unable to create the exact registry match.");
    Port accelerator, owner, pci;
    accelerator.value = IOServiceGetMatchingService(kIOMainPortDefault,matching);
    uint64_t actual = 0, ownerID = 0, pciID = 0;
    if (!exactClass(accelerator.value,"HackyNaviAccelerator") ||
        !IOObjectConformsTo(accelerator.value,"IOAccelerator") ||
        IORegistryEntryGetRegistryEntryID(accelerator.value,&actual) != KERN_SUCCESS || actual != registry ||
        IORegistryEntryGetParentEntry(accelerator.value,kIOServicePlane,&owner.value) != KERN_SUCCESS ||
        !exactClass(owner.value,"HackyNaviDevice") ||
        IORegistryEntryGetRegistryEntryID(owner.value,&ownerID) != KERN_SUCCESS || !ownerID || ownerID == registry ||
        IORegistryEntryGetParentEntry(owner.value,kIOServicePlane,&pci.value) != KERN_SUCCESS ||
        !IOObjectConformsTo(pci.value,"IOPCIDevice") ||
        IORegistryEntryGetRegistryEntryID(pci.value,&pciID) != KERN_SUCCESS || !pciID)
        return fail(report,@"Registry ID is not the actual HackyNavi accelerator/owner/PCI chain.");
    CFTypeRef property = IORegistryEntryCreateCFProperty(accelerator.value,CFSTR("HackyNaviOwnerRegistryID"),kCFAllocatorDefault,0);
    int64_t claimed = 0;
    bool ownerMatches = property && CFGetTypeID(property) == CFNumberGetTypeID() &&
        !CFNumberIsFloatType(static_cast<CFNumberRef>(property)) &&
        CFNumberGetValue(static_cast<CFNumberRef>(property),kCFNumberSInt64Type,&claimed) &&
        claimed > 0 && uint64_t(claimed) == ownerID;
    if (property) CFRelease(property);
    uint32_t vendor = 0, device = 0;
    if (!ownerMatches || !pciWord(pci.value,CFSTR("vendor-id"),vendor) ||
        !pciWord(pci.value,CFSTR("device-id"),device) || vendor != 0x1002 || device != 0x744c)
        return fail(report,@"Actual owner property or Navi31 PCI 1002:744c identity does not match.");
    report[@"owner_registry_id"] = @(ownerID); report[@"pci_registry_id"] = @(pciID);
    report[@"pci_vendor"] = @(vendor); report[@"pci_device"] = @(device);
    return true;
}
bool librarySnapshot(const char* path, std::vector<uint8_t>& output, NSMutableDictionary* report) {
    File file; file.value = ::open(path,O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat status{};
    if (file.value < 0 || ::fstat(file.value,&status) || !S_ISREG(status.st_mode) || status.st_size != off_t(LibraryBytes))
        return fail(report,@"Expected the actual regular 6,354-byte graphics.metallib fixture.");
    output.resize(LibraryBytes);
    size_t done = 0;
    while (done < output.size()) {
        const ssize_t count = ::read(file.value,output.data()+done,output.size()-done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fail(report,@"Unable to snapshot the complete actual metallib.");
        done += size_t(count);
    }
    uint8_t extra = 0; ssize_t count;
    do { count = ::read(file.value,&extra,1); } while (count < 0 && errno == EINTR);
    const auto hash = sha256(output.data(),output.size());
    if (count != 0 || hash != LibrarySha) return fail(report,@"Metallib differs from the retained real Apple graphics fixture.");
    report[@"metallib_sha256"] = [NSString stringWithUTF8String:hash.c_str()]; return true;
}
bool color(const uint8_t* p) {
    const int expected[] = {64,128,191,255};
    for (unsigned c = 0; c < 4; ++c) if (std::abs(int(p[c])-expected[c]) > 1) return false;
    return true;
}
bool zero(const uint8_t* p) { return !(p[0] | p[1] | p[2] | p[3]); }
bool verify(const std::vector<uint8_t>& bytes, NSMutableDictionary* report) {
    NSUInteger interior = 0, exterior = 0, boundary = 0, mismatches = 0;
    std::vector<uint8_t> packed(Side*RowBytes);
    for (NSUInteger y = 0; y < Side; ++y) {
        const uint8_t* row = bytes.data()+HostGuard+y*ReadPitch;
        std::memcpy(packed.data()+y*RowBytes,row,RowBytes);
        for (NSUInteger x = 0; x < Side; ++x) {
            // Genuine Apple vertices map to (0,64),(64,64),(32,0).
            // Pixel centers: Y >= 2*abs(X-64), in doubled integer units.
            // Skip only the narrow sloped-edge band; no edge-rule assumption.
            const int edge = int(2*y+1)-2*std::abs(int(2*x+1)-int(Side));
            if (std::abs(edge) <= 4) { ++boundary; continue; }
            const bool inside = edge > 0;
            if (inside) ++interior; else ++exterior;
            if (!(inside ? color(row+x*4) : zero(row+x*4))) {
                if (mismatches < 12) std::fprintf(stderr,"FAIL: pixel (%lu,%lu) RGBA=%u,%u,%u,%u expected %s\n",
                    static_cast<unsigned long>(x),static_cast<unsigned long>(y),row[x*4],row[x*4+1],row[x*4+2],row[x*4+3],inside?"64,128,191,255 +/-1":"untouched zero");
                ++mismatches;
            }
        }
    }
    bool guards = true;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const bool pixel = i >= HostGuard && i < HostGuard+Side*ReadPitch && (i-HostGuard)%ReadPitch < RowBytes;
        if (!pixel && bytes[i] != 0xcd) guards = false;
    }
    const auto at = [&](NSUInteger x, NSUInteger y) { return packed.data()+y*RowBytes+x*4; };
    // Bottom is wide; top is narrow. Mirroring Y cannot pass this pair.
    const bool orientation = zero(at(8,8)) && color(at(8,56)) && zero(at(55,8)) && color(at(55,56));
    report[@"pixels_total"] = @(Side*Side); report[@"interior_checked"] = @(interior);
    report[@"exterior_checked"] = @(exterior); report[@"edge_band_skipped"] = @(boundary);
    report[@"pixel_mismatches"] = @(mismatches); report[@"host_padding_guards_intact"] = @(guards);
    report[@"metal_y_orientation_correct"] = @(orientation);
    const auto hash = sha256(packed.data(),packed.size());
    report[@"actual_rgba_sha256"] = [NSString stringWithUTF8String:hash.c_str()];
    return (!mismatches && guards && orientation && interior && exterior && interior+exterior+boundary == Side*Side) ||
        fail(report,@"Actual readback failed color, exterior, orientation or host-padding checks.");
}
bool exercise(uint64_t registry, const char* path, NSMutableDictionary* report) {
    setPhase(report,@"fixture and physical identity");
    std::vector<uint8_t> snapshot;
    if (!librarySnapshot(path,snapshot,report) || !physicalTarget(registry,report)) return false;
    setPhase(report,@"public Metal discovery");
    id<MTLDevice> device = nil;
    for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
        if (candidate.registryID != registry) continue;
        if (device) return fail(report,@"Duplicate public Metal registry identity.");
        device = candidate;
    }
    if (!device || ![NSStringFromClass([device class]) isEqualToString:@"HNMetalDevice"])
        return fail(report,@"The exact target is not a publicly discovered HNMetalDevice; no fallback is allowed.");
    report[@"device_name"] = device.name; report[@"device_class"] = NSStringFromClass([device class]);
    // A null destructor asks libdispatch to copy these real verified bytes.
    dispatch_data_t data = dispatch_data_create(snapshot.data(),snapshot.size(),dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),nullptr);
    if (!data) return fail(report,@"Unable to retain the real metallib snapshot.");
    setPhase(report,@"actual library and render pipeline");
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    if (!library || error) return fail(report,errorText(error,@"Actual library creation returned nil."));
    id<MTLFunction> vertex = [library newFunctionWithName:@"hn_triangle_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"hn_constant_fragment"];
    if (!vertex || !fragment || vertex.device != device || fragment.device != device ||
        vertex.functionType != MTLFunctionTypeVertex || fragment.functionType != MTLFunctionTypeFragment)
        return fail(report,@"Expected actual vertex and fragment functions from the same target device.");
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    if (!descriptor) return fail(report,@"Unable to create render descriptor.");
    descriptor.vertexFunction = vertex; descriptor.fragmentFunction = fragment;
    descriptor.rasterSampleCount = 1; descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.colorAttachments[0].blendingEnabled = NO; descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline || error || pipeline.device != device) return fail(report,errorText(error,@"Actual render pipeline creation failed."));
    setPhase(report,@"managed texture and upload");
    MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:Side height:Side mipmapped:NO];
    if (!textureDescriptor) return fail(report,@"Unable to create texture descriptor.");
    textureDescriptor.storageMode = MTLStorageModeManaged; textureDescriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    textureDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
    if (!texture || texture.device != device || texture.storageMode != MTLStorageModeManaged ||
        texture.width != Side || texture.height != Side || texture.pixelFormat != MTLPixelFormatRGBA8Unorm)
        return fail(report,@"Actual Managed RGBA8 target allocation failed.");
    std::vector<uint8_t> initial(Side*RowBytes,0);
    [texture replaceRegion:MTLRegionMake2D(0,0,Side,Side) mipmapLevel:0 withBytes:initial.data() bytesPerRow:RowBytes];
    setPhase(report,@"render and ordered readback");
    id<MTLCommandQueue> queue = [device newCommandQueueWithMaxCommandBufferCount:1];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!queue || !command) return fail(report,@"Actual command queue/buffer creation failed.");
    auto counters = std::make_shared<Counters>();
    [command addScheduledHandler:^(id<MTLCommandBuffer> completed) { (void)completed; ++counters->scheduled; }];
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) { (void)completed; ++counters->completed; }];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!pass) return fail(report,@"Unable to create render pass descriptor.");
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
    if (!render) return fail(report,@"Actual render encoder creation failed.");
    [render setRenderPipelineState:pipeline];
    const MTLViewport viewport = {0,0,double(Side),double(Side),0,1};
    const MTLScissorRect scissor = {0,0,Side,Side};
    [render setViewport:viewport]; [render setScissorRect:scissor];
    [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [render endEncoding];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (!blit) return fail(report,@"Actual ordered texture readback encoder creation failed.");
    [blit synchronizeResource:texture]; [blit endEncoding];
    report[@"gpu_submission_attempted"] = @YES;
    [command commit]; [command waitUntilCompleted];
    report[@"command_status"] = @(command.status); report[@"scheduled_handlers"] = @(counters->scheduled.load());
    report[@"completed_handlers"] = @(counters->completed.load());
    if (command.status != MTLCommandBufferStatusCompleted || command.error ||
        counters->scheduled != 1 || counters->completed != 1)
        return fail(report,errorText(command.error,@"Command or exactly-once callback completion failed."));
    setPhase(report,@"verify actual pixels");
    std::vector<uint8_t> readback(HostGuard+Side*ReadPitch+HostGuard,0xcd);
    [texture getBytes:readback.data()+HostGuard bytesPerRow:ReadPitch fromRegion:MTLRegionMake2D(0,0,Side,Side) mipmapLevel:0];
    return verify(readback,report);
}
}
int main(int argc, char** argv) {
    // Validate explicit target authorization before calling any Metal function.
    if (argc != 6 || std::strcmp(argv[1],"--run") || std::strcmp(argv[2],"--registry") || std::strcmp(argv[4],"--metallib")) {
        std::fprintf(stderr,"Usage: %s --run --registry ACCELERATOR_ID --metallib /absolute/graphics.metallib\nTarget-only; do not run on the build Mac. JSON report goes to stdout.\n",argv[0]); return 2;
    }
    char* end = nullptr; errno = 0; const char* value = argv[3];
    const uint64_t registry = std::strtoull(value,&end,0);
    if (errno || !*value || *value < '0' || *value > '9' || !registry || !end || *end || argv[5][0] != '/') return 2;
    @autoreleasepool {
        NSMutableDictionary* report = [@{@"success":@NO,@"test":@"public-metal-offscreen-triangle",@"registry_id":@(registry),
            @"width":@(Side),@"height":@(Side),@"gpu_submission_attempted":@NO} mutableCopy];
        if (!report) { std::fprintf(stderr,"FAIL: unable to allocate report.\n"); return 1; }
        bool success = false;
        @try {
            try { success = exercise(registry,argv[5],report); }
            catch (const std::exception& error) { fail(report,[NSString stringWithUTF8String:error.what()]); }
        } @catch (NSException* error) { fail(report,error.description); }
        report[@"success"] = @(success);
        NSError* serializationError = nil;
        NSData* json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&serializationError];
        if (!json || serializationError || std::fwrite(json.bytes,1,json.length,stdout) != json.length || std::fputc('\n',stdout) == EOF) return 1;
        return success ? 0 : 1;
    }
}
