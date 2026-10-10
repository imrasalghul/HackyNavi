# Corrected core loaded on the Navi 31 target

The supplied `live-9eda6a40f88c-20261011-153308-790` capture confirms that the
corrected early-core binary loads on macOS 15.8.1 / 24H32. The loaded-kext list
reports UUID `71CBC771-1C22-370B-B1E9-8D1FB1C0E211`, matching the issued
`9eda6a40f88c2a2c42f33dc2e6ef6c2593d070865f64927c83b12a46860b34de`
executable. Both HackyNavi and HackyNaviDevice are registered, matched and
active. This is actual target evidence that the c39 early-prelink failure is
resolved, beyond the prior native link and BootKC import checks.

All twelve collector commands report exit zero. The diagnostic analyzer passes
its live-capture checks without errors. It was deliberately run with
`-hackynavi-device` and `-radvesa`, without `-hackynavi-engines`.
EngineBringupRequested, EngineActive, EngineStartupVerified and
AccelerationAvailable are false. This boot does not test engine activation,
late accelerator loading, Metal discovery, rendering or display takeover.
The preparation receipt's false runtime-test fields describe preparation;
the new loaded UUID and live registry observations are the runtime evidence.

## Captured hardware

- Target PCI identity 1002:744c, revision c8, subsystem 148c:2422 matches.
- ROM: 57,856 live bytes read and bound to the exact PCI/subsystem identity;
  ATOM firmware info and VRAM-usage parsing succeed.
- Discovery: 1,756-byte binary, 61 published IP records, validated layout.
- VRAM: 25,753,026,560 bytes, with a 268,435,456-byte CPU BAR0 window.
  Reservations remain unestablished.
- Display register acquisition completed and matched its read passes, but
  classification reports `display address translation is not supported`.
  CompleteClassification and ReservationUseAllowed remain false.
- VCN0 power status reads all ones; this does not prove a powered-off engine.
  First-attach media deferral has a separate existing contract and must not
  treat this diagnostic as DMA-idle authority.

## First-engine readiness changes

The new display evidence supersedes the earlier readiness audit's statement
that no deterministic display-classification rejection was observed. The
global AGP bottom/top values are both zero; the tested 9eda `framebufferMatches`
required bottom greater than top. The active pipe also has TLB_CONTROL0 and
other values outside the existing decoder's narrow assumptions. Its raw
primary/earliest address is 0x8000000000, while the raw in-use high word is zero;
this discrepancy cannot be discarded without source-supported semantics.

Other raw pipe0 fields requiring interpretation include TilingConfig0x80,
FlipControl0x04100000, FlipControl2=0x40, CursorControl0x01000000 and
RequestSize0x055d0300. The viewport fields decode to 1920x1080 with pitch1920.
They describe this captured boot surface, not the monitor's maximum resolution
or the Windows 4K165 link. No bit is ignored or programmed on the strength of
this log alone. A source-backed decoder/reservation correction must precede
an engine test; otherwise that test is expected to fail the current preflight.

Subsequent source review corrected four narrow assumptions: a disjoint AGP
window of [0,16MiB) is allowed; linear 2D surfaces are allowed; the documented
flip-delay field is accepted; and DCN3.2 primary/earliest comparison replaces
an unconditional ordinary in-use comparison. These corrections still reject
the actual capture's unexplained TLB0 and unclassified remaining pipes.
The new `3fa3f6d4723b` package adds read-only timing/HUBP/VM observations to
investigate those conditions. See [address audit](boot-display-address-audit.md)
and [capture scope](boot-display-context-capture.md). It does not enable engines.

## Retained evidence

All 18 supplied files are copied unchanged under
`build/target-evidence/live-9eda6a40f88c-20261011-153308-790/capture`.
Sibling `manifest.json` binds their lengths/hashes; `analysis.json` is the
analyzer result; `summary.json` contains keyed raw register observations and
has SHA-256 `c4b49e019523690dd64ce0219fc18742d4a01a7f866f82845d0644ac4149b9ec`.
The separately supplied c39 OpenCore logs remain historical failure evidence.
This analysis makes no change to the USB, boot arguments or target GPU.
