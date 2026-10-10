// SPDX-License-Identifier: MIT
#include "dml32_encode.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
extern "C" int hn_test_calculations(int,hn_dml_plan[4]);
namespace c=hackynavi::displaycalculation;
namespace b=hackynavi::displaybandwidth;
static unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::fprintf(stderr,"%d: %s\n",__LINE__,#x);}}while(0)
static void empty(const c::Encoding& e) {
    CHECK(!e.registers.fieldCount&&!e.registers.identity.calculationId&&!e.mode.pixelClockHz);
    for(auto f:e.registers.fields)CHECK(!f);
    for(auto w:e.registers.watermarks)CHECK(!w.sourceId);
}
int main() {
    const c::Binding binding={100000,50000,2,0};
    for(int fourK=0;fourK<2;++fourK) {
        hn_dml_plan p[4]={};CHECK(hn_test_calculations(fourK,p));
        c::Calculation sets[4]={};
        for(unsigned n=0;n<4;++n)sets[n]={{1,2,3,100+n},HN_DML_OK,&p[n]};
        const auto timing=sets[0];c::Encoding e={};
        CHECK(c::encode(timing,sets,binding,e)==c::Status::Ok);
        CHECK(e.mode.width==(fourK?3840u:1920u)&&e.mode.pixelClockHz==(fourK?594000000u:148500000u));
        CHECK(e.timingClocks.dispKHz==uint32_t(std::ceil(p[0].dispclk_mhz*1000)));
        CHECK(e.timingClocks.dramKTransfersPerSecond==uint32_t(std::ceil(p[0].dram_speed_mts*1000)));
        CHECK(e.registers.detKbytes==1152&&e.registers.compbufKbytes==128);
        CHECK(e.registers.fields[size_t(b::Field::rq_rq_regs_l_dpte_group_size)]==0);
        CHECK(e.registers.fields[size_t(b::Field::rq_rq_regs_c_swath_height)]==0);
        CHECK(e.registers.fields[size_t(b::Field::dlg_refcyc_per_htotal)]==p[0].dlg.refcyc_per_htotal);
        CHECK(e.registers.fields[size_t(b::Field::ttu_min_ttu_vblank)]==p[0].ttu.min_ttu_vblank);
        for(unsigned n=0;n<4;++n) {
            CHECK(e.registers.watermarks[n].sourceId==100+n);
            CHECK(e.registers.watermarks[n].urgentNs==uint32_t(p[n].urgent_us*1000));
            CHECK(e.watermarkClocks[n].dcfKHz==uint32_t(std::ceil(p[n].dcfclk_mhz*1000)));
            if(n)CHECK(e.registers.watermarks[n].urgentNs>=e.registers.watermarks[n-1].urgentNs+999);
        }
        b::Plan kernelPlan;CHECK(b::plan(e.registers,kernelPlan)==b::Status::Ok&&kernelPlan.valid());
        auto badBinding=binding;badBinding.dchubReferenceKHz=49999;
        CHECK(c::encode(timing,sets,badBinding,e)==c::Status::InvalidReferenceClock);empty(e);
        badBinding=binding;badBinding.timerDivider=1;
        CHECK(c::encode(timing,sets,badBinding,e)==c::Status::InvalidRegisterPlan);empty(e);
        for(unsigned n=0;n<4;++n) {
            const auto saved=sets[n];
            sets[n].status=HN_DML_MODE_REJECTED;
            CHECK(c::encode(timing,sets,binding,e)==c::Status::InvalidCalculation);empty(e);sets[n]=saved;
            sets[n].result=nullptr;
            CHECK(c::encode(timing,sets,binding,e)==c::Status::InvalidCalculation);empty(e);sets[n]=saved;
            sets[n].identity.deviceGeneration++;
            CHECK(c::encode(timing,sets,binding,e)==c::Status::StaleCalculation);empty(e);sets[n]=saved;
            sets[n].identity.modeGeneration++;
            CHECK(c::encode(timing,sets,binding,e)==c::Status::StaleCalculation);empty(e);sets[n]=saved;
            sets[n].identity.clockGeneration++;
            CHECK(c::encode(timing,sets,binding,e)==c::Status::StaleCalculation);empty(e);sets[n]=saved;
        }
        sets[3].identity.calculationId=sets[1].identity.calculationId;
        CHECK(c::encode(timing,sets,binding,e)==c::Status::InvalidRegisterPlan);empty(e);sets[3].identity.calculationId=103;
        hn_dml_plan changed=p[3];sets[3].result=&changed;
        changed.pipe.pipe.dest.htotal++;
        CHECK(c::encode(timing,sets,binding,e)==c::Status::ModeMismatch);empty(e);
        changed=p[3];changed.pipe.pipe.src.data_pitch++;
        CHECK(c::encode(timing,sets,binding,e)==c::Status::ModeMismatch);empty(e);
        changed=p[3];changed.pipe.pipe.src.gpuvm=true;
        CHECK(c::encode(timing,sets,binding,e)==c::Status::UnsupportedProfile);empty(e);
        changed=p[3];changed.pipe.dout.dsc_enable=true;
        CHECK(c::encode(timing,sets,binding,e)==c::Status::UnsupportedProfile);empty(e);
        const double invalid[]={-1,std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(),4294967.296,1e300};
        for(double v:invalid) {
            changed=p[3];changed.urgent_us=v;
            CHECK(c::encode(timing,sets,binding,e)==c::Status::NumericOverflow);empty(e);
        }
        changed=p[3];changed.fraction_urgent_bandwidth=1.001;
        CHECK(c::encode(timing,sets,binding,e)==c::Status::NumericOverflow);empty(e);
        changed=p[3];changed.urgent_us=UINT32_MAX/1000.0;
        CHECK(c::encode(timing,sets,binding,e)==c::Status::InvalidRegisterPlan);empty(e);
        sets[3].result=&p[3];
        changed=p[0];auto badTiming=timing;badTiming.result=&changed;
        changed.dlg.refcyc_per_htotal=UINT32_MAX;
        CHECK(c::encode(badTiming,sets,binding,e)==c::Status::InvalidRegisterPlan);empty(e);
        changed=p[0];changed.pipe.clks_cfg.dppclk_mhz++;
        CHECK(c::encode(badTiming,sets,binding,e)==c::Status::NumericOverflow);empty(e);
        CHECK(c::encode(timing,sets,binding,e)==c::Status::Ok);
    }
    std::printf("DML integer encoding: %u checks, %u failures; CPU evidence only\n",checks,failures);
    return failures?1:0;
}
