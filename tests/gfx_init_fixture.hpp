// SPDX-License-Identifier: MIT
// Shared host-only GC11 register/firmware/SMU emulator.
#ifndef HACKYNAVI_TEST_GFX_INIT_FIXTURE_HPP
#define HACKYNAVI_TEST_GFX_INIT_FIXTURE_HPP
#include "gfx_firmware_fixture.hpp"
#include "hackynavi/gfx_init.hpp"
namespace gfxinit_test {
using namespace hackynavi;
using namespace gfxfw_test;
namespace gi=hackynavi::gfxinit;
struct Event { bool write; uint32_t reg,value,selector; };
struct Device {
    Simulation hw; Fixtures files; gfxfw::State firmware; smu::State power;
    smu::Layout smuLayout={}; VramAllocation csb={};
    std::vector<uint8_t> scratch;
    std::map<uint64_t,uint32_t> banks;
    std::vector<Event> events;
    uint32_t selector=0; size_t calls=0,failAt=0,mismatchAt=0;
    Device():scratch(4096,0) {
        gfxfw::Result result={}; CHECK(hw.run(files.files(),firmware,result)==gfxfw::Status::Ok);
        hw.registers[(0xa000+0x40bc)*4]=0x1f;
        hw.registers[(0xa000+0x2a00)*4]=1; hw.registers[(0xa000+0x2903)*4]=1;
        hw.registers[(0x1260+0x1688)*4]=1;
        CHECK(smu::buildLayout(ips(),0x100000,smuLayout)==smu::Status::Ok);
        smu::Result sr={};
        CHECK(smu::initialize(smuLayout,firmware,smuIo(),{3,1},power,sr)==smu::Status::Ok);
        CHECK(smu::disableGfxOff(smuIo(),{3,1},power,sr)==smu::Status::Ok && power.awake());
        CHECK(hw.allocator.allocate(4096,4096,csb)==VramAllocatorStatus::Ok);
        events.clear(); calls=0; hw.accesses=hw.writes=0;
    }
    static bool banked(uint32_t reg) {
        const uint32_t regs[]={0x1260*4+0x1e44*4,0x1260*4+0x1e59*4,0x1260*4+0x1e45*4,0x1260*4+0x1e79*4,
            0xa000*4+0x2900*4,0xa000*4+0x2938*4};
        for(auto r:regs)if(reg==r)return true; return false;
    }
    static bool read(void* context,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<Device*>(context);
        bool good=true;
        if(banked(reg))value=s.banks[(uint64_t(s.selector)<<32)|reg];
        else good=Simulation::read(&s.hw,reg,value);
        if(reg!=0 && reg!=4 && reg!=0x18) {
            ++s.calls; if(s.calls==s.mismatchAt)value^=1;
            s.events.push_back({false,reg,value,s.selector});
            if(s.calls==s.failAt)good=false;
        }
        return good;
    }
    static bool write(void* context,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<Device*>(context); bool good=true;
        if(reg==(0xa000+0x900)*4)s.selector=value;
        if(reg==s.smuLayout.message) {
            s.hw.registers[s.smuLayout.response]=1;
            if(value==2)s.hw.registers[s.smuLayout.argument]=0x4e0200;
            if(value==3)s.hw.registers[s.smuLayout.argument]=0x40;
        } else if(banked(reg))s.banks[(uint64_t(s.selector)<<32)|reg]=value;
        else good=Simulation::write(&s.hw,reg,value);
        if(reg!=0 && reg!=4 && reg!=0x18) {
            ++s.calls; s.events.push_back({true,reg,value,s.selector});
            if(s.calls==s.failAt)good=false;
        }
        return good;
    }
    smu::Io smuIo() { return {this,read,write,Simulation::delayUs}; }
    gi::Status run(gi::State& state,gi::Result& out) {
        return gi::prepare(ips(),files.files(),firmware,power,hw.allocator,csb.handle,scratch.data(),scratch.size(),
            {this,0x100000,read,write},{this,read,write},{this,read,write},hw.hdp,state,out);
    }
};

} // namespace gfxinit_test
#endif
