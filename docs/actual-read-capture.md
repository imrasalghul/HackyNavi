# Partial and successful hardware read evidence

The read-capture adapters preserve the existing ordered read callbacks from
BootDisplay and cold-DMA checks. They add no reads, writes, validity probes,
retries, power changes or admission predicates. Every original callback return
value reaches the original protocol unchanged, including failure. All existing
domain gating and failure short-circuiting therefore stay in the protocol.

The journal bounds follow those protocols: 801 reads for two BootDisplay passes
plus three cold SOS reads; 239 for the two cold-DMA passes and final checks;
1205 for owned BootDisplay capture including its separate programming/clock
recheck and final SOS. Powered-off windows reduce those counts. Overflow records
`Truncated` and loses diagnostic records only; it never changes hardware I/O.

Each contiguous attempted prefix has ordered register byte offsets and callback
returned values. `CallbackSucceededMask` distinguishes a successful callback
from a word returned by a failed callback. `NonAllOnesMask` additionally requires
a value other than `0xffffffff`. Neither means that the value passed semantic
checks, that the power domain is readable now, or that hardware is owned. Failed
callback values are retained as returned data, with both masks clear. Skipped or
unattempted reads have no entry; zero storage is never manufactured evidence.
The original protocol result remains separate from these masks.

Native owners allocate journals with IOMalloc. The serializer copies numbers
into immutable dictionaries/arrays and releases the temporary journal after
the call; properties retain no Operation, live C-array alias, or hardware lease.
No-memory storage omission and property-publication failure are explicit and
do not change the underlying admission result.

Device BootDisplay publishes `RawReadJournal` independently of whether both
passes completed or classification succeeded. The existing power inventory
remains separate. Engine publishes `EngineBootDisplayPreflightReadEvidence`,
`EngineBootDisplayOwnedReadEvidence`, and `EngineColdDmaReadEvidence`, then
retains those immutable dictionaries in their corresponding stage journal.
All describe historical evidence and explicitly grant no current authority.

This does not collect gated windows, continue after an original failed guard,
or move cold-DMA before the actual DMCUB stop. It records the successful/raw
cold-DMA observations that previously disappeared and partial BootDisplay reads
that previously had only the last probe available. Broader firmware, recovery,
queue and stop instrumentation is outside these adapters. No offline test
establishes target bootability, GPU execution, display takeover or Metal output.

`scripts/check-read-capture-local.py --output <fresh-directory>` runs existing
protocol suites plus sequence-fidelity checks at every possible callback
failure/all-ones position, gated-window cases, full successful observations,
semantic failures, bounded overflow and null-storage behavior. Mutants that
discard the callback result or failed-read record must fail. It also compiles
the actual native allocator/serializer and adapters with real kernel headers
and a 2KiB per-frame limit. Native Engine/Device integration is compiled separately.
