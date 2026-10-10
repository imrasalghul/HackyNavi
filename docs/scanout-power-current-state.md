# Scanout current power-state guards

This extends the sourced current-state interpretation in
`display-power-current-state.md` to the scanout consumers. The original
native global-clock validation under `build/native-global-clock-local-2`
remains an immutable earlier snapshot.

Pinned Linux `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0` polls the current
DOMAIN_PGFSM_PWR_STATUS field, not the desired-state bit, in
`dcn32_hubp_pg_control`. Current ON therefore permits complete raw STATUS
`0` or `0x10000000`, with complete CONFIG `0` or `1`. Unknown/reserved bits,
an explicit gate request, intermediate current states and all-ones reads
reject access. The supplied target observation of `0x10000000/0` is an
instance of this encoding, not proof of current idleness or ownership.

`HackyNaviScanoutSurface::power`, `pixel_pipeline`, `hubp_scanout` and
`hubp_aperture` now require these exact ON encodings. Each power guard reads
STATUS, CONFIG, STATUS, CONFIG, STATUS and compares every repeated raw
observation. The first complete guard retains both raw values. Later guards
must match that pair, including the desired-state and FORCEON bits. A change
from one individually valid value to another is rejected; it is not hidden by
masking the fields down to current state.

The native surface retains this pair through its lifetime and clears it at
initialization or verified reset. The pixel and HUBP controllers retain it
through their actual ownership and retirement; an admission failure before
any write can start a new attempt. Aperture programming has one local Access
object, so its pair lives through that bounded programming operation. Native
and portable accesses retain their actual transport failures and ambiguous
write ownership. Pixel register reads and HUBP reads/writes are bracketed by
the same retained power guard; a failed post-access observation cannot report
successful programming or release a touched owner.

`display_bandwidth` also observes unused domains. It accepts exact OFF STATUS
`0x80000000` or `0x90000000` only with CONFIG `0x100`. OFF is classified from
the current field; it never permits a gated HUBP register read. Always-on OTG
checks remain required. The selected domain must be ON. All four raw pairs
remain retained across its guard passes and owned configuration/retirement.
Successful retirement or a failure before any write permits a later complete
new configuration; no touched owner resets a baseline to hide a change.

`display_clock` has no PGFSM/CONFIG register in its portable layout. It already
requires the caller to establish a powered domain. The native
`configureDppClock` path supplies fresh `displayidle` current-state/config
admission under the same FunctionGroup Operation and owner locks before the
DTO protocol. This change does not invent power registers in that API or turn
the portable DTO helper into a power owner.

The four protocol suites exercise all four pipes, both ON status encodings,
both ON configurations, reserved bits, actual observed changes between valid
values, retention after writes, and successful unchanged retirement. Bandwidth
also covers both OFF forms and rejects gated accesses in its test transport.
Existing per-access transport/all-ones/ambiguous-write tests remain active.
The numeric register models are tests only; production uses the existing
actual callbacks. Final strict/UBSan results and real kernel-header objects
are recorded under `build/scanout-power-local-2`. No GPU or native mode was
executed, and startup remains disconnected from the new clock transaction.
