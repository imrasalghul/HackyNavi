// SPDX-License-Identifier: MIT
#include "hackynavi/avi.hpp"
#include <cstring>
#include <iostream>
namespace a=hackynavi::avi;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
a::Config rgb(){return {a::Encoding::Rgb,a::Aspect::Ratio16x9,a::Scan::Underscan,a::Colorimetry::Unspecified,
    a::RgbRange::Full,a::YccRange::Limited,16,true,false,true,false};}
void verify(const hackynavi::vpg::Packet& p){
    unsigned sum=0;for(auto b:p.header)sum+=b;for(auto b:p.payload)sum+=b;CHECK((sum&255)==0);
    CHECK(p.header[0]==0x82&&p.header[1]==2&&p.header[2]==13&&p.header[3]==0);
    for(unsigned i=6;i<32;++i)CHECK(p.payload[i]==0);
}
}
int main(){
    hackynavi::vpg::Packet p={};auto c=rgb();CHECK(a::encode(c,p)==a::Status::Ok);
    const uint8_t expected[]={0x1d,0x12,0x28,0x08,0x10,0};CHECK(std::memcmp(p.payload,expected,sizeof(expected))==0);verify(p);
    c.encoding=a::Encoding::YCbCr444;c.colorimetry=a::Colorimetry::Bt709;c.rgbRange=a::RgbRange::Default;
    CHECK(a::encode(c,p)==a::Status::Ok);const uint8_t ycc[]={0x65,0x52,0xa8,0,0x10,0};CHECK(std::memcmp(p.payload,ycc,sizeof(ycc))==0);verify(p);
    const auto saved=p;c.yccRange=a::YccRange::Full;CHECK(a::encode(c,p)==a::Status::UnsupportedReceiver);CHECK(std::memcmp(&p,&saved,sizeof(p))==0);
    c.yccQuantizationSelectable=true;CHECK(a::encode(c,p)==a::Status::Ok&&p.payload[5]==0x40);verify(p);
    c=rgb();c.rgbQuantizationSelectable=false;CHECK(a::encode(c,p)==a::Status::UnsupportedReceiver);
    c.rgbRange=a::RgbRange::Default;CHECK(a::encode(c,p)==a::Status::Ok&&(p.payload[3]&12)==0);
    for(unsigned vic=0;vic<128;++vic){c=rgb();c.vic=uint8_t(vic);c.graphicsContent=true;
        CHECK(a::encode(c,p)==a::Status::Ok&&p.payload[4]==vic&&p.payload[3]==0x88);verify(p);}
    for(unsigned value=128;value<256;++value){c=rgb();c.vic=uint8_t(value);CHECK(a::encode(c,p)==a::Status::InvalidArgument);}
    c=rgb();c.colorimetry=a::Colorimetry::Bt601;CHECK(a::encode(c,p)==a::Status::InvalidArgument);
    c=rgb();c.aspect=a::Aspect(3);CHECK(a::encode(c,p)==a::Status::InvalidArgument);
    c=rgb();c.scan=a::Scan(3);CHECK(a::encode(c,p)==a::Status::InvalidArgument);
    c=rgb();c.encoding=a::Encoding(1);CHECK(a::encode(c,p)==a::Status::InvalidArgument);
    hackynavi::edid::Monitor sink={};c=rgb();CHECK(a::encode(c,sink,p)==a::Status::UnsupportedReceiver);
    sink.hdmi=true;CHECK(a::encode(c,sink,p)==a::Status::UnsupportedReceiver);
    sink.rgbQuantizationSelectable=true;CHECK(a::encode(c,sink,p)==a::Status::Ok);
    std::cout<<checks<<" AVI checks, "<<failures<<" failures\n";return failures?1:0;
}
