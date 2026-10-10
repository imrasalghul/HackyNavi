# Function-group acquisition evidence

The engine worker now retains two immutable records before lifecycle creation:

- `FunctionGroupCreation`: the actual factory result and last stage (allocation,
  admission, pair lookup, graphics lease, audio lease, audio begin, graphics
  begin, topology, or complete), plus the CPU snapshot taken before factory
  failure cleanup destroys its owners.
- `FunctionGroupAdmission`: the actual `beginOperation` result and issued
  generation, plus the immediately following CPU snapshot. It is published on
  success and failure, including the previous silent waiting-for-power return.

The factory's optional `CreationObservation` output is initialized before any
allocation. Failed `init()` does not call `snapshot`, since some locks may not
exist. A completed initialization uses the existing group-then-lease lock order
and copies state before `shutdown`. There are no new PCI reads. After a normal
Operation is issued, the following snapshot uses only CPU state under that
same lock order; it does not validate or replace the Operation.

Each available function snapshot records the observed PCI identity, attachment,
open/borrowed-open state, PM initialization/registration, power, terminal and
admission-revocation state. `LastPublishedResult` is the lease's last stored
status, which may precede an early `begin` failure. The top-level creation
result is the actual factory return. If no lease was created, its child has
`LeaseSnapshotAvailable=false`; a zero-filled identity is never published as a
successful measurement. Pair-search-local identities are not retained here.

These are latest immutable observations, not an unbounded history of transient
PM wake attempts. Snapshot generation and issued generation are separate:
concurrent PM revocation can change the former after admission. Every record
states `HistoricalEvidenceOnly=true` and `CurrentHardwareAuthority=false`.
Current scope and device checks remain unchanged before hardware access.

Allocation/serialization/publication failure sets the corresponding
`FunctionGroupCreationEvidenceTruncated` or
`FunctionGroupAdmissionEvidenceTruncated` property and preserves the prior
immutable dictionary. Consumers must consider that flag and group instance /
generation instead of interpreting an earlier record as the latest attempt.

The change records the existing PM/open process. It does not force a power
state, bypass an audio owner, issue engine control writes, or introduce a
capture-only test build. Native compilation checks are in
`scripts/check-function-group-evidence-native-local.py`; they do not execute
kernel objects, PM callbacks, hardware, or GPU workloads.
