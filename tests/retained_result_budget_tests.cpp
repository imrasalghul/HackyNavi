// SPDX-License-Identifier: MIT
// Tests the production atomic accounting only; no simulated GPU completion.
#include "hackynavi/retained_result_budget.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
using hackynavi::resultbudget::Budget;
using hackynavi::resultbudget::Reservation;
namespace {
unsigned checks=0,failures=0;
#define CHECK(test) do {++checks;if(!(test)){++failures;std::cerr<<__LINE__<<": " #test "\n";}} while(false)
void limitsAndFailures() {
    Budget budget,foreign;Reservation held[Budget::kMaxHolders],extra;
    CHECK(!budget.reserve(0,extra));CHECK(!budget.reserve(UINT32_MAX,extra));
    CHECK(!budget.release(extra));CHECK(!extra.bytes());
    CHECK(budget.reserve(Budget::kMaxBytes,held[0]));
    CHECK(!budget.reserve(1,extra));CHECK(!budget.reserve(1,held[0]));
    CHECK(!foreign.release(held[0]));
    CHECK(budget.usage().bytes==Budget::kMaxBytes&&budget.usage().holders==1);
    CHECK(budget.release(held[0]));CHECK(!budget.release(held[0]));
    CHECK(!budget.usage().bytes&&!budget.usage().holders);
    for(unsigned i=0;i<Budget::kMaxHolders;++i)CHECK(budget.reserve(16,held[i]));
    CHECK(!budget.reserve(16,extra));
    CHECK(budget.usage().holders==Budget::kMaxHolders&&budget.usage().bytes==16*Budget::kMaxHolders);
    for(unsigned i=0;i<Budget::kMaxHolders;++i)CHECK(budget.release(held[i]));
    // Allocation failure returns a reservation without allocating storage;
    // the next attempt must receive the whole capacity, without counter drift.
    for(unsigned i=0;i<1024;++i) {
        CHECK(budget.reserve(Budget::kMaxBytes,extra));CHECK(budget.release(extra));
    }
    CHECK(!budget.usage().bytes&&!budget.usage().holders);
}
void concurrentAccounting() {
    Budget budget;
    std::atomic<bool> start{false};std::atomic<unsigned> violations{0},successes{0},denials{0};
    std::vector<std::thread> threads;
    for(unsigned worker=0;worker<16;++worker)threads.emplace_back([&,worker] {
        while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned iteration=0;iteration<4000;++iteration) {
            Reservation reservation;
            const uint32_t bytes=(1+(worker+iteration)%16)*1024U*1024U;
            if(!budget.reserve(bytes,reservation)) {++denials;continue;}
            ++successes;
            const auto used=budget.usage();
            if(used.bytes>Budget::kMaxBytes||used.holders>Budget::kMaxHolders||reservation.bytes()!=bytes)++violations;
            if((iteration&15)==0)std::this_thread::yield();
            if(!budget.release(reservation)||reservation.bytes())++violations;
        }
    });
    start.store(true,std::memory_order_release);
    for(auto& thread:threads)thread.join();
    CHECK(!violations.load());CHECK(successes.load()>0);
    CHECK(successes.load()+denials.load()==64000);
    CHECK(!budget.usage().bytes&&!budget.usage().holders);
}
}
int main(){limitsAndFailures();concurrentAccounting();
    std::cout<<checks<<" retained-result budget checks; "<<failures<<" failures\n";return failures?1:0;}
