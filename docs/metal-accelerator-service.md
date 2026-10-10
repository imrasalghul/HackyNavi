# Public accelerator service integration

Update after the c39 target test: this genuine subclass now lives in the
separate late-loading `HackyNaviAccelerator.kext`. The early core cannot import
its SystemKC superclass during OpenCore injection. See
[current split and lifetime contract](accelerator-late-loading.md). The
monolithic build records below are historical and did not pass target loading.

The implementation uses `HackyNaviAccelerator : IOAccelerator` as a real child
of its existing `HackyNaviDevice`. There is no second PCI open, engine owner,
GART lifetime or private Apple client implementation. Metal's measured loader
matches the public superclass; see [loader evidence](metal-loader-contract.md).

The existing engine startup calls `publishAccelerator` only after its actual
lifecycle initialization and GPU verification have returned success. Creation,
installation in the owner, attach, start and registration all remain inside
that startup's normal FunctionGroup Operation. Paired retirement drains this
operation before entering the retirement handler, preventing it from missing
a child that is still starting.

The child checks its exact direct provider and its installed identity in the
owner. It publishes that provider's actual 64-bit registry ID as
`HackyNaviOwnerRegistryID`, with `MetalPluginName=HackyNaviMetal` and
`MetalPluginClassName=HNMetalDevice`. It explicitly rejects private user-client
opens. The real frontend must verify the direct provider/ID relationship and
open the existing versioned native Context on that owner. Its Metal registry
ID is the actual child ID, distinct from the native owner's ID.

The parent owns the initial child reference, and the child explicitly retains
the provider. Engine retirement removes the parent's pointer, requests actual
required asynchronous service termination and releases its child reference
before retiring hardware. Actual service-interest notifications withdraw the
discovered device. Native operation admission is already revoked by the
function-group gate; an outstanding user-space port does not revive it. No
Device mutex is held across IOService lifecycle calls. Failed attach/start
unwinds locally; failures after start terminate the service.

Validation on 2026-10-11:

- Independent source review: `build/accelerator-kernel-check/vcn-independent-review.json`.
- Strict compilation with real kernel headers and a 2 KiB frame limit for the
  child, Device and DeviceEngines objects passed.
- `build/macos-accelerator-final` compiled all 94 translation units and resolved
  all 412 imports. The four actual `IOAccelerator` imports resolve to
  `com.apple.iokit.IOGraphicsFamily`, version 599. Its flat system bundle plist
  reports compatible version `1.0.0b1`; the declared `2.4.1` dependency was
  accepted by the no-load dependency diagnostic.
- Parent validation checked 239 source files and all six bundle files.
  Executable SHA-256:
  `12633219109b34fd9440e07bb2662cf462595bc742e18f9ed2aa6ae908bd4065`.
  UUID: `0E7454F6-F2F4-33E6-B126-541F99FC1252`.
  The 9,868 KiB temporary stage was removed.

The no-load diagnostic preserves the expected staging ownership errors
(501:20 versus 0:0); dependency resolution does not establish loadability.
This kext has not been loaded or exercised on the target. HNMetalDevice and
trusted bundle packaging remain in development, so this is not a demonstrated
discoverable Metal GPU or an accelerated display driver. The later live-DMCUB
UCLK integration is outside this particular full build.
