# 3fa target result and next graphics-test constraint

The supplied `live-3fa3f6d4723b-20261011-163144-730` capture passes the existing
live-capture analyzer against `build/macos-vertex-display-context-1/HackyNavi.kext`.
The actual loaded UUID is `F2DDA1C3-D4B2-3C4E-AF63-E70C25298DAC`, matching the
issued core. The target reports macOS 15.8.1 build 24H32. Both HackyNavi services
are present and all 12 collector commands returned zero. The 18 supplied files
(438,406 bytes) are retained unchanged under
`build/target-evidence/live-3fa3f6d4723b-20261011-163144-730/capture`, with an
adjacent per-file SHA-256 manifest and analyzer result. No engine test ran.

The new display-context capture completed all 208 reads, including power guards.
All 32 context words were valid in both passes and unchanged. Pipe0 has enabled
VTG/OTG timing and active clocks. Pipes1–3 have disabled timing and functional
HUBP clocks, while their R-clock observations still report on. MALL/SubVP
selections and context0 page-table words are zero. All four HUBPs remain powered,
with blanking and TTU-disable clear.

Relative to 9eda, the only original display-register difference is pipe0's
transient IN_BLANK status bit. Its inherited surface remains linear RGB32,
1920x1080 with 1920-pixel pitch, primary/EARLIEST address `0x8000000000`, VMID0
and TLB0. This describes the firmware scanout buffer, not the monitor's 4K165
capability or a measured accelerated desktop mode.

The timing inventory question is answered. The TLB0 address contract and actual
retirement of the dormant requesters still require source-backed implementation.
Zero context words and stopped timing do not grant allocator ownership. The
independent register/source analysis is
`build/target-3fa-display-review-1/report.json`, SHA-256
`98b66acde2637f6198140ff5ada933c58da109f3c2fe765a2529e107ea5e1ec2`.

The user requires **no more probe-only reboots**. The next issued test must
attempt actual graphics and collect all necessary preflight, startup, Metal,
readback and failure evidence in the same run. Repeating the same context
capture would not resolve the remaining address contract. The complete
[next graphics-test audit](next-graphics-test-audit.md) records the dependencies
and missing observations; fixing the known startup rejection precedes release.

The development runner `scripts/run-macos-graphics-test.sh` serializes public
Metal compute, constant triangle, fragment-buffer and vertex-buffer readback
tests, with exact registry selection, fixture/tool hash checks and evidence
before, between and after tests. It stops escalation after a failed test or lost
engine generation. It has not run on the target and is not a release package.
The expanded collector remains read-only; it does not install or run graphics.

Keep the issued 3fa ZIP and the 9eda rollback byte-identical. No USB contents or
target configuration were changed while analyzing this capture.
