// SPDX-License-Identifier: MIT
// Actual same-owner graphics capture: private DMA, topology and canonical IB.
#include "HackyNaviVMContext.hpp"
#include "HackyNaviGARTController.hpp"
#include "hackynavi/gfx11_graphics_draw.hpp"
#include "hackynavi/gfx11_graphics_memory.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
using namespace hackynavi;
namespace {
uint64_t nowUs() { uint64_t ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);return ns/1000; }
struct Interval { uint64_t first, bytes; };
constexpr size_t kMaximumIntervals = HackyNaviVMContext::kBufferSlots *
    HackyNaviDMABuffer::kMaxSegments + vmpt::kMaxPages + 2;
bool pointer(const void* p, size_t bytes, size_t alignment = 1) {
    return p && bytes && !(reinterpret_cast<uintptr_t>(p) % alignment) &&
        bytes - 1 <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(p);
}
bool sameHandle(HackyNaviVMContext::Handle a, HackyNaviVMContext::Handle b) {
    return a.id == b.id && a.generation == b.generation;
}
void swap(Interval& a, Interval& b) { const Interval t = a; a = b; b = t; }
void sift(Interval* intervals, size_t count, size_t root) {
    while (root < count/2) {
        size_t child = root*2 + 1;
        if (child + 1 < count && intervals[child].first < intervals[child+1].first) ++child;
        if (intervals[root].first >= intervals[child].first) return;
        swap(intervals[root], intervals[child]); root = child;
    }
    // An even-sized heap has its last parent at count/2-1; the loop above
    // includes it. No recursion or attacker-dependent stack allocation.
}
bool disjoint(Interval* intervals, size_t count) {
    if (!count) return false;
    for (size_t n = count/2; n; --n) sift(intervals, count, n-1);
    for (size_t n = count; n > 1; --n) {
        swap(intervals[0], intervals[n-1]); sift(intervals, n-1, 0);
    }
    for (size_t n = 1; n < count; ++n)
        if (intervals[n].first - intervals[n-1].first < intervals[n-1].bytes) return false;
    return true;
}
bool ownerGeometry(const HackyNaviDMABuffer* dma, size_t& segments) {
    if (!dma || !dma->cpuAddress() || !dma->size() || dma->size() > HackyNaviDMABuffer::kMaxBytes ||
        (dma->size() & 4095) || !pointer(dma->cpuAddress(), size_t(dma->size()), 4096) ||
        !dma->segmentCount() || dma->segmentCount() > HackyNaviDMABuffer::kMaxSegments ||
        dma->mappingKind() == HackyNaviDMABuffer::MappingKind::Unallocated) return false;
    uint64_t bytes = 0;
    for (uint32_t n = 0; n < dma->segmentCount(); ++n) {
        const auto* s = dma->segment(n);
        if (!s || !s->length || ((s->dmaAddress | s->length) & 4095) ||
            s->dmaAddress >= gmc11::kSystemDmaAddressLimit ||
            s->length > gmc11::kSystemDmaAddressLimit - s->dmaAddress ||
            s->length > dma->size()-bytes) return false;
        bytes += s->length;
    }
    if (bytes != dma->size() || dma->segmentCount() > kMaximumIntervals-segments) return false;
    segments += dma->segmentCount(); return true;
}
} // namespace

struct HackyNaviVMContext::GraphicsCapture {
    gfx11draw::VertexFragmentBufferedRequest request;
    gfx11draw::BufferedWorkspace workspace;
    gfx11draw::VertexBufferedWorkspace vertexWorkspace;
    graphicsmemory::Mapping placements[kMappingSlots];
    graphicsmemory::Plan placement;
    CapturedResult result;
    uint64_t topologyGeneration, captureGeneration;
    Interval* intervals;
    size_t intervalCapacity;
    bool sealed,buffered,vertexBuffered;
    const gfxisolated::Binding& binding(size_t n) const {
        return n==0?request.draw.code:n==1?request.draw.target:n==2?request.draw.attributes:
            n==3?request.draw.indirect:n==4&&vertexBuffered?request.vertex:request.fragment;
    }
    size_t bindingCount() const {return 4+size_t(buffered)+size_t(vertexBuffered);}
    bool control(size_t n) const { return n==0||n==2||n==3; }
    ShaderClass shaderClass() const {
        return vertexBuffered?(buffered?ShaderClass::Gfx1100PositionVertexFragmentBuffersV1:
            ShaderClass::Gfx1100PositionVertexBufferV1):(buffered?
            ShaderClass::Gfx1100PositionFragmentBufferV1:ShaderClass::Gfx1100PositionConstantColorV1);
    }
    bool shaderClass(ShaderClass value) const {return value==shaderClass();}
    gfx11graphics::Status admitImage() {
        return vertexBuffered?(buffered?gfx11graphics::validateVertexFragmentBuffers(request.draw.image,vertexWorkspace.draw.program):
            gfx11graphics::validateVertexBuffer(request.draw.image,vertexWorkspace.draw.program)):
            (buffered?gfx11graphics::validateFragmentBuffer(request.draw.image,workspace.draw.program):
            gfx11graphics::validate(request.draw.image,workspace.draw.program));
    }
    gfx11draw::Status encode(uint32_t* output,size_t capacity,size_t& words) {
        if(vertexBuffered) {
            if(buffered)return gfx11draw::encode(request,vertexWorkspace,output,capacity,words);
            const gfx11draw::VertexBufferedRequest vertex={request.draw,request.vertex,request.vertexOffset};
            return gfx11draw::encode(vertex,vertexWorkspace,output,capacity,words);
        }
        if(buffered) {
            const gfx11draw::BufferedRequest fragment={request.draw,request.fragment,request.fragmentOffset};
            return gfx11draw::encode(fragment,workspace,output,capacity,words);
        }
        return gfx11draw::encode(request.draw,workspace.draw,output,capacity,words);
    }
    gfx11draw::Status validate(const uint32_t* commands,size_t words) {
        if(vertexBuffered) {
            if(buffered)return gfx11draw::validate(request,vertexWorkspace,commands,words);
            const gfx11draw::VertexBufferedRequest vertex={request.draw,request.vertex,request.vertexOffset};
            return gfx11draw::validate(vertex,vertexWorkspace,commands,words);
        }
        if(buffered) {
            const gfx11draw::BufferedRequest fragment={request.draw,request.fragment,request.fragmentOffset};
            return gfx11draw::validate(fragment,workspace,commands,words);
        }
        return gfx11draw::validate(request.draw,workspace.draw,commands,words);
    }
};

bool HackyNaviVMContext::graphicsMappingsValidLocked(const gfxisolated::Binding& target,bool controls) const {
    const auto* p=capturedGraphics_;
    if(controls&&(!p||!p->sealed))return false;
    uint64_t seenBuffers=0;uint32_t seenBindings=0;
    for(size_t i=0;i<kMappingSlots;++i) {
        const auto& m=mappings_[i];if(!m.bytes)continue;
        if(!m.buffer.id||m.buffer.id>kBufferSlots||!m.buffer.generation)return false;
        const auto& b=buffers_[m.buffer.id-1];const uint64_t bit=UINT64_C(1)<<(m.buffer.id-1);
        if((seenBuffers&bit)||!b.dma||b.generation!=m.buffer.generation||b.mappings!=1||
           ((m.va|m.bytes|m.offset)&4095)||m.va<vmpt::kVaBegin||m.va>=vmpt::kVaEnd||
           m.bytes>vmpt::kVaEnd-m.va||m.offset>b.dma->size()||m.bytes>b.dma->size()-m.offset)return false;
        seenBuffers|=bit;
        for(size_t j=0;j<i;++j) {
            const auto& old=mappings_[j];
            if(old.bytes&&m.va<old.va+old.bytes&&old.va<m.va+m.bytes)return false;
        }
        const gfxisolated::Binding* expected=nullptr;size_t index=1;
        if(controls) {
            for(index=0;index<p->bindingCount();++index)if(m.buffer.id==p->binding(index).id) {expected=&p->binding(index);break;}
        } else if(m.buffer.id==target.id)expected=&target;
        if(expected) {
            if(!sameHandle(m.buffer,{expected->id,expected->generation})||
               expected->owner!=reinterpret_cast<uintptr_t>(this)||expected->epoch!=epoch_||
               m.va!=expected->gpu||m.bytes!=expected->bytes||m.offset!=expected->bufferOffset||
               m.access!=expected->access||b.dma->size()!=expected->allocationBytes||
               b.internal!=(controls&&p->control(index)))return false;
            seenBindings|=1u<<index;
        } else {
            // A persistent unrelated data mapping cannot become hidden code,
            // another target alias or an internal control. Only the explicitly
            // retained vertex/fragment resources can be addressed by shaders;
            // the canonical IB never addresses unrelated mappings.
            if(b.internal||(m.access!=Read&&m.access!=(Read|Write)))return false;
        }
    }
    for(uint32_t n=0;n<kBufferSlots;++n) {
        const auto& b=buffers_[n];const bool mapped=bool(seenBuffers&(UINT64_C(1)<<n));
        if(b.mappings!=uint32_t(mapped)||(b.internal&&!mapped)||(!b.dma&&(mapped||b.internal)))return false;
        if(!controls&&b.internal)return false;
    }
    return seenBindings==(controls?((1u<<p->bindingCount())-1):2u);
}

bool HackyNaviVMContext::graphicsStorageDistinctLocked() const {
    GraphicsCapture* p=capturedGraphics_;
    if(!p || !p->intervals || !p->intervalCapacity || p->intervalCapacity>kMaximumIntervals ||
       !pointer(p->intervals,p->intervalCapacity*sizeof(Interval),alignof(Interval)))return false;
    size_t required=2,cpuCount=0;
    for(const auto& b:buffers_)if(b.dma) {
        if(!ownerGeometry(b.dma,required)||required>p->intervalCapacity)return false;
        p->intervals[cpuCount++]={reinterpret_cast<uintptr_t>(b.dma->cpuAddress()),b.dma->size()};
    }
    if(!tree_.pageCount()||tree_.pageCount()>vmpt::kMaxPages)return false;
    for(uint16_t n=0;n<tree_.pageCount();++n) {
        const auto* page=tree_.page(n);
        const auto* dma=page?static_cast<const HackyNaviDMABuffer*>(page->storage.owner):nullptr;
        if(!ownerGeometry(dma,required)||required>p->intervalCapacity||dma->size()!=4096||dma->segmentCount()!=1||
           page->storage.bytes!=dma->cpuAddress()||page->storage.dmaAddress!=dma->segment(0)->dmaAddress)return false;
        p->intervals[cpuCount++]={reinterpret_cast<uintptr_t>(dma->cpuAddress()),dma->size()};
    }
    if(required!=p->intervalCapacity)return false;
    p->intervals[cpuCount++]={reinterpret_cast<uintptr_t>(p),sizeof(*p)};
    p->intervals[cpuCount++]={reinterpret_cast<uintptr_t>(p->intervals),p->intervalCapacity*sizeof(Interval)};
    if(!disjoint(p->intervals,cpuCount))return false;
    size_t ioCount=0;
    for(const auto& b:buffers_)if(b.dma)for(uint32_t n=0;n<b.dma->segmentCount();++n) {
        const auto* s=b.dma->segment(n);p->intervals[ioCount++]={s->dmaAddress,s->length};
    }
    for(uint16_t n=0;n<tree_.pageCount();++n) {
        const auto* dma=static_cast<const HackyNaviDMABuffer*>(tree_.page(n)->storage.owner);
        const auto* s=dma->segment(0);p->intervals[ioCount++]={s->dmaAddress,s->length};
    }
    return ioCount+2==p->intervalCapacity && disjoint(p->intervals,ioCount);
}

bool HackyNaviVMContext::graphicsCaptureValidLocked() const {
    GraphicsCapture* p=capturedGraphics_;
    if(!p||!p->sealed||capturedProduct_||!p->shaderClass(capturedClass_)||
       !(hardwareHold_?gart_->accessReady():gart_->normalAccessReady())||!providerReady(hardwareHold_)||
       !capturedWords_||capturedWords_>gfx11draw::kMaxCommandDwords||
       p->captureGeneration!=captureGeneration_||!p->topologyGeneration||
       p->topologyGeneration!=gart_->ownerGeneration_||
       !gart_->graphicsTopologyValid_||!gart_->graphicsTopologyAttempted_||
       gart_->graphicsTopologyGeneration_!=p->topologyGeneration||
       !gart_->colorTopologyValid_||!gart_->colorTopologyAttempted_||
       gart_->colorTopologyGeneration_!=p->topologyGeneration||
       capturedParameters_.id||capturedParameters_.generation||
       p->request.draw.owner!=reinterpret_cast<uintptr_t>(this)||p->request.draw.epoch!=epoch_||p->request.draw.vmid!=vmid_||
       !sameHandle(capturedShader_,{p->request.draw.code.id,p->request.draw.code.generation})||
       !sameHandle(capturedIndirect_,{p->request.draw.indirect.id,p->request.draw.indirect.generation})||
       !sameHandle(capturedAttributes_,{p->request.draw.attributes.id,p->request.draw.attributes.generation})||
       capturedIndirectGpu_!=p->request.draw.indirect.gpu||
       !sameHandle(capturedResult_.buffer,p->result.buffer)||
       capturedResult_.offset!=p->result.offset||capturedResult_.bytes!=p->result.bytes)return false;
    const auto& t=gart_->graphicsTopologySnapshot_.facts;
    const auto& color=gart_->colorTopologySnapshot_.topology;
    if(p->request.draw.topology.color.gb_addr_config!=color.gb_addr_config||
       p->request.draw.topology.color.revision_strap!=color.revision_strap||
       p->request.draw.topology.maxShaderEngines!=t.maxSe||p->request.draw.topology.maxRenderBackends!=t.maxRenderBackends||
       p->request.draw.topology.minGoodCuPerSa!=t.minGoodCuPerSa)return false;
    for(size_t n=0;n<p->bindingCount();++n) {
        const auto& b=p->binding(n);
        if(!b.id||b.id>kBufferSlots||b.owner!=p->request.draw.owner||b.epoch!=epoch_)return false;
        const auto& owner=buffers_[b.id-1];
        if(!owner.dma||!owner.dma->cpuAddress()||owner.generation!=b.generation||owner.mappings!=1||
           owner.internal!=p->control(n)||owner.dma->size()!=b.allocationBytes)return false;
        if(n==0&&p->request.draw.image.image!=static_cast<const uint8_t*>(owner.dma->cpuAddress()))return false;
    }
    if(!graphicsMappingsValidLocked(p->request.draw.target,true)||!graphicsStorageDistinctLocked()||
       !sameHandle(p->result.buffer,{p->request.draw.target.id,p->request.draw.target.generation})||
       p->result.offset!=p->request.draw.target.bufferOffset||p->result.bytes!=p->request.draw.targetLayout.total_bytes||
       !p->result.bytes||p->result.bytes>kMaxBufferBytes||
       p->request.draw.target.bytes!=((p->result.bytes+4095)&~UINT64_C(4095)))return false;
    const auto* commands=static_cast<const uint32_t*>(buffers_[capturedIndirect_.id-1].dma->cpuAddress());
    if(p->validate(commands,capturedWords_)!=gfx11draw::Status::Ok)return false;
    for(size_t n=capturedWords_;n<gfx11draw::kMaxCommandDwords;++n)if(commands[n])return false;
    // The attribute-ring contents are actual GPU-writable scratch. Only its
    // private ownership/binding is immutable, never a presumed zero image.
    return hardwareHold_?gart_->accessReady():gart_->normalAccessReady();
}

IOReturn HackyNaviVMContext::captureProductGraphics(HackyNaviFunctionGroup::Operation& scope,
    const ProductGraphicsRequest& request) {
    return captureGraphicsProduct(scope,request,{0,0},0,{0,0},0);
}
IOReturn HackyNaviVMContext::captureProductGraphics(HackyNaviFunctionGroup::Operation& scope,
    const ProductBufferedGraphicsRequest& request) {
    const ProductBufferedGraphicsRequest incoming=request;
    if(!incoming.fragment.id||!incoming.fragment.generation)return kIOReturnBadArgument;
    return captureGraphicsProduct(scope,incoming.draw,incoming.fragment,incoming.fragmentOffset,{0,0},0);
}
IOReturn HackyNaviVMContext::captureProductGraphics(HackyNaviFunctionGroup::Operation& scope,
    const ProductVertexBufferedGraphicsRequest& request) {
    const ProductVertexBufferedGraphicsRequest incoming=request;
    if(!incoming.vertex.id||!incoming.vertex.generation)return kIOReturnBadArgument;
    return captureGraphicsProduct(scope,incoming.draw,{0,0},0,incoming.vertex,incoming.vertexOffset);
}
IOReturn HackyNaviVMContext::captureProductGraphics(HackyNaviFunctionGroup::Operation& scope,
    const ProductVertexFragmentBufferedGraphicsRequest& request) {
    const ProductVertexFragmentBufferedGraphicsRequest incoming=request;
    if(!incoming.vertex.id||!incoming.vertex.generation||!incoming.fragment.id||!incoming.fragment.generation)
        return kIOReturnBadArgument;
    return captureGraphicsProduct(scope,incoming.draw,incoming.fragment,incoming.fragmentOffset,
        incoming.vertex,incoming.vertexOffset);
}
IOReturn HackyNaviVMContext::captureGraphicsProduct(HackyNaviFunctionGroup::Operation& scope,
    const ProductGraphicsRequest& request,Handle fragment,uint64_t fragmentOffset,Handle vertex,uint64_t vertexOffset) {
    if(!gart_)return kIOReturnNotReady;
    const ProductGraphicsRequest incoming=request;
    const bool buffered=fragment.id!=0;
    const bool vertexBuffered=vertex.id!=0;
    gfx11graphics::Layout imageLayout{};
    if(incoming.imageBytes!=gfx11draw::kCodeBytes||!pointer(incoming.image,incoming.imageBytes)||
       gfx11graphics::layout(incoming.vertexBytes,incoming.fragmentBytes,imageLayout)!=gfx11graphics::Status::Ok||
       imageLayout.imageBytes!=incoming.imageBytes||!incoming.target.id||!incoming.target.generation||
       (incoming.targetOffset&4095)||(buffered&&((fragmentOffset&15)||fragment.id==incoming.target.id))||
       (vertexBuffered&&((vertexOffset&15)||vertex.id==incoming.target.id||(buffered&&vertex.id==fragment.id)))||
       !incoming.targetLayout.total_bytes||incoming.targetLayout.total_bytes>kMaxBufferBytes)
        return kIOReturnBadArgument;
    // Do not inspect selectors for an already sealed/failed VM. Topology
    // methods own the GART mutex, so invoke them before taking the VM lock.
    IOLockLock(gart_->lock_);
    IOReturn status=mutableState();
    if(status==kIOReturnSuccess&&!gart_->colorTopologyReady(scope))status=kIOReturnNotReady;
    finish(status);if(status!=kIOReturnSuccess)return status;
    HNColorTopology color{};graphicstopology::Facts facts{};
    status=gart_->copyColorTopology(scope,color);
    if(status==kIOReturnNotReady) {
        const uint64_t now=nowUs();if(now>UINT64_MAX-1000000)return kIOReturnTimeout;
        status=gart_->captureColorTopology(scope,now+1000000,color);
    }
    if(status!=kIOReturnSuccess)return status;
    status=gart_->copyGraphicsTopology(scope,facts);
    if(status==kIOReturnNotReady) {
        const uint64_t now=nowUs();if(now>UINT64_MAX-1000000)return kIOReturnTimeout;
        status=gart_->captureGraphicsTopology(scope,now+1000000,facts);
    }
    if(status!=kIOReturnSuccess)return status;
    IOLockLock(gart_->lock_);
    status=mutableState();if(status!=kIOReturnSuccess)return finish(status);
    if(!gart_->colorTopologyReady(scope)||!gart_->graphicsTopologyValid_||
       gart_->graphicsTopologyGeneration_!=scope.generation())return finish(kIOReturnOffline);
    status=beginCaptureLocked();if(status!=kIOReturnSuccess)return finish(status);
    // Numeric outputs above never authorize bindings. Copy only from the
    // private retained owner records while the actual VM/GART lock is held.
    const auto& topology=gart_->graphicsTopologySnapshot_.facts;
    if(!topology.maxSe||topology.maxSe>6||topology.maxRenderBackends!=topology.maxSe*4||
       topology.minGoodCuPerSa<2||topology.minGoodCuPerSa>16||(topology.minGoodCuPerSa&1))
        return finish(kIOReturnUnsupported);
    const HNColorRequest dimensions={incoming.targetLayout.width,incoming.targetLayout.height};
    if(hn_color_target_validate(&gart_->colorTopologySnapshot_.topology,&dimensions,&incoming.targetLayout)!=HN_COLOR_OK)
        return finish(kIOReturnBadArgument);
    const uint64_t targetBytes=(incoming.targetLayout.total_bytes+4095)&~UINT64_C(4095);
    gfxisolated::Binding target{};uint64_t targetGpu=0;
    if(!bindingLocked(incoming.target,incoming.targetOffset,targetBytes,Read|Write,target,targetGpu)||
       target.bufferOffset!=incoming.targetOffset||target.bytes!=targetBytes||target.access!=(Read|Write)||
       !graphicsMappingsValidLocked(target,false))return finish(kIOReturnNotPermitted);
    gfxisolated::Binding resource{};uint64_t fragmentGpu=0;
    if(buffered&&(!bindingLocked(fragment,fragmentOffset,16,Read,resource,fragmentGpu)||
        resource.access!=Read||resource.bufferOffset>fragmentOffset||resource.id==target.id))
        return finish(kIOReturnNotPermitted);
    gfxisolated::Binding vertexResource{};uint64_t vertexGpu=0;
    if(vertexBuffered&&(!bindingLocked(vertex,vertexOffset,48,Read,vertexResource,vertexGpu)||
        vertexResource.access!=Read||vertexResource.bufferOffset>vertexOffset||vertexResource.id==target.id||
        (buffered&&vertexResource.id==resource.id)))return finish(kIOReturnNotPermitted);
    auto* p=static_cast<GraphicsCapture*>(IOMalloc(sizeof(GraphicsCapture)));
    if(!p)return finish(kIOReturnNoMemory);
    bzero(p,sizeof(*p));capturedGraphics_=p;
    p->buffered=buffered;p->vertexBuffered=vertexBuffered;
    if(buffered) {
        p->request.fragment=resource;p->request.fragmentOffset=fragmentOffset-resource.bufferOffset;
    }
    if(vertexBuffered) {
        p->request.vertex=vertexResource;p->request.vertexOffset=vertexOffset-vertexResource.bufferOffset;
    }
    // Every subsequent failure retains the partial DMA and page ownership.
    // No public write/free/mapping operation can reuse this failed capture.
    p->request.draw.owner=reinterpret_cast<uintptr_t>(this);p->request.draw.epoch=epoch_;p->request.draw.vmid=vmid_;
    p->request.draw.topology={gart_->colorTopologySnapshot_.topology,topology.maxSe,topology.maxRenderBackends,topology.minGoodCuPerSa};
    p->topologyGeneration=scope.generation();p->captureGeneration=captureGeneration_;
    p->request.draw.targetLayout=incoming.targetLayout;p->result={incoming.target,incoming.targetOffset,incoming.targetLayout.total_bytes};
    p->request.draw.target=target;
    size_t mappings=0;
    for(const auto& m:mappings_)if(m.bytes)p->placements[mappings++]={m.va,m.bytes};
    const uint64_t attributes=uint64_t(topology.maxSe)*gfx11draw::kAttributeBytesPerSe;
    if(status==kIOReturnSuccess&&graphicsmemory::place(p->placements,mappings,attributes,
        gfx11draw::kIndirectBytes,p->placement)!=graphicsmemory::Status::Ok)status=kIOReturnNoSpace;
    gfxisolated::Binding* controls[]={&p->request.draw.code,&p->request.draw.attributes,&p->request.draw.indirect};
    const uint64_t bytes[]={gfx11draw::kCodeBytes,attributes,gfx11draw::kIndirectBytes};
    const uint64_t addresses[]={p->placement.code,p->placement.attributes,p->placement.indirect};
    const uint32_t permissions[]={Read|Execute,Read|Write,Read|Execute};
    for(unsigned n=0;n<3&&status==kIOReturnSuccess;++n) {
        Handle handle{};uint64_t gpu=0;
        status=allocateBufferLocked(bytes[n],4096,handle);if(status!=kIOReturnSuccess)break;
        Buffer& b=buffers_[handle.id-1];
        // Allocation is fresh, coherent system DMA. Its whole CPU image is
        // zeroed before any binding exists; only immutable code is copied.
        bzero(b.dma->cpuAddress(),size_t(bytes[n]));
        if(n==0) {
            bcopy(incoming.image,b.dma->cpuAddress(),incoming.imageBytes);
            p->request.draw.image={static_cast<const uint8_t*>(b.dma->cpuAddress()),incoming.imageBytes,
                incoming.vertexBytes,incoming.fragmentBytes};
            if(p->admitImage()!=gfx11graphics::Status::Ok)
                status=kIOReturnBadArgument;
        }
        if(status==kIOReturnSuccess)status=mapLocked(handle,0,addresses[n],bytes[n],permissions[n]);
        if(status==kIOReturnSuccess&&!bindingLocked(handle,0,bytes[n],permissions[n],*controls[n],gpu))
            status=kIOReturnInternalError;
        b.internal=true; // including a partial allocation/map: never client-visible
    }
    size_t capacity=2;
    if(status==kIOReturnSuccess) {
        for(const auto& b:buffers_)if(b.dma&&!ownerGeometry(b.dma,capacity)) {status=kIOReturnBadArgument;break;}
        for(uint16_t n=0;n<tree_.pageCount()&&status==kIOReturnSuccess;++n) {
            const auto* page=tree_.page(n);
            if(!page||!ownerGeometry(static_cast<const HackyNaviDMABuffer*>(page->storage.owner),capacity))status=kIOReturnBadArgument;
        }
    }
    if(status==kIOReturnSuccess) {
        p->intervals=static_cast<Interval*>(IOMalloc(capacity*sizeof(Interval)));
        if(!p->intervals)status=kIOReturnNoMemory;else p->intervalCapacity=capacity;
    }
    if(status!=kIOReturnSuccess) {state_=State::FailedUnpublished;return finish(status);}
    auto* commands=static_cast<uint32_t*>(buffers_[p->request.draw.indirect.id-1].dma->cpuAddress());size_t words=0;
    if(!graphicsStorageDistinctLocked()||p->encode(commands,gfx11draw::kMaxCommandDwords,words)!=gfx11draw::Status::Ok) {
        state_=State::FailedUnpublished;return finish(kIOReturnBadArgument);
    }
    capturedClass_=p->shaderClass();
    capturedShader_={p->request.draw.code.id,p->request.draw.code.generation};
    capturedIndirect_={p->request.draw.indirect.id,p->request.draw.indirect.generation};
    capturedAttributes_={p->request.draw.attributes.id,p->request.draw.attributes.generation};
    capturedIndirectGpu_=p->request.draw.indirect.gpu;capturedWords_=static_cast<uint32_t>(words);capturedResult_=p->result;
    p->sealed=true;state_=State::Captured;
    if(!captureValidLocked()) {state_=State::FailedUnpublished;return finish(kIOReturnNotPermitted);}
    return finish(kIOReturnSuccess);
}

bool HackyNaviVMContext::graphicsControlMaskLocked(uint64_t& mask) const {
    mask=0;
    const auto* p=capturedGraphics_;
    if(!p||!p->sealed||capturedProduct_||!p->shaderClass(capturedClass_)||
       capturedParameters_.id||capturedParameters_.generation)return false;
    const Handle handles[]={capturedShader_,capturedAttributes_,capturedIndirect_};
    const gfxisolated::Binding* bindings[]={&p->request.draw.code,&p->request.draw.attributes,&p->request.draw.indirect};
    for(unsigned n=0;n<3;++n) {
        const auto h=handles[n];const auto& b=*bindings[n];
        if(!h.id||h.id>kBufferSlots||!h.generation||!sameHandle(h,{b.id,b.generation}))return false;
        const auto& owner=buffers_[h.id-1];const uint64_t bit=UINT64_C(1)<<(h.id-1);
        if((mask&bit)||!owner.dma||!owner.internal||owner.generation!=h.generation||owner.mappings!=1)return false;
        mask|=bit;
    }
    for(uint32_t n=0;n<kBufferSlots;++n)if(buffers_[n].internal!=bool(mask&(UINT64_C(1)<<n)))return false;
    return mask!=0;
}
bool HackyNaviVMContext::graphicsWritableControlLocked(const Mapping& mapping) const {
    const auto* p=capturedGraphics_;
    if(!p||!p->sealed||!p->shaderClass(capturedClass_))return false;
    const auto& b=p->request.draw.attributes;
    return sameHandle(mapping.buffer,capturedAttributes_)&&sameHandle(mapping.buffer,{b.id,b.generation})&&
        mapping.va==b.gpu&&mapping.bytes==b.bytes&&mapping.offset==0&&b.bufferOffset==0&&
        mapping.bytes==b.allocationBytes&&mapping.access==(Read|Write)&&b.access==(Read|Write);
}
void HackyNaviVMContext::releaseGraphicsCaptureLocked() {
    if(!capturedGraphics_)return;
    auto* p=capturedGraphics_;capturedGraphics_=nullptr;
    if(p->intervals)IOFree(p->intervals,p->intervalCapacity*sizeof(Interval));
    bzero(p,sizeof(*p));IOFree(p,sizeof(GraphicsCapture));
}

