# DAL memory maximum and DCF deep-sleep floor

The portable DAL owner now implements two independent prerequisites for the
cold clock transaction. This is command ownership and fault handling; no
target firmware operation or measured clock is established by the host tests.

`requestMemoryMaximum` sends `0x0a` with `(2 << 16) | MHz` for PPCLK_UCLK.
`requestDcfDeepSleepFloor` sends `0x0d` with MHz directly. The native caller must
round its calculated DCF deep-sleep requirement upward from kHz, validate the
real DPM/DC bounds, and retain cold pipe, firmware/FAMS, scope and deadline
ownership. The portable API does not accept a caller-supplied clock selector.
The DCF message has a 32-bit parameter; the initial profile explicitly bounds
its input to nonzero 16-bit MHz, matching the existing DPM frequency domain.

Each command can be attempted only once until independently verified reset.
Separate private journals record the requested MHz immediately before the
message write, then distinguish the response ACK from a successful argument
read. A write callback returning failure may already have delivered the
command. The raw argument is retained without interpreting it as a frequency
sample or fulfillment; zero is allowed. Neither command has a sourced
fulfillment bit. Even matching UCLK minimum and maximum still require actual
metrics and the separate retained policy before fixed-clock authorization.

A maximum below this owner's fulfilled UCLK minimum is rejected before I/O.
Subsequent UCLK minimum requests above the owned maximum are also rejected.
Repeated attempts, including a different value, do not send another command.
Transport failure seals the mailbox owner and preserves all existing journals;
there is no retry after an ambiguous operation. One shared response-read budget
still bounds each operation, including waiting for the previous response.

Both journals participate in `policyMayBeActive`. Normal release returns
`ResetRequired` without partially releasing any existing display minimum.
Only `clearAfterVerifiedReset` clears the records. This does not invent a
previous foreign limit, assume zero is withdrawal, or promise a firmware
default after reset. Native bridge and calculation integration are separate.

The wire contracts and callers are from Linux commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:

- `dalsmc.h`: messages `SetHardMaxByFreq=0x0a` and
  `SetMinDeepSleepDcfclk=0x0d`; no maximum or deep-sleep fulfillment bit.
- `dcn30_clk_mgr_smu_msg.c:230`: actual PPCLK/frequency packing for maximum.
- `dcn30_clk_mgr_smu_msg.c:296`: direct MHz parameter for the deep-sleep floor.
- `dcn32_clk_mgr.c:701`: independent deep-sleep calculation and ceiling to MHz.
- `dcn32_clk_mgr.c:1032`: actual PPCLK_UCLK hard-maximum caller.

`build/display-clock-prerequisites-local/report.json` records strict C++11 and
UBSan runs, each with 13,443 checks and zero failures. The existing DAL
protocol harness checks the new exact wire commands, every failed read/write
boundary (including delivered writes that report failure), response errors,
bounded timeouts, reset clearing, interlocks and independent journal retention.
These are host protocol tests, not GPU execution. The source freeze additionally
binds the unchanged pinned source files and prior upstream comparison report.
