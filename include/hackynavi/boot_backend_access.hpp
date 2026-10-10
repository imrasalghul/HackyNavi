// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_BACKEND_ACCESS_HPP
#define HACKYNAVI_BOOT_BACKEND_ACCESS_HPP
#include "hackynavi/boot_display_backend.hpp"
namespace hackynavi { namespace bootbackend {
// One-shot transport sequencing used by the native observer. Address derivation
// remains in capture's validated, immutable discovery traversal. This helper
// supplies no PCI/MMIO/power permission and never executes a read itself.
class ReadTicket {
public:
    ReadTicket():count_(0),offset_(0),phase_(Idle){}
    bool permit(const Result& r,Bank bank,uint8_t instance,uint32_t offset) {
        if(phase_==Failed || !expected(r,bank,instance,offset))return fail();
        if(phase_==Idle) {
            if(r.reads!=count_ || r.lastReadSucceeded)return fail();
            offset_=offset;phase_=Armed;return true;
        }
        if(phase_!=Read || r.reads!=count_+1 || !r.lastReadSucceeded || offset!=offset_)
            return fail();
        ++count_;phase_=Idle;return true;
    }
    bool consume(const Result& r,uint32_t offset) {
        if(phase_!=Armed || !expected(r,r.bank,r.instance,offset) ||
            offset!=offset_ || r.reads!=count_+1 || r.lastReadSucceeded)return fail();
        phase_=Read;return true;
    }
    bool complete() const {return phase_==Idle && count_==2*kSamplesPerPass;}
    void revoke(){phase_=Failed;}
private:
    enum Phase { Idle,Armed,Read,Failed };
    uint32_t count_,offset_;Phase phase_;
    bool fail(){phase_=Failed;return false;}
    bool expected(const Result& r,Bank bank,uint8_t instance,uint32_t offset) const {
        if(count_>=2*kSamplesPerPass || r.pass!=count_/kSamplesPerPass ||
            bank!=r.bank || instance!=r.instance || offset!=r.lastRegister || (offset&3))return false;
        size_t index=count_%kSamplesPerPass;Bank wanted;size_t n=0,reg=0;
        if(index<kLinks){wanted=Bank::Link;n=index;}
        else if((index-=kLinks)<kLinks*(kLinkRegisters-1)) {
            wanted=Bank::Link;n=index/(kLinkRegisters-1);reg=1+index%(kLinkRegisters-1);
        } else if((index-=kLinks*(kLinkRegisters-1))<kLinks*kFrontendRegisters) {
            wanted=Bank::Frontend;n=index/kFrontendRegisters;reg=index%kFrontendRegisters;
        } else if((index-=kLinks*kFrontendRegisters)<kPipes*kTimingRegisters) {
            wanted=Bank::Timing;n=index/kTimingRegisters;reg=index%kTimingRegisters;
        } else {
            index-=kPipes*kTimingRegisters;wanted=Bank::Combiner;
            n=index/kCombinerRegisters;reg=index%kCombinerRegisters;
        }
        return bank==wanted && instance==n && r.reg==reg;
    }
};
} }
#endif
