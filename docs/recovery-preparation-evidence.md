# Recovery preparation evidence

`HackyNaviLifecycleController::prepareRecovery` publishes a new immutable
`RecoveryPreparation` dictionary on its actual open owner for each completed
call, before Engine startup. It records the real Operation generation, state
admission, initial scope check, Reset Access check, Reset and ATOM attempts and
results, and the existing final scope check when reached. No diagnostic result
is consulted by an admission, allocation, or reset-proof path.

The nested `ResetPreparation` captures the actual discovery and ROM outcomes,
IP/layout diagnostics, resolved reset registers, complete native PCI snapshot,
and the existing SOS/Scratch3/Scratch7 samples. Those three arrays are in that
order. `RawAttemptedMask`, `RawReadSucceededMask`, and `RawValidMask` distinguish
an unattempted field, a rejected callback, all-ones, and an accepted sample. A
callback can reject after the volatile read if its scope was revoked; the raw
value then has no validity claim. No final SOS read was added:
`FinalSosSampleAttempted=false` makes that limit explicit. A partial PCI snapshot
is not serialized because its underlying API supplies no per-field validity.

Reset `AttemptedSteps` and `CompletedSteps` use these bit indices:

| Bit | Step |
| --- | --- |
| 0–2 | Admission, discovery acquisition, IP layout |
| 3–6 | ROM layout, ROM capture, subsystem check, FirmwareInfo |
| 7–9 | HDP, NBIO, PSP layout |
| 10–12 | Debug message, debug response, scratch layout |
| 13–14 | PCI snapshot, independent PCI verification |
| 15–18 | SOS, Scratch3, Scratch7, complete |

Subsystem completion means the existing ROM identity comparison and access
check passed. A recorded subsystem value alone is not successful validation.

The nested `AtomPreparation` is copied before a failed factory destroys its
object. It retains allocation/binding/provider stage, parse/open/parameter
statuses, actual reviewed-table offset/size/CRC, scratch base, and structural
analyzer result/counts. This describes existing CPU validation and does not
mean ASIC_Init executed. Unattempted parser fields are initialized to invalid;
the attempted mask remains required when interpreting them.

ATOM step bits 0–13 are allocation, binding, state, provider, ROM copy, image,
ATOM, FirmwareInfo, program, parameters, profile, workspace, analysis, complete.

Every dictionary states `HistoricalEvidenceOnly=true` and
`CurrentHardwareAuthority=false`. Serialization failures set
`RecoveryPreparationEvidenceTruncated=true`. A failed top-level serialization
does not replace an earlier dictionary; readers must use its generation and
the truncation flag. `EvidenceComplete` means the requested preparation record
was serialized, not that GPU startup or recovery completed. No caller retains
factory pointers, callbacks, raw leases, or a proof through these records.

## Remaining first-graphics-run evidence

This closes the missing preparation outcome before `Engine::start`. It does
not execute the Engine training/LSDMA/CP/DMCUB preflight batch if preparation
fails, and it does not add new PCI/MMIO samples. A failed low-level callback
can still combine provider unavailability and scope revocation; the record
does not invent a more specific cause.

Later recovery needs its own actual MODE1 outcome, PCI restore/blackout/BME
results, ASIC_Init execution/wait result, attempted retirement checks, and
retained-owner/proof-transfer result. Selected stage results already exist,
but complete post-stop IH/DWB/MCIF/MALL, GART/queue/firmware, and recovery
histories are separate work. The current raw data does not settle dormant
HUBP state, TLB0 interpretation, or physical-console provenance.

`scripts/check-recovery-evidence-native-local.py` compiles the real Reset,
Lifecycle, and ATOM translation units against the installed kernel SDK with
warnings as errors and a 2 KiB frame limit. It records source/dependency hashes
and emitted x86_64 Mach-O objects. It is compilation evidence only; no IOKit
object, native GPU, reset, or Metal workload executes during that check.
