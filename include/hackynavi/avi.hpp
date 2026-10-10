// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_AVI_HPP
#define HACKYNAVI_AVI_HPP
#include "vpg.hpp"
#include "edid.hpp"
namespace hackynavi { namespace avi {
enum class Encoding : uint8_t { Rgb=0,YCbCr444=2 };
enum class Aspect : uint8_t { Unspecified=0,Ratio4x3=1,Ratio16x9=2 };
enum class Scan : uint8_t { Unspecified=0,Overscan=1,Underscan=2 };
enum class Colorimetry : uint8_t { Unspecified=0,Bt601=1,Bt709=2 };
enum class RgbRange : uint8_t { Default=0,Limited=1,Full=2 };
enum class YccRange : uint8_t { Limited=0,Full=1 };
enum class Status { Ok,InvalidArgument,UnsupportedReceiver };
struct Config {
    Encoding encoding;Aspect aspect;Scan scan;Colorimetry colorimetry;
    RgbRange rgbRange;YccRange yccRange;
    uint8_t vic; // CTA VIC 0..127, zero for non-CTA detailed timings.
    bool activeAspectSame,graphicsContent;
    // Derived from sink CTA Video Capability Data Block by the modeset owner.
    // These do not program the pixel pipeline: actual output range must match.
    bool rgbQuantizationSelectable,yccQuantizationSelectable;
};
// HDMI AVI version2, 13 data bytes, RGB/444 with no repetition or bars.
// Caller supplies the selected mode's actual VIC/aspect and pipeline encoding.
// Extended colorimetry/HDR/3D require their own descriptors and are rejected
// here. Output remains unchanged on rejection; all unused SRAM bytes are zero.
Status encode(const Config&,vpg::Packet&);
// Uses the parsed sink capability bits, replacing caller-supplied flags.
// Rejects non-HDMI sinks and 444 when it is not advertised by CTA.
Status encode(const Config&,const edid::Monitor&,vpg::Packet&);
const char* statusString(Status);
} }
#endif
