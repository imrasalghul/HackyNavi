// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_CONSOLE_HPP
#define HACKYNAVI_BOOT_CONSOLE_HPP
#include <IOKit/IOReturn.h>
#include "hackynavi/efi_console.hpp"

namespace hackynavi { namespace bootconsole {
enum class AddressKind { Unknown, BootPhysicalOnBar, TaggedPhysical };
struct Capture {
    IOReturn result,firstQuery,secondQuery;
    eficonsole::Input first,second;
    uint64_t firstDisplay,secondDisplay;
    uint64_t firstRawBase,secondRawBase;
    AddressKind addressKind;
    bool stable;
    eficonsole::Status reservationStatus;
    eficonsole::Result reservation;
};
// Read actual platform console metadata twice and validate its physical range
// against the caller's current, validated memory layout on firmware-first
// attach. Raw early physical bases must lie within this BAR; later explicit
// physical tags are decoded. Kernel virtual/unknown untagged addresses fail
// instead of becoming NotOnBar evidence. No PCI/MMIO access,
// mapping, screen disable, reservation insertion or display takeover occurs.
// Stable metadata is an observation, not exclusion of later console changes.
// The caller must union the returned reservation with firmware/DCN allocations
// and retain it through the real framebuffer console handoff. NotOnBar is
// distinct from an empty/complete framebuffer inventory; every other failed
// query or range validation must remain a failure in the caller's admission.
IOReturn capture(const MemoryLayout&,Capture&);
} }
#endif
