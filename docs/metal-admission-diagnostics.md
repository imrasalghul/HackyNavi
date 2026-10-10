# Metal device admission diagnostics

The real `HNMetalDevice` initializer now reports failures that previously
disappeared when the principal entry point returned `nil`. Each failed admission
emits one bounded `HackyNavi Metal admission:` record to stderr and the macOS
unified log. A saturating atomic counter permits at most 32 records per process;
successful initialization and unsupported ordinary Metal calls do not log.

The record identifies the first failing stage, supplied accelerator port,
whether an NSError exists, numeric native status/IOReturn, and the actual reason.
Stages distinguish bundle identity and each bundled compiler helper, helper
validation, limits ABI, active dyld platform, exact accelerator/provider
relationship, private SPI contract, superclass/port retention, native context
open, capability admission, architecture creation, and identity revalidation.
The helper checks stop on their first error rather than replacing it with a
later helper failure. No environment path or unrelated service fallback is added.

For a limits ABI mismatch the error contains the actual `limits` selector and
expected/framework encodings. For an SPI method mismatch it contains the first
failed selector plus expected/framework/implementation encodings. Each encoding
is capped at 512 bytes with its full observed byte length reported. Protocol
method count mismatches report expected/actual counts; required class methods,
missing protocols/superclass and Objective-C protocol adoption have separate
reasons. The first SPI failure is retained by the existing dispatch-once check
so later discovery calls can report the original cause. An incompatible ABI is
still rejected before opening the native GPU owner.

The line payload is bounded by a 2048-byte stack buffer. Its reason is truncated
to 1720 bytes; the encoding lengths identify clipped values. This is a failure
report, not evidence of successful Metal discovery, GPU execution or an image.
The diagnostic collector can include the unified record using its existing
`HackyNavi` predicate; stderr also travels with a directly invoked test.

Only `userspace/metal/native/HNMetalDevice.mm` is changed in the implementation.
The vertex-buffer native context/profile, exact private method contract,
capability limits, compiler helpers, rendering path and admission decisions
are preserved. Local receipt `build/metal-admission-observability-local-1/report.json`
records the source comparison; root must compile this Objective-C++ source with
the real macOS SDK in the next frontend build before shipping it.
