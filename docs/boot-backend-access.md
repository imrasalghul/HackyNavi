# Native inherited-backend access

`HackyNaviBootBackendCapture::observe` is a synchronous, read-only native bridge
to the existing 117-register `bootbackend::capture`. It requires the actual
FunctionGroup normal Operation and actual uncached BAR5 map. The Engine now calls
it after actual inherited DMCUB stop and publishes historical evidence through
Device engine diagnostics. No target capture has run through this bridge.

## DCN 3.2 control-register access

The pinned Linux revision is `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.
These are specific control-register access rules for that generation, not a
claim that every DCN block is always powered or that pixel clocks are running.

| Capture bank | Reads per pass | Source basis and boundary |
| --- | ---: | --- |
| Link | 15 | `dcn10_is_dig_enabled` reads DIG backend enable to discover whether a link is enabled; `dcn31_link_encoder_set_dio_phy_mux`, used by DCN32, performs register RMW on DIO_LINK mux before choosing an encoder. DIG backend controls are independent of whether the transmitter is emitting symbols. No PHY register or transmitter command is accessed. |
| Frontend | 30 | `enc32_reset_fifo` reads DIG_FE_CNTL and RMWs DIG_FIFO_CTRL0 even when `DIG_SYMCLK_FE_ON` is zero. It skips the reset-completion wait in that case. `enc32_stream_encoder_hdmi_set_stream_attribute` uses HDMI_CONTROL, VBI packet controls and HDMI_GC directly. `enc32_stream_encoder_dp_unblank` reads/polls disabled DP stream control before enabling its FIFO. Thus clock-on is needed for specific completion protocols, not to query the control registers. |
| Timing/ODM | 48 | DCN32 installs `optc31_read_otg_state` and `optc31_read_reg_state`. The latter directly reads every sampled OTG/ODM register. `dcn10_log_hw_state` reads all timing generators and OPP blank state before filtering disabled OTGs. `optc1_enable_optc_clock` separately controls and polls OTG/OPTC pixel clocks. These reads do not touch ODM pixel memory. |
| Combiner | 24 | DCN32 installs `mpc1_read_mpcc_state` and `mpc1_get_mpc_out_mux`; the former reads selector/status state before filtering disconnected MPCCs. `mpc1_remove_mpcc` clears the update-lock selector along with top/bottom/OPP selectors, including BIOS-leftover disconnect. No DPP or LUT RAM window is sampled. |

`dcn32_hubp_pg_control` manages domains 0–3, and `dcn32_dsc_pg_control`
manages domains 16–19. `dcn32_enable_power_gating_plane` names those same HUBP
and DSC domains. The new capture never accesses their HUBP/DPP/DSC switched
windows. Requiring an unrelated HUBP PGFSM to permit an OTG or DIO control read
would conflate independent hardware blocks and unnecessarily prevent observing
disabled pipes. Absence from a power-gating function alone is not the evidence;
the generation's actual disabled-instance read paths above establish the scope.

The generic `dm_read_reg_func` does invoke the IPS exit wrapper. The exact
generation matters: `dm_get_default_ips_mode` explicitly identifies hardware
older than DCN 3.5 as having no IPS. `dc_exit_ips_for_hw_access_internal` only
performs that operation when `caps.ips_support` is true. The DCN32 resource
constructor does not advertise IPS. This bridge must not be widened to later
DCN generations without their real wake/acknowledgment protocol.

DCN32 still has memory low-power and firmware policies. For example,
`dcn32_apply_idle_power_optimizations` sends CAB/MALL commands; the OPTC default
can power down unassigned pixel memories. These facts do not turn a control-bank
observation into CAB, DRAM, pixel-memory, clock, or firmware ownership. No RAM
aperture or autonomous firmware window is read by this capture.

## Actual native checks

`Operation` admission runs the FunctionGroup's fresh paired PCI identity/D0 and
topology checks. Its subsequent `valid`/`matchesGraphics` checks are lexical
thread, generation, lease, PM/removal/revocation and lifetime checks. They do not
reread PMCSR. This bridge therefore also calls the existing `nativepci::ready`
before and after **every MMIO load**. That uses raw bounded PCI capability
traversal, real PMCSR D0, exact Navi31 identity/revision, enabled memory decode,
and unchanged identity/config readbacks. It performs no PCI writes and requires
no particular BME value.

The bridge retains the actual PCI device, open owner and map for the synchronous
call. It obtains the actual assigned BAR5 descriptor through
`nativepci::AssignedMemory`, which reads the retained assigned-resource property;
it does not use the IOPCIFamily helper that can change tunnel L1 state. The map's
physical start, full length, uncached option and virtual address must match. The
32-bit BAR5 value is freshly rechecked around each load. If BAR4 describes a
64-bit memory BAR, BAR5 is its high half and admission fails. Zero, I/O, unknown
BAR5 types, moved mappings and inaccessible config also fail.

`ReadTicket`, used by the actual callbacks, admits exactly the core's two-pass
bank/instance/register order. The already validated discovery traversal is the
sole source of register offsets. Each offset is bounded against the retained
map and armed once; the transport must consume precisely that pending address
and read counter. The corresponding post-read permission closes the ticket.
Out-of-order, duplicated, unpaired, replayed or revoked callbacks fail closed.
There is no public generic MMIO callback and no caller-provided permission flag.

The elapsed deadline uses monotonic uptime ticks, converted as a difference to
nanoseconds. It starts before map validation and covers all parsing, PCI
checks and reads, with a maximum one-second requested budget. Backward time,
expiry, Operation revocation, D0/config loss or mapping mismatch stops access
without a retry. The object stores the latest actual MMIO attempt/value and
whether its load occurred, separately from the core's successful-transport
flag, so a post-read D0 loss does not erase the fact that a load occurred.

The raw Capture and Report live in the returned heap object; no doubled capture
or register array is placed on the kernel stack. The bridge releases its map,
PCI and owner references before returning. The object retains evidence, not a
live Operation or permission to use it later. Any native failure clears the
capture's `valid` bit. Allocation failure returns null; a non-null object may
still contain a failed or partial observation.

## Callsite and ownership boundary

The integrated **observation-only** call uses the already admitted Engine
Operation, actual discovery/ROM and completed inherited DMCUB stop. It
reports an unsupported backend before any terminal display action. It must
not be confused with the existing diagnostic Device's cached PM flags, which
are not this native guard. Unsupported signal/profile/layout returns remain
`kIOReturnUnsupported`; changing hardware returns Busy, transport failure IOError,
device/access loss Offline, deadline Timeout and ticket-sequence faults
InternalError. Startup continues only for success or an unsupported profile.
No observation is used as a teardown or clock-transition token.

The current native compile/link evidence is
`build/macos-boot-backend-integration-2`, independently checked in
`build/boot-backend-native-parent-2`. The original local native object predates
the explicit IOReturn mapping; its callback/access implementation is unchanged.
The full native build keeps the existing 1 KiB Observation bound. Separate
local real-kernel-header compiles enforce the 2 KiB frame limit on the affected
owners; `build/boot-backend-native-mapping-local-2` covers the final capture
failure-mapping edit. No target execution occurred.

The normal Operation serializes this driver's operations and PM teardown, but
does not exclude SMM or autonomous firmware. The caller must also avoid direct
external register/reset writers. Two equal passes detect observed changes;
they cannot freeze a display. A future terminal owner must use a fresh capture
after actual inherited DMCUB stop/readback and retain its own control journal,
audio/SCDC/transmitter ownership and raw revalidation through the transition.
This bridge performs none of those writes and never returns a shutdown token.

The initial classifier remains HDMI-TMDS only. A 4K165 HDMI FRL or DP/DSC route
can be observed only as a rejection at the supported boundary; no assumption
about the user's current wire protocol is made. Read-only source/tests/native
compilation do not establish target hardware behavior.
