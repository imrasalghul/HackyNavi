// SPDX-License-Identifier: MIT
// Tests the actual native observer's one-shot callback gate, not PCI/MMIO.
#include "hackynavi/boot_backend_access.hpp"
#include <iostream>
#include <vector>
namespace b=hackynavi::bootbackend;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
std::vector<b::Result> requests() {
    std::vector<b::Result> out;
    auto add=[&](unsigned pass,b::Bank bank,size_t instance,size_t reg) {
        b::Result r={};r.pass=uint8_t(pass);r.bank=bank;r.instance=uint8_t(instance);r.reg=uint8_t(reg);
        r.reads=uint32_t(out.size());r.lastRegister=uint32_t(0x1000+(out.size()%b::kSamplesPerPass)*4);out.push_back(r);
    };
    for(unsigned pass=0;pass<2;++pass) {
        for(size_t n=0;n<b::kLinks;++n)add(pass,b::Bank::Link,n,0);
        for(size_t n=0;n<b::kLinks;++n)for(size_t r=1;r<b::kLinkRegisters;++r)add(pass,b::Bank::Link,n,r);
        for(size_t n=0;n<b::kLinks;++n)for(size_t r=0;r<b::kFrontendRegisters;++r)add(pass,b::Bank::Frontend,n,r);
        for(size_t n=0;n<b::kPipes;++n)for(size_t r=0;r<b::kTimingRegisters;++r)add(pass,b::Bank::Timing,n,r);
        for(size_t n=0;n<b::kPipes;++n)for(size_t r=0;r<b::kCombinerRegisters;++r)add(pass,b::Bank::Combiner,n,r);
    }
    return out;
}
void step(b::ReadTicket& ticket,b::Result r) {
    CHECK(ticket.permit(r,r.bank,r.instance,r.lastRegister));
    ++r.reads;CHECK(ticket.consume(r,r.lastRegister));
    r.lastReadSucceeded=true;CHECK(ticket.permit(r,r.bank,r.instance,r.lastRegister));
}
}
int main() {
    const auto all=requests();CHECK(all.size()==234);
    b::ReadTicket good;CHECK(!good.complete());for(auto r:all)step(good,r);CHECK(good.complete());
    CHECK(!good.permit(all[0],all[0].bank,all[0].instance,all[0].lastRegister));CHECK(!good.complete());
    // Every exact position, including both pass boundaries and every bank,
    // rejects wrong fields/order, replay, unpaired transport and revocation.
    for(size_t at=0;at<all.size();++at)for(unsigned mutation=0;mutation<19;++mutation) {
        b::ReadTicket ticket;for(size_t i=0;i<at;++i)step(ticket,all[i]);
        auto r=all[at];const auto original=r;
        bool accepted=false;
        switch(mutation) {
        case 0:r.bank=b::Bank(255);break;
        case 1:++r.instance;break;
        case 2:++r.reg;break;
        case 3:++r.pass;break;
        case 4:++r.reads;break;
        case 5:r.lastReadSucceeded=true;break;
        case 6:++r.lastRegister;break;
        case 7:ticket.revoke();break;
        case 8:accepted=ticket.consume(r,r.lastRegister);break;
        default:
            CHECK(ticket.permit(r,r.bank,r.instance,r.lastRegister));
            if(mutation==9){accepted=ticket.permit(r,r.bank,r.instance,r.lastRegister);break;}
            ++r.reads;
            if(mutation==10){accepted=ticket.consume(r,r.lastRegister+4);break;}
            if(mutation==11){--r.reads;accepted=ticket.consume(r,r.lastRegister);break;}
            if(mutation==12){r.lastReadSucceeded=true;accepted=ticket.consume(r,r.lastRegister);break;}
            CHECK(ticket.consume(r,r.lastRegister));
            if(mutation==13){accepted=ticket.consume(r,r.lastRegister);break;}
            r.lastReadSucceeded=true;
            if(mutation==14)r.lastReadSucceeded=false;
            if(mutation==15)++r.reads;
            if(mutation==16)r.lastRegister+=4;
            if(mutation==17)ticket.revoke();
            if(mutation==18)r.bank=b::Bank(255);
            accepted=ticket.permit(r,r.bank,r.instance,r.lastRegister);break;
        }
        if(mutation<8)accepted=ticket.permit(r,r.bank,r.instance,r.lastRegister);
        CHECK(!accepted);CHECK(!ticket.complete());
        CHECK(!ticket.permit(original,original.bank,original.instance,original.lastRegister));
        CHECK(!ticket.consume(original,original.lastRegister));
    }
    // Callback tuple must match the core's currently issued request exactly.
    for(unsigned field=0;field<3;++field) {
        b::ReadTicket ticket;auto r=all[0];
        CHECK(!ticket.permit(r,field==0?b::Bank::Frontend:r.bank,
            field==1?1:r.instance,field==2?r.lastRegister+4:r.lastRegister));
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
