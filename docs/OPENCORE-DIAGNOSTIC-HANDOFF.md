# Agent handoff: first HackyNavi diagnostic boot

Result update: the user completed this passive test and supplied logs. Loading,
exact binary UUID and target PCI observation passed; see
[first boot evidence](validation-2026-10-10-passive-boot.md). The frozen package
retains its original pre-test metadata. This procedure does not authorize the
later active-register or engine-startup test stages.

Prepare the user's existing OpenCore USB for a first **passive diagnostic**
HackyNavi test on their Ryzen / RX 7900 XTX macOS 15.8.1 machine. The user has
authorized preparing this test. Their installation and USB already exist;
preserve their working configuration. Do not load anything on `victor@mini`,
which is only the build host. Do not reboot automatically.

Use the frozen package `HackyNavi-diagnostic-dd4f86a48836.zip` in
`C:/Users/Ras/Documents/ChatGPT/HackyNavi/build/releases/`, or the copy supplied
with this prompt. Its extracted root contains `HackyNavi.kext`, evidence,
`SHA256SUMS.txt`, `PACKAGE.json`, `Kernel-Add-entry.plist`, and the collector.
Do not substitute the mutable `build/macos-native` directory or rebuild it.

The executable is an unsigned x86_64 MH_KEXT_BUNDLE, version 0.1.0, identifier
`org.hackynavi.driver.HackyNavi`. Executable SHA-256:

```text
dd4f86a48836ca83667140fadd846e4a6b98f62eefd6be00d199a874991f6d47
```

This binary passed native compile/link and resolved all 343 kernel imports on
the Intel build host running macOS 15.8 (24H22). At packaging time it had not
been loaded; the later passive boot result is linked above. Native diagnostics reported build-folder ownership differences;
these are recorded in the included report. Compilation is not proof of boot
compatibility on the user's target. Its frozen source is included in evidence.

Default behavior is the `HackyNavi` IOResources observer. It reads target PCI
configuration and existing registry VBIOS properties; it does not map GPU MMIO,
claim the GPU, start engines, change display modes, or provide Metal. The
separate `HackyNaviDevice` personality requires `-hackynavi-device` and is
**not part of this first test**. Keep that argument absent from the effective
boot arguments. Do not inject a fake device ID or a ROM just to make this test
appear successful. Existing device spoofing, if present, should be reported.

1. Identify the actual USB and its EFI partition by disk identity, capacity,
   and existing `EFI/OC` content; never guess a drive letter or disk number.
   Back up its full working EFI to a separate location and record the backup.
   Preserve a bootable rollback route. Do not format any disk or alter an
   internal EFI partition.
2. Extract the package and verify its per-file hashes. Verify the executable
   hash again after copying the **whole** `HackyNavi.kext` directory to
   `EFI/OC/Kexts/HackyNavi.kext` on that USB.
3. Inspect the installed OpenCore version and current `config.plist`.
   Add/update exactly one `Kernel -> Add` entry using the supplied plist
   dictionary (it is a fragment, **not a replacement config**):

   | Key | Value |
   | --- | --- |
   | Arch | `x86_64` |
   | BundlePath | `HackyNavi.kext` |
   | Comment | `HackyNavi passive diagnostic dd4f86a48836; no acceleration` |
   | Enabled | Boolean `true` |
   | ExecutablePath | `Contents/MacOS/HackyNavi` |
   | MinKernel | `24.0.0` |
   | MaxKernel | `24.99.99` |
   | PlistPath | `Contents/Info.plist` |

   Preserve existing patches, quirks, kext order, boot arguments, SMBIOS and
   security settings. No new dependencies such as Lilu are needed for this
   bundle. Check current settings for injection compatibility; report any
   concrete blocker rather than broadly disabling security or replacing the
   working EFI. Do not install into `/Library/Extensions` or rebuild system
   kernel collections for this USB-injection test.
4. Validate the resulting plist and use **ocvalidate from the same OpenCore
   release**. Compare the configuration before/after, ensure the paths exist,
   check for duplicate entries, and record validation results. A successful
   configuration check is not proof that the kext will load. OpenCore field
   semantics and validator requirement are documented in the
   [official reference](https://github.com/acidanthera/OpenCorePkg/blob/master/Docs/Configuration.tex).
5. Save a brief receipt with the USB identity, backup path, executable hash,
   configuration diff and exact recovery steps. To undo this change, set this
   entry's `Enabled` to false or restore the saved configuration and kext state.
   Tell the user the prepared USB is ready; let them perform the first boot.
6. After the user boots through that USB, run the supplied collector on the
   **7900 XTX target** (locally or over SSH):

   ```sh
   sh /path/to/extracted-package/collect-macos-diagnostics.sh
   ```

   Return the resulting folder, package ID, `sw_vers`/Darwin build, visible
   desktop behavior, and any panic/hang. The collector is read-only and
   records command failures explicitly. Empty `HackyNaviDevice` output is
   expected. Success for this stage means kext loaded, observer published,
   and target PCI snapshot captured. Missing VBIOS properties are acceptable.
   If the kext does not load, retain the OpenCore log and loader error before
   proposing configuration changes. If boot fails, restore the saved state
   from the other OS/recovery route and retain any panic evidence.

Do not report graphics acceleration, engine startup, or Metal success from
this diagnostic test. Return the evidence to the driver-development session;
the next test stage will use an explicitly identified build and procedure.
