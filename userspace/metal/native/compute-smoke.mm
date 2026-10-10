// SPDX-License-Identifier: MIT
// Target-only public Metal test. The build runner compiles but NEVER runs it.
// No direct Context calls, injected device, software executor or fallback GPU.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

namespace {
constexpr NSUInteger kElements=4099,kGuard=64,kWords=kElements+kGuard;
constexpr uint32_t kSentinel=0xd3e5a791;
using Words=std::array<uint32_t,kWords>;
const char* message(NSError* error,const char* fallback) {
    const char* value=error.description.UTF8String;return value?value:fallback;
}
struct Counters { std::atomic<unsigned> scheduled{0},completed{0}; };
void phase(const char* text) {
    std::fprintf(stderr,"PHASE: %s\n",text);std::fflush(stderr);
}
bool check(bool value,const char* text) {
    if (!value) std::fprintf(stderr,"FAIL: %s\n",text);
    return value;
}
bool verify(id<MTLBuffer> buffer,const Words& input,uint32_t add) {
    const auto* actual=static_cast<const uint32_t*>(buffer.contents);
    if (!check(actual!=nullptr,"Managed contents pointer")) return false;
    for (NSUInteger i=0;i<kWords;++i) {
        const uint32_t expected=i<kElements?input[i]+add:kSentinel;
        if (actual[i]!=expected) {
            std::fprintf(stderr,"FAIL: word %llu actual=0x%08x expected=0x%08x\n",
                static_cast<unsigned long long>(i),actual[i],expected);
            return false;
        }
    }
    return true;
}
bool dispatch(id<MTLCommandQueue> queue,id<MTLComputePipelineState> pipeline,
              id<MTLBuffer> source,id<MTLBuffer> destination) {
    id<MTLCommandBuffer> command=[queue commandBuffer];
    if (!check(command!=nil,"Create actual Metal command buffer")) return false;
    auto counters=std::make_shared<Counters>();
    [command addScheduledHandler:^(id<MTLCommandBuffer> value) { (void)value;++counters->scheduled; }];
    [command addCompletedHandler:^(id<MTLCommandBuffer> value) { (void)value;++counters->completed; }];
    id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];
    if (!check(compute!=nil,"Create actual compute encoder")) return false;
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:source offset:0 atIndex:0];
    [compute setBuffer:destination offset:0 atIndex:1];
    [compute dispatchThreads:MTLSizeMake(kElements,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [compute endEncoding];
    id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
    if (!check(blit!=nil,"Create ordered managed-readback encoder")) return false;
    [blit synchronizeResource:destination];
    [blit endEncoding];
    phase("commit encoded compute and ordered readback");
    [command commit];
    phase("wait for actual command completion");
    [command waitUntilCompleted];
    phase("verify command status and exactly-once callbacks");
    if (command.status!=MTLCommandBufferStatusCompleted || command.error) {
        std::fprintf(stderr,"FAIL: command status=%lu error=%s\n",static_cast<unsigned long>(command.status),
            message(command.error,"(none)"));
        return false;
    }
    return check(counters->scheduled==1&&counters->completed==1,"Scheduled/completed handlers ran exactly once");
}
int exercise(uint64_t registry,NSString* path) {
    phase("public Metal discovery of the exact target");
    id<MTLDevice> device=nil;
    for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
        if (candidate.registryID!=registry) continue;
        if (!check(device==nil,"Unique public Metal registry identity")) return 1;
        device=candidate;
    }
    if (!check(device!=nil,"Actual target discovered by public MTLCopyAllDevices") ||
        !check([NSStringFromClass([device class]) isEqualToString:@"HNMetalDevice"],"Actual HackyNavi Metal device class")) return 1;
    NSError* error=nil;
    phase("load actual add_one metallib and create compute pipeline");
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
    if (!library || error) {
        std::fprintf(stderr,"FAIL: actual metallib load: %s\n",message(error,"no library"));return 1;
    }
    id<MTLFunction> function=[library newFunctionWithName:@"add_one"];
    if (!check(function!=nil&&function.functionType==MTLFunctionTypeKernel,"Real add_one compute function")) return 1;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline || error) {
        std::fprintf(stderr,"FAIL: pipeline creation: %s\n",message(error,"no pipeline"));return 1;
    }
    if (!check(pipeline.maxTotalThreadsPerThreadgroup>=64,"Pipeline admits 64-thread local size")) return 1;
    Words input{},empty{};
    for (NSUInteger i=0;i<kWords;++i) { input[i]=uint32_t(i)*0x9e3779b9u+17u;empty[i]=kSentinel; }
    const MTLResourceOptions options=MTLResourceStorageModeManaged|MTLResourceHazardTrackingModeTracked;
    phase("allocate three Managed buffers and upload source/guards");
    id<MTLBuffer> a=[device newBufferWithBytes:input.data() length:sizeof(input) options:options];
    id<MTLBuffer> b=[device newBufferWithBytes:empty.data() length:sizeof(empty) options:options];
    id<MTLBuffer> c=[device newBufferWithLength:sizeof(empty) options:options];
    if (!check(a&&b&&c,"Three real Managed allocations") ||
        !check(a.storageMode==MTLStorageModeManaged&&b.storageMode==MTLStorageModeManaged&&c.storageMode==MTLStorageModeManaged,
               "Managed storage mode") || !check(a.contents&&b.contents&&c.contents,"Owned CPU contents")) return 1;
    // Exercise both initialized allocation and explicit ranged/full upload.
    input[0]=0xfffffffdu;
    static_cast<uint32_t*>(a.contents)[0]=input[0];[a didModifyRange:NSMakeRange(0,sizeof(uint32_t))];
    std::memcpy(c.contents,empty.data(),sizeof(empty));[c didModifyRange:NSMakeRange(0,sizeof(empty))];
    id<MTLCommandQueue> queue=[device newCommandQueueWithMaxCommandBufferCount:1];
    if (!check(queue!=nil,"Real capacity-one Metal queue")) return 1;
    phase("A to B GPU submission");
    if (!dispatch(queue,pipeline,a,b)) return 1;
    phase("verify B data and padding from actual readback");
    if (!verify(b,input,1)) return 1;
    // Reuse GPU output B directly as input. No CPU upload to B occurs here.
    phase("B to C GPU submission without CPU upload to B");
    if (!dispatch(queue,pipeline,b,c)) return 1;
    phase("verify retained B and new C data and padding from actual readback");
    if (!verify(b,input,1) || !verify(c,input,2)) return 1;
    std::printf("PASS: actual public Metal A->B->C, registry=%llu, %llu elements, %llu guard words, Managed upload/readback\n",
        static_cast<unsigned long long>(registry),static_cast<unsigned long long>(kElements),static_cast<unsigned long long>(kGuard));
    return 0;
}
}
int main(int argc,char** argv) {
    if (argc!=6 || std::strcmp(argv[1],"--run") || std::strcmp(argv[2],"--registry") ||
        std::strcmp(argv[4],"--metallib")) {
        std::fprintf(stderr,"Usage: %s --run --registry ID --metallib /absolute/add_one.metallib\nTarget-only; build Mac must not run this test.\n",argv[0]);return 2;
    }
    char* end=nullptr;errno=0;
    const char* value=argv[3];const uint64_t registry=std::strtoull(value,&end,0);
    if (errno||!*value||*value<'0'||*value>'9'||!registry||!end||*end||argv[5][0]!='/') return 2;
    @autoreleasepool {
        @try {
            try {
                NSString* path=[NSString stringWithUTF8String:argv[5]];
                if (!path) return 2;
                return exercise(registry,path);
            } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what());return 1; }
        } @catch (NSException* error) { std::fprintf(stderr,"FAIL: %s\n",error.description.UTF8String);return 1; }
    }
}
