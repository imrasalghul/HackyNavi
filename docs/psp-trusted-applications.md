# Owned PSP13 trusted applications

This slice parses the actual pinned PSP13 TA firmware and transports owned HDCP
and DTM TA sessions through the existing exclusive PSP ring. It does not establish
that inherited HDCP authentication, firmware display actors, or sink traffic have
stopped. There is no native startup activation or inherited HDMI mutation here.

## Sources and actual image

Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0` supplies
[`amdgpu_ucode.h`](https://raw.githubusercontent.com/torvalds/linux/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h),
[`psp_gfx_if.h`](https://raw.githubusercontent.com/torvalds/linux/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h), and
[`amdgpu_psp.c`](https://raw.githubusercontent.com/torvalds/linux/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c).
Relevant definitions are `ta_firmware_header_v2_0`, `enum ta_fw_type`,
`psp_prep_ta_load_cmd_buf`, `psp_ta_load`, `psp_ta_invoke`, `psp_ta_unload`,
`psp_hdcp_initialize`, `psp_dtm_initialize`, and `psp_hw_fini`.

The Linux firmware pin is `afabaf773c4c2e2c841429933a6a084a5af4d14d`.
The unchanged `.research/firmware/amdgpu/psp_13_0_0_ta.bin` is 271,616 bytes,
SHA256 `ac2454a36618d8568e798c6000bf3e19fdabf46d46038b92635adc3cb01bd198`.
The original `LICENSE.amdgpu` and download manifest remain alongside the corpus.
Its v2.0, IP13.0 container contains ASD, HDCP, DTM and RAS. HDCP is absolute
offset209408, length28928, version385876051; DTM is offset238336, length8448,
version301989919. Numeric version fields are preserved without interpretation.

`pspta::parse` supports the observed 36-byte header/256-byte payload layout,
up to eight source-defined unique TA types, nonzero four-byte-aligned entries,
and a complete nonoverlapping payload partition. It requires HDCP and DTM.
Every range is bounded before publication. Output owns metadata, not borrowed
image pointers. Invalid input clears output; overlapping input/output is rejected
without modifying either. The stored CRC is reported metadata only: the pinned
Linux common validation does not define a CRC computation or authenticate it.
Correct family selection and the PSP's actual signed-image admission remain
separate requirements. Mutating opaque payload bytes is intentionally not a
metadata-parser rejection or a claimed firmware authentication test.

## Ring ownership

The existing `pspring::State` now owns exactly two private TA records. The only
accepted kinds are HDCP and DTM. Each kind can load once per State/reset lifetime.
Successful unload reaches `Retired`; it cannot be used to reconstruct a new owner.
Load requires the actual live TMR and distinct allocator-owned firmware/shared
allocations. Staging firmware must be page aligned, uploaded/readback-verified,
and HDP-published by the caller. The parsed image's original file offset need not
be page aligned: copy it to a page-aligned staging offset first. Shared storage
must be exactly16KiB, initialized and published, and must stay exclusive.

The real 1024-byte command encodes LOAD_TA1, UNLOAD_TA2 or INVOKE_CMD3. Firmware
and shared GPU MC addresses are derived from live handles. There is no CPU-pointer
address substitution. Unload/invoke use only the actual PSP response session ID
retained from this owner's load. Zero is a valid representable returned ID; a
separate validity bit distinguishes absence. Simultaneously retained duplicate
session IDs are rejected because unload could not distinguish their owners.
`taInformation` is a diagnostic copy and cannot create session authority.

Before any TA publication, the owner records the attempt and retains all relevant
allocations. Failure in guards, upload, publication, pointer delivery, readback,
fence/response validation or a nonzero response seals the ring and retains the
TA record. Every wait checks the entire864-byte request against private pending
fields, not only its command ID. The persistent-buffer invoke leaves the entire
dynamic scatter/gather list zero. All live bindings are revalidated during later
commands and waits. General firmware staging validation rejects any retained TA
shared/input allocation as an alias.

A successful load response releases the firmware staging hold, as the pinned
Linux load path permits; it retains the shared buffer and returned session.
Confirmed unload releases only that TA's shared hold. Late successful observation
after an ambiguous command may retire that particular job (including unload),
but never reopens a faulted ring. A failed/ambiguous load remains retained; no
blind unload without a trustworthy returned ID is attempted. TMR destruction
and ring stop reject any retained TA before hardware access. This is lifetime
accounting, not memory deallocation: callers must honor it when owning BOs.

The low-level invoke API transports an application command but does not interpret
its shared input/output. PSP transport success is not HDCP/DTM application success.
Future native exposure must implement typed application messages/status checks;
do not expose arbitrary commands as a public driver interface. Shared-buffer
mutation while in flight and all other PSP actors must be excluded by the real
native owner. Before unload, that owner also quiesces every TA consumer.

## Native load/unload ownership

`HackyNaviFirmwareTA.cpp` implements lazy HDCP/DTM loading from the embedded
unchanged TA container. The caller must already hold the real function-group
Operation and matching GART Access. Firmware and GART locks exclude other PSP
users across the entire operation. Loading requires this controller's healthy
FirmwareLoaded state, running idle PSP ring and live TMR, plus both GART hubs.
Neither native startup nor a public user-client selector calls this API yet.

Each kind owns a new page-aligned firmware allocation and distinct exact 16KiB
shared allocation. Actual VRAM writes and readback verification initialize both
before HDP publication and LOAD_TA. The same Operation generation and monotonic
deadline guard every selector, data, HDP publication and posting-read callback.
The native owner keeps its firmware allocation even after successful load, until
acknowledged unload or whole-owner retirement. No caller can supply a session ID.

Post-mutation errors seal the native firmware owner and retain its allocations.
A successful unload checks the portable retained record's actual acknowledgement
before releasing either private allocation. Ordinary owner retirement unloads
each retained owned TA before destroying TMR/ring, after queues and display
consumers retire. An ambiguous TA command prevents that path and leaves reset
recovery responsible for proving physical retirement. GART's callback-scoped
retirement access supplies its already established outer deadline and epoch.
No native opaque invoke API, inherited-session adoption or HDMI unlock is added.

## Exact inherited-HDCP gap

The pinned display module's `hdcp_psp.c` queries/destroys authentication sessions
using an owned `hdcp->auth.id`; DTM topology operations use an owned display index.
These are different from a PSP TA load's session ID. No audited source operation
enumerates or globally disables every inherited authentication/display session.
The Linux early-return cases for an uninitialized or missing TA do not prove
inherited state absent. New TA loading, new TMR setup, or unloading our new TA
cannot manufacture that proof. Consequently the existing HDMI mute/stop modules
remain disconnected until real protection, DDC/sink, DMCUB and transmitter
ownership can be established. No boolean unlock is introduced here.

## Validation scope

`scripts/check-psp-ta-local.py --output <fresh-directory>` compiles the unchanged
pinned PSP ABI as an independent offset oracle; runs actual-image parser and
focused protocol tests under strict compilation and UBSan; runs the existing ring
regressions once; and compiles the two portable and two native production
translation units against real MacKernelSDK headers with a 2KiB frame limit,
optimization and emitted stack-usage records. Reports bind sources, actual
firmware, upstream references, compiler, dependency graphs and resulting files.
The protocol peer is offline test-only fault injection. It is not linked into the
driver and is not firmware/GPU execution, HDCP success, or target validation.

CMake and `scripts/build-host.py` include the TA protocol tests. CMake additionally
registers actual-image checks when `HACKYNAVI_PSP_TA_IMAGE` exists; the host script
passes the pinned local image when present. A clean checkout without the firmware
can run the protocol cases, and its output explicitly says metadata was not tested.
The focused script always requires and verifies the actual pinned firmware.
