// SPDX-License-Identifier: MIT
// Tests actual production host lifecycle/order bookkeeping only. No backend,
// device, GPU result, fake command execution, or completion token is supplied.
#include "../command_lifecycle.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace hackynavi::compute::command;
namespace {
uint64_t checks = 0;
void require(bool condition, const char* message) {
    ++checks; if (!condition) throw std::runtime_error(message);
}
struct Snapshot {
    Phase phase;
    bool active, mutableEncoding, terminal, scheduled, completed;
    uint32_t used;
};
Snapshot snapshot(const Scheduler& queue, const Lifecycle& state) {
    return {state.phase(), state.active(), state.mutableEncoding(), state.terminal(),
            state.scheduledReady(), state.completedReady(), queue.usedSlots()};
}
bool same(const Snapshot& a, const Snapshot& b) {
    return a.phase == b.phase && a.active == b.active && a.mutableEncoding == b.mutableEncoding &&
        a.terminal == b.terminal && a.scheduled == b.scheduled && a.completed == b.completed && a.used == b.used;
}
void failed(bool result, const Snapshot& before, const Scheduler& queue, const Lifecycle& state) {
    require(!result && same(before,snapshot(queue,state)), "rejected operation preserves host state");
}
void callbackRetirement(Scheduler& queue, Ticket ticket, Lifecycle& state) {
    require(state.terminal() && state.scheduledReady()==(state.phase()==Phase::Error) && !state.completedReady(),
        "terminal success still waits for scheduled handlers; error skips them");
    require(queue.release(ticket) && !queue.contains(ticket), "terminal execution returns capacity before callbacks");
    require(!state.completedReady(), "slot release does not complete callbacks");
    const auto released = snapshot(queue,state);
    failed(queue.markCallbacksDone(ticket),released,queue,state);
    failed(queue.release(ticket),released,queue,state);
    if (state.phase()==Phase::Completed) {
        failed(queue.markCallbacksDone(state),released,queue,state);
        require(queue.markScheduledCallbacksDone(state) && state.scheduledReady() && !state.completedReady(),
            "scheduled handlers complete independently of completion handlers");
    } else failed(queue.markScheduledCallbacksDone(state),released,queue,state);
    require(queue.markCallbacksDone(state) && state.completedReady(), "retained lifecycle callback completion enables completed wait");
    const auto before = snapshot(queue,state);
    failed(queue.markCallbacksDone(state),before,queue,state);
    failed(queue.markScheduledCallbacksDone(state),before,queue,state);
    require(state.completedReady() && state.terminal(), "final lifecycle survives ticket release");
}
void phaseRules() {
    static_assert(uint32_t(Phase::NotEnqueued)==0 && uint32_t(Phase::Enqueued)==1 && uint32_t(Phase::Committed)==2 &&
        uint32_t(Phase::Scheduled)==3 && uint32_t(Phase::Completed)==4 && uint32_t(Phase::Error)==5,"public SDK phase values");
    Scheduler queue(2), other(2);
    Lifecycle state, otherState;
    Ticket ticket, otherTicket, output;
    require(state.mutableEncoding() && !state.scheduledReady() && !state.completedReady() && !state.active(), "fresh lifecycle");
    require(queue.reserve(state,ticket) && other.reserve(otherState,otherTicket), "reserve actual metadata slots");
    require(!queue.contains(otherTicket) && !other.contains(ticket), "ticket origin excludes cross-queue collision");
    auto before = snapshot(queue,state);
    failed(queue.markScheduled(ticket),before,queue,state);
    failed(queue.finish(ticket,true),before,queue,state);
    failed(queue.finish(ticket,false),before,queue,state);
    failed(queue.markCallbacksDone(ticket),before,queue,state);
    failed(queue.markCallbacksDone(state),before,queue,state);
    failed(other.markCallbacksDone(state),before,queue,state);
    failed(queue.release(ticket),before,queue,state);
    require(!queue.takeReady(output) && output == Ticket{}, "reserved-only slot cannot execute and output unchanged");
    Ticket duplicate = otherTicket;
    require(!queue.reserve(state,duplicate) && duplicate == otherTicket && !other.reserve(state,duplicate), "same lifecycle cannot be reserved twice or cross-queue");
    require(queue.enqueue(ticket) && state.phase()==Phase::Enqueued && state.mutableEncoding(), "enqueue remains encodable");
    before = snapshot(queue,state);
    failed(queue.enqueue(ticket),before,queue,state);
    require(!queue.takeReady(output) && output==Ticket{}, "uncommitted enqueue cannot begin");
    require(queue.commit(ticket) && state.phase()==Phase::Committed && !state.mutableEncoding(), "commit seals encoding");
    before = snapshot(queue,state);
    failed(queue.commit(ticket),before,queue,state);
    failed(queue.cancel(ticket),before,queue,state);
    failed(queue.markScheduled(ticket),before,queue,state);
    require(queue.takeReady(output) && output==ticket && state.active() && !state.scheduledReady(), "worker admission is not a scheduled observation");
    before = snapshot(queue,state);
    failed(queue.finish(ticket,true),before,queue,state);
    require(queue.markScheduled(ticket) && state.phase()==Phase::Scheduled && !state.scheduledReady() && !state.completedReady(), "observed schedule phase still waits for scheduled handlers");
    before = snapshot(queue,state);
    failed(queue.markScheduled(ticket),before,queue,state);
    require(queue.finish(ticket,true) && state.phase()==Phase::Completed && !state.active(), "finish releases active worker");
    callbackRetirement(queue,ticket,state);
    duplicate = otherTicket;
    require(!queue.reserve(state,duplicate) && duplicate==otherTicket, "terminal command is one-shot after slot release");
    require(other.cancel(otherTicket), "cancel uncommitted second queue");
}
void capacityAndGenerations() {
    for (uint32_t capacity : {0U,65U,UINT32_MAX}) {
        Scheduler invalid(capacity); Lifecycle state; Ticket output;
        require(!invalid.valid() && !invalid.reserve(state,output) && output==Ticket{} && !invalid.usedSlots(), "invalid capacity fails closed");
    }
    Scheduler queue;
    std::array<Lifecycle,Scheduler::kMaxSlots> states;
    std::array<Ticket,Scheduler::kMaxSlots> tickets;
    for (uint32_t i=0;i<Scheduler::kMaxSlots;++i)
        require(queue.reserve(states[i],tickets[i]) && queue.contains(tickets[i]), "reserve full bounded capacity");
    Lifecycle extra; Ticket untouched=tickets[0];
    require(queue.usedSlots()==64 && !queue.reserve(extra,untouched) && untouched==tickets[0], "capacity overflow preserves output");
    for (uint32_t i=0;i<Scheduler::kMaxSlots;++i) require(queue.cancel(tickets[i]), "cancel uncommitted reservations");
    require(!queue.usedSlots() && queue.reserve(extra,untouched), "abandoned reservation returns capacity");
    for (const auto old : tickets) {
        const auto before=snapshot(queue,extra);
        require(!queue.contains(old), "old generation is stale");
        failed(queue.enqueue(old),before,queue,extra);
        failed(queue.commit(old),before,queue,extra);
        failed(queue.cancel(old),before,queue,extra);
        failed(queue.markScheduled(old),before,queue,extra);
        failed(queue.finish(old,false),before,queue,extra);
        failed(queue.markCallbacksDone(old),before,queue,extra);
        failed(queue.release(old),before,queue,extra);
    }
    Ticket malformed=untouched; malformed.slot=UINT32_MAX;
    require(!queue.contains(malformed) && !queue.cancel(malformed), "invalid slot rejects");
    malformed=untouched; malformed.generation=0;
    require(!queue.contains(malformed) && !queue.cancel(malformed), "zero generation rejects");
    require(queue.cancel(untouched), "release final reservation");
    Ticket previous=untouched;
    for (uint32_t i=0;i<10000;++i) {
        Lifecycle state;Ticket next;
        require(queue.reserve(state,next) && next.generation>previous.generation && !queue.contains(previous), "slot reuse monotonically changes generation");
        require(queue.cancel(next), "cancel repeated abandoned command");previous=next;
    }
}
void cancellationAndCallbackProgress() {
    Scheduler queue(3);Lifecycle first,second,third;Ticket a,b,c,out;
    require(queue.reserve(first,a) && queue.reserve(second,b) && queue.reserve(third,c), "reserve ordered commands");
    require(queue.enqueue(a) && queue.commit(b), "earlier explicit enqueue precedes later implicit enqueue");
    require(!queue.takeReady(out), "uncommitted oldest blocks later committed work");
    require(queue.cancel(a) && queue.takeReady(out) && out==b, "abandonment removes FIFO obstruction");
    require(queue.finish(b,false) && second.phase()==Phase::Error && second.scheduledReady(), "error terminal releases worker without schedule event");
    require(!second.completedReady() && queue.commit(c) && queue.takeReady(out) && out==c, "later work progresses before earlier callbacks");
    require(queue.markScheduled(c) && queue.finish(c,false) && third.phase()==Phase::Error, "later cleanup error remains terminal failure");
    callbackRetirement(queue,c,third);callbackRetirement(queue,b,second);
    require(!queue.usedSlots(), "error paths release all host reservations");
}
void capacityOneCallbackReentry() {
    Scheduler queue(1), other(1);Lifecycle first,second,third;Ticket a,b,c,out;
    require(queue.reserve(first,a) && queue.commit(a) && queue.takeReady(out) && out==a,
        "capacity-one command enters actual host FIFO");
    require(!queue.reserve(second,b) && b==Ticket{}, "active capacity-one execution prevents another reservation");
    require(queue.markScheduled(a) && queue.finish(a,true) && queue.release(a),
        "native worker completion returns execution capacity before callback delivery");
    require(!first.completedReady() && first.terminal() && !queue.usedSlots(),
        "completion wait remains pending independently of free capacity");
    // The real ObjC callback creates its next command against these same
    // production admission operations. No executor/GPU result is simulated.
    require(queue.reserve(second,b) && b.slot==a.slot && b.generation>a.generation &&
        queue.commit(b) && queue.takeReady(out) && out==b,
        "capacity-one completion callback can reserve and commit next command");
    const auto secondBefore=snapshot(queue,second);
    const auto firstBefore=snapshot(queue,first);
    failed(queue.finish(a,false),secondBefore,queue,second);
    failed(queue.markScheduled(a),secondBefore,queue,second);
    failed(queue.markCallbacksDone(a),secondBefore,queue,second);
    failed(queue.release(a),secondBefore,queue,second);
    failed(queue.cancel(a),secondBefore,queue,second);
    failed(other.markCallbacksDone(first),firstBefore,queue,first);
    failed(other.markScheduledCallbacksDone(first),firstBefore,queue,first);
    failed(queue.markCallbacksDone(first),firstBefore,queue,first);
    require(queue.markScheduledCallbacksDone(first) && first.scheduledReady() && !first.completedReady() &&
        same(secondBefore,snapshot(queue,second)), "detached scheduled handlers unblock only the old scheduled wait");
    require(queue.markCallbacksDone(first) && first.completedReady() &&
        same(secondBefore,snapshot(queue,second)), "old lifecycle callback completion cannot mutate reused active slot");
    const auto firstDone=snapshot(queue,first);
    failed(queue.markCallbacksDone(first),firstDone,queue,first);
    require(queue.finish(b,false) && queue.release(b) && second.phase()==Phase::Error &&
        second.scheduledReady() && !second.completedReady(), "error completion also returns capacity before handlers");
    require(queue.reserve(third,c) && c.slot==b.slot && queue.enqueue(c) && queue.cancel(c),
        "abandoned enqueued replacement returns capacity");
    require(third.completedReady() && !queue.usedSlots(), "abandonment completes its host lifecycle without callback duty");
    require(queue.markCallbacksDone(second) && second.completedReady(), "detached error lifecycle completes after later cancellation");
    require(!queue.markCallbacksDone(third), "canceled lifecycle cannot complete twice");
}
void scheduledAndCompletedWaits() {
    Scheduler queue(1), other(1);Lifecycle state;Ticket ticket,out;
    require(!queue.markScheduledCallbacksDone(state),"unowned lifecycle cannot publish scheduled callback completion");
    require(queue.reserve(state,ticket) && queue.commit(ticket),"reserve and commit scheduled-wait state");
    auto before=snapshot(queue,state);
    failed(queue.markScheduledCallbacksDone(state),before,queue,state);
    require(queue.takeReady(out) && queue.markScheduled(ticket),"actual worker observation changes only scheduling phase");
    require(!state.scheduledReady() && !state.completedReady(),"scheduled handler may still be running at observed phase");
    before=snapshot(queue,state);
    failed(other.markScheduledCallbacksDone(state),before,queue,state);
    require(queue.markScheduledCallbacksDone(state) && state.scheduledReady() && !state.completedReady(),
        "completed scheduled handlers unblock scheduled wait before terminal completion");
    before=snapshot(queue,state);
    failed(queue.markScheduledCallbacksDone(state),before,queue,state);
    failed(queue.markCallbacksDone(state),before,queue,state);
    require(queue.finish(ticket,true) && queue.release(ticket) && state.scheduledReady() && !state.completedReady(),
        "terminal result preserves scheduled completion but still waits for completion handlers");
    require(queue.markCallbacksDone(state) && state.completedReady(),"completion handlers satisfy only final wait");
}
void allEnqueueCommitOrders() {
    std::array<uint32_t,4> enqueued{{0,1,2,3}};
    do {
        std::array<uint32_t,4> committed{{0,1,2,3}};
        do {
            Scheduler queue(4);std::array<Lifecycle,4> states;std::array<Ticket,4> tickets;
            for (uint32_t i=0;i<4;++i) require(queue.reserve(states[i],tickets[i]), "reserve permutation slots");
            for (const auto i:enqueued) require(queue.enqueue(tickets[i]), "assign enqueue order independently of creation");
            std::array<bool,4> isCommitted{};std::vector<uint32_t> observed;
            for (const auto newlyCommitted:committed) {
                require(queue.commit(tickets[newlyCommitted]), "commit selected queued slot");isCommitted[newlyCommitted]=true;
                for (;;) {
                    Ticket actual;const bool ready=queue.takeReady(actual);
                    const bool expected=observed.size()<4 && isCommitted[enqueued[observed.size()]];
                    require(ready==expected,"FIFO readiness equals oldest actual enqueue, not commit order");
                    if (!ready) { require(actual==Ticket{},"not-ready output unchanged");break; }
                    const auto index=enqueued[observed.size()];
                    require(actual==tickets[index] && states[index].active(),"worker receives exact FIFO generation");
                    Ticket unchanged=tickets[(index+1)%4];
                    require(!queue.takeReady(unchanged) && unchanged==tickets[(index+1)%4],"only one active command");
                    if (index&1) require(queue.finish(actual,false),"host failure transition");
                    else require(queue.markScheduled(actual) && queue.finish(actual,true),"host success transition");
                    require(queue.release(actual) && !states[index].completedReady(),
                        "execution releases admission independently of pending handlers");
                    require(queue.usedSlots()==3-observed.size(),"only unfinished execution reservations count toward capacity");
                    observed.push_back(index);
                }
            }
            require(observed==std::vector<uint32_t>(enqueued.begin(),enqueued.end()),"all executions preserve enqueue order");
            Lifecycle extra;Ticket extraTicket;
            require(queue.reserve(extra,extraTicket),"pending callbacks do not consume execution capacity");
            // Deliberately finish handlers in reverse execution order: worker
            // progress and callback retirement are distinct host obligations.
            for(auto it=observed.rbegin();it!=observed.rend();++it) {
                const auto before=snapshot(queue,extra);
                failed(queue.markCallbacksDone(tickets[*it]),before,queue,extra);
                if(states[*it].phase()==Phase::Completed)
                    require(queue.markScheduledCallbacksDone(states[*it]) && states[*it].scheduledReady() && !states[*it].completedReady(),
                        "detached scheduled handlers precede final handlers");
                require(queue.markCallbacksDone(states[*it]) && states[*it].completedReady(),
                    "reverse-order detached callback completion remains exact");
                require(same(before,snapshot(queue,extra)),"old callback completion preserves replacement admission");
            }
            require(queue.cancel(extraTicket) && !queue.usedSlots(),"replacement abandonment releases final reservation");
        } while (std::next_permutation(committed.begin(),committed.end()));
    } while (std::next_permutation(enqueued.begin(),enqueued.end()));
}
}
int main() {
    try {
        phaseRules();capacityAndGenerations();cancellationAndCallbackProgress();capacityOneCallbackReentry();scheduledAndCompletedWaits();allEnqueueCommitOrders();
        std::cout << "{\"success\":true,\"host_lifecycle_only\":true,\"hardware_executed\":false,\"checks\":" << checks << "}\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "command lifecycle failed after " << checks << " checks: " << error.what() << '\n';return 1;
    }
}
