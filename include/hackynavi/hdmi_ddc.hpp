// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HDMI_DDC_HPP
#define HACKYNAVI_HDMI_DDC_HPP
#include "hackynavi/discovery.hpp"
namespace hackynavi { namespace hdmiddc {
constexpr size_t kPorts=5,kFifoBytes=538,kMaxMessages=4;
enum class Register : size_t { Setup,Speed,HardwareStatus,Arbitration,Control,
    Status,Transaction0,Transaction1,Transaction2,Transaction3,Data,TimeBase,
    MemoryPowerControl,MemoryPowerStatus,GpioMask,GpioEnable,GpioAux,PhyAux,Count };
constexpr size_t kRegisters=static_cast<size_t>(Register::Count);
struct Layout { bool valid; uint64_t barBytes; uint32_t registers[kPorts][kRegisters]; };
enum class Status { Ok,InvalidArgument,DiscoveryRejected,MissingDmu,UnsupportedDmu,
    OutsideBar,InvalidClock,Closed,Busy,Faulted,TransportError,DeviceUnavailable,
    OwnershipLost,PowerTimeout,VerifyFailed,CompletionTimeout,Nack,WireTimeout,
    Aborted,Interrupted,BufferOverflow };
struct Io {
    void* context;
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); // false may have landed
    void (*delayUs)(void*,uint32_t);
};
struct Clock {
    uint32_t crystalKHz; // actual ATOM PLL crystal_frequency, never guessed
    uint32_t requestedKHz; // 1..400; normally50/100 for DDC, caller-selected
};
struct Speed {
    uint32_t timeBase,referenceKHz,inputKHz,prescaler;
    // Programmed SCL frequency is inputKHz / prescaler (not rounded here).
};
// Mirrors dce_i2c_hw set_speed: TIME_BASE override*1000 or
// (crystalKHz/2)*2, divided by XTAL_REF_DIV (zero encodes2), then by speed.
// Rejects overflow, zero and unrepresentable prescale before any MMIO write.
Status calculateSpeed(const Clock&,uint32_t timeBase,Speed&);
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);
struct Message {
    uint8_t address; // seven-bit I2C slave, not an on-wire shifted byte
    bool read;
    uint16_t bytes;
    const uint8_t* data; // write payload only; read bytes returned in Result
};
struct Result {
    uint32_t reads,writes,polls,lastRegister,rawStatus,timeoutUs;
    bool submitted,completed;
    uint16_t bytes;
    uint8_t data[kFifoBytes];
};
class Engine {
public:
    Engine();
    // Owns the ONE shared DCN3.2 hardware I2C engine/FIFO, selecting DDC1..5
    // with port0..4. The caller holds the global display GPIO/I2C/AUX lock,
    // powers DCN/reference clocks, excludes firmware/HDCP users, modesets,
    // reset/suspend/removal, and confirms ATOM connector-to-DDC routing.
    // Opens physical I2C GPIO mode, wakes I2C SRAM, acquires HW arbitration,
    // and programs speed/time limit. No HDMI output or PHY mode is enabled.
    Status open(const Layout&,const Io&,uint8_t port,const Clock&,Result&);
    // One complete hardware command, 1..4 repeated-START messages and STOP
    // on the final message. Up to3 writes followed by a final read, or all
    // writes. No intermediate read (Linux executes/STOPs reads immediately).
    // Total address+write+read bytes<=538; zero-byte writes are address probes.
    // Requires an observed DONE with no error, never treats idle/zero as ACK.
    // Bounded Linux-derived clock-stretch timeout, no automatic write retries.
    Status transfer(const Message*,size_t count,Result&);
    // E-DDC128-byte block: optional segment0x30, offset0x50, repeated-start
    // read0x50. Omits segment write for block0/1 as drm_do_probe_ddc_edid does.
    Status readEdidBlock(uint8_t block,Result&);
    // Actual SCDC bus transactions at seven-bit slave0x54.
    Status readScdc(uint8_t offset,uint16_t bytes,Result&);
    Status writeScdc(uint8_t offset,const uint8_t*,uint16_t bytes,Result&);
    // Reset completed SW status, release with the source's second-release
    // workaround, verify ownership departed, restore selected GPIO/clock bits.
    // Ambiguous I/O/lost ownership/in-flight timeout seals this object; caller
    // retains exclusion until independently reset, no automatic destructive reset.
    Status close(Result&);
    bool isOpen() const { return phase_==Phase::Open; }
    uint8_t portIndex() const { return port_; }
    bool needsRecovery() const { return phase_==Phase::Faulted; }
    bool ownsController() const { return phase_!=Phase::Closed; }
    const Speed& speed() const { return speed_; }
    // CPU bookkeeping only after independently verified DCN reset/power loss.
    void clearAfterVerifiedReset();
private:
    enum class Phase { Closed,Opening,Open,Faulted };
    Phase phase_; Io io_; Speed speed_; uint8_t port_;
    uint32_t reg_[kRegisters],saved_[kRegisters];
    Status read(Register,uint32_t&,Result&);
    Status write(Register,uint32_t,Result&);
    Status update(Register,uint32_t mask,uint32_t value,Result&,bool verify=false);
    Status owned(Result&);
    Status fail(Status);
    Engine(const Engine&)=delete;
    Engine& operator=(const Engine&)=delete;
};
const char* statusString(Status);
} }
#endif
