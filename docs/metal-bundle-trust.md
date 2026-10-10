# Measured Metal bundle lookup and trust policy

The measured loader has a feasible filesystem-development path for a custom
`HackyNaviMetal.bundle`. Its trust predicate explicitly returns success when
the owner's active policy permits `CSR_ALLOW_UNRESTRICTED_FS`. This establishes
the path/trust gate only: loading the executable through NSBundle/dyld and
constructing the real device are still untested. No installation, policy change,
trust-function invocation, Metal object creation or GPU work was performed.

These measurements are from the Intel mini running macOS **15.8, build 24H22**,
not a verification of the user's reported 15.8.1 build. The sandbox image is
`/usr/lib/system/libsystem_sandbox.dylib`, UUID
`4f88fd83-3d9b-30c6-aac7-f9d3d2ace5b9`. All offsets below are image-relative.

## Exact lookup

`gpu_bundle_find_trusted` at `0x11ff` is 439 bytes. Its verified two-element
path array searches in this order:

1. `/Library/GPUBundles`
2. `/System/Library/Extensions`

It formats `%s/%s.bundle`, using the registry's `MetalPluginName` as the second
component. Therefore the packaging name is `HackyNaviMetal`, with the candidate
`/Library/GPUBundles/HackyNaviMetal.bundle`. Supplying a filename already ending
in `.bundle` adds the suffix twice. `/Library/Extensions` and a build directory
are not search roots in this measured function.

For each candidate it requires a successful `stat` and directory type, then
calls `gpu_bundle_is_path_trusted`. A zero trust result selects the path; `-1`
aborts with failure; another result continues searching. Missing candidates
continue, while other `stat` errors abort. The output buffer must accommodate
the complete selected path. These are necessary lookup conditions, not proof
that a selected executable will load.

## Actual trust predicate

The complete 46-byte function at `0x13c8` has this control flow:

```c
result = rootless_check_trusted_class(path, "KernelExtensionManagement");
if (result == 1)
    result = !_dyld_shared_cache_contains_path(path);
return result;
```

Both callee identities and the literal class string were resolved from actual
code references. The class wrapper at `0x13b6` passes the path, descriptor `-1`
and class to `rootless_check_trusted_internal` at `0x87b`.

### Owner-selected filesystem development policy

The internal function loads the immediate `2` at `0x898` and calls the actual
`csr_check` through its resolved stub at `0x89d`. It clears the return accumulator,
tests the call result, and on zero jumps directly from `0x8a7` to the success
return at `0x9da`. The class, authenticated-volume and kernel policy checks are
not entered on that branch.

Apple's pinned XNU `xnu-11417.140.69`, commit
`43a90889846e00bfb5cf1d255cdc0a701a1e05a4`, names value `2` as
`CSR_ALLOW_UNRESTRICTED_FS` (bit 1). The x86 implementation returns zero when
the requested mask is allowed by the active configuration. This is distinct
from `CSR_ALLOW_UNTRUSTED_KEXTS` (bit 0). Merely permitting development kexts
does not establish this filesystem permission. See the pinned
[constant](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/csr.h#L43)
and [x86 check](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_csr.c#L303).

Thus, **if the owner has selected an active filesystem-development policy that
makes `csr_check(CSR_ALLOW_UNRESTRICTED_FS)` return zero**, this measured loader
accepts the trust predicate for an existing candidate directory. No loader
interposition, forged certificate identity or replacement trust implementation
is involved. This investigation did not query or change the owner's target
policy. It does not prescribe disabling unrelated policy bits.

### Normal protected-class path

When the early policy check does not allow the filesystem permission, the
internal function checks the actual authenticated-volume helper. Otherwise its
path branch delegates to `__mac_syscall("Sandbox", 2, ...)` with the measured
operation `file-write-data` and the `KernelExtensionManagement` class. The
private kernel policy decision is not reimplemented here.

The actual mini's `/System/Library/Sandbox/rootless.conf` assigns that class to
`/Library/GPUBundles`. The directory is root:wheel, mode 0755, and has the actual
`com.apple.rootless` value `KernelExtensionManagement`. These directory facts
do not prove a newly copied child bundle satisfies the normal-policy check.

## Staging signatures are a separate boundary

Apple-published `kext_tools` commit
`3343f4b73df0084403d4fd96006f86b495c11149` has a GPU-companion staging path that
validates signatures before placing bundles in `/Library/GPUBundles`. Its
requirement accepts Apple signing or a specific NVIDIA kext certificate/team,
and uses strict static-code validation. Its daemon entitlement includes the
`KernelExtensionManagement` storage class. See
[staging.m](https://github.com/apple-oss-distributions/kext_tools/blob/3343f4b73df0084403d4fd96006f86b495c11149/staging.m#L21)
and [daemon entitlements](https://github.com/apple-oss-distributions/kext_tools/blob/3343f4b73df0084403d4fd96006f86b495c11149/kextd-entitlements.plist).

That published stager is not the measured loader predicate, and its equivalence
to the installed 15.8 staging service has not been established. Its certificate
rule does not govern the explicitly measured early filesystem-development
branch. Conversely, `GPUCompanionBundles`, ordinary Developer ID signing or
ad-hoc signing alone is not evidence of acceptance by that published stager.

## Remaining executable and frontend checks

The existing Metal-loader capture still requires an actual `_MTLDevice`
subclass and real initialization/association behavior. This follow-up confirmed
that the installed class and metaclass are exported through `dlsym`, with exact
pointer agreement against Objective-C runtime metadata. The actual SDK 26.2
Metal.tbd also lists `_MTLDevice` under its x86_64 macOS exports. The captured
72,269-byte `MTLDevice.h` matches the earlier native runtime SDK hash.

None of this proves NSBundle/dyld acceptance, dependent-library/helper packaging,
code-signing/library-validation behavior in an application, the 15.8.1 target's
equivalence, or hardware operation. Those are subsequent tests of the genuine
bundle and backend. A build-time or path-trust success must not be reported as
system device discovery or Metal acceleration.

## Reproducible evidence

`build/metal-bundle-trust/` contains stdin-only capture inputs, execution results,
all 1,307 selected instruction bytes, literal-byte-checked disassembly, exact
path/config metadata, SDK files and pinned Apple source/provenance. Two failed
read attempts remain recorded: an initially unhandled internal tail target and
the Xcode Python build lacking `os.getxattr`; the successful successor reads
use bounded exact-function capture and the native read-only `getxattr` API.

`scripts/validate-metal-bundle-trust-evidence.py` checks hashes, instruction
coverage, path-array order, actual callee/string references, the CSR early-return
instructions, SDK export evidence and pinned source files. Its output is
`build/metal-bundle-trust/validation.json`. No remote files or processes remain.
