// SPDX-License-Identifier: MIT
// XNU xnu-11417.140.69 iokit/Kernel/IOPlatformExpert.cpp:getConsoleInfo
// delegates to pexpert/i386/pe_init.c:PE_current_console. PE_init_platform
// preserves the boot framebuffer's physical address in PE_state.video. Later
// console updates may instead store virtual or bit0-tagged physical addresses
// (osfmk/console/video_console.c:initialize_screen); validate that distinction.
#include "HackyNaviBootConsole.hpp"
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <pexpert/pexpert.h>

#if !defined(__x86_64__)
#error HackyNaviBootConsole requires the x86_64 PE_Video address layout
#endif
namespace hackynavi { namespace bootconsole { namespace {
eficonsole::Input input(const PE_Video& video) {
    return {video.v_baseAddr,video.v_rowBytes,video.v_width,video.v_height,
        video.v_depth,video.v_offset,video.v_length,video.v_rotate};
}
bool same(const eficonsole::Input& a,const eficonsole::Input& b) {
    return a.physicalBase==b.physicalBase&&a.rowBytes==b.rowBytes&&a.width==b.width&&
        a.height==b.height&&a.depth==b.depth&&a.offset==b.offset&&a.length==b.length&&
        a.rotation==b.rotation;
}
} // namespace
IOReturn capture(const MemoryLayout& memory,Capture& out) {
    out={};out.result=out.firstQuery=out.secondQuery=kIOReturnNotReady;
    out.reservationStatus=eficonsole::Status::InvalidGeometry;
    auto* platform=IOService::getPlatform();
    if(!platform||platform->isInactive())return out.result;
    platform->retain();
    PE_Video first={},second={};
    out.firstQuery=platform->getConsoleInfo(&first);
    if(out.firstQuery!=kIOReturnSuccess) {
        platform->release();out.result=out.firstQuery;return out.result;
    }
    out.first=input(first);out.firstDisplay=first.v_display;out.firstRawBase=first.v_baseAddr;
    out.secondQuery=platform->getConsoleInfo(&second);
    const bool live=!platform->isInactive()&&IOService::getPlatform()==platform;
    platform->release();
    if(out.secondQuery!=kIOReturnSuccess){out.result=out.secondQuery;return out.result;}
    out.second=input(second);out.secondDisplay=second.v_display;out.secondRawBase=second.v_baseAddr;
    out.stable=live&&same(out.first,out.second)&&out.firstDisplay==out.secondDisplay;
    if(!out.stable)return out.result;
    const uint64_t physical=out.secondRawBase&~UINT64_C(3);
    // Never treat a canonical kernel virtual pointer as another GPU's physical
    // console. Untagged early-boot values are accepted only inside the assigned
    // target BAR. An untagged value elsewhere has unknown provenance.
    if(!physical||physical>=(UINT64_C(1)<<48)) {out.result=kIOReturnUnsupported;return out.result;}
    if(out.secondRawBase&1)out.addressKind=AddressKind::TaggedPhysical;
    else if(physical>=memory.bar0PhysicalAddress&&
        physical-memory.bar0PhysicalAddress<memory.bar0Bytes)
        out.addressKind=AddressKind::BootPhysicalOnBar;
    else {out.result=kIOReturnUnsupported;return out.result;}
    out.first.physicalBase=out.second.physicalBase=physical;
    out.reservationStatus=eficonsole::buildReservation(memory,out.second,out.reservation);
    switch(out.reservationStatus) {
    case eficonsole::Status::Ok:out.result=kIOReturnSuccess;break;
    case eficonsole::Status::NotOnBar:out.result=kIOReturnNotFound;break;
    default:out.result=kIOReturnUnsupported;break;
    }
    return out.result;
}
} }
