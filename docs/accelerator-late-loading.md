# Boot core and late accelerator loading

The c39afcee43f9 target boot failed before HackyNavi ran. OpenCore's actual
2026-10-11 14:38:48 log records, at lines 688–690, that IOGraphicsFamily was
absent from BootKC, the HackyNavi vtable patch failed, and injection returned
Invalid Parameter. The driver therefore produced no observer or device service.
The prior build's successful diagnostic capture did not validate this new link.

The new dependency came from the genuine `HackyNaviAccelerator : IOAccelerator`
class. Its four superclass imports resolve from SystemKernelExtensions.kc on
the measured Intel Mac. Native `kmutil print-diagnostics` sees both BootKC and
SystemKC; success there did not establish OpenCore injection compatibility.
The binaries have the same Mach-O flags and load-command categories, and both
omit a minimum-OS load command. The missing command was not the regression.

The implementation now separates two real kernel bundles:

| Bundle | Load stage | Responsibility |
| --- | --- | --- |
| `HackyNavi.kext` | OpenCore boot injection | PCI ownership, observer, firmware, memory, engines, submission, recovery, native user client |
| `HackyNaviAccelerator.kext` | Late loading/AuxKC after IOGraphicsFamily is available | Genuine IOAccelerator subclass, Metal discovery properties, exact native-owner relationship |

The late bundle must not be added to OpenCore `Kernel/Add`. Compiling or
packaging it does not install it into the target's late-loading path or prove
the target's signing/collection admission. That remains a separate target
deployment step. The core can collect diagnostics without this bundle. Public
Metal discovery requires the actual late bundle, a successful engine startup,
and the actual Metal user-space bundle.

No engine implementation was removed. No IOService is presented as an
IOAccelerator, and no C++ implementation of the Apple superclass was copied
into the boot core. The Metal frontend's actual class and owner checks remain
`HackyNaviAccelerator -> HackyNaviDevice`.

## Lifetime and publication

`HackyNaviDevice` sets private `acceleratorReady_` only after the real lifecycle
startup and its GPU verification succeed in a current FunctionGroup Operation.
`HackyNaviAcceleratorReady` is a registry hint for matching, not authority.
The engine worker requests asynchronous matching only after the startup
Operation leaves scope, avoiding recursive acquisition of the normal gate.

The auxiliary personality matches the real HackyNaviDevice and readiness hint.
Its start method initializes the real IOAccelerator superclass, retains its
actual provider, and prepares owner and plugin properties before publication.
It then calls the provider through the versioned, kernel-only IOService
`callPlatformFunction` contract in `HackyNaviAcceleratorProtocol.hpp`.
No project C++ symbols or object layout are imported across bundles.
The auxiliary bundle therefore does not declare a core library dependency:
the on-disk BootKC used to construct a later collection does not contain the
OpenCore-injected core. The actual provider relationship and protocol version
establish compatibility. A retained live provider also retains its class's
kernel module through the normal IOKit object lifetime.

For publication, the core obtains a fresh normal FunctionGroup Operation,
checks current private readiness, power and revocation state, exact child
class, runtime IOAccelerator ancestry, direct provider and registry identity,
and installs one retained IOService reference. The core calls the child's
asynchronous `registerService` before the Operation ends. It therefore cannot
publish after hardware retirement has drained that Operation unnoticed.
Publication is a commit: a revocation that arrives during registration leaves
the retained child for the queued retirement callback. The core returns
startup success rather than terminating a published child and racing its
start-failure cleanup. Native engine requests already reject the revoked epoch.

Retirement clears private readiness and the matching hint, removes the retained
child, then requests required asynchronous termination. The native submission
gate has already been revoked. A retained userspace service port cannot grant
access to an old engine epoch.

Auxiliary stop sends an exact-pointer withdrawal request. This only releases
the core's CPU/service reference and does not acquire a normal Operation;
otherwise stop during retirement could deadlock the gate being drained. Stop
also releases the retained provider. Unknown protocol versions, extra
parameters, a different owner, a different class or a different child are
rejected. The protocol is not exposed through the user client.

The later [native accelerator-port route](accelerator-native-client-route.md)
adds exact-type user-client forwarding through this real child. The core
binds each forwarded session to its privately recorded publication generation
under a fresh normal Operation; direct native tools retain their owner route.

## Evidence

The subsequent full native build `build/macos-accelerator-split-1` now passes
for both bundles: 110 core units, two late-service units, 412 actual core imports
from BootKC and 289 resolved late-service imports. All 279 archived inputs,
both bundle archives and ten embedded firmware images pass independent checks
in `build/boot-collection-gate-review-2/native-artifact-review.json`. The remote
11,184 KiB stage was removed. Staging ownership and SDK deployment warnings
remain recorded; no bundle was installed or loaded on the build Mac. The
replacement core still needs a target boot, and late-service deployment is
not yet tested.

`build/loader-c39-audit-1/report.json` preserves the initial binary comparison.
`target-supplement.json` binds the subsequently retrieved actual OpenCore log.
`build/accelerator-split-local-1/report.json` records strict compilation of the
four changed kernel translation units against real kernel headers, with a
2 KiB stack-frame limit and the exact dependency hashes. These are static
checks, not successful loading or hardware execution.

OpenCore's provider search and vtable limitation are documented in its actual
[1.0.8 PrelinkedKext.c](https://github.com/acidanthera/OpenCorePkg/blob/1.0.8/Library/OcAppleKernelLib/PrelinkedKext.c#L695).
