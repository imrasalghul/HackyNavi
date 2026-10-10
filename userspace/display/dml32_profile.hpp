// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DML32_PROFILE_HPP
#define HACKYNAVI_DML32_PROFILE_HPP
#include "dml32_plan.h"
#include "hackynavi/display_model_input.hpp"
#include "hackynavi/display_smu.hpp"
#include "hackynavi/edid.hpp"
namespace hackynavi { namespace displayprofile {
enum class Policy : uint32_t { ColdFixedDcCeilingV1=1 };
enum class Status { Ok,UnsupportedPolicy,InvalidBoard,InvalidReference,AmbiguousLatency,
    InvalidInventory,NoClockWithinLimit,InvalidMode,CalculationFailed };
enum class Output : uint32_t { HdmiTmds=1,DisplayPortLegacy=2 };
struct Mode {
    edid::Timing timing;
    uint32_t pitchPixels;
    Output output;
    uint32_t hdmiTmdsLimitKHz,dpRateMbps;
    uint8_t dpLanes;
};
struct Profile {
    Policy policy;
    soc_bounding_box_st soc;
    ip_params_st ip;
    displaymodel::ReferenceInfo reference;
    uint16_t selectedClockMHz[displaysmu::kDpmClockCount];
};
// Trusted in-process observation adapter, not a wire ABI or native authority.
// Copies source-backed DCN32 ASIC capacities/latency/derate policy, then fills
// ALL board topology/VCO and programmable clocks from supplied observations.
// Nonzero ROM latency overrides are rejected because pinned Linux and its ROM
// header disagree on units. Zero means use named ASIC policy, never measured0.
//
// One synthetic DC-limited calculation state: each independent clock uses its
// greatest reported legal value <= its own DC ceiling (and1950MHz DISP/DPP
// ASIC cap). No firmware indices are correlated into observed voltage states.
// This follows the explicit max-DC tuple inserted by Linux's DCN32 synthetic
// state policy, specialized to the initial fixed-clock single-pipe owner.
// Its joint clocks are REQUIREMENTS, not evidence of simultaneous operation.
// Native use requires fresh own-generation observations and actual retained
// holds/readback for every clock, PM/WM/transition policy, and surface/mode.
Status build(const displaymodel::BoardInfo&,const displaymodel::ReferenceInfo&,
    const displaysmu::DpmInventory&,Policy,Profile&);
// Constructs the exact single linear RGB32/direct-VRAM pipe from a proposed
// progressive SDR24 timing and actual pitch/link limits. EDID advertisement,
// allocation and trained-link ownership must be checked by the native caller.
Status makePipe(const Profile&,const Mode&,display_e2e_pipe_params_st&);
// Runs the actual vendor DML32 calculation; status is never inferred from
// zero-initialized output. No watermark policy or A/B/C/D copies are fabricated.
Status calculate(const displaymodel::BoardInfo&,const displaymodel::ReferenceInfo&,
    const displaysmu::DpmInventory&,Policy,const Mode&,hn_dml_plan&,hn_dml_status&);
} }
#endif
