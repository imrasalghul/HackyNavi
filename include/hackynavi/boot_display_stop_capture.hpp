// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_DISPLAY_STOP_CAPTURE_HPP
#define HACKYNAVI_BOOT_DISPLAY_STOP_CAPTURE_HPP
#include "hackynavi/boot_display_capture.hpp"
#include "hackynavi/boot_display_stop.hpp"
namespace hackynavi { namespace bootdisplaystop {
// Owned capture additionally rechecks all programming and active clocks:
// 12 opening + 11 VGA/global + 4*(22*4 + 4 clock) + 12 closing + final SOS.
constexpr size_t kReadCaptureCapacity = bootdisplay::kReadCaptureCapacity +
    6 * bootdisplay::kPipeCount + bootdisplay::kVgaCount + bootdisplay::kGlobalRegisterCount +
    bootdisplay::kPipeCount * (4 * bootdisplay::kPipeRegisterCount + 4) + 1;
static_assert(kReadCaptureCapacity == 1205, "owned capture read bound");
using ReadCapture = readcapture::Journal<kReadCaptureCapacity>;
// Only the read-only capture entry point is wrapped; stop writes are not
// intercepted and cannot use this read-only adapter.
inline Status captureRecorded(const bootdisplay::Layout& layout, const MemoryLayout& memory,
    const Io& io, State& state, Result& out, ReadCapture* journal) {
    if(!journal)return capture(layout,memory,io,state,out);
    *journal=ReadCapture{};
    if(!io.read32)return capture(layout,memory,io,state,out);
    readcapture::Reader<kReadCaptureCapacity> reader(io.context,io.read32,*journal);
    return capture(layout,memory,{&reader,readcapture::Reader<kReadCaptureCapacity>::read,nullptr,nullptr},state,out);
}
} }
#endif
