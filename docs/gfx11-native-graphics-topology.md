# Native harvested graphics topology

`captureGraphicsTopology` extends the same GART owner's completed color
topology with strict GC discovery metadata and observed harvesting masks. It
has no automatic startup caller and does not render or submit commands.

The actual ROM extraction `.research/rom/discovery-000b5d00.bin` has SHA256
`e52312475450105abdf58683db2016cb5b407b617cb6ba78b4f10830cd37d34b`.
Its identical second ROM copy is at `discovery-00195d00.bin`. The validated
1756-byte binary contains a 132-byte GC1.2 table at offset1344. That table
declares six maximum SEs, two SAs per SE, four RBs per SE and four WGPs per SA.
These are firmware-declared maxima, not an observation of active units.
The parser accepts that verified schema only, checks its ID/version/size after
complete discovery/checksum validation, and bounds all counts before shifts,
division or register selection. A newer schema has an explicit unsupported
result; there is no default device profile.

The real native capture requires the exact normal Operation/Access, GART
creation generation, healthy BothEnabled/firmware ownership and live PCI power,
decode and BME. The prior color capture must belong to this same generation.
A one-second maximum absolute deadline and the scope checks bracket every
MMIO access. Fresh complete PCI/GB_ADDR_CONFIG/NBIO captures precede and follow
the harvesting operation and must exactly match the private prior capture.

The GRBM bank selector is actual GC BASE1 offset `0x2200`. Its initial value is
read twice and required to match before any mutation. Reserved fields and a
nonzero instance index are rejected; SE/SA indices must fit the metadata, and
broadcast forms must have their unused index bits zero. Both a validated
specific-bank value and the usual broadcast value can be preserved. We never
invent `0xe0000000` as the caller's original selector.

Each sweep reads full hardware/user SA and RB disable words. For each active
SA only, it writes the source-derived instance-broadcast plus exact SE/SA
selector, reads it back, reads full hardware/user WGP disable words, and checks
the selector again. Disabled SAs are explicitly marked unobserved and never
accessed. Global words are read again and compared in full. Only a completely
successful stable sweep writes the previously verified original selector and
checks exact readback. This is the last normal operation of the sweep, not a
cleanup attempt after failure. Any failed read/write, changed selector/global
word, revocation or deadline stops immediately without another write.

Two complete raw sweeps must match, including all observed bits and the exact
original selector. Raw input is retained alongside derived facts. Active WGPs
expand to pairs of CU bits. Active SE count comes from nonempty CU masks;
active RB masks combine hardware/user RB masks and the active-SA mask, matching
the pinned Linux computation. Mesa's gfx11 lower CU distribution bound is
`floor(activeCu / (activeSe * maxSaPerSe * 2)) * 2`; it is not the smallest
individual SA mask. No live units or a zero resulting lower bound is rejected.

The native write callback admits only the current phase's private register:
RSMU INDEX for the before/after color check, or GRBM_GFX_INDEX for harvesting.
It checks the already-recorded attempted-write count before accessing hardware.
Selector writes and final restoration attempts/completions have separate
private counters. A failed attempted capture faults and retains GART, clears
firmware admission and preserves its existing publication/allocation holds.
No best-effort restoration, reset, retirement or replay is implied.

The numeric diagnostic copy is not a transferable authority. The future
atomic VM graphics capture must consume the actual private GART record under
the same owner/generation and mutex. Draw tile-steering is a separate explicit
policy: pinned Linux sets it to zero in software. This capture does not
misrepresent that software policy as a measured hardware register.

Source definitions are Linux commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`: `discovery.h`,
`amdgpu_discovery.c:2018–2022`, `gfx_v11_0.c:2010–2107,7310–7398`, and
`gc_11_0_0_offset.h` / `gc_11_0_0_sh_mask.h`. CU aggregation is from Mesa
`35b085c4c067f198ce0b6437d94678e47b9c645f`, `ac_gpu_info.c:1244–1313`.

Host tests use the actual ROM binary for metadata parsing and checksum-repaired
negative schema tests. Separate protocol tests inject failures at every access,
all-ones reads and every single-bit observation change; enumerate all4096 active
SA subsets; and compare CU/RB derivation against independent enumeration. These
are tests of the implementation, not hardware observations. Native compilation
uses actual kernel headers and the 2 KiB frame guard. Exact evidence is recorded
in `build/gfx11-graphics-topology-local`; no GPU or Metal object is executed.
