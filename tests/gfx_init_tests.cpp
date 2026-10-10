// SPDX-License-Identifier: MIT
// Host-only protocol models; successful checks are not real GPU execution.
#include "gfx_init_fixture.hpp"
#include "hackynavi/gfx_init.hpp"
using namespace hackynavi;
using namespace gfxfw_test;
using namespace gfxinit_test;
namespace gi=hackynavi::gfxinit;
namespace {
uint32_t hash(const uint8_t* data,size_t n) { uint32_t h=2166136261U; for(size_t i=0;i<n;++i)h=(h^data[i])*16777619U; return h; }
void clearStateTests() {
    std::vector<uint8_t> cs(4096,0xa5);
    CHECK(gi::encodeClearState(ips(),cs.data(),cs.size())==gi::Status::Ok);
    // Independently calculated from the pinned AMD table and PM4 wire ABI.
    CHECK(hash(cs.data(),3840)==0x85f125f5);
    CHECK(le32(cs.data())==0xc0004a00 && le32(cs.data()+4)==0x20000000);
    CHECK(le32(cs.data()+20)==0xc0d76900 && le32(cs.data()+24)==0);
    CHECK(le32(cs.data()+3840-8)==0xc0001200 && le32(cs.data()+3840-4)==0);
    for(size_t i=3840;i<cs.size();++i)CHECK(cs[i]==0xa5);
    for(size_t n=0;n<3840;++n) {
        cs.assign(4096,0xa5); CHECK(gi::encodeClearState(ips(),cs.data(),n)==gi::Status::InvalidArgument);
        CHECK(cs[0]==0xa5 && cs.back()==0xa5);
    }
    auto ip=ips(); ip.blocks[0].basesDwords[1]=0x12345;
    CHECK(gi::encodeClearState(ip,cs.data(),cs.size())==gi::Status::UnsupportedLayout && cs[0]==0xa5);
}
void initializationTests() {
    std::vector<size_t> failuresAt;
    {
        Device d; gi::State state; gi::Result r={};
        CHECK(d.run(state,r)==gi::Status::Ok && state.prepared() && state.needsClearStateRetention());
        CHECK(r.stage==gi::Stage::Complete && r.transfer.bytesVerified==4096 && !d.hw.failed);
        const size_t at=size_t(d.csb.range.offset-Simulation::vramStart);
        CHECK(hash(&d.hw.memory[at],3840)==0x85f125f5);
        for(size_t i=3840;i<4096;++i)CHECK(d.hw.memory[at+i]==0);
        const uint32_t low[]={uint32_t((0x1260+0x1e44)*4),uint32_t((0x1260+0x1e45)*4),uint32_t((0xa000+0x2900)*4)};
        const uint32_t high[]={uint32_t((0x1260+0x1e59)*4),uint32_t((0x1260+0x1e79)*4),uint32_t((0xa000+0x2938)*4)};
        for(unsigned engine=0;engine<3;++engine)for(unsigned pipe=0;pipe<(engine==2?4U:2U);++pipe) {
            const uint64_t bank=uint64_t(pipe|(engine==2?4U:0U))<<32;
            CHECK(d.banks[bank|low[engine]]==0x400 && d.banks[bank|high[engine]]==0);
        }
        CHECK(d.selector==0 && (d.hw.registers[(0xa000+0x803)*4]&0x143c0000)==0x14000000);
        CHECK(!(d.hw.registers[(0xa000+0x2904)*4]&0xf0000));
        // Fixture starts with idle but unhalted MEC; prepare must halt it too.
        CHECK((d.hw.registers[(0xa000+0x2904)*4]&0x40000000)!=0);
        CHECK((d.hw.registers[(0xa000+0x19a2)*4]&0x20000000)!=0);
        CHECK(d.banks[(0xa000+0x9e4)*4]==0xc00c && (d.hw.registers[(0xa000+0x4c80)*4]&3)==3);
        uint64_t mc=0,physical=0; CHECK(d.hw.allocator.addresses(d.csb.handle,mc,physical)==VramAllocatorStatus::Ok);
        CHECK(d.hw.registers[(0xa000+0x988)*4]==uint32_t(mc>>32) && d.hw.registers[(0xa000+0x987)*4]==uint32_t(mc));
        CHECK(d.hw.registers[(0xa000+0x989)*4]==960);
        // Representative failures across every register-programming group,
        // including both writes and readbacks, with a landed false write.
        for(size_t i=0;i<d.events.size();++i) if(i<25 || i%11==0 || i+12>=d.events.size())failuresAt.push_back(i+1);
        const size_t count=d.events.size(); CHECK(d.run(state,r)==gi::Status::InvalidState && d.events.size()==count && r.clearStateRetentionRequired);
    }
    for(size_t fail:failuresAt) {
        Device d; gi::State state; gi::Result r={}; d.failAt=fail;
        CHECK(d.run(state,r)!=gi::Status::Ok && !state.prepared());
        CHECK(d.calls==fail && !d.hw.failed);
        const size_t count=d.events.size(); CHECK(d.run(state,r)==gi::Status::InvalidState && count==d.events.size());
    }
    for(unsigned mode=0;mode<7;++mode) {
        Device d; gi::State state; gi::Result r={};
        if(mode==0)d.hw.registers[mesControl]=0;
        if(mode==1)d.hw.registers[(0xa000+0x2a00)*4]=0;
        if(mode==2)d.hw.registers[(0xa000+0x2903)*4]=0;
        if(mode==3)d.hw.registers[(0x1260+0x1689)*4]=1;
        if(mode==4)d.hw.registers[(0xa000+0x40bc)*4]=0;
        if(mode==5)d.hw.registers[(0xa000+0x2904)*4]=0x4000000;
        if(mode==6)d.hw.registers[(0x1260+0x1688)*4]=0;
        CHECK(d.run(state,r)!=gi::Status::Ok && !state.prepared() && !r.registerWritesMayHaveOccurred);
    }
}
void shaderMemoryPolicyTests() {
    const uint32_t select=(0xa000+0x900)*4,config=(0xa000+0x9e4)*4,bases=(0xa000+0x9e3)*4;
    std::vector<size_t> accesses,readbacks;
    {
        Device d; gi::State state; gi::Result r={};
        // Deliberately dirty every VMID so a global-register model or an
        // omitted/wrong selector cannot make the observed banks look correct.
        for(unsigned vmid=0;vmid<16;++vmid) {
            const uint64_t bank=uint64_t(vmid<<4)<<32;
            d.banks[bank|config]=0x12340000+vmid;
            d.banks[bank|bases]=0x56780000+vmid;
        }
        CHECK(d.run(state,r)==gi::Status::Ok && state.prepared());
        CHECK(d.banks[config]==0xc00c && d.banks[bases]==0x56780000); // preserve VMID0 bases
        for(unsigned vmid=1;vmid<16;++vmid) {
            const uint64_t bank=uint64_t(vmid<<4)<<32;
            CHECK(d.banks[bank|config]==(vmid<=7?0xc00c:0x12340000+vmid));
            CHECK(d.banks[bank|bases]==(vmid<=7?0x20001000:0x56780000+vmid));
            // No nonzero context is enabled by shader-memory setup.
            CHECK((d.hw.registers[(0x1260+0x1688+vmid)*4]&1)==0);
        }
        CHECK(d.selector==0);
        bool inPolicy=false;
        for(size_t i=0;i<d.events.size();++i) {
            const auto& e=d.events[i];
            if(e.write && e.reg==select && e.value==0x10)inPolicy=true;
            if(!inPolicy)continue;
            accesses.push_back(i+1);
            if(!e.write)readbacks.push_back(i+1);
            if(!e.write && e.reg==select && e.value==0)break;
        }
        CHECK(accesses.size()==7*6+2 && readbacks.size()==7*3+1);
        if(accesses.size()==44)for(unsigned vmid=1;vmid<=7;++vmid) {
            const size_t start=accesses[(vmid-1)*6]-1;
            const uint32_t regs[]={select,config,bases},values[]={vmid<<4,0xc00c,0x20001000};
            for(unsigned n=0;n<3;++n) {
                const auto& w=d.events[start+n*2];const auto& v=d.events[start+n*2+1];
                CHECK(w.write && !v.write && w.reg==regs[n] && v.reg==regs[n]);
                CHECK(w.value==values[n] && v.value==values[n] && w.selector==(vmid<<4) && v.selector==(vmid<<4));
            }
        }
    }
    // Every potentially landed write and every post-write read must stop the
    // transaction immediately on failure, including the final selector reset.
    for(size_t at:accesses) {
        Device d; gi::State state; gi::Result r={};d.failAt=at;
        CHECK(d.run(state,r)!=gi::Status::Ok && d.calls==at && !state.prepared());
        CHECK(r.stage==gi::Stage::Constants && r.registerWritesMayHaveOccurred);
        CHECK(!state.needsClearStateRetention() && !d.hw.registers[(0xa000+0x989)*4]);
        CHECK(!(d.hw.registers[(0xa000+0x4c80)*4]&3));
        CHECK(d.run(state,r)==gi::Status::InvalidState && d.calls==at);
    }
    for(size_t at:readbacks)for(unsigned mode=0;mode<2;++mode) {
        Device d; gi::State state; gi::Result r={};
        if(mode)d.allOnesAt=at;else d.mismatchAt=at;
        CHECK(d.run(state,r)==(mode?gi::Status::DeviceUnavailable:gi::Status::ReadbackMismatch));
        CHECK(d.calls==at && !state.prepared() && r.stage==gi::Stage::Constants);
        CHECK(!state.needsClearStateRetention() && !d.hw.registers[(0xa000+0x989)*4]);
        CHECK(d.run(state,r)==gi::Status::InvalidState && d.calls==at);
    }
}
struct RetirementDevice {
    Device d; gi::State state;
    bool acknowledge=true;
    unsigned safeCommands=0,delays=0,busySamples=0;
    static constexpr uint32_t control=(0xa000+0x4c00)*4,safe=(0xa000+0x980)*4,
        gpm=(0xa000+0x4e6b)*4,srm=(0xa000+0x4c9b)*4,fifo=(0xa000+0x4c88)*4;
    RetirementDevice() {
        gi::Result r={}; CHECK(d.run(state,r)==gi::Status::Ok);
        d.hw.registers[control]=1; d.hw.registers[fifo]=1;
        d.events.clear(); d.calls=0;
    }
    static bool read(void* c,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<RetirementDevice*>(c);
        const bool good=Device::read(&s.d,reg,value);
        if(reg==gpm && s.busySamples) { --s.busySamples; value|=0x600; }
        return good;
    }
    static bool write(void* c,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<RetirementDevice*>(c);
        const bool good=Device::write(&s.d,reg,value);
        if(reg==safe && value==3) {
            ++s.safeCommands;
            if(s.acknowledge)s.d.hw.registers[safe]=2;
        }
        return good;
    }
    static void delay(void* c,uint32_t us) { auto& s=*static_cast<RetirementDevice*>(c); CHECK(us==1); ++s.delays; }
    gi::Status retire(gi::Result& r,gi::RetirementPollOptions p={3,1}) {
        return gi::retireClearState(ips(),0x100000,d.power,d.hw.allocator,{this,read,write,delay},p,state,r);
    }
};
constexpr uint32_t RetirementDevice::control;
constexpr uint32_t RetirementDevice::safe;
constexpr uint32_t RetirementDevice::gpm;
constexpr uint32_t RetirementDevice::srm;
constexpr uint32_t RetirementDevice::fifo;
void retirementTests() {
    size_t completeCalls=0;
    {
        RetirementDevice d; gi::Result r={}; d.busySamples=2;
        CHECK(d.retire(r)==gi::Status::Ok && !d.state.prepared() && !d.state.needsClearStateRetention());
        CHECK(!r.clearStateRetentionRequired && r.stage==gi::Stage::Complete && d.safeCommands==1 && d.delays==2);
        CHECK(!(d.d.hw.registers[RetirementDevice::control]&1));
        CHECK(!(d.d.hw.registers[(0xa000+0x4c80)*4]&3));
        CHECK(d.d.hw.registers[(0xa000+0x989)*4]==0 && d.d.hw.registers[(0xa000+0x987)*4]==0 &&
            d.d.hw.registers[(0xa000+0x988)*4]==0);
        CHECK((d.d.hw.registers[(0xa000+0x803)*4]&0x14000000)==0x14000000);
        const size_t calls=d.d.calls;
        CHECK(d.retire(r)==gi::Status::InvalidState && d.d.calls==calls);
        CHECK(d.d.run(d.state,r)==gi::Status::InvalidState);
        CHECK(d.d.hw.allocator.release(d.d.csb.handle)==VramAllocatorStatus::Ok);
    }
    {
        RetirementDevice d; gi::Result r={};
        CHECK(d.retire(r)==gi::Status::Ok); completeCalls=d.d.calls;
    }
    // Fail every register access in the successful teardown. Device::write
    // applies the value before reporting failure, exercising ambiguous writes.
    for(size_t at=1;at<=completeCalls;++at) {
        RetirementDevice d; gi::Result r={}; d.d.failAt=at;
        CHECK(d.retire(r)!=gi::Status::Ok && d.d.calls==at);
        CHECK(d.state.needsClearStateRetention() && r.clearStateRetentionRequired && !d.state.prepared());
        CHECK(d.retire(r)==gi::Status::InvalidState && d.d.calls==at);
        CHECK(d.d.run(d.state,r)==gi::Status::InvalidState && d.d.calls==at);
    }
    // Each independent busy indication must prevent retirement; time passing
    // does not turn a stopped command engine into completed DMA.
    for(unsigned mode=0;mode<9;++mode) {
        RetirementDevice d; gi::Result r={};
        if(mode==0)d.acknowledge=false;
        if(mode==1)d.d.hw.registers[RetirementDevice::srm]=1;
        if(mode==2)d.d.hw.registers[RetirementDevice::srm]=2;
        if(mode==3)d.d.hw.registers[RetirementDevice::fifo]=0;
        if(mode==4)d.d.hw.registers[RetirementDevice::fifo]=3;
        if(mode==5)d.d.hw.registers[RetirementDevice::gpm]=1;
        if(mode==6)d.d.hw.registers[RetirementDevice::gpm]=0x400;
        if(mode==7)d.d.hw.registers[RetirementDevice::gpm]=0x10000;
        if(mode==8)d.d.hw.registers[RetirementDevice::gpm]=0x01000000;
        CHECK(d.retire(r)==gi::Status::QuiesceTimeout && d.state.needsClearStateRetention());
        CHECK(!d.state.prepared() && d.d.hw.registers[(0xa000+0x989)*4]==960);
    }
    const uint32_t drainRegs[]={uint32_t((0xa000+0x4c04)*4),uint32_t((0xa000+0x4cb6)*4),uint32_t((0x1260+0xda2)*4)};
    const uint32_t drainMasks[]={0xff,0x1ef,0x04004000};
    for(unsigned reg=0;reg<3;++reg)for(unsigned bit=0;bit<32;++bit)if(drainMasks[reg]&(1U<<bit)) {
        RetirementDevice d; gi::Result r={}; d.d.hw.registers[drainRegs[reg]]=1U<<bit;
        CHECK(d.retire(r)==gi::Status::QuiesceTimeout && d.state.needsClearStateRetention());
        CHECK(!d.state.prepared() && d.d.hw.registers[(0xa000+0x989)*4]==960);
    }
    const uint32_t guardRegs[]={uint32_t((0xa000+0x803)*4),uint32_t((0xa000+0x2904)*4),mesControl,
        uint32_t((0x1260+0x1f40)*4),uint32_t((0x1260+0x1f41)*4),uint32_t((0xa000+0x900)*4),
        uint32_t((0xa000+0x989)*4),RetirementDevice::safe};
    for(unsigned i=0;i<sizeof(guardRegs)/sizeof(guardRegs[0]);++i) {
        RetirementDevice d; gi::Result r={};
        d.d.hw.registers[guardRegs[i]]=(i==0 || i==1 || i==2)?0:1;
        CHECK(d.retire(r)!=gi::Status::Ok && !r.registerWritesMayHaveOccurred);
        CHECK(d.state.needsClearStateRetention() && !d.state.prepared());
    }
    {
        RetirementDevice d; gi::Result r={};
        CHECK(d.retire(r,{0,1})==gi::Status::InvalidArgument && d.d.calls==0 && d.state.prepared());
        CHECK(d.retire(r,{1000000,1000})==gi::Status::InvalidArgument && d.d.calls==0);
        d.d.hw.registers[RetirementDevice::control]=0;
        CHECK(d.retire(r)==gi::Status::Ok && d.safeCommands==0);
    }
}

// Starts only from real portable firmware+SMU initialization in Device. Never
// calls prepare() and never constructs a prepared gfxinit::State.
struct AutoloadDevice {
    Device d; gi::AutoloadRetirement state;
    bool acknowledge=true; unsigned commands=0,delays=0,busySamples=0;
    uint32_t mutateReg=0,mutateValue=0; size_t mutateAt=0;
    static constexpr uint32_t csbHi=(0xa000+0x988)*4,csbLo=(0xa000+0x987)*4,
        csbLength=(0xa000+0x989)*4,srmControl=(0xa000+0x4c80)*4;
    AutoloadDevice() {
        // These are test observations, not a production inference that
        // firmware autoload necessarily leaves halted CP or a zero CSIB.
        d.hw.registers[(0xa000+0x803)*4]=0x14000000;
        d.hw.registers[(0xa000+0x2904)*4]=0x40000000;
        d.hw.registers[RetirementDevice::control]=1;
        d.hw.registers[RetirementDevice::fifo]=1;
        CHECK(!state.attempted()&&!state.complete()&&!state.needsFirmwareRetention());
    }
    static bool read(void* c,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<AutoloadDevice*>(c);
        CHECK(s.state.attempted()&&!s.state.complete());
        if(s.mutateAt==s.d.calls+1)s.d.hw.registers[s.mutateReg]=s.mutateValue;
        const bool good=Device::read(&s.d,reg,value);
        if(reg==RetirementDevice::gpm&&s.busySamples) { --s.busySamples; value|=0x600; }
        return good;
    }
    static bool write(void* c,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<AutoloadDevice*>(c);
        CHECK(s.state.attempted()&&!s.state.complete());
        CHECK(reg==RetirementDevice::control||reg==RetirementDevice::safe);
        const bool good=Device::write(&s.d,reg,value);
        if(reg==RetirementDevice::safe&&value==3) {
            ++s.commands;
            if(s.acknowledge)s.d.hw.registers[reg]=2;
        }
        return good;
    }
    static void delay(void* c,uint32_t us) {
        auto& s=*static_cast<AutoloadDevice*>(c);
        CHECK(us==1&&s.state.attempted()&&!s.state.complete()); ++s.delays;
    }
    gi::RetirementIo io() { return {this,read,write,delay}; }
    gi::Status retire(gi::Result& out,gi::RetirementPollOptions p={3,1}) {
        return gi::retireAutoloadRlc(ips(),0x100000,d.firmware,d.power,io(),p,state,out);
    }
};
constexpr uint32_t AutoloadDevice::csbHi;
constexpr uint32_t AutoloadDevice::csbLo;
constexpr uint32_t AutoloadDevice::csbLength;
constexpr uint32_t AutoloadDevice::srmControl;
void autoloadRetirementTests() {
    std::vector<Event> events;
    for(unsigned enabled=0;enabled<2;++enabled) {
        AutoloadDevice d; gi::Result r={}; d.d.hw.registers[RetirementDevice::control]=enabled;
        CHECK(d.retire(r)==gi::Status::Ok&&d.state.complete()&&!d.state.needsFirmwareRetention());
        CHECK(d.state.attempted()&&r.stage==gi::Stage::Complete&&!r.clearStateRetentionRequired);
        CHECK(d.commands==enabled&&r.writes==1+enabled);
        CHECK(!(d.d.hw.registers[RetirementDevice::control]&1));
        CHECK(d.d.hw.registers[AutoloadDevice::csbHi]==0&&d.d.hw.registers[AutoloadDevice::csbLo]==0&&
            d.d.hw.registers[AutoloadDevice::csbLength]==0&&d.d.hw.registers[AutoloadDevice::srmControl]==0);
        const size_t calls=d.d.calls;
        CHECK(d.retire(r)==gi::Status::InvalidState&&d.d.calls==calls&&d.state.complete());
        if(enabled)events=d.d.events;
    }
    // Fail each actual access, including writes which land before reporting
    // failure. Every read also independently rejects transport all-ones.
    for(size_t i=0;i<events.size();++i)for(unsigned kind=0;kind<2;++kind) {
        if(kind&&events[i].write)continue;
        AutoloadDevice d; gi::Result r={};
        if(kind)d.d.allOnesAt=i+1;else d.d.failAt=i+1;
        CHECK(d.retire(r)!=(gi::Status::Ok)&&d.d.calls==i+1);
        CHECK(d.state.attempted()&&!d.state.complete()&&d.state.needsFirmwareRetention());
        CHECK(!r.clearStateRetentionRequired);
        CHECK(d.retire(r)==gi::Status::InvalidState&&d.d.calls==i+1&&d.state.needsFirmwareRetention());
    }
    const uint32_t binding[]={AutoloadDevice::csbHi,AutoloadDevice::csbLo,AutoloadDevice::csbLength,
        AutoloadDevice::srmControl};
    for(auto reg:binding)for(uint32_t value:{1U,2U,3U,UINT32_MAX}) {
        AutoloadDevice d; gi::Result r={}; d.d.hw.registers[reg]=value;
        CHECK(d.retire(r)!=gi::Status::Ok&&!r.writes&&d.state.needsFirmwareRetention());
        CHECK(d.d.hw.registers[reg]==value); // unknown ownership never cleared
    }
    // Mutate each binding after it was initially proven. Every later actual
    // observation must reject the change, including final post-drain reads.
    for(auto reg:binding) {
        unsigned seen=0;
        for(size_t i=0;i<events.size();++i)if(!events[i].write&&events[i].reg==reg) {
            if(!seen++)continue;
            AutoloadDevice d; gi::Result r={};d.mutateAt=i+1;d.mutateReg=reg;d.mutateValue=1;
            CHECK(d.retire(r)!=gi::Status::Ok&&d.d.calls==i+1&&d.state.needsFirmwareRetention());
            CHECK(d.d.hw.registers[reg]==1);
        }
        CHECK(seen==3);
    }
    const uint32_t guardRegs[]={uint32_t((0xa000+0x803)*4),uint32_t((0xa000+0x2904)*4),mesControl,
        uint32_t((0x1260+0x1f40)*4),uint32_t((0x1260+0x1f41)*4),uint32_t((0xa000+0x900)*4),
        uint32_t((0xa000+0x40bc)*4),uint32_t((0xa000+0x2a00)*4),uint32_t((0xa000+0x2903)*4),
        uint32_t((0x1260+0x1688)*4),RetirementDevice::safe};
    for(unsigned i=0;i<sizeof(guardRegs)/sizeof(guardRegs[0]);++i) {
        AutoloadDevice d; gi::Result r={};
        d.d.hw.registers[guardRegs[i]]=(i==3||i==4||i==5||i==10)?1:0;
        CHECK(d.retire(r)!=gi::Status::Ok&&!r.writes&&d.state.needsFirmwareRetention());
    }
    for(unsigned vmid=1;vmid<16;++vmid) {
        AutoloadDevice d; gi::Result r={};d.d.hw.registers[(0x1260+0x1688+vmid)*4]=1;
        CHECK(d.retire(r)!=gi::Status::Ok&&!r.writes&&d.state.needsFirmwareRetention());
    }
    const uint32_t drainRegs[]={RetirementDevice::gpm,RetirementDevice::srm,
        uint32_t((0xa000+0x4c04)*4),uint32_t((0xa000+0x4cb6)*4),uint32_t((0x1260+0xda2)*4)};
    const uint32_t masks[]={0xff01ffe1,3,0xff,0x1ef,0x04004000};
    for(unsigned reg=0;reg<5;++reg)for(unsigned bit=0;bit<32;++bit)if(masks[reg]&(1U<<bit)) {
        AutoloadDevice d; gi::Result r={};d.d.hw.registers[drainRegs[reg]]=1U<<bit;
        CHECK(d.retire(r)==gi::Status::QuiesceTimeout&&d.state.needsFirmwareRetention()&&d.delays==2);
    }
    for(uint32_t value:{0U,2U,3U}) {
        AutoloadDevice d; gi::Result r={};d.d.hw.registers[RetirementDevice::fifo]=value;
        CHECK(d.retire(r)==gi::Status::QuiesceTimeout&&d.state.needsFirmwareRetention());
    }
    {
        AutoloadDevice d;gi::Result r={};d.acknowledge=false;
        CHECK(d.retire(r)==gi::Status::QuiesceTimeout&&r.stage==gi::Stage::EnterSafeMode&&d.delays==2);
        CHECK(d.d.hw.registers[RetirementDevice::control]==1&&d.state.needsFirmwareRetention());
    }
    for(unsigned busy=0;busy<=3;++busy) {
        AutoloadDevice d;gi::Result r={};d.busySamples=busy;
        CHECK(d.retire(r)==(busy==3?gi::Status::QuiesceTimeout:gi::Status::Ok));
        CHECK(d.delays==(busy==3?2:busy));
    }
    {
        AutoloadDevice d;gi::Result r={};gfxfw::State unloaded;smu::State asleep;
        CHECK(gi::retireAutoloadRlc(ips(),0x100000,unloaded,d.d.power,d.io(),{3,1},d.state,r)==gi::Status::InvalidState);
        CHECK(gi::retireAutoloadRlc(ips(),0x100000,d.d.firmware,asleep,d.io(),{3,1},d.state,r)==gi::Status::InvalidState);
        CHECK(d.retire(r,{0,1})==gi::Status::InvalidArgument&&d.retire(r,{1000000,1000})==gi::Status::InvalidArgument);
        auto io=d.io();io.write32=nullptr;
        CHECK(gi::retireAutoloadRlc(ips(),0x100000,d.d.firmware,d.d.power,io,{3,1},d.state,r)==gi::Status::InvalidArgument);
        auto ip=ips();ip.blocks[0].major=10;
        CHECK(gi::retireAutoloadRlc(ip,0x100000,d.d.firmware,d.d.power,d.io(),{3,1},d.state,r)==gi::Status::UnsupportedLayout);
        CHECK(gi::retireAutoloadRlc(ips(),4,d.d.firmware,d.d.power,d.io(),{3,1},d.state,r)==gi::Status::OutsideBar);
        CHECK(!d.d.calls&&!d.state.attempted()&&!d.state.needsFirmwareRetention());
        CHECK(d.retire(r)==gi::Status::Ok);
    }
}
} // namespace
int main() {
    clearStateTests(); initializationTests(); shaderMemoryPolicyTests(); retirementTests(); autoloadRetirementTests();
    std::cout<<checks<<" GC11 initialization checks; "<<failures<<" failures\n";
    return failures?1:0;
}
