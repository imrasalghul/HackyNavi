// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DP_AUX_HPP
#define HACKYNAVI_DP_AUX_HPP
#include "hackynavi/discovery.hpp"

namespace hackynavi { namespace dpaux {
constexpr size_t kEngineCount = 5, kMaxPayload = 16;
enum class Register : size_t { Control, SwControl, Arbitration, Interrupt,
    Status, Data, TxControl, RxControl0, RxControl1, GpioMask, GpioAux, Count };
constexpr size_t kRegisterCount = static_cast<size_t>(Register::Count);
struct Layout {
    bool valid;
    uint64_t barBytes;
    uint32_t registers[kEngineCount][kRegisterCount]; // BAR byte offsets
};
enum class Status {
    Ok, InvalidArgument, DiscoveryRejected, MissingDmu, UnsupportedDmu,
    OutsideBar, Closed, Busy, Faulted, TransportError, DeviceUnavailable,
    OwnershipLost, ResetTimeout, CompletionTimeout, HpdDisconnected,
    ReplyTimeout, InvalidReply, Nack, Deferred, ShortReply, WritePending,
    VerifyFailed, RetryExhausted
};
struct Io {
    void* context;
    // Ordered uncached native32 MMIO, serialized against every other DCN,
    // DDC GPIO and firmware AUX user. false means the operation may have run.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t&);
    bool (*write32)(void*, uint32_t byteOffset, uint32_t);
    void (*delayUs)(void*, uint32_t microseconds);
};
enum class Operation : uint8_t { NativeWrite = 0x80, NativeRead = 0x90,
    I2cWrite = 0x00, I2cRead = 0x10, I2cWriteStatus = 0x20 };
struct Request {
    Operation operation;
    uint32_t address; // DPCD 20-bit or I2C seven-bit slave address
    uint8_t length;   // 1..16 native; 0..16 I2C; status must be zero
    bool mot;        // I2C middle-of-transaction, never valid for native
    const uint8_t* writeData; // required only for nonempty writes
};
struct Result {
    uint32_t rawStatus, lastRegister, reads, writes, polls, attempts;
    uint8_t reply; // high nibble of the actual wire status; 0xff if absent
    uint8_t length, data[kMaxPayload];
    bool submitted, completed; // hardware GO attempted, then DONE observed
};
// Discovery is fully parsed, exact unharvested DMU271 v3.2.0, die0/instance0,
// 32-bit bases only. No MMIO. Every register for all five engines is bounded.
Status buildLayout(const uint8_t*, size_t, uint64_t barBytes, Layout&);

class Engine {
public:
    Engine();
    // The caller owns the physical connector, serializes all display GPIO
    // accesses (GpioAux is shared), and excludes reset, removal and power changes
    // until close succeeds. D0, DCN reference clock/power, AUX PHY power and the
    // BIOS board-specific connector routing must already be established.
    // engine0..4 maps to DDC1..5; hpd0..4 is the ATOM-resolved HPD source, not
    // necessarily the engine index. This implements direct physical AUX only,
    // not USB4/DPIA tunnelling or DMCUB-owned AUX. It never steals DMCU ownership.
    // Sets GPIO hardware/AUX mode, AUX reset+ownership, DCN32 TX/RX tuning,
    // HPD selection and the 552us hardware receive timeout (2400us SW poll).
    Status open(const Layout&, const Io&, uint8_t engine, uint8_t hpd, Result&);
    // One actual packet, at most16 payload bytes. NACK/DEFER are not success.
    // A completed timeout may be retried; ambiguous MMIO, lost ownership or a
    // non-completing request faults this instance and retains pin ownership.
    // No MMIO after a fault until the outer owner independently resets DCN.
    Status transfer(const Request&, Result&);
    // At most32 actual packets. Retries AUX/I2C DEFER, wire timeouts (3 total),
    // malformed replies (2 total), and short native ACK reads (7 total). I2C
    // short reads return their partial bytes without replaying the sink read
    // and advancing its pointer a second time. I2C ACK-with-M
    // and I2C DEFER writes use WRITE_STATUS, never replay the accepted write.
    // Delay300us between protocol retries; budget is bounded, no infinite loop.
    Status transferWithRetries(const Request&, Result&);
    // Sends a real address-only I2C STOP if a MOT chain remains open, releases
    // arbitration, and restores only the owned GPIO bits. A failed STOP or
    // ambiguous release keeps this object faulted; never claim pins are free.
    Status close(Result&);
    bool isOpen() const { return phase_ == Phase::Open; }
    uint8_t engineIndex() const { return engine_; }
    uint8_t hpdIndex() const { return hpd_; }
    bool needsRecovery() const { return phase_ == Phase::Faulted; }
    bool ownsPins() const { return phase_ != Phase::Closed; }
    // CPU state only. Solely after independently verified DCN reset/power loss
    // and withdrawal of all users; this method does not reset hardware.
    void clearAfterVerifiedReset();
private:
    enum class Phase { Closed, Opening, Open, Faulted };
    Phase phase_;
    Io io_;
    uint32_t reg_[kRegisterCount], gpioSaved_, gpioAuxSaved_;
    uint8_t engine_, hpd_, i2cAddress_;
    bool motOpen_;
    Status read(Register, uint32_t&, Result&);
    Status write(Register, uint32_t, Result&);
    Status update(Register, uint32_t mask, uint32_t value, Result&, bool verify = false);
    Status wait(Register, uint32_t mask, uint32_t value, uint32_t count,
                uint32_t delayUs, Status timeout, Result&);
    Status owned(Result&);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
};
const char* statusString(Status);
} }
#endif
