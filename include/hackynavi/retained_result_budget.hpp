// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

namespace hackynavi { namespace resultbudget {
class Budget;
// An exact reservation cannot be copied or released through another budget.
// Release is explicit: native storage must be freed BEFORE capacity is returned.
class Reservation {
    friend class Budget;
public:
    Reservation():issuer_(nullptr),bytes_(0) {}
    Reservation(const Reservation&)=delete;
    Reservation& operator=(const Reservation&)=delete;
    uint32_t bytes() const {return bytes_;}
private:
    Budget* issuer_;
    uint32_t bytes_;
};
class Budget {
public:
    static constexpr uint32_t kMaxBytes=64U*1024U*1024U;
    static constexpr uint32_t kMaxHolders=64;
    struct Usage {uint32_t bytes,holders;};
    constexpr Budget():state_(0) {}
    Budget(const Budget&)=delete;
    Budget& operator=(const Budget&)=delete;
    bool reserve(uint32_t bytes,Reservation& out) {
        if(!bytes||bytes>kMaxBytes||out.issuer_||out.bytes_)return false;
        uint64_t previous=__atomic_load_n(&state_,__ATOMIC_RELAXED);
        for(;;) {
            const uint32_t used=uint32_t(previous),holders=uint32_t(previous>>32);
            if(holders>=kMaxHolders||used>kMaxBytes||bytes>kMaxBytes-used)return false;
            const uint64_t next=previous+(UINT64_C(1)<<32)+bytes;
            if(__atomic_compare_exchange_n(&state_,&previous,next,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
                out.issuer_=this;out.bytes_=bytes;return true;
            }
        }
    }
    bool release(Reservation& reservation) {
        if(reservation.issuer_!=this||!reservation.bytes_)return false;
        const uint32_t bytes=reservation.bytes_;
        uint64_t previous=__atomic_load_n(&state_,__ATOMIC_RELAXED);
        for(;;) {
            const uint32_t used=uint32_t(previous),holders=uint32_t(previous>>32);
            // No unsigned wrap/borrow can hide a broken native release path.
            if(!holders||holders>kMaxHolders||used>kMaxBytes||bytes>used)return false;
            const uint64_t next=previous-(UINT64_C(1)<<32)-bytes;
            if(__atomic_compare_exchange_n(&state_,&previous,next,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
                reservation.issuer_=nullptr;reservation.bytes_=0;return true;
            }
        }
    }
    Usage usage() const {
        const uint64_t current=__atomic_load_n(&state_,__ATOMIC_ACQUIRE);
        return {uint32_t(current),uint32_t(current>>32)};
    }
private:
    alignas(8) uint64_t state_;
};
} }
