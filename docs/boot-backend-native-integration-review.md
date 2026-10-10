# Native inherited-backend observation integration

Independent review of `build/macos-boot-backend-integration-2` found no concrete
defect in the new observation callsite, failure translation, or publication.
The reproducible artifact verification is in
`build/boot-backend-native-vcn-review-2/review.json`.

`ObserveBootBackend` is appended to the Engine stage enum. The call executes
once under the original normal FunctionGroup operation, after the existing
inherited DMCUB stop and before the existing HUBP capture. It adds no register
write or firmware command. The bounded capture retains its PCI device, opener,
and map only during the synchronous access scope. Its returned OSObject owns
raw evidence, with no operation, device, mapping, reservation, or stop token.
The Engine still retains its pre-existing hardware ownership after an ambiguous
startup failure; the observation object does not release or replace it.

Stable unsupported routes and formats return `kIOReturnUnsupported` and allow
the independent offscreen startup path to continue. A changed raw sample returns
Busy; a failed read returns IOError; unavailable/all-ones or denied access returns
Offline. Native deadline, scope, PCI, mapping, and sequence failures override the
classifier status and clear validity. An impossible successful classifier with
an incomplete one-shot read ticket is InternalError. Those failures stop startup.
`Unsettled` is also an unsupported classifier state, not a stop/idle proof; any
future display transition still needs its own fresh admission and ownership.

IORegistry publishes `ObservationOnly`, classifier-attempted, result, access
failure, read counts and last-access evidence. Route fields appear only when the
capture is valid. The surrounding lifecycle snapshot is historical evidence.
Allocation failure publishes NoMemory and invalid evidence rather than a route.
Failure to construct the complete nested dictionary does not attach it.

The actual artifact contains 109 translation units, 414 resolved kernel imports,
and the current precise error mapping. It was built and inspected without loading
the kext or exercising hardware. The native build verifies the Engine observation
size assertion of at most 1024 bytes. The earlier strict, real-header 2048-byte
frame check binds the unchanged Engine, Device, Lifecycle and portable-capture
sources; its native capture source predates the later error-mapping-only change.
Do not label that earlier object as a frame measurement of the later source.
The complete build has no frame-warning flag. Existing SDK deployment and staged
bundle ownership diagnostics remain recorded in the build report.

This is a single HDMI-TMDS observation profile. It does not establish downstream
stop, firmware exclusion, sink state, DIO/PHY ownership, clock authority, or
support for the target monitor's 4K165 FRL/DP-DSC modes.
