# Combined engine attempt evidence

The engine-enabled path now preserves a pre-control diagnostic batch and then
continues to the existing real lifecycle/engine startup. It is not a separate
capture-only package and does not relax any startup predicate.

## Actual scope and historical capture

`runEngineAttempt` holds the actual FunctionGroup normal Operation. After
validating and retaining the assigned BAR5 map, it runs the existing Device
discovery, runtime ROM, posted/media-access, memory, boot-display and display
context captures under the Device mutex. Each diagnostic MMIO/selector callback
checks that exact scope before and after access. Semantic failures do not skip
independent captures while the scope and each capture's prerequisites remain
valid. No callback restores selectors or performs further access after scope
revocation.

A valid memory/BAR layout also permits two actual platform-console queries,
even when display address classification rejects TLB0. Console `InputAddress`
is retained with explicit `AddressKind`; an unknown input is not labelled a
decoded physical address. No virtual-to-physical guess or framebuffer mapping
is performed.

The immutable `PreEngineCapture` retains those dictionaries, the diagnostic
power generation and the real Operation generation as provenance only. Before
lifecycle creation, all live snapshots, runtime ROM/discovery copies and their
`SnapshotValid` flag are cleared. The actual assigned BAR5 mapping stays retained.
Engine startup independently reacquires and admits its own inputs; diagnostic
properties never become an allocation, reservation or takeover certificate.

## Additional engine prerequisites

After its own discovered memory and posted-state observation, but before the
first audio or GPU control write, Engine collects `EnginePreflightEvidence`:

- Training: two actual C2PMSG35 samples bracketed by three SOS samples.
- LSDMA: its twelve resolved words twice, with SOS before each bank and after
  the second bank. Nonzero SOS stops dependent access; ordinary idle mismatch
  remains raw evidence and does not hide the rest of the bank.
- Inherited DMCUB: resolved control, GPINT, documented window and ring registers.
- CP/SDMA: the portable read-only inventory of all 77 existing bootstrap words,
  then final SOS. Each value retains attempted, callback-success, validity and
  guard comparison/mismatch bits. Semantic mismatch and transport/access loss
  remain different outcomes. The large inventory stays off the kernel stack.

The CP/SDMA/LSDMA cold access contract is unchanged. These captures do not stop
engines, wake power-gated windows, grant first-attach authority or replace fresh
mandatory startup checks. DMCUB stop and subsequent cold-DMA admission keep their
existing ordering. Actual FunctionGroup scope loss prevents controls after the
batch. Every evidence dictionary states that it supplies no current authority.

`EngineStageEvidence` retains immutable per-stage dictionaries before mutable
protocol results can be overwritten by retirement. It preserves selected
audio, DMCUB, cold-DMA, reservation ranges and PSP results. Its private array is
capped at 80 records; publication/allocation failure marks
`EngineStageEvidenceTruncated`. This journal supplements existing actual CP,
shader and reusable-compute observations; it is not complete instrumentation of
every subsystem or proof that any stage ran on this GPU.

## Remaining evidence and execution gaps

- Recovery preparation happens before Engine start. If it fails, Device's
  `PreEngineCapture` and the new immutable `RecoveryPreparation` survive, but
  the extra Engine batch has not run. Reset/ATOM preparation and paired-function
  creation/admission now retain actual outcomes; see
  [recovery preparation](recovery-preparation-evidence.md) and
  [function acquisition](function-group-acquisition-evidence.md). Later runtime
  recovery remains a separate, incompletely journaled operation.
- BootDisplay now retains its ordered partial read prefix as well as the
  original complete snapshot/power inventory. Engine preflight, owned capture
  and post-DMCUB cold-DMA checks publish immutable bounded raw journals without
  adding reads. See [read capture](actual-read-capture.md). BootDisplayContext
  retains its existing per-pass validity masks.
- The pre-control batch does not run the post-DMCUB cold-DMA checker early or
  claim IH/DWB/MCIF/MALL quiescence. The actual startup cold-DMA check's full
  successful and partial raw observations are now retained; this does not add
  a read trace around later recovery's separate checks.
- The stage journal retains selected fields, not every audio, firmware, GART,
  queue, allocator or recovery detail. These broader audit gaps remain open.
- The combined native core/aux build now compiles and links in
  `build/macos-combined-graphics-evidence-2`. No IOKit scope,
  engine startup, queue submission, Metal draw or physical display takeover has
  been executed by these validation commands.

Run `scripts/check-engine-batch-native-local.py --output <fresh-directory>` for
the real Engine/Device/DeviceEngines objects and their frozen header closures.
It enforces a 2KiB stack-frame limit. The bootstrap inventory has its own
executable protocol/fault and source-oracle tests; native IOKit authority is
not replaced with a mock for testing.
