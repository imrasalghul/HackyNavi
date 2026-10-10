# Cold global DISPCLK / DPPCLK ownership

The portable DAL transaction below is implemented in `display_smu.hpp/.cpp`
and covered by protocol tests. Native ownership and later publication remain
separate integration work; the contract below specifies their requirements.
There is no startup activation or target clock-transition evidence from this
work. The original proposal is retained by the hash-bound evidence under
`build/cold-global-display-clock-design`.

## Source facts and the current blocker

The primary source is Linux commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`, retained under `.research/linux`:

- `dcn32_clk_mgr.c:304–323` rounds the requested DISP/DPP frequency through the
  VCO integer divider before sending the preset, specifically to avoid a DPP
  reference/DTO mismatch. Lines 793–802 and 832–841 send the preset **floored**
  to MHz. Ceiling the original pixel frequency is a different operation.
- Lines 374–379 compute the new DISPCLK DID and read the old hardware WDIVIDER.
  Entering DID 127 from any other DID requires a temporary DID 126 request
  (410–424), then the final DISPCLK request (461–471). Leaving DID 127 has no
  corresponding temporary 126 command in this function.
- The active OTG-master FIFO workarounds are separate: drop pixels when leaving
  127 (382–409), and add pixels after the 126 prelude when entering 127
  (437–458). This proposal admits only fully cold physical pipes, not active
  scanout. It cannot be extended to live transitions by omitting these loops.
- Direct DENTIST register updates in this function are conditional on
  `override_dispclk_programming`. The normal PMFW path is the DAL command.
  This proposal does not copy that debug override or directly write a DID.
- Lines 827–850 order live DPP changes around DTO updates: update DTO before
  lowering the global reference; raise the global reference before lowering
  the DTO ratio. Cold admission must establish that no DTO is active instead
  of pretending this live sequence has run.
- `dcn32_clk_mgr_smu_msg.c:281–300` sends message 9 with `(PPCLK << 16) | MHz`.
  The same function queries hard-min status on supported PMFW; lines 193–206
  establish Navi31's `0x4e6a00` gate. Lines 236–246 require DPREFCLK together
  with the requested clock: DISP mask `0x05`, DPP mask `0x06`. A fulfilled
  minimum is not a DENTIST/DFS divider readback.
- That helper's preceding comment says MHz, but its logs and the actual DTB
  caller (`dcn32_clk_mgr.c:817–824`) use the returned value as kHz. Keep the
  existing interpreted DISP/DPP result, the raw reply and the derived hardware
  programming separate; do not turn this into a measured clock counter.

The existing `displaydentist::plan` and `capture` already provide the integer
arithmetic and two identical real register snapshots. Capture validates the
PLL against the retained reference but deliberately does not require the
change-done bits or divider agreement; it reports those facts. A mutating owner
must explicitly require them before it publishes a usable reference.

`displaysmu::Controller::requestMinimum` rejects `MHz < requested_[clock]`.
Therefore simply issuing two calls fails at the second command when entering
127. For an illustrative 4,300,000 kHz programmed VCO, DID 126 represents
69,354 kHz and the intermediate request is floor 69 MHz. A 30,000 kHz
requirement plans a final floor 30 MHz request and DID 127 programming of
33,593 kHz. Request MHz, rounded preset kHz, and derived programmed kHz are
three distinct values. This example is arithmetic, not a target measurement.

Clearing `requested_`, reconstructing Controller, or invoking
`clearAfterVerifiedReset` between the two requests would discard real retained
ownership and is not a valid solution.

## Implemented portable transition contract

`requestMinimum` remains monotonic for existing callers. The distinct one-shot
`requestColdPreset` transaction uses the **same** Controller, with the same
mailbox I/O, firmware gate and retained minimum state. Limit its clock selector
to DISP/DPP and nonzero sourced presets within the 1950 MHz profile. Do not add
an `allowLower` argument to the public legacy API.

The transaction accepts the retained programmed VCO and required kHz, computes
its own `displaydentist::plan`, and requires a phase-observation callback
supplied by the native owner. This callback reads the actual PLL/DFS/
DENTIST state and rechecks the installed native scope, FAMS/DAL coordination
and cold physical/local ownership. Its result comes from those operations,
not a caller-supplied `cold=true` or a copied prior StopResult.

The private journal binds the VCO, original observed DID, required kHz, final
preset and optional exact DID126 prelude. Legal DID/preset arithmetic is checked
at admission. The helper rejects an existing cold attempt, an already-owned legacy request
for that clock, or a malformed/unbounded plan before I/O. This first version
is a one-shot acquisition per clock/reset epoch, not an arbitrary reclocking
API. A completed first acquisition does not authorize subsequent requests.

For DISP entering 127, the one transaction performs:

1. Require a fresh matching initial observation and the native cold guard.
2. Journal the exact 126 request **before** a possibly delivered message write.
   Send message 9 and complete the real DISP+DPREF hard-min-status sequence.
3. Invoke the real phase observer. Require matching PLL, stable WDIVIDER and
   RDIVIDER both 126, DFS DISP DID 126, and DISP change-done. Check physical
   idle, inbox identity/idle and the same actual generation again. If PMFW
   does not produce the required programming, retain the intermediate request
   and fail; do not proceed to 127 or write DENTIST directly.
4. Journal and send the lower final request, preserving the intermediate
   history. Complete hard-min fulfillment and the final actual observation.
5. Publish output only after all final native checks succeed.

For an initial DID already 127 and a final DID127, no 126 prelude is required
by the source. Leaving 127 or any non-127 transition has a single final command
under the same cold-only contract. DPP has a single command; it must not inherit
the DISP-specific 126 workaround.

Use one shared response-read counter across both commands and every fulfillment
query, plus the native single elapsed deadline spanning all phase callbacks.
Do not recursively call `requestMinimum` twice and reset its result/read budget.
A private common request helper can preserve the existing message, response,
frequency and fulfillment implementation; only the new transaction's internal
exact 126-to-final step may descend below its just-established intermediate
request. There is no public general lowering bypass.

Each clock's private `ColdPresetJournal` retains started/completed, original
VCO/DID, intermediate/final requested MHz, attempted message stages, actual
ACK/reply/fulfillment state, last fulfilled floor and phase-observation results.
Generation and actual scope belong to the native owner, never a copied portable
boolean or caller token. The last fulfilled floor stays distinct from a newer
potentially delivered request. Any read/write failure, timeout, malformed
reply or failed post-write observation seals the Controller and leaves both
attempts retained. No replay, successful-zero release or owner reconstruction.
An attempted intermediate or final message makes `policyMayBeActive` true and
causes reset-required retirement for this new owner. It does not inherit the
unaudited legacy zero-MHz withdrawal interpretation. A failed initial
observation terminally faults the Controller but does not claim a clock message
was sent. Both clock journals clear only in `clearAfterVerifiedReset`.

The exact callable boundary is:

```cpp
Status Controller::requestColdPreset(
    Clock clock, uint32_t programmedVcoKHz, uint32_t requiredKHz,
    const ColdPresetIo&, const Poll&, ColdPresetFrequency&, ColdPresetResult&);
```

The observer receives the clock, `InitialObservation`,
`IntermediateObservation` or `FinalObservation`, and a `ColdPresetTarget`
containing VCO, programmed kHz and DID. Initial target DID/kHz are zero to
explicitly request the old programming; every later target is computed by the
Controller. It returns raw PLL, raw DENTIST, raw DFS DID, programmed VCO and
derived kHz. The helper independently checks legal divider arithmetic,
change-done and matching DENTIST/DFS fields; later phases also require the
original raw PLL. It requires exact raw firmware-reply/derived-kHz agreement
after each fulfilled phase. A true callback result alone cannot bypass these
checks. The callback must perform the actual stable-read and native authority
checks described below.

`ColdPresetFrequency` is cleared at entry and filled only after the final
observation passes. `ColdPresetResult` separately retains stage and the shared
mailbox counters on failure. The getter `coldPresetJournal(Clock)` exposes a
borrowed immutable journal only for DISP/DPP. No output reference is published
from a completed intermediate step. The numeric callback fixtures in
`tests/display_smu_tests.cpp` test protocol decisions and retention, not hardware.

## Native owner and admission

Add a native request entry point to the existing Display owner, taking the
actual `Operation&`, required DISP/DPP kHz and a bounded timeout. It operates
under Display -> Firmware -> GART, with the timestamp captured before locking.
The existing legacy `requestClockMinimum` does not supply this contract: it
lacks an explicit scope/deadline, FAMS generation/inbox checks, the owned
DPM/reference comparison and actual final divider confirmation.

Before mutation, require the actual owner relationships, Display Ready,
Firmware ready, normal GART Access, completed same-generation private FAMS
and DAL0x14 journals, and the actual owned inbox mapping/producer/RPTR state.
Reject all queue/VM owners, native surface, AUX/DDC, links, streams and DTO
owners. Verify all physical OTGs stopped/not busy, HUBPs blank/drained where
powered, and MPC inputs/outputs disconnected and idle. The absence of software
routes alone does not establish that there are no active OTG masters.

Require the private current DPM/reference snapshot from this same generation
and serial. Retain a bounded private copy needed for validation, then invalidate
the public cached observation **before** any possibly delivered mutation.
Validate both intermediate and final **derived programmed** frequencies against
the actual applicable DPM/DC ceilings and the 1950 MHz limit, with checked
integer arithmetic. Do not check only floor MHz: it can conceal an over-limit
derived preset. DISP/DPP requests are divider-derived presets; the pinned
callers do not require every such preset to equal an enumerated DPM table row.
Do not impose UCLK's exact-row selection rule here or invent a missing table.

Read PLL/DFS/DENTIST immediately before selecting the transition. Require
DISP WDIVIDER/RDIVIDER/DFS agreement and change-done before using the old DID.
For DPP, require its WDIVIDER (`DENTIST` bits 30:24), DFS DPP DID, and DPP
change-done to agree. These exact fields are in `dcn_3_2_0_sh_mask.h:28–47`;
the present capture already retains the full raw register. It needs an explicit
DPP agreement predicate at the consumer boundary. Stable raw values alone
with a clear done bit are not completed programming.

After each actual message phase, take another complete stable capture with the
same PLL reference and required clocks. Require the predicted DID/programmed
frequency, fulfilled clock+DPREF bits, legal reported frequency, fresh physical
idle and owned-inbox checks. Initially reject any unexplained discrepancy
between firmware's interpreted frequency and the derived programmed frequency;
do not introduce an arbitrary tolerance or silently substitute one for another.
This is a stricter acceptance policy, not a claim that Linux compares them.

Both clocks are still cold throughout. If the native entry point accepts the
pair together, preserve the sourced order: a DPP raise before DISP programming;
a DPP lowering after DISP programming. No DTO adjustment is performed because
actual cold admission forbids those consumers. Requests for an active pipe stay
unsupported until its separate FIFO/DTO/OTG transition ownership is implemented.

## Private reference publication and later use

Before the first possibly delivered phase, invalidate the old private global
reference for every clock being changed. On full success, publish a private
record binding the actual Display, Operation generation, new serial, DPM source
serial, PLL raw value, final requested preset, fulfillment and exact observed
DID/programmed kHz. Only then set the corresponding DTO/reference value.
Intermediate DID126 success alone cannot publish a final DISP reference.

`configureDppClock` currently trusts `dppReferenceKHz_` written directly from a
legacy response. The new route must not consider that scalar a sufficient
authorization. Its future native callsite must validate the private completed
cold journal and same generation, reject stale/invalidated reference state, and
obtain a fresh matching DPP programming observation before computing DTO phase.
The DTO reference must agree with actual verified global programming. Existing
`displayclock::Controller` then owns the real DTO programming/readbacks.

Invalidating the public DPM cache after a policy change must not erase the
private history of a delivered clock request. Conversely, retained history must
not stay marked usable after a later conflicting clock change, failed callback,
reset or device-generation change. Observation storage and authority need
separate validity fields and lifetime rules.

## Retirement and implementation order

The new acquisition remains disconnected from startup while the documented
early-failure and reset-prohibited-callback gaps remain. Its complete FAMS/DAL
and cold-preset journals stay owned through the new stop split. After graphics/
VM/RLC and coordinated Firmware clock retirement, actual DMCUB STOP removes
runtime DMA but does not release these reset-only journals. Busy /
StoppedRetained and a real permitted full-device reset remain the terminal path.

The smallest implementation sequence is:

1. Portable bounded cold preset transaction and private journal on the existing
   DAL owner. Test all source DID127 branches, the exact descending pair,
   response-budget exhaustion across both phases, failures before and after
   each potentially delivered write, malformed readback, no replay, retained
   last-fulfilled versus attempted floors, and reset-only clearing. Keep the
   legacy monotonic API tests unchanged.
2. Native global request wrapper and real phase observer using the existing
   Display/FAMS/DPM guard. No new PCI owner, DMCUB client, register override or
   copied boolean authority. Compile with actual kernel headers and frame
   limits; independently review lock handoff, guard lifetime and provenance.
3. Bind `configureDppClock` to the actual private completed reference record
   with generation and fresh hardware confirmation. Do not advertise a usable
   global owner merely because the request transport compiles.
4. Only after the broader transaction/lifecycle conditions are solved, connect
   the native entry point to startup/modeset preparation and obtain target
   evidence. No current source audit, unit test or native build substitutes
   for that execution.

No exact SOC/FCLK/UCLK operating-point, bandwidth/DML admission, physical
frequency measurement or active display modeset follows from this proposal.
