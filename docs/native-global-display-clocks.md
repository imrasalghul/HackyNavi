# Native cold global display clocks

The Display owner now implements `requestGlobalClockPreset` for DISP and DPP.
It uses the existing DAL controller's one-shot cold transaction, including the
actual DID126 prelude when entering DISP DID127. It is not connected to startup
or an exposed modeset API yet, and no hardware result has been observed.

The caller supplies its real FunctionGroup Operation, required kHz and timeout.
Display holds Display -> Firmware -> GART locks for the whole operation. Its
elapsed deadline starts before locking and wraps all physical reads, mailbox
reads/writes, delays and DMCUB inbox checks. Cold admission excludes every
queue/VM owner, native surface, AUX/DDC owner, link, stream and DTO owner. It
requires actual physical pipe idle, running owned DMCUB, completed private
FAMS/DAL journals and the same generation/inbox mapping and pointers.

Planning uses the owner's actual current DPM inventory, retained ROM-derived
board description and live PLL/reference snapshot. The selected DISP/DPP
ceiling is the minimum of the real table maximum, DC maximum and 1950 MHz.
The final divider-derived frequency and any DID126 intermediate frequency must
fit that ceiling before mutation; checking only floored request MHz would miss
an over-limit programmed value. The caller cannot supply a topology, inventory,
clock reference or prevalidated boolean. The public inventory cache is
invalidated before sending a request while the source is retained privately.

Every phase callback repeats cold admission, captures two identical complete
PLL/DFS/DENTIST snapshots and checks idle/coordination again. DISP requires
WDIVIDER/RDIVIDER/DFS agreement and change-done; DPP requires its WDIVIDER/DFS
agreement and change-done. The portable transaction independently checks raw
fields, exact phase DID, PLL continuity, derived frequency and the interpreted
firmware response. The response and raw journal remain separate observations.
These checks establish observed programming, not a measured frequency counter.

Only complete success publishes a private usable reference with generation,
serial, source-inventory serial, FAMS serial and the final actual observation.
Copied results are diagnostic. Failed operations, normal retirement, reset and
legacy clock requests invalidate usability without erasing retained command
history. Cold journals remain reset-only policies in the existing retirement
path. No zero-MHz withdrawal or owner reconstruction clears them.

`configureDppClock` now requires an Operation and bounded timeout. Before the
existing actual DTO protocol, it verifies the private DPP transaction and takes
another same-generation programming observation. The native scanout surface's
blanked preparation checks both private DISP/DPP references and fresh hardware
readbacks before any pixel/aperture/HUBP write. Legacy minimum replies no longer
publish the scalar values used to authorize these operations. These changes
have no automatic startup callsite.

The first profile only acquires each global clock once per reset epoch. Active
FIFO/DTO transition ordering, full DML/bandwidth/clock selection, early startup
retirement and reset-prohibited callback recovery remain required before wiring
it into a complete mode transaction. GPU rendering and display output are not
established by host protocol tests or kernel compilation.
