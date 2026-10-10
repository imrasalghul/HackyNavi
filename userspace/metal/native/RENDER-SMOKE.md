# Target-only offscreen Metal test

`hackynavi-metal-render-smoke` uses public Metal discovery, library/function/pipeline creation,
Managed texture upload, one real triangle draw, ordered blit synchronization and
actual texture readback. It never opens a native Context directly or substitutes
another GPU. Build it on the Mac build host; **do not run it there**.

When the driver and matching graphics frontend are ready on the Navi31 target:

```sh
./hackynavi-metal-render-smoke --run --registry ACCELERATOR_ID \
  --metallib /absolute/path/graphics.metallib > render-report.json
```

`ACCELERATOR_ID` must be the current public Metal registry ID of the actual
`HackyNaviAccelerator`, not its parent owner's ID. The test requires the exact
`HNMetalDevice` class and independently checks the read-only registry chain
through `HackyNaviDevice` to PCI `1002:744c`. It fails if that device is missing;
there is no default-device fallback.

Use the retained real Apple fixture
`userspace/compiler/fixtures/apple-graphics/graphics.metallib`. The tool snapshots
the regular file and verifies its 6,354-byte length and SHA-256
`b99ae2fa65e9cfda2e13c146434fee1f0c0ef0cd2956beeba1cec01750641197` before loading those
same bytes. It requests `hn_triangle_vertex` and `hn_constant_fragment`.

The pass uses a 64×64 RGBA8Unorm Managed render target, one sample, zero sentinel
upload, Load/Store, and three vertices. It requires actual successful command
completion and exactly one scheduled/completed callback. It checks all pixels
outside a narrow sloped-edge band: interior RGBA is `(64,128,191,255)` within one
byte, exterior remains exactly zero. Top/bottom probes reject a vertically
mirrored triangle. Host row padding and outer guards must remain unchanged.

Stdout is a JSON report with the failing phase or actual success, identities,
command status, checked/skipped counts, orientation, guards and actual packed
RGBA SHA-256. Stderr carries diagnostics. Exit 0 means these checks passed; 1
means failure; 2 means invalid arguments. This validates an offscreen draw only,
not display scanout, presentation, general rendering or full Metal conformance.
The actual executable compiled and linked against SDK26.2 with deployment
target macOS15 in `build/metal-graphics-native-2`. SHA-256:
`43e4a39553e1af01a8b1833c2a29ae297988275399d0e8ba28e078400673c14a`.
It has not been run on the target yet.
