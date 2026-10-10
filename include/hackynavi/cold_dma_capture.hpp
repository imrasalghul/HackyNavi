// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_COLD_DMA_CAPTURE_HPP
#define HACKYNAVI_COLD_DMA_CAPTURE_HPP
#include "hackynavi/cold_dma.hpp"
#include "hackynavi/read_capture.hpp"

namespace hackynavi { namespace colddma {
// Each pass: two 3-word cold guards, IH11, WB18, global MALL1,
// and four pipes*(five 3-word power checks + four MALL words) = 112.
// Final four power probes and one cold guard add 15; maximum 239 reads.
constexpr size_t kReadCaptureCapacity = 2 * (6 + kIhRegisters + kWbRegisters +
    1 + kPipes * (15 + 4)) + 3 * kPipes + 3;
static_assert(kReadCaptureCapacity == 239, "update read journal bound with cold DMA protocol");
using ReadCapture = readcapture::Journal<kReadCaptureCapacity>;
inline Status checkStoppedRecorded(const Layout& layout, const Io& io,
    Result& out, ReadCapture* journal) {
    if (!journal) return checkStopped(layout,io,out);
    *journal = ReadCapture{};
    if (!io.read32) return checkStopped(layout,io,out);
    readcapture::Reader<kReadCaptureCapacity> reader(io.context,io.read32,*journal);
    return checkStopped(layout,{&reader,readcapture::Reader<kReadCaptureCapacity>::read},out);
}
} }
#endif
