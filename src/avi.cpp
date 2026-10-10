// SPDX-License-Identifier: MIT
// AVI byte layout/checksum follows CTA-861 as represented by AMD's
// dc_resource.c:set_avi_info_frame at Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0. Explicit bytes avoid C bitfield ABI.
#include "hackynavi/avi.hpp"
namespace hackynavi { namespace avi {
Status encode(const Config& c,vpg::Packet& output){
    if((c.encoding!=Encoding::Rgb&&c.encoding!=Encoding::YCbCr444)||
        uint8_t(c.aspect)>2||uint8_t(c.scan)>2||uint8_t(c.colorimetry)>2||
        uint8_t(c.rgbRange)>2||uint8_t(c.yccRange)>1||c.vic>127)return Status::InvalidArgument;
    if(c.encoding==Encoding::Rgb){
        if(c.colorimetry!=Colorimetry::Unspecified||c.yccRange!=YccRange::Limited)return Status::InvalidArgument;
        if(c.rgbRange!=RgbRange::Default&&!c.rgbQuantizationSelectable)return Status::UnsupportedReceiver;
    }else{
        if(c.colorimetry==Colorimetry::Unspecified||c.rgbRange!=RgbRange::Default)return Status::InvalidArgument;
        if(c.yccRange!=YccRange::Limited&&!c.yccQuantizationSelectable)return Status::UnsupportedReceiver;
    }
    vpg::Packet p={};p.header[0]=0x82;p.header[1]=2;p.header[2]=13;
    p.payload[1]=uint8_t((uint8_t(c.encoding)<<5)|(c.activeAspectSame?0x10:0)|uint8_t(c.scan));
    p.payload[2]=uint8_t((uint8_t(c.colorimetry)<<6)|(uint8_t(c.aspect)<<4)|(c.activeAspectSame?8:0));
    p.payload[3]=uint8_t((uint8_t(c.rgbRange)<<2)|(c.graphicsContent?0x80:0));
    p.payload[4]=c.vic;p.payload[5]=uint8_t(uint8_t(c.yccRange)<<6);
    uint8_t checksum=uint8_t(0x82+2+13);
    for(size_t i=1;i<=13;++i)checksum=uint8_t(checksum+p.payload[i]);
    p.payload[0]=uint8_t(0u-checksum);output=p;return Status::Ok;
}
Status encode(const Config& config,const edid::Monitor& sink,vpg::Packet& output){
    if(!sink.hdmi||(config.encoding==Encoding::YCbCr444&&!sink.ycbcr444))return Status::UnsupportedReceiver;
    Config c=config;c.rgbQuantizationSelectable=sink.rgbQuantizationSelectable;
    c.yccQuantizationSelectable=sink.yccQuantizationSelectable;return encode(c,output);
}
const char* statusString(Status s){switch(s){case Status::Ok:return "ok";case Status::InvalidArgument:return "invalid-argument";
    case Status::UnsupportedReceiver:return "unsupported-receiver";}return "unknown";}
} }
