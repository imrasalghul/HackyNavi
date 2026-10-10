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
        CHECK(d.hw.registers[(0xa000+0x9e4)*4]==0xc00c && (d.hw.registers[(0xa000+0x4c80)*4]&3)==3);
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
} // namespace
int main() {
    clearStateTests(); initializationTests(); retirementTests();
    std::cout<<checks<<" GC11 initialization checks; "<<failures<<" failures\n";
    return failures?1:0;
}
