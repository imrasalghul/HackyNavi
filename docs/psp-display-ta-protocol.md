# Typed owned-display PSP protocols

`pspdisplayta` implements real DTM topology V3 add/remove and HDCP1/HDCP2
authentication-session create/query/destroy messages over the allocator-owned
PSP TA shared buffers. The native firmware controller loads and unloads the
actual signed HDCP/DTM images and now binds these typed commands to its retained
Display/GART owners. Display startup does not invoke the typed API yet. It does not authenticate a sink,
enable encryption, or establish ownership of inherited firmware sessions.

## Wire source and supported topology

The ABI is Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`,
[`hdcp_psp.h`](https://raw.githubusercontent.com/torvalds/linux/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/modules/hdcp/hdcp_psp.h)
and its adjacent `hdcp_psp.c`. The unchanged header is 19,243 bytes, SHA256
`684213a47d01468dada6590b1edf94ed325e9a903fc435929832635863aeecb3`.
Tests include that header directly and construct expected full messages using
its actual structs and enum values. Production uses bounded little-endian byte
encoding without importing Linux kernel dependencies.

DTM shared struct size is 80 bytes: command at 0, response ID at 4, application
status at 8, reserved at 12, input at 16, output at 76. HDCP shared struct size is
10,264 bytes: command at 0, status at 4, reserved at 8, input at 12, output at
5,136. Both reside in the actual persistent 16KiB allocations required by the
PSP TA load ABI. Every invocation initializes the complete allocation and sets
the application status to GenericFailure before publication. A PSP transport
acknowledgement alone cannot turn the unchanged sentinel into success.

The topology API currently represents Navi31/DCN3.2 direct DIG HDMI TMDS and
DisplayPort SST with 8b/10b coding. It checks four OTGs and five DDC, stream and
transmitter instances; the caller supplies the actual owned stream's mapping.
`amdgpu_dm_hdcp.c` supplies controller/DDC +1 translation. `link_dpms.c` and
`resource_transmitter_to_phy_idx` in `dc_resource.c` show matching direct
transmitter, link encoder, PHY and DIO output indices for DCN3.2. The DCN3.1
Yellow Carp exception is outside this target. FRL/HPO, DP 128b/132b, MST,
eDP/ASSR, USB4/DPIA and Miracast are not represented by this API. Topology data
alone does not prove live stream ownership; the native adapter must provide it.

## Authority and lifetime

A noncopyable, nonmovable `State` binds both already loaded ring-private TA
records and their live 16KiB shared BOs. Both loads must be acknowledged, idle,
retained and never previously invoked. A prior raw invoke prevents binding,
because its application objects cannot be reconstructed as an empty ledger.
The owner takes both exclusive leases together; public raw invoke and unload
reject a leased record. The typed path obtains TA session IDs and allocation
handles directly from those private records. No caller supplies either ID.

Topology slots and authentication tokens are issued only after acknowledged
application success. Each token contains a private owner, slot and monotonic
serial. Authentication create stores the actual returned HDCP session handle;
zero is valid, `0xffff` is the source-defined invalid sentinel, and simultaneous
duplicate handles are rejected. Subsequent query/destroy uses this stored
handle. Reusing a topology slot or creating another authentication invalidates
older tokens. Tokens from another owner cannot issue commands. The tokens are
kernel misuse guards, not a substitute for caller serialization or access control.

The caller retains `State`, ring, allocator, shared BOs, at least 16KiB scratch
and callbacks under one actual native Operation/GART Access. Scratch is reused
only after the preceding PSP command completes. Every I/O is guarded before
and after the callback, and the guard is checked again after validating the
full shared response. No callback may mutate the bindings or allocator. All
parameter/output aliases with owner, ring, allocator and scratch are rejected
before any output reset, token mutation or I/O. Bind also excludes Io aliasing.

Before publication, the ledger records that topology or authentication may
exist. Any ambiguous upload, HDP publication, invoke, wait, response, guard loss
or nonzero application status faults the owner and ring and retains both TAs.
The actual native fault callback must seal its firmware owner and preserve the
underlying storage. A later successful PSP observation cannot revive this
typed owner. Recovery requires independently proven reset retirement.

Successful application status is checked separately from PSP response status.
Responses must preserve command/input/reserved bytes outside each command's
documented output. The DTM response ID is reported without an invented equality
rule because the pinned caller does not define one. This strict response policy
is not yet firmware-tested. HDCP1 create returns the real primary An/AKSV/AInfo;
queries expose source-defined protection/content values only after validation.
Those values describe this owned authentication session, never inherited ones.

Topology removal requires its owned authentication to have been destroyed.
Retirement releases the two leases only after all owned session destroys and
topology removals have acknowledged success. The native controller may then
unload the actual TAs and finally retire TMR/ring in its existing dependency order.

## Native owner integration

`HackyNaviFirmwareController` retains the typed `State`, private topology and
authentication tokens, actual Display references and its existing persistent
PSP scratch allocation. A normal FunctionGroup Operation and GART Access are
required before inspecting this ledger or clearing output storage. The native
entry point locks Display, Firmware and GART in that order. Each real MMIO,
VRAM transfer, HDP flush and response read is guarded by that same Operation
and one measured deadline, including time spent acquiring the locks.

Native topology is currently restricted to a driver-owned running HDMI TMDS
stream. It checks the actual Display lease, live DMCUB inbox, enabled DIO link,
running stream, selected OTG and ROM-derived route. DDC, stream encoder and PHY
IDs are derived from those retained objects. External numeric IDs or copied
diagnostics cannot register an inherited stream. Both real signed TAs are
loaded lazily into distinct allocator-owned staging/shared buffers; no opaque
userspace invoke selector exists.

The Engine retirement path calls typed cleanup before stopping Display
consumers. Its exact issued RetirementScope must be installed in GART Access;
`retirementAccessReady(scope)` checks pointer identity, owner thread, binding
and current scope validity. Cleanup can only destroy saved authentication
tokens, remove saved topology, release the typed leases and unload the actual
TA IDs. It cannot load new TAs or register a new display. A completely unused
owner is a no-op. An ambiguous application or PSP response faults Firmware and
preserves TA storage and Display references for real reset recovery. Retained
Display references are dropped only after releasing Firmware/GART locks.

Display reset release also requires the exact RetirementScope and the real
provider-bound reset proof. A memory-client reset does not prove downstream
display retirement: an owned scanout, physical link, stream, DTO, power domain,
AUX pin or DDC controller causes rejection before any journal is cleared.
The DMCUB-only reset path remains available. This prevents reset recovery from
discarding physical-display ownership it has not retired.

These native paths compile against real x86_64 macOS kernel headers; no native
IOKit owner has been instantiated or invoked on the GPU for this feature.
The remaining owned modeset/startup path must call the topology API after
establishing a real running stream. DP integration awaits its native stream
owner; the portable topology encoding does not provide that ownership.

## Remaining display work

This implements ownership and transport for the listed commands, not the HDCP
authentication exchange over DDC/AUX, repeater validation, locality checking or
encryption enable. Those require the remaining HDCP protocol state machine and
owned sink transport. HDCP is not required for ordinary unprotected desktop
rendering, but inherited firmware protection and transport actors still need
independent retirement before taking over an existing display.

The source enum contains `HDCP_DESTROY_ALL_SESSIONS` (17). The pinned caller does
not invoke it or document an inherited/global scope. Its name is insufficient
proof that a new TA can clear every inherited protection actor; no such operation
or authority is fabricated. The inherited HDMI takeover prerequisites remain.

## Reproducible validation

Run `scripts/check-psp-display-ta-local.py --output <fresh-directory>` on the
Windows development host. It runs typed protocol tests plus existing actual-image
TA and ring regressions with strict warnings and UBSan. It compiles the three
portable production units against real MacKernelSDK headers for x86_64 macOS15,
with emitted stack usage and a 2KiB frame limit. Reports bind compiler, sources,
upstream files, dependencies before/after compilation and generated artifacts.

Run `scripts/check-psp-display-ta-native-local.py --output <fresh-directory>`
for the four actual native Firmware, FirmwareTA, GART and Display units. The
script freezes all real kernel-header dependency closures before compilation,
checks that each output is a real x86_64 Mach-O object, and enforces the same
2KiB frame bound. This does not link a kext or execute IOKit/GPU operations.

The protocol peer exists only under `tests/` and is explicitly offline fault
injection. Passing tests proves byte encoding and owner behavior for those
cases, not PSP firmware acceptance, hardware HDCP, display takeover or acceleration.
