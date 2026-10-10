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
constexpr const char* VertexSha = "a98e9a44782d8de99124e71ac5ff52477880005471ff2029648b3d5da9cd43f4";
constexpr const char* ConstantSha = "b99ae2fa65e9cfda2e13c146434fee1f0c0ef0cd2956beeba1cec01750641197";
constexpr const char* FragmentSha = "d5feaa1df63810fbb0ee3013497d01cb814f7cbb02a99c32ea25867859acdefd";
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
bool librarySnapshot(const char* path, size_t expectedBytes, const char* expectedSha, NSString* key,
                     std::vector<uint8_t>& output, NSMutableDictionary* report) {
    File file; file.value = ::open(path,O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat status{};
    if (file.value < 0 || ::fstat(file.value,&status) || !S_ISREG(status.st_mode) || status.st_size != off_t(expectedBytes))
        return fail(report,@"Expected the exact regular Apple metallib fixture.");
    output.resize(expectedBytes);
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
    if (count != 0 || hash != expectedSha) return fail(report,@"Metallib differs from the retained actual Apple fixture.");
    report[key] = [NSString stringWithUTF8String:hash.c_str()]; return true;
}
bool color(const uint8_t* p, const std::array<int,4>& expected) {
    for (unsigned c = 0; c < 4; ++c) if (std::abs(int(p[c])-expected[c]) > 1) return false;
    return true;
}
bool zero(const uint8_t* p) { return !(p[0] | p[1] | p[2] | p[3]); }
bool verify(const std::vector<uint8_t>& bytes, const std::array<int,4>& expected, unsigned geometry, NSMutableDictionary* report) {
    NSUInteger interior = 0, exterior = 0, boundary = 0, mismatches = 0;
    std::vector<uint8_t> packed(Side*RowBytes);
    for (NSUInteger y = 0; y < Side; ++y) {
        const uint8_t* row = bytes.data()+HostGuard+y*ReadPitch;
        std::memcpy(packed.data()+y*RowBytes,row,RowBytes);
        for (NSUInteger x = 0; x < Side; ++x) {
            // Uploaded float4 positions map to the left or right half of the
            // target: (0,64),(32,64),(16,0), translated +32 for geometry1.
            // Evaluate doubled pixel centers; ignore only a narrow edge band.
            const int center = geometry ? 96 : 32;
            const int edge = int(2*y+1)-4*std::abs(int(2*x+1)-center);
            if (std::abs(edge) <= 8) { ++boundary; continue; }
            const bool inside = edge > 0;
            if (inside) ++interior; else ++exterior;
            if (!(inside ? color(row+x*4,expected) : zero(row+x*4))) {
                if (mismatches < 12) std::fprintf(stderr,"FAIL: pixel (%lu,%lu) RGBA=%u,%u,%u,%u expected %s\n",
                    static_cast<unsigned long>(x),static_cast<unsigned long>(y),row[x*4],row[x*4+1],row[x*4+2],row[x*4+3],inside?"uploaded color +/-1":"untouched zero");
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
    const NSUInteger selected = geometry ? 40 : 8, other = geometry ? 8 : 40;
    const bool orientation = zero(at(selected,8)) && color(at(selected,56),expected) &&
        zero(at(other,8)) && zero(at(other,56));
    report[@"uploaded_geometry_index"] = @(geometry);
    report[@"pixels_total"] = @(Side*Side); report[@"interior_checked"] = @(interior);
    report[@"exterior_checked"] = @(exterior); report[@"edge_band_skipped"] = @(boundary);
    report[@"pixel_mismatches"] = @(mismatches); report[@"host_padding_guards_intact"] = @(guards);
    report[@"metal_y_orientation_correct"] = @(orientation);
    const auto hash = sha256(packed.data(),packed.size());
    report[@"actual_rgba_sha256"] = [NSString stringWithUTF8String:hash.c_str()];
    return (!mismatches && guards && orientation && interior && exterior && interior+exterior+boundary == Side*Side) ||
        fail(report,@"Actual readback failed color, exterior, orientation or host-padding checks.");
}
bool rejects(void (^operation)(void)) {
    @try { operation(); } @catch (NSException* error) {
        return [error.name isEqualToString:NSInvalidArgumentException];
    }
    return false;
}
id<MTLLibrary> library(id<MTLDevice> device, const std::vector<uint8_t>& bytes, NSError* __autoreleasing* error) {
    dispatch_data_t data=dispatch_data_create(bytes.data(),bytes.size(),dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),nullptr);
    return data ? [device newLibraryWithData:data error:error] : nil;
}
bool exercise(uint64_t registry, const char* vertexPath, const char* constantPath,
              const char* fragmentPath, NSMutableDictionary* report) {
    setPhase(report,@"actual fixtures and physical identity");
    std::vector<uint8_t> vertexBytes,constantBytes,fragmentBytes;
    if (!librarySnapshot(vertexPath,3542,VertexSha,@"vertex_metallib_sha256",vertexBytes,report) ||
        !librarySnapshot(constantPath,6354,ConstantSha,@"constant_metallib_sha256",constantBytes,report) ||
        !librarySnapshot(fragmentPath,3496,FragmentSha,@"fragment_metallib_sha256",fragmentBytes,report) ||
        !physicalTarget(registry,report)) return false;
    id<MTLDevice> device=nil;
    for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
        if (candidate.registryID!=registry) continue;
        if (device) return fail(report,@"Duplicate public Metal registry identity.");
        device=candidate;
    }
    if (!device || ![NSStringFromClass([device class]) isEqualToString:@"HNMetalDevice"])
        return fail(report,@"Exact public HNMetalDevice discovery failed; no fallback exists.");
    report[@"device_name"]=device.name;
    NSError* error=nil;
    id<MTLLibrary> vertexLibrary=library(device,vertexBytes,&error);
    if (!vertexLibrary || error) return fail(report,errorText(error,@"Vertex library failed."));
    error=nil;
    id<MTLLibrary> constantLibrary=library(device,constantBytes,&error);
    if (!constantLibrary || error) return fail(report,errorText(error,@"Constant fragment library failed."));
    error=nil;
    id<MTLLibrary> fragmentLibrary=library(device,fragmentBytes,&error);
    if (!fragmentLibrary || error) return fail(report,errorText(error,@"Buffer fragment library failed."));
    id<MTLFunction> vertex=[vertexLibrary newFunctionWithName:@"hn_buffer_vertex"];
    id<MTLFunction> constant=[constantLibrary newFunctionWithName:@"hn_constant_fragment"];
    id<MTLFunction> fragment=[fragmentLibrary newFunctionWithName:@"hn_buffer_fragment"];
    if (!vertex || !constant || !fragment || vertex.device!=device || constant.device!=device || fragment.device!=device ||
        vertex.functionType!=MTLFunctionTypeVertex || constant.functionType!=MTLFunctionTypeFragment || fragment.functionType!=MTLFunctionTypeFragment)
        return fail(report,@"Expected genuine vertex-buffer, constant and buffer-fragment functions.");
    id<MTLBuffer> positions=[device newBufferWithLength:160 options:MTLResourceStorageModeManaged|MTLResourceHazardTrackingModeTracked];
    id<MTLBuffer> colors=[device newBufferWithLength:64 options:MTLResourceStorageModeManaged|MTLResourceHazardTrackingModeTracked];
    id<MTLCommandQueue> queue=[device newCommandQueueWithMaxCommandBufferCount:1];
    if (!positions || positions.device!=device || positions.length!=160 || !positions.contents ||
        !colors || colors.device!=device || colors.length!=64 || !colors.contents || !queue)
        return fail(report,@"Actual Managed source buffers or queue creation failed.");
    const auto vertexAddress=positions.gpuAddress,fragmentAddress=colors.gpuAddress;
    const float geometry[2][12]={{-1,-1,0,1, 0,-1,0,1, -.5f,1,0,1},
                               {0,-1,0,1, 1,-1,0,1, .5f,1,0,1}};
    const float emptyGeometry[12]={0,0,0,1, 0,0,0,1, 0,0,0,1};
    const float colorValues[2][4]={{.25f,.5f,.75f,1},{.75f,.25f,.5f,1}};
    const float poison[4]={0,1,0,1};
    const std::array<int,4> expected[2]={{{64,128,191,255}},{{191,64,128,255}}};
    NSMutableArray* profiles=[NSMutableArray new];
    if (!profiles) return fail(report,@"Report allocation failed.");
    report[@"profiles"]=profiles;
    for (unsigned profile=0;profile<2;++profile) {
        setPhase(report,@"actual immutable graphics pipeline");
        MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
        if (!descriptor) return fail(report,@"Render descriptor allocation failed.");
        descriptor.vertexFunction=vertex; descriptor.fragmentFunction=profile?fragment:constant;
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        error=nil;
        id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline || error || pipeline.device!=device) return fail(report,errorText(error,@"Actual vertex-buffer pipeline failed."));
        NSMutableArray* passes=[NSMutableArray new];
        NSMutableDictionary* profileReport=[@{@"profile":profile?@"vertex-and-fragment-buffers":@"vertex-buffer",@"passes":passes} mutableCopy];
        if (!passes || !profileReport) return fail(report,@"Profile report allocation failed.");
        [profiles addObject:profileReport];
        for (unsigned passIndex=0;passIndex<2;++passIndex) {
            NSMutableDictionary* passReport=[NSMutableDictionary new];
            if (!passReport) return fail(report,@"Pass report allocation failed.");
            [passes addObject:passReport];
            setPhase(report,@"actual position and color uploads");
            auto* vertexHost=static_cast<uint8_t*>(positions.contents);
            std::memset(vertexHost,0xa5,160);
            // Two full disjoint float4[3] ranges, with guards around each.
            // Offset16 is captured before changing encoder state in pass0;
            // pass1 changes to80 before recording the draw.
            std::memcpy(vertexHost+16,geometry[0],48);
            // The second upload changes offset80 from a degenerate triangle
            // to the right-hand triangle. Ignoring the upload, the offset,
            // or per-draw binding capture produces the wrong pixels.
            std::memcpy(vertexHost+80,passIndex?geometry[1]:emptyGeometry,48);
            [positions didModifyRange:NSMakeRange(0,160)];
            auto* colorHost=static_cast<uint8_t*>(colors.contents);
            std::memset(colorHost,0xa5,64);
            std::memcpy(colorHost+16,colorValues[0],16);
            std::memcpy(colorHost+32,passIndex?colorValues[1]:poison,16);
            [colors didModifyRange:NSMakeRange(0,64)];
            if (positions.gpuAddress!=vertexAddress || colors.gpuAddress!=fragmentAddress)
                return fail(report,@"A source BO reservation changed across uploads.");
            passReport[@"vertex_offset"]=@(passIndex?80:16);
            passReport[@"vertex_gpu_address"]=@(vertexAddress);
            passReport[@"fragment_offset"]=@(profile?(passIndex?32:16):0);
            passReport[@"fragment_gpu_address"]=@(fragmentAddress);
            MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:Side height:Side mipmapped:NO];
            td.storageMode=MTLStorageModeManaged; td.hazardTrackingMode=MTLHazardTrackingModeTracked; td.usage=MTLTextureUsageRenderTarget;
            id<MTLTexture> target=td?[device newTextureWithDescriptor:td]:nil;
            if (!target || target.device!=device) return fail(report,@"Actual target allocation failed.");
            std::vector<uint8_t> zeros(Side*RowBytes,0);
            [target replaceRegion:MTLRegionMake2D(0,0,Side,Side) mipmapLevel:0 withBytes:zeros.data() bytesPerRow:RowBytes];
            id<MTLCommandBuffer> command=[queue commandBuffer];
            MTLRenderPassDescriptor* rd=[MTLRenderPassDescriptor renderPassDescriptor];
            if (!command || !rd) return fail(report,@"Command/pass allocation failed.");
            rd.colorAttachments[0].texture=target; rd.colorAttachments[0].loadAction=MTLLoadActionLoad;
            rd.colorAttachments[0].storeAction=MTLStoreActionStore;
            id<MTLRenderCommandEncoder> render=[command renderCommandEncoderWithDescriptor:rd];
            if (!render) return fail(report,@"Actual render encoder failed.");
            [render setRenderPipelineState:pipeline];
            if (!rejects(^{[render setVertexBufferOffset:0 atIndex:0];}) ||
                !rejects(^{[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}) ||
                !rejects(^{[render setVertexBuffer:positions offset:1 atIndex:0];}) ||
                !rejects(^{[render setVertexBuffer:positions offset:128 atIndex:0];}) ||
                !rejects(^{[render setVertexBuffer:positions offset:16 atIndex:1];}) ||
                !rejects(^{[render setVertexBuffer:nil offset:16 atIndex:0];}))
                return fail(report,@"A missing, unaligned, short or wrong-index vertex binding was accepted.");
            [render setVertexBuffer:positions offset:16 atIndex:0];
            if (!rejects(^{[render setVertexBufferOffset:1 atIndex:0];}) ||
                !rejects(^{[render setVertexBufferOffset:128 atIndex:0];}) ||
                !rejects(^{[render setVertexBufferOffset:16 atIndex:1];}))
                return fail(report,@"An invalid vertex offset change was accepted.");
            if (profile) {
                if (!rejects(^{[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}))
                    return fail(report,@"Combined pipeline accepted a missing fragment buffer.");
                [render setFragmentBuffer:positions offset:16 atIndex:0];
                if (!rejects(^{[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}))
                    return fail(report,@"Combined pipeline accepted one BO for both source roles.");
                [render setFragmentBuffer:colors offset:16 atIndex:0];
                if (passIndex) [render setFragmentBufferOffset:32 atIndex:0];
            }
            passReport[@"invalid_binding_cases_rejected"]=@YES;
            if (passIndex) [render setVertexBufferOffset:80 atIndex:0];
            [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            if (!passIndex) {
                [render setVertexBufferOffset:80 atIndex:0];
                if (profile) [render setFragmentBufferOffset:32 atIndex:0];
            }
            [render setVertexBuffer:nil offset:0 atIndex:0];
            if (profile) [render setFragmentBuffer:nil offset:0 atIndex:0];
            [render endEncoding]; render=nil;
            if (profile && passIndex) { positions=nil; colors=nil; }
            id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
            if (!blit) return fail(report,@"Ordered readback encoder failed.");
            [blit synchronizeResource:target]; [blit endEncoding];
            auto counters=std::make_shared<Counters>();
            [command addScheduledHandler:^(id<MTLCommandBuffer> done){(void)done;++counters->scheduled;}];
            [command addCompletedHandler:^(id<MTLCommandBuffer> done){(void)done;++counters->completed;}];
            setPhase(report,@"actual GPU submission and readback"); report[@"gpu_submission_attempted"]=@YES;
            [command commit]; [command waitUntilCompleted];
            passReport[@"command_status"]=@(command.status);
            passReport[@"scheduled_handlers"]=@(counters->scheduled.load());
            passReport[@"completed_handlers"]=@(counters->completed.load());
            if (command.status!=MTLCommandBufferStatusCompleted || command.error || counters->scheduled!=1 || counters->completed!=1)
                return fail(report,errorText(command.error,@"Actual draw, retirement or completion callback failed."));
            std::vector<uint8_t> readback(HostGuard+Side*ReadPitch+HostGuard,0xcd);
            [target getBytes:readback.data()+HostGuard bytesPerRow:ReadPitch fromRegion:MTLRegionMake2D(0,0,Side,Side) mipmapLevel:0];
            if (!verify(readback,expected[profile?passIndex:0],passIndex,passReport)) return false;
        }
        if ([passes[0][@"actual_rgba_sha256"] isEqual:passes[1][@"actual_rgba_sha256"]])
            return fail(report,@"The same shader and source BO did not produce different uploaded geometry.");
        profileReport[@"same_pipeline_two_source_uploads"]=@YES;
        profileReport[@"actual_pixel_results_differ"]=@YES;
    }
    setPhase(report,@"verified actual vertex-buffer pixels");
    return true;
}
}
int main(int argc,char** argv) {
    if (argc!=10 || std::strcmp(argv[1],"--run") || std::strcmp(argv[2],"--registry") ||
        std::strcmp(argv[4],"--vertex-metallib") || std::strcmp(argv[6],"--constant-metallib") || std::strcmp(argv[8],"--fragment-metallib")) {
        std::fprintf(stderr,"Usage: %s --run --registry ACCELERATOR_ID --vertex-metallib /absolute/vertex-buffer.metallib --constant-metallib /absolute/graphics.metallib --fragment-metallib /absolute/fragment-buffer.metallib\nTARGET ONLY; never run on the build Mac.\n",argv[0]); return 2;
    }
    char* end=nullptr;errno=0;const auto registry=std::strtoull(argv[3],&end,0);
    if (errno || !argv[3][0] || argv[3][0]<'0' || argv[3][0]>'9' || !registry || !end || *end || argv[5][0]!='/' || argv[7][0]!='/' || argv[9][0]!='/') return 2;
    @autoreleasepool {
        NSMutableDictionary* report=[@{@"success":@NO,@"test":@"public-metal-vertex-buffers",@"registry_id":@(registry),
            @"gpu_submission_attempted":@NO,@"width":@(Side),@"height":@(Side)} mutableCopy];
        if (!report) return 1;
        bool success=false;
        @try {
            try {success=exercise(registry,argv[5],argv[7],argv[9],report);}
            catch(const std::exception& error){fail(report,[NSString stringWithUTF8String:error.what()]);}
        } @catch(NSException* error){fail(report,error.description);}
        report[@"success"]=@(success);
        NSError* error=nil;NSData* json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
        if (!json || error || std::fwrite(json.bytes,1,json.length,stdout)!=json.length || std::fputc('\n',stdout)==EOF) return 1;
        return success?0:1;
    }
}
