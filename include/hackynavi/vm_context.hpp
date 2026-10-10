// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/gpu_vm.hpp"
#include "hackynavi/ip_layout.hpp"
namespace hackynavi { namespace vmcontext {
enum class Hub { Gfx, Mm };
enum class Status { Ok, InvalidArgument, InvalidState, UnsupportedLayout, OutsideBar,
    InheritedContext, GlobalState, ReadFailed, DeviceUnavailable, WriteFailed,
    ReadbackMismatch, PublishFailed, SemaphoreTimeout, InvalidateTimeout };
enum class Phase { Fresh, Enabled, Detached, Faulted };
enum class Stage { Preflight, Guard, Root, Publish, Semaphore, Invalidate, Ack, Release,
                   PrivateInvalidate, Control, Complete };
struct Root {
    uint64_t address; // final GPU physical VRAM address or prepared device DMA address
    uint64_t bytes;   // exactly one owned 4KiB PDB2 root; never a flat VMID0 table
    gmc11::MemoryDomain domain;
    bool snooped;
};
struct Config {
    uint8_t vmid; // graphics VMIDs1..7; never system VMID0 or KFD VMIDs8..15
    Root root;
    uint64_t vaBegin, vaEnd; // page-aligned, exclusive end, lower canonical half
    uint64_t epoch; // native owner's actual reset/resource epoch, not a readiness flag
};
struct Io {
    void* context;
    bool (*read32)(void*, uint32_t, uint32_t&);
    bool (*write32)(void*, uint32_t, uint32_t);
    void (*delayUs)(void*, uint32_t);
    // Publish ALL table writes with actual DMA/VRAM synchronization and HDP
    // flush+posting read. CPU fences alone do not implement this callback.
    bool (*publishTablesAndHdp)(void*);
};
struct Poll { uint32_t iterations, delayUs; }; // bounded to100ms total
struct Result {
    Status status;
    Stage stage;
    uint32_t reads, writes, polls, lastOffset, lastValue;
    bool writesMayHaveOccurred, semaphoreMayBeHeld, retentionRequired;
};
struct Access;
class Context final {
public:
    Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Phase phase() const { return phase_; }
    const Config& config() const { return config_; }
    bool requiresHardwareRetirement() const { return retained_; }
private:
    friend struct Access;
    Phase phase_;
    Config config_;
    Hub hub_;
    uint32_t registers_[24];
    bool retained_;
};
// Narrow source-backed GMC11 context protocol. Tables must implement the
// four-level PDB2->PDB1->PDB0->PTB tree,512 entries/level,4KiB leaf pages,
// translate-further off. Root description is numerically validated here; only
// the native arena owner can prove actual residency, contents and BO ownership.
// Caller holds the selected VMID, all its producers and engine17 exclusively;
// powered hubs/caches stay enabled, GFXOFF/KIQ/MES/reset/PM are excluded.
// Native owners must prevent queue arrivals and VMID reuse for this interval.
// HackyNaviGraphicsQueue supplies serialized ownership for isolated one-job
// clients; this portable module alone owns no queue or native completion.
// Both hubs need separate Context objects referencing the SAME owned tree.
// Every other context and the global aperture/fault policy remain untouched.
// Non-default fault behavior + per-context fault interrupts are programmed;
// servicing IH/fault containment/scheduling is NOT implemented by this module.
// This alone must never advertise userspace isolation or enable raw submission.
Status enable(const Navi31IpLayout&, uint64_t barBytes, Hub, const Config&,
              const Io&, const Poll&, Context&, Result&);
// Re-enable only this exact successfully detached context. No replacement
// root, layout, VMID, range or epoch is accepted. Rechecks disabled control,
// remembered translation registers and global guards, publishes real tables,
// then enables/invalidates with readback. Retention remains latched on success
// and every failure. The native caller must separately prove prior producer
// completion and exclusive ownership before editing data/tables or invoking
// this protocol; Detached alone never grants DMA retirement or mutable access.
Status reactivate(Context&, const Io&, const Poll&, Result&);
Status flush(Context&, const Io&, const Poll&, Result&);
// Caller has stopped/excluded producers before detaching. Successful disable
// and TLB ACK prove translation state only, NOT outstanding DMA retirement.
// Retention stays latched even after success; no public free/reset/reuse API.
// Native owner must retain root, descendants and mapped BOs until verified
// producer fences/drain or the existing typed hardware reset proof retires all.
Status detach(Context&, const Io&, const Poll&, Result&);
const char* statusString(Status);
} }
