// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISPLAY_BANDWIDTH_HPP
#define HACKYNAVI_DISPLAY_BANDWIDTH_HPP
#include "discovery.hpp"
namespace hackynavi { namespace displaybandwidth {
// Exact integer outputs used by the DCN3.2 hubp3_setup +
// hubp2_setup_interdependent callbacks. Values are DML register units, not ns.
enum class Field : uint8_t {
    rq_plane1_base_address,
    rq_drq_expansion_mode,
    rq_prq_expansion_mode,
    rq_mrq_expansion_mode,
    rq_crq_expansion_mode,
    rq_rq_regs_l_chunk_size,
    rq_rq_regs_l_min_chunk_size,
    rq_rq_regs_l_meta_chunk_size,
    rq_rq_regs_l_min_meta_chunk_size,
    rq_rq_regs_l_dpte_group_size,
    rq_rq_regs_l_mpte_group_size,
    rq_rq_regs_l_swath_height,
    rq_rq_regs_l_pte_row_height_linear,
    rq_rq_regs_c_chunk_size,
    rq_rq_regs_c_min_chunk_size,
    rq_rq_regs_c_meta_chunk_size,
    rq_rq_regs_c_min_meta_chunk_size,
    rq_rq_regs_c_dpte_group_size,
    rq_rq_regs_c_swath_height,
    rq_rq_regs_c_pte_row_height_linear,
    dlg_refcyc_h_blank_end,
    dlg_dlg_vblank_end,
    dlg_min_dst_y_next_start,
    dlg_refcyc_per_htotal,
    dlg_refcyc_x_after_scaler,
    dlg_dst_y_after_scaler,
    dlg_ref_freq_to_pix_freq,
    dlg_refcyc_per_pte_group_vblank_l,
    dlg_dst_y_per_pte_row_nom_l,
    dlg_refcyc_per_pte_group_nom_l,
    dlg_dst_y_per_meta_row_nom_l,
    dlg_refcyc_per_meta_chunk_nom_l,
    dlg_refcyc_per_line_delivery_l,
    dlg_refcyc_per_line_delivery_c,
    dlg_refcyc_per_pte_group_vblank_c,
    dlg_dst_y_per_pte_row_nom_c,
    dlg_refcyc_per_pte_group_nom_c,
    dlg_dst_y_per_meta_row_nom_c,
    dlg_refcyc_per_meta_chunk_nom_c,
    ttu_qos_level_low_wm,
    ttu_qos_level_high_wm,
    ttu_refcyc_per_req_delivery_l,
    ttu_qos_level_fixed_l,
    ttu_qos_ramp_disable_l,
    ttu_refcyc_per_req_delivery_c,
    ttu_qos_level_fixed_c,
    ttu_qos_ramp_disable_c,
    ttu_refcyc_per_req_delivery_cur0,
    ttu_qos_level_fixed_cur0,
    ttu_qos_ramp_disable_cur0,
    dlg_refcyc_per_pte_group_flip_l,
    dlg_refcyc_per_vm_dmdata,
    dlg_dst_y_prefetch,
    dlg_vratio_prefetch,
    dlg_vratio_prefetch_c,
    dlg_dst_y_per_vm_vblank,
    dlg_dst_y_per_row_vblank,
    dlg_dst_y_per_vm_flip,
    dlg_dst_y_per_row_flip,
    dlg_refcyc_per_meta_chunk_vblank_l,
    dlg_refcyc_per_meta_chunk_vblank_c,
    dlg_refcyc_per_meta_chunk_flip_l,
    dlg_refcyc_per_line_delivery_pre_l,
    dlg_refcyc_per_line_delivery_pre_c,
    ttu_refcyc_per_req_delivery_pre_l,
    ttu_refcyc_per_req_delivery_pre_c,
    ttu_refcyc_per_req_delivery_pre_cur0,
    ttu_refcyc_per_req_delivery_pre_cur1,
    ttu_min_ttu_vblank,
    ttu_qos_level_flip,
    Count
};
constexpr size_t kFieldCount=size_t(Field::Count), kTimingRegisterCount=39;
enum class Profile : uint32_t { SingleLinearRgb32DirectVram=1 };
struct Identity { uint64_t deviceGeneration,modeGeneration,clockGeneration,calculationId; };
// Each slot must uniquely identify its real calculation/policy record. The IDs are
// provenance, not proof of a lock, valid clocks, or hardware ownership.
struct Watermarks {
    uint64_t sourceId;
    uint32_t urgentNs,fracUrgentFlipPerThousand,fracUrgentNomPerThousand;
    uint32_t urgentLatencyNs,stutterEnterExitNs,stutterExitNs;
    uint32_t dramChangeNs,fabricChangeNs,retrainingNs;
};
struct Input {
    Profile profile;
    Identity identity;
    uint32_t fieldCount,fields[kFieldCount];
    Watermarks watermarks[4]; // A/B/C/D, all explicit; never synthesize others from A.
    uint32_t detKbytes,compbufKbytes;
    uint32_t width,height,htotal,vtotal,vblankEnd;
    uint32_t vstartupStart,vreadyOffset,vupdateWidth,vupdateOffset;
    uint32_t dccgReferenceKHz,dchubReferenceKHz,timerDivider;
    uint8_t pipe;
};
struct Layout { bool valid;uint32_t baseDwords;uint64_t barBytes; };
enum class Status { Ok,InvalidArgument,InvalidPlan,FieldOverflow,DiscoveryRejected,
    UnsupportedDmu,OutsideBar,InvalidState,OwnershipRejected,Revoked,
    PowerUnavailable,ClockUnavailable,ActivePipe,ForeignControl,TransportError,
    DeviceUnavailable,VerifyFailed,BufferState,HardwareFault,Timeout };
struct Poll { uint32_t iterations,intervalUs; }; // each wait <=100ms
struct Result { Status status;uint32_t reads,writes,polls,lastRegister,lastValue;
    bool writeAttempted,retentionRequired; };
class Controller;
class Plan {
public:
    Plan();
    bool valid() const{return valid_;}
    const Identity& identity() const{return identity_;}
    uint8_t pipe() const{return pipe_;}
    uint64_t watermarkSource(size_t set) const{return set<4?watermarkSources_[set]:0;}
    uint32_t width() const{return width_;}
    uint32_t height() const{return height_;}
    uint32_t dccgReferenceKHz() const{return dccgReferenceKHz_;}
    uint32_t dchubReferenceKHz() const{return dchubReferenceKHz_;}
private:
    friend Status plan(const Input&,Plan&);
    friend class Controller;
    bool valid_;Identity identity_;uint8_t pipe_;
    uint64_t watermarkSources_[4];
    uint32_t timing_[kTimingRegisterCount],watermarks_[4][9];
    uint32_t detSegments_,compbufSegments_,vready_,timerDivider_;
    uint32_t width_,height_;
    uint32_t dccgReferenceKHz_,dchubReferenceKHz_;
};
Status plan(const Input&,Plan&);
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);
// enter MUST acquire the native common operational gate, validate this plan
// against the owner's complete retained calculation/mode/clock record,
// and establish/hold measured DCCG/DCFHUB/display clocks and PM/firmware exclusion.
// It must fail when exclusion is unavailable. leave releases that same gate.
// valid observes immediate callback-entry revocation while the gate is held.
// read32/write32 additionally enforce bounded per-I/O revocation. A numeric
// epoch alone cannot implement these callbacks. No native implementation yet.
struct Io {
    void* context;
    bool (*enter)(void*,const Plan&);
    void (*leave)(void*);
    bool (*valid)(void*,const Identity&);
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);
    void (*delayUs)(void*,uint32_t);
};
enum class Phase { Closed,Programming,Configured,Faulted,Retired };
// Never starts a requestor, OTG, cursor, link or DMA. Initial programming
// requires all OTGs stopped and all powered HUBPs blank/drained, selected HUBP
// powered/clocked, no VM/DCC/MALL/dynamic metadata/cursor/unbounded fetch, and
// native owner excludes firmware/PM throughout. All four DET allocations and
// global WM sets are owned together. No active modeset or dynamic WM updates.
class Controller {
public:
    Controller();
    Status configure(const Layout&,const Io&,const Plan&,const Poll&,Result&);
    // Verified global blank/drain retires programming ownership, retaining
    // programmed values. Does not restore earlier clocks/WM/DET or drop clocks.
    // Next owner must program a complete new plan before enabling fetch.
    Status retire(Result&);
    Phase phase() const{return phase_;}
    bool requiresHardwareRetirement() const{return touched_;}
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;Io io_;Plan plan_;Phase phase_;bool touched_;
    // Retain exact STATUS/CONFIG, including desired state, across owned access.
    uint32_t powerStatus_[4],powerConfig_[4];bool powerValid_[4];
    Status finish(Status,Result&);
    Status read(uint32_t offset,uint32_t&,Result&);
    Status update(uint32_t offset,uint32_t mask,uint32_t value,Result&,uint32_t clear=0);
    Status guard(Result&);
    Status power(uint8_t,uint32_t&,Result&);
    Status waitBuffer(uint32_t offset,uint32_t size,const Poll&,Result&);
    Status buffers(const Poll&,Result&);
};
} }
#endif
