// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_DISPLAY_CAPTURE_HPP
#define HACKYNAVI_BOOT_DISPLAY_CAPTURE_HPP
#include "hackynavi/boot_display.hpp"
#include "hackynavi/read_capture.hpp"

namespace hackynavi { namespace bootdisplay {
// Each pass: 12 opening power reads, 11 global/VGA reads, 4*(22*4+3)
// guarded HUBP/postcheck reads and 12 closing power reads = 399. Cold adds
// three SOS reads to the two passes. Gated windows only reduce that bound.
constexpr size_t kReadCaptureCapacity = 2 * (6 * kPipeCount + kVgaCount +
    kGlobalRegisterCount + kPipeCount * (4 * kPipeRegisterCount + 3)) + 3;
static_assert(kReadCaptureCapacity == 801, "update read journal bound with capture protocol");
using ReadCapture = readcapture::Journal<kReadCaptureCapacity>;
// Keep journal off the kernel stack. Null means no diagnostic storage: the
// original protocol still runs unchanged, and its caller must report omission.
inline Status captureRecorded(const Layout& layout, const MemoryLayout& memory,
    const Io& io, Capture& out, ReadCapture* journal, bool postedCold) {
    if (!journal) return postedCold ? capturePostedCold(layout,memory,io,out) : capture(layout,memory,io,out);
    *journal = ReadCapture{};
    if (!io.read32) return postedCold ? capturePostedCold(layout,memory,io,out) : capture(layout,memory,io,out);
    readcapture::Reader<kReadCaptureCapacity> reader(io.context,io.read32,*journal);
    const Io traced = {&reader,readcapture::Reader<kReadCaptureCapacity>::read};
    return postedCold ? capturePostedCold(layout,memory,traced,out) : capture(layout,memory,traced,out);
}
} }
#endif
