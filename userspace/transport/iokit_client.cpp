// SPDX-License-Identifier: MIT
// Transport mechanism studied from Cobalt64's MIT ac_darwin_iokit.c.
// New ABI, validation and implementation; no Cobalt register or VMID0 methods.
#include "iokit_client.hpp"
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <new>
#include <string.h>

namespace hackynavi { namespace transport {
namespace a = nativeabi;
namespace {
Result ok() { return {Status::Ok, 0}; }
Result fail(Status s) { return {s, 0}; }
Result systemResult(kern_return_t kr) {
    switch (kr) {
    case kIOReturnSuccess: return ok();
    case kIOReturnUnsupported: return {Status::Unsupported, kr};
    case kIOReturnNoMemory: case kIOReturnNoResources: case kIOReturnNoSpace: return {Status::OutOfMemory, kr};
    case kIOReturnNotFound: return {Status::NotFound, kr};
    case kIOReturnBusy: case kIOReturnExclusiveAccess: return {Status::Busy, kr};
    case kIOReturnTimeout: return {Status::Timeout, kr};
    case kIOReturnNoDevice: case kIOReturnOffline: case kIOReturnAborted: return {Status::DeviceLost, kr};
    case kIOReturnBadArgument: return {Status::Invalid, kr};
    case kIOReturnNotPermitted: return {Status::Unsupported, kr};
    default: return {Status::Transport, kr};
    }
}
struct ServiceRef {
    io_service_t value=IO_OBJECT_NULL;
    ~ServiceRef(){if(value)IOObjectRelease(value);}
    ServiceRef()=default;
    ServiceRef(const ServiceRef&)=delete;
    ServiceRef& operator=(const ServiceRef&)=delete;
};
Result acceleratorIdentity(io_service_t accelerator,uint64_t acceleratorID,
    uint64_t ownerID,ServiceRef& owner) {
    io_name_t name{};
    auto kr=IOObjectGetClass(accelerator,name);
    if(kr!=KERN_SUCCESS)return systemResult(kr);
    if(strcmp(name,"HackyNaviAccelerator")||!IOObjectConformsTo(accelerator,"IOAccelerator")||
        IOObjectConformsTo(accelerator,"IOServiceCompatibility"))return fail(Status::Invalid);
    uint64_t actual=0;
    kr=IORegistryEntryGetRegistryEntryID(accelerator,&actual);
    if(kr!=KERN_SUCCESS)return systemResult(kr);
    if(actual!=acceleratorID)return fail(Status::Invalid);
    kr=IORegistryEntryGetParentEntry(accelerator,kIOServicePlane,&owner.value);
    if(kr!=KERN_SUCCESS)return systemResult(kr);
    kr=IOObjectGetClass(owner.value,name);
    if(kr!=KERN_SUCCESS)return systemResult(kr);
    if(strcmp(name,a::kServiceClass))return fail(Status::Invalid);
    kr=IORegistryEntryGetRegistryEntryID(owner.value,&actual);
    if(kr!=KERN_SUCCESS)return systemResult(kr);
    if(actual!=ownerID)return fail(Status::Invalid);
    auto property=IORegistryEntryCreateCFProperty(accelerator,CFSTR("HackyNaviOwnerRegistryID"),
        kCFAllocatorDefault,0);
    int64_t claimed=0;
    const bool consistent=property&&CFGetTypeID(property)==CFNumberGetTypeID()&&
        !CFNumberIsFloatType(static_cast<CFNumberRef>(property))&&
        CFNumberGetValue(static_cast<CFNumberRef>(property),kCFNumberSInt64Type,&claimed)&&
        claimed>0&&uint64_t(claimed)==ownerID;
    if(property)CFRelease(property);
    return consistent?ok():fail(Status::Invalid);
}
}
Client::~Client() { close(); }
Result Client::open(uint64_t registryId) {
    return openProfile(registryId, a::kRequiredCapabilities, a::kGenericMinor);
}
Result Client::openBuiltIn(uint64_t registryId) {
    return openProfile(registryId, a::kBuiltInCapabilities, a::kMinor);
}
Result Client::openCompiler(uint64_t registryId) {
    return openProfile(registryId, a::kCompilerCapabilities, a::kCompilerMinor);
}
Result Client::openProduct(uint64_t registryId) {
    return openProfile(registryId, a::kProductCapabilities, a::kProductMinor);
}
Result Client::openReusableProduct(uint64_t registryId) {
    return openProfile(registryId, a::kReusableCapabilities, a::kReusableMinor);
}
Result Client::openGraphicsProduct(uint64_t registryId) {
    return openProfile(registryId, a::kGraphicsCapabilities, a::kGraphicsMinor);
}
Result Client::openBufferedGraphicsProduct(uint64_t registryId) {
    return openProfile(registryId, a::kBufferedGraphicsCapabilities, a::kBufferedGraphicsMinor);
}
Result Client::openVertexBufferedGraphicsProduct(uint64_t registryId) {
    return openProfile(registryId,a::kVertexBufferedGraphicsCapabilities,a::kVertexBufferedGraphicsMinor);
}
Result Client::openBufferedGraphicsAccelerator(uint32_t acceleratorPort,
    uint64_t acceleratorID,uint64_t ownerID) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(connection_)return fail(Status::Busy);
    if(!acceleratorPort||!acceleratorID||!ownerID||acceleratorID==ownerID)return fail(Status::Invalid);
    ServiceRef accelerator,owner;
    const auto retained=IOObjectRetain(acceleratorPort);
    if(retained!=KERN_SUCCESS)return systemResult(retained);
    accelerator.value=acceleratorPort;
    auto result=acceleratorIdentity(accelerator.value,acceleratorID,ownerID,owner);
    if(!result)return result;
    // Deliberately open the supplied actual accelerator, not a newly found
    // provider port. The kernel binds this route to its publication generation.
    result=openServiceLocked(accelerator.value,ownerID,a::kBufferedGraphicsCapabilities,
        a::kBufferedGraphicsMinor);
    if(!result)return result;
    ServiceRef after;
    result=acceleratorIdentity(accelerator.value,acceleratorID,ownerID,after);
    if(result&&!IOObjectIsEqualTo(owner.value,after.value))result=fail(Status::DeviceLost);
    if(!result){closeLocked();return result;}
    return ok();
}
Result Client::openVertexBufferedGraphicsAccelerator(uint32_t acceleratorPort,
    uint64_t acceleratorID,uint64_t ownerID) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(connection_)return fail(Status::Busy);
    if(!acceleratorPort||!acceleratorID||!ownerID||acceleratorID==ownerID)return fail(Status::Invalid);
    ServiceRef accelerator,owner;
    const auto retained=IOObjectRetain(acceleratorPort);
    if(retained!=KERN_SUCCESS)return systemResult(retained);
    accelerator.value=acceleratorPort;
    auto result=acceleratorIdentity(accelerator.value,acceleratorID,ownerID,owner);
    if(!result)return result;
    result=openServiceLocked(accelerator.value,ownerID,a::kVertexBufferedGraphicsCapabilities,
        a::kVertexBufferedGraphicsMinor);
    if(!result)return result;
    ServiceRef after;
    result=acceleratorIdentity(accelerator.value,acceleratorID,ownerID,after);
    if(result&&!IOObjectIsEqualTo(owner.value,after.value))result=fail(Status::DeviceLost);
    if(!result){closeLocked();return result;}
    return ok();
}
Result Client::openProfile(uint64_t registryId, uint64_t required, uint16_t minor) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_) return fail(Status::Busy);
    io_iterator_t iterator = IO_OBJECT_NULL;
    auto kr = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(a::kServiceClass), &iterator);
    if (kr != KERN_SUCCESS) return systemResult(kr);
    io_service_t selected = IO_OBJECT_NULL;
    bool ambiguous = false;
    uint64_t selectedId = 0;
    while (auto service = IOIteratorNext(iterator)) {
        uint64_t id = 0;
        kr = IORegistryEntryGetRegistryEntryID(service, &id);
        if (kr == KERN_SUCCESS && id && (!registryId || registryId == id)) {
            if (selected) ambiguous = true;
            else { selected = service; selectedId = id; service = IO_OBJECT_NULL; }
        }
        if (service) IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if (ambiguous) { IOObjectRelease(selected); return fail(Status::AmbiguousDevice); }
    if (!selected) return fail(Status::NotFound);
    const auto result=openServiceLocked(selected,selectedId,required,minor);
    IOObjectRelease(selected);
    return result;
}
Result Client::openServiceLocked(uint32_t service,uint64_t ownerID,uint64_t required,uint16_t minor) {
    io_connect_t connection = IO_OBJECT_NULL;
    const auto kr = IOServiceOpen(service, mach_task_self(), a::kUserClientType, &connection);
    if (kr != KERN_SUCCESS) return systemResult(kr);
    connection_ = connection;
    a::HelloRequest request{a::makeHeader(sizeof(request), 0, minor), required, 0};
    a::HelloReply reply{};
    auto result = exchange(a::Selector::Hello, &request, sizeof(request), &reply, sizeof(reply));
    if (!result) { closeLocked(); return result; }
    const auto validation = a::validateHello(reply, ownerID, required);
    if (validation != a::Validation::Ok || reply.header.minor != minor) {
        closeLocked(); return fail(validation == a::Validation::Unsupported ? Status::Unsupported : Status::Protocol);
    }
    records_ = new (std::nothrow) Record[kMaxBuffers];
    if (!records_) { closeLocked(); return fail(Status::OutOfMemory); }
    hello_ = reply; submitted_ = resultBytes_ = 0; lost_ = sealed_ = false;
    for (auto& retained : results_) retained = {};
    return ok();
}
Result Client::closeLocked() {
    Result result = ok();
    if (connection_) {
        if (records_) for (uint32_t n = 0; n < kMaxBuffers; ++n) {
            const auto& r = records_[n];
            if (r.mapping.address)
                IOConnectUnmapMemory64(connection_, r.allocation.memoryType, mach_task_self(),
                                      mach_vm_address_t(reinterpret_cast<uintptr_t>(r.mapping.address)));
        }
        // Closing is not evidence of GPU retirement. Kernel clientClose must
        // retain submitted resources until a GPU fence or verified reset.
        if (hello_.header.epoch && (hello_.capabilities & (a::SealedSingleJob | a::ReusableCompute))) {
            const auto request = header(sizeof(a::Header)); a::Header reply{}; size_t actual = sizeof(reply);
            const auto kr = IOConnectCallStructMethod(connection_, uint32_t(a::Selector::Close),
                &request, sizeof(request), &reply, &actual);
            result = systemResult(kr);
            if (result && (actual != sizeof(reply) || a::validateHeader(reply, sizeof(reply),
                hello_.header.epoch, hello_.header.minor) != a::Validation::Ok)) result = fail(Status::Protocol);
        }
        const auto closed = systemResult(IOServiceClose(connection_));
        if (result && !closed) result = closed;
    }
    delete[] records_; records_ = nullptr; connection_ = 0; hello_ = {}; submitted_ = resultBytes_ = 0; lost_ = sealed_ = false;
    for (auto& retained : results_) retained = {};
    return result;
}
Result Client::close() { std::lock_guard<std::mutex> lock(mutex_); return closeLocked(); }
a::Header Client::header(size_t bytes) const { return a::makeHeader(bytes, hello_.header.epoch, hello_.header.minor); }
Result Client::exchange(a::Selector selector, const void* in, size_t inBytes, void* out, size_t outBytes) {
    if (!connection_) return fail(Status::NotOpen);
    // Already published history is CPU-only. A failed later submission must
    // never authorize a retry, but cannot erase an older retained observation.
    const bool history = reusable() && (selector == a::Selector::Wait ||
        selector == a::Selector::ReadResult || selector == a::Selector::ReleaseResult);
    if (lost_ && !history) return fail(Status::DeviceLost);
    size_t actual = outBytes;
    const auto kr = IOConnectCallStructMethod(connection_, uint32_t(selector), in, inBytes, out, &actual);
    if (kr != KERN_SUCCESS) {
        auto result = systemResult(kr);
        if (result.status == Status::DeviceLost) lost_ = true;
        return result;
    }
    if (actual != outBytes) { lost_ = true; return fail(Status::Protocol); }
    return ok();
}
Result Client::info(a::HelloReply& out) {
    std::lock_guard<std::mutex> lock(mutex_); out = {};
    if (!connection_) return fail(Status::NotOpen);
    if (lost_) return fail(Status::DeviceLost);
    out = hello_; return ok();
}
Client::Record* Client::find(const Buffer& buffer) {
    if (!records_ || buffer.epoch != hello_.header.epoch || !a::validHandle(buffer.handle)) return nullptr;
    for (uint32_t n = 0; n < kMaxBuffers; ++n)
        if (a::sameHandle(records_[n].allocation.handle, buffer.handle)) return &records_[n];
    return nullptr;
}
Client::HistoricalResult* Client::findResult(Fence fence) {
    if (!fence.sequence || fence.epoch != hello_.header.epoch) return nullptr;
    for (auto& retained : results_) if (retained.sequence == fence.sequence) return &retained;
    return nullptr;
}
Client::HistoricalResult* Client::freeResult() {
    for (auto& retained : results_) if (!retained.sequence) return &retained;
    return nullptr;
}
Result Client::allocate(uint64_t bytes, uint64_t alignment, a::Domain domain, uint32_t flags, Buffer& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_) return fail(Status::NotOpen);
    if (out.epoch || out.handle.id || out.handle.generation) return fail(Status::Invalid);
    if (sealed_) return fail(Status::Unsupported);
    a::AllocateRequest request{header(sizeof(request)), bytes, alignment, uint32_t(domain), flags, 0};
    if (a::validateAllocate(request, hello_) != a::Validation::Ok) return fail(Status::Invalid);
    Record* slot = nullptr;
    for (uint32_t n = 0; n < kMaxBuffers; ++n) if (!records_[n].allocation.handle.id) { slot = &records_[n]; break; }
    if (!slot) return fail(Status::OutOfMemory);
    a::AllocateReply reply{};
    auto result = exchange(a::Selector::Allocate, &request, sizeof(request), &reply, sizeof(reply));
    if (!result) return result;
    if (a::validateHeader(reply.header, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok ||
        !a::validHandle(reply.handle) || reply.bytes != bytes || reply.domain != uint32_t(domain) ||
        reply.flags != flags || reply.reserved || bool(reply.memoryType) != bool(flags & a::CpuVisible)) {
        lost_ = true; return fail(Status::Protocol);
    }
    for (uint32_t n = 0; n < kMaxBuffers; ++n) {
        const auto& old = records_[n].allocation;
        if (old.handle.id && (old.handle.id == reply.handle.id || (reply.memoryType && old.memoryType == reply.memoryType))) {
            lost_ = true; return fail(Status::Protocol);
        }
    }
    slot->allocation = reply; out = {reply.handle, hello_.header.epoch}; return ok();
}
Result Client::release(Buffer& buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_) return fail(Status::NotOpen);
    auto* record = find(buffer); if (!record) return fail(Status::Invalid);
    if (record->mapping.address) return fail(Status::Busy);
    if (sealed_) return fail(Status::Unsupported);
    a::FreeRequest request{header(sizeof(request)), buffer.handle, 0};
    a::Header reply{};
    auto result = exchange(a::Selector::Free, &request, sizeof(request), &reply, sizeof(reply));
    if (!result) return result;
    if (a::validateHeader(reply, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok) {
        lost_ = true; return fail(Status::Protocol);
    }
    *record = {}; buffer = {}; return ok();
}
Result Client::mapCpu(const Buffer& buffer, Mapping& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_) return fail(Status::NotOpen);
    if (lost_) return fail(Status::DeviceLost);
    auto* record = find(buffer);
    if (!record || out.address || out.bytes) return fail(Status::Invalid);
    if (!(hello_.capabilities & a::CpuMapping) || !record->allocation.memoryType) return fail(Status::Unsupported);
    if (record->mapping.address) return fail(Status::Busy);
    mach_vm_address_t address = 0; mach_vm_size_t size = 0;
    const auto kr = IOConnectMapMemory64(connection_, record->allocation.memoryType, mach_task_self(),
                                       &address, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) return systemResult(kr);
    if (!address || size != record->allocation.bytes || address % hello_.pageBytes || address > UINTPTR_MAX - size) {
        if (address) IOConnectUnmapMemory64(connection_, record->allocation.memoryType, mach_task_self(), address);
        lost_ = true; return fail(Status::Protocol);
    }
    record->mapping = {reinterpret_cast<void*>(uintptr_t(address)), size}; out = record->mapping; return ok();
}
Result Client::unmapCpu(const Buffer& buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_) return fail(Status::NotOpen);
    auto* record = find(buffer);
    if (!record || !record->mapping.address) return fail(Status::Invalid);
    const auto kr = IOConnectUnmapMemory64(connection_, record->allocation.memoryType, mach_task_self(),
                                         mach_vm_address_t(reinterpret_cast<uintptr_t>(record->mapping.address)));
    if (kr != KERN_SUCCESS) return systemResult(kr);
    record->mapping = {}; return ok();
}
Result Client::va(const Buffer& buffer, uint64_t address, uint64_t offset, uint64_t bytes, uint32_t access, bool unmap) {
    if (!connection_) return fail(Status::NotOpen);
    auto* record = find(buffer); if (!record) return fail(Status::Invalid);
    if (sealed_) return fail(Status::Unsupported);
    a::VaRequest request{header(sizeof(request)), buffer.handle, address, offset, bytes, access, 0};
    if (a::validateVa(request, hello_, record->allocation.bytes, unmap) != a::Validation::Ok) return fail(Status::Invalid);
    a::Header reply{};
    auto result = exchange(unmap ? a::Selector::UnmapVa : a::Selector::MapVa, &request, sizeof(request), &reply, sizeof(reply));
    if (!result) return result;
    if (a::validateHeader(reply, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok) {
        lost_ = true; return fail(Status::Protocol);
    }
    return ok();
}
Result Client::mapVa(const Buffer& buffer, uint64_t address, uint64_t offset, uint64_t bytes, uint32_t access) {
    std::lock_guard<std::mutex> lock(mutex_); return va(buffer, address, offset, bytes, access, false);
}
Result Client::unmapVa(const Buffer& buffer, uint64_t address, uint64_t offset, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_); return va(buffer, address, offset, bytes, 0, true);
}
Result Client::submit(const a::Ib* ibs, uint32_t ibCount, const a::Resource* resources, uint32_t resourceCount, Fence& fence) {
    std::lock_guard<std::mutex> lock(mutex_); fence = {};
    if (!connection_) return fail(Status::NotOpen);
    if (!(hello_.capabilities & a::CapturedValidatedIb)) return fail(Status::Unsupported);
    if (!ibs || !resources || !ibCount || ibCount > a::kMaxIbs || !resourceCount || resourceCount > a::kMaxResources)
        return fail(Status::Invalid);
    const size_t bytes = sizeof(a::SubmitRequest) + ibCount * sizeof(a::Ib) + resourceCount * sizeof(a::Resource);
    uint8_t message[a::kMaxMessage]{};
    a::SubmitRequest request{header(bytes), ibCount, resourceCount, 0};
    memcpy(message, &request, sizeof(request));
    memcpy(message + sizeof(request), ibs, ibCount * sizeof(a::Ib));
    memcpy(message + sizeof(request) + ibCount * sizeof(a::Ib), resources, resourceCount * sizeof(a::Resource));
    a::CapturedSubmit captured{};
    if (a::decodeSubmit(message, bytes, hello_, captured) != a::Validation::Ok) return fail(Status::Invalid);
    for (uint32_t n = 0; n < resourceCount; ++n)
        if (!find({captured.resources[n].handle, hello_.header.epoch})) return fail(Status::Invalid);
    for (uint32_t n = 0; n < ibCount; ++n) {
        const auto& ib = captured.ibs[n]; auto* record = find({ib.handle, hello_.header.epoch});
        if (!record || ib.offset >= record->allocation.bytes || uint64_t(ib.words) * 4 > record->allocation.bytes - ib.offset)
            return fail(Status::Invalid);
    }
    a::SubmitReply reply{};
    auto result = exchange(a::Selector::Submit, message, bytes, &reply, sizeof(reply));
    if (!result) {
        // A failed submit may have reached hardware. No automatic retry/reuse.
        // The kernel owns retention of every BO referenced by the attempt.
        lost_ = true; return result;
    }
    if (a::validateHeader(reply.header, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok ||
        reply.reserved || !reply.sequence || reply.sequence <= submitted_) { lost_ = true; return fail(Status::Protocol); }
    submitted_ = reply.sequence; fence = {reply.sequence, hello_.header.epoch}; return ok();
}
Result Client::wait(Fence fence, uint64_t timeoutNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_) return fail(Status::NotOpen);
    if (!fence.sequence || fence.epoch != hello_.header.epoch || timeoutNs > a::kMaxWaitNs)
        return fail(Status::Invalid);
    if (reusable()) { if (!findResult(fence)) return fail(Status::NotFound); }
    else if (fence.sequence > submitted_) return fail(Status::Invalid);
    a::WaitRequest request{header(sizeof(request)), fence.sequence, timeoutNs};
    a::WaitReply reply{};
    auto result = exchange(a::Selector::Wait, &request, sizeof(request), &reply, sizeof(reply));
    if (!result) return result;
    if (a::validateWait(reply, hello_.header.epoch, fence.sequence, hello_.header.minor) != a::Validation::Ok || reply.completedSequence > submitted_ ||
        (reusable() && reply.state == uint32_t(a::FenceState::Complete) && reply.completedSequence != fence.sequence)) {
        lost_ = true; return fail(Status::Protocol);
    }
    if (reply.state == uint32_t(a::FenceState::DeviceLost)) { lost_ = true; return fail(Status::DeviceLost); }
    return reply.state == uint32_t(a::FenceState::Complete) ? ok() : fail(Status::Timeout);
}
Result Client::bufferIo(const Buffer& buffer, uint64_t offset, void* data, size_t bytes, bool write) {
    if (!connection_) return fail(Status::NotOpen);
    if (!(hello_.capabilities & a::CopiedBufferIO) || sealed_) return fail(Status::Unsupported);
    auto* record = find(buffer);
    if (!record || !data || !bytes || offset > record->allocation.bytes || bytes > record->allocation.bytes - offset)
        return fail(Status::Invalid);
    auto* cursor = static_cast<uint8_t*>(data);
    while (bytes) {
        const uint32_t chunk = uint32_t(bytes > a::kMaxCopyBytes ? a::kMaxCopyBytes : bytes);
        uint8_t input[sizeof(a::BufferIoRequest) + a::kMaxCopyBytes]{};
        const size_t inBytes = sizeof(a::BufferIoRequest) + (write ? chunk : 0);
        a::BufferIoRequest request{header(inBytes), buffer.handle, offset, chunk, 0};
        memcpy(input, &request, sizeof(request));
        if (write) memcpy(input + sizeof(request), cursor, chunk);
        uint8_t output[sizeof(a::DataReply) + a::kMaxCopyBytes]{};
        const size_t outBytes = write ? sizeof(a::Header) : sizeof(a::DataReply) + chunk;
        auto result = exchange(write ? a::Selector::WriteBuffer : a::Selector::ReadBuffer,
                               input, inBytes, output, outBytes);
        if (!result) return result;
        a::Header returned{}; memcpy(&returned, output, sizeof(returned));
        if (a::validateHeader(returned, outBytes, hello_.header.epoch, hello_.header.minor) != a::Validation::Ok) {
            lost_ = true; return fail(Status::Protocol);
        }
        if (!write) {
            a::DataReply reply{}; memcpy(&reply, output, sizeof(reply));
            if (reply.bytes != chunk || reply.reserved) { lost_ = true; return fail(Status::Protocol); }
            memcpy(cursor, output + sizeof(reply), chunk);
        }
        bytes -= chunk; offset += chunk; cursor += chunk;
    }
    return ok();
}
Result Client::write(const Buffer& buffer, uint64_t offset, const void* data, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    return bufferIo(buffer, offset, const_cast<void*>(data), bytes, true);
}
Result Client::read(const Buffer& buffer, uint64_t offset, void* data, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_); return bufferIo(buffer, offset, data, bytes, false);
}
Result Client::compute(a::ComputeRequest& request, Fence& fence) {
    fence = {};
    if (!connection_) return fail(Status::NotOpen);
    if (!(hello_.capabilities & a::BuiltInCompute) || sealed_) return fail(Status::Unsupported);
    request.header = header(sizeof(request));
    if (a::validateCompute(request, hello_.header.epoch, hello_.header.minor) != a::Validation::Ok) return fail(Status::Invalid);
    auto* destination = find({request.destination, hello_.header.epoch});
    if (!destination || request.destinationOffset > destination->allocation.bytes ||
        request.bytes > destination->allocation.bytes - request.destinationOffset) return fail(Status::Invalid);
    if (request.operation == uint32_t(a::ComputeOperation::Copy)) {
        auto* source = find({request.source, hello_.header.epoch});
        if (!source || request.sourceOffset > source->allocation.bytes ||
            request.bytes > source->allocation.bytes - request.sourceOffset) return fail(Status::Invalid);
    }
    auto* retained = reusable() ? freeResult() : nullptr;
    if (reusable() && !retained) return fail(Status::OutOfMemory);
    a::SubmitReply reply{};
    auto result = exchange(a::Selector::Compute, &request, sizeof(request), &reply, sizeof(reply));
    // A failed operation can already have published control addresses. Never
    // retry or resume mutation on this connection after an attempted compute.
    sealed_ = true;
    if (!result) { lost_ = true; return result; }
    if (a::validateHeader(reply.header, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok ||
        reply.reserved || !reply.sequence || reply.sequence <= submitted_) { lost_ = true; return fail(Status::Protocol); }
    submitted_ = reply.sequence; resultBytes_ = request.bytes;
    if (retained) { *retained = {reply.sequence, request.bytes}; sealed_ = false; }
    fence = {reply.sequence, hello_.header.epoch}; return ok();
}
Result Client::clear(const Buffer& destination, uint64_t offset, uint32_t bytes,
                     const uint32_t pattern[4], Fence& fence) {
    std::lock_guard<std::mutex> lock(mutex_); fence = {};
    if (!pattern || !find(destination)) return fail(Status::Invalid);
    a::ComputeRequest request{}; request.destination = destination.handle;
    request.destinationOffset = offset; request.bytes = bytes; request.operation = uint32_t(a::ComputeOperation::Clear);
    memcpy(request.pattern, pattern, sizeof(request.pattern)); return compute(request, fence);
}
Result Client::copy(const Buffer& source, uint64_t sourceOffset, const Buffer& destination,
                    uint64_t destinationOffset, uint32_t bytes, Fence& fence) {
    std::lock_guard<std::mutex> lock(mutex_); fence = {};
    if (!find(source) || !find(destination)) return fail(Status::Invalid);
    a::ComputeRequest request{}; request.source = source.handle; request.destination = destination.handle;
    request.sourceOffset = sourceOffset; request.destinationOffset = destinationOffset;
    request.bytes = bytes; request.operation = uint32_t(a::ComputeOperation::Copy); return compute(request, fence);
}
Result Client::compilerAddOne(const Buffer& source, uint64_t sourceOffset, const Buffer& destination,
                              uint64_t destinationOffset, Fence& fence) {
    std::lock_guard<std::mutex> lock(mutex_); fence = {};
    if (!connection_) return fail(Status::NotOpen);
    if (lost_) return fail(Status::DeviceLost);
    if (hello_.header.minor != a::kCompilerMinor ||
        (hello_.capabilities & a::kCompilerCapabilities) != a::kCompilerCapabilities || sealed_)
        return fail(Status::Unsupported);
    const auto* src = find(source); const auto* dst = find(destination);
    // The fixed class requires distinct retained BOs, even for disjoint ranges.
    if (!src || !dst || source.handle.id == destination.handle.id ||
        sourceOffset > src->allocation.bytes || a::kCompilerResultBytes > src->allocation.bytes - sourceOffset ||
        destinationOffset > dst->allocation.bytes || a::kCompilerResultBytes > dst->allocation.bytes - destinationOffset)
        return fail(Status::Invalid);
    a::CompilerComputeRequest request{header(sizeof(request)), source.handle, destination.handle,
        sourceOffset, destinationOffset, uint32_t(a::CompilerShaderClass::AppleAddOneGfx1100V1), 0};
    if (a::validateCompilerCompute(request, hello_) != a::Validation::Ok) return fail(Status::Invalid);
    a::SubmitReply reply{};
    // Nothing on this connection may mutate or retry after the RPC might have
    // reached hardware. A successful reply must carry the native GPU fence.
    sealed_ = true;
    auto result = exchange(a::Selector::CompilerCompute, &request, sizeof(request), &reply, sizeof(reply));
    if (!result) { lost_ = true; return result; }
    if (a::validateHeader(reply.header, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok ||
        reply.reserved || !reply.sequence || reply.sequence <= submitted_) { lost_ = true; return fail(Status::Protocol); }
    submitted_ = reply.sequence; resultBytes_ = a::kCompilerResultBytes;
    fence = {reply.sequence, hello_.header.epoch}; return ok();
}
Result Client::product(const ProductDispatch& dispatch, Fence& fence) {
    std::lock_guard<std::mutex> lock(mutex_); fence = {};
    if (!connection_) return fail(Status::NotOpen);
    if (lost_) return fail(Status::DeviceLost);
    const uint64_t required = reusable() ? a::kReusableCapabilities : a::kProductCapabilities;
    if ((hello_.header.minor != a::kProductMinor && !reusable()) ||
        (hello_.capabilities & required) != required || sealed_) return fail(Status::Unsupported);
    auto* retained = reusable() ? freeResult() : nullptr;
    if (reusable() && !retained) return fail(Status::OutOfMemory);
    if (!dispatch.variants || !dispatch.variantCount || dispatch.variantCount > a::kMaxProductVariants ||
        !dispatch.resources || !dispatch.resourceCount || dispatch.resourceCount > a::kMaxProductResources ||
        dispatch.resourceCount > hello_.maxResources) return fail(Status::Invalid);
    a::CapturedProductCompute captured{};
    auto& request = captured.request;
    const size_t bytes = sizeof(request) + dispatch.variantCount * sizeof(a::ProductVariant) +
        dispatch.resourceCount * sizeof(a::ProductResource);
    request.header = header(bytes);
    request.variantCount = dispatch.variantCount; request.resourceCount = dispatch.resourceCount;
    memcpy(request.grid, dispatch.grid, sizeof(request.grid));
    memcpy(request.nominalLocal, dispatch.nominalLocal, sizeof(request.nominalLocal));
    request.result = dispatch.result.handle; request.resultOffset = dispatch.resultOffset;
    request.resultBytes = dispatch.resultBytes;
    for (uint32_t n = 0; n < dispatch.variantCount; ++n) {
        const auto& source = dispatch.variants[n]; auto& target = captured.variants[n];
        const auto* record = find(source.code);
        if (!record || record->mapping.address || source.offset > record->allocation.bytes ||
            source.codeBytes > record->allocation.bytes - source.offset) return fail(Status::Invalid);
        target.code = source.code.handle; target.offset = source.offset;
        target.codeBytes = source.codeBytes; target.executableBytes = source.executableBytes;
        memcpy(target.local, source.local, sizeof(target.local)); target.waveSize = source.waveSize;
        target.numSgprs = source.numSgprs; target.numVgprs = source.numVgprs;
    }
    for (uint32_t n = 0; n < dispatch.resourceCount; ++n) {
        const auto& source = dispatch.resources[n]; auto& target = captured.resources[n];
        const auto* record = find(source.buffer);
        if (!record || source.offset > record->allocation.bytes ||
            source.bytes > record->allocation.bytes - source.offset) return fail(Status::Invalid);
        target.binding = source.binding; target.buffer = source.buffer.handle;
        target.offset = source.offset; target.bytes = source.bytes;
    }
    const auto* resultRecord = find(dispatch.result);
    if (!resultRecord || dispatch.resultOffset > resultRecord->allocation.bytes ||
        dispatch.resultBytes > resultRecord->allocation.bytes - dispatch.resultOffset ||
        a::validateProductCompute(captured, hello_.header.epoch, hello_.header.minor) != a::Validation::Ok)
        return fail(Status::Invalid);
    uint8_t message[sizeof(a::CapturedProductCompute)]{};
    memcpy(message, &request, sizeof(request));
    memcpy(message + sizeof(request), captured.variants, request.variantCount * sizeof(a::ProductVariant));
    memcpy(message + sizeof(request) + request.variantCount * sizeof(a::ProductVariant),
           captured.resources, request.resourceCount * sizeof(a::ProductResource));
    a::SubmitReply reply{};
    // Transport errors can occur after publication. Do not allow retry, BO
    // mutation or another dispatch; native close owns completion/reset cleanup.
    sealed_ = true;
    auto result = exchange(a::Selector::ProductCompute, message, bytes, &reply, sizeof(reply));
    if (!result) { lost_ = true; return result; }
    if (a::validateHeader(reply.header, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok ||
        reply.reserved || !reply.sequence || reply.sequence <= submitted_) { lost_ = true; return fail(Status::Protocol); }
    submitted_ = reply.sequence; resultBytes_ = request.resultBytes;
    if (retained) { *retained = {reply.sequence, request.resultBytes}; sealed_ = false; }
    fence = {reply.sequence, hello_.header.epoch}; return ok();
}
Result Client::graphicsInformation(HNColorTopology& topology) {
    std::lock_guard<std::mutex> lock(mutex_); topology = {};
    if (!connection_) return fail(Status::NotOpen);
    if (lost_) return fail(Status::DeviceLost);
    if (!a::graphicsProfile(hello_.header.minor) || sealed_ ||
        (hello_.capabilities & a::kGraphicsCapabilities) != a::kGraphicsCapabilities) return fail(Status::Unsupported);
    const auto request = header(sizeof(a::Header)); a::GraphicsInformationReply reply{};
    const auto result = exchange(a::Selector::GraphicsInformation,&request,sizeof(request),&reply,sizeof(reply));
    if (!result) { lost_ = true; return result; }
    if (a::validateGraphicsInformation(reply,hello_) != a::Validation::Ok) {
        lost_ = true; return fail(Status::Protocol);
    }
    topology = reply.topology; return ok();
}
Result Client::graphicsProduct(const ProductDraw& draw, Fence& fence) {
    return graphicsProduct(draw,nullptr,0,nullptr,0,fence);
}
Result Client::graphicsProduct(const BufferedProductDraw& draw, Fence& fence) {
    return graphicsProduct(draw.draw,&draw.fragment,draw.fragmentOffset,nullptr,0,fence);
}
Result Client::graphicsProduct(const VertexBufferedProductDraw& draw,Fence& fence) {
    return graphicsProduct(draw.draw,nullptr,0,&draw.vertex,draw.vertexOffset,fence);
}
Result Client::graphicsProduct(const VertexFragmentBufferedProductDraw& draw,Fence& fence) {
    return graphicsProduct(draw.draw,&draw.fragment,draw.fragmentOffset,&draw.vertex,draw.vertexOffset,fence);
}
Result Client::graphicsProduct(const ProductDraw& draw,const Buffer* fragment,uint64_t fragmentOffset,
    const Buffer* vertex,uint64_t vertexOffset,Fence& fence) {
    std::lock_guard<std::mutex> lock(mutex_); fence = {};
    if (!connection_) return fail(Status::NotOpen);
    if (lost_) return fail(Status::DeviceLost);
    if (!a::graphicsProfile(hello_.header.minor) || sealed_ ||
        (hello_.capabilities & a::kGraphicsCapabilities) != a::kGraphicsCapabilities) return fail(Status::Unsupported);
    if(fragment&&(!a::bufferedGraphicsProfile(hello_.header.minor)||
        (hello_.capabilities&a::kBufferedGraphicsCapabilities)!=a::kBufferedGraphicsCapabilities))return fail(Status::Unsupported);
    if(vertex&&(hello_.header.minor!=a::kVertexBufferedGraphicsMinor||
        (hello_.capabilities&a::kVertexBufferedGraphicsCapabilities)!=a::kVertexBufferedGraphicsCapabilities))return fail(Status::Unsupported);
    auto* retained = freeResult(); if (!retained) return fail(Status::OutOfMemory);
    const auto* code = find(draw.code); const auto* target = find(draw.target);
    if (!code || !target || code->mapping.address || target->mapping.address ||
        draw.codeOffset > code->allocation.bytes || a::kGraphicsImageBytes > code->allocation.bytes-draw.codeOffset ||
        draw.targetOffset > target->allocation.bytes || draw.targetLayout.total_bytes > target->allocation.bytes-draw.targetOffset)
        return fail(Status::Invalid);
    if(fragment) {
        const auto* source=find(*fragment);
        if(!source||source->mapping.address||fragmentOffset>source->allocation.bytes||
            source->allocation.bytes-fragmentOffset<16)return fail(Status::Invalid);
    }
    if(vertex) {
        const auto* source=find(*vertex);
        if(!source||source->mapping.address||vertexOffset>source->allocation.bytes||
            source->allocation.bytes-vertexOffset<48)return fail(Status::Invalid);
    }
    a::ProductBufferedGraphicsRequest buffered{};
    a::ProductVertexFragmentBufferedGraphicsRequest vertexBuffered{};
    auto& request=vertex?vertexBuffered.vertexDraw.draw:buffered.draw;
    const size_t requestBytes=vertex?(fragment?sizeof(vertexBuffered):sizeof(vertexBuffered.vertexDraw)):
        (fragment?sizeof(buffered):sizeof(request));
    const void* wire=vertex?static_cast<const void*>(&vertexBuffered):static_cast<const void*>(&buffered);
    request.header=header(requestBytes);
    request.code = draw.code.handle; request.codeOffset = draw.codeOffset;
    request.vertexBytes = draw.vertexBytes; request.fragmentBytes = draw.fragmentBytes;
    request.target = draw.target.handle; request.targetOffset = draw.targetOffset; request.targetLayout = draw.targetLayout;
    if(vertex) {
        vertexBuffered.vertexDraw.vertex=vertex->handle;vertexBuffered.vertexDraw.vertexOffset=vertexOffset;
        vertexBuffered.vertexDraw.vertexBufferBytes=48;
        if(fragment){vertexBuffered.fragment=fragment->handle;vertexBuffered.fragmentOffset=fragmentOffset;
            vertexBuffered.fragmentBufferBytes=16;}
    } else if(fragment){buffered.fragment=fragment->handle;buffered.fragmentOffset=fragmentOffset;buffered.fragmentBytes=16;}
    if ((vertex?(fragment?a::validateProductVertexFragmentBufferedGraphics(vertexBuffered,hello_.header.epoch,hello_.header.minor):
        a::validateProductVertexBufferedGraphics(vertexBuffered.vertexDraw,hello_.header.epoch,hello_.header.minor)):
        fragment?a::validateProductBufferedGraphics(buffered,hello_.header.epoch,hello_.header.minor):
        a::validateProductGraphics(request,hello_.header.epoch,hello_.header.minor)) != a::Validation::Ok)
        return fail(Status::Invalid);
    // The RPC may publish DMA before a transport failure becomes visible.
    // Only successful native completion + consumption reopens this connection.
    sealed_ = true; a::SubmitReply reply{};
    const auto selector=vertex?(fragment?a::Selector::ProductVertexFragmentBufferedGraphics:a::Selector::ProductVertexBufferedGraphics):
        (fragment?a::Selector::ProductBufferedGraphics:a::Selector::ProductGraphics);
    auto result = exchange(selector,wire,requestBytes,&reply,sizeof(reply));
    if (!result) { lost_ = true; return result; }
    if (a::validateHeader(reply.header,sizeof(reply),hello_.header.epoch,hello_.header.minor) != a::Validation::Ok ||
        reply.reserved || !reply.sequence || reply.sequence <= submitted_) { lost_ = true; return fail(Status::Protocol); }
    submitted_ = reply.sequence; resultBytes_ = request.targetLayout.total_bytes;
    *retained = {reply.sequence,resultBytes_}; sealed_ = false;
    fence = {reply.sequence,hello_.header.epoch}; return ok();
}
Result Client::readResult(Fence fence, uint64_t offset, void* data, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_) return fail(Status::NotOpen);
    if (!(hello_.capabilities & a::BuiltInCompute)) return fail(Status::Unsupported);
    if (!data || !bytes || !fence.sequence || fence.epoch != hello_.header.epoch) return fail(Status::Invalid);
    const auto* retained = reusable() ? findResult(fence) : nullptr;
    if (reusable() && !retained) return fail(Status::NotFound);
    const uint64_t resultBytes = retained ? retained->bytes : resultBytes_;
    if ((!reusable() && (fence.sequence != submitted_ || !sealed_)) ||
        offset > resultBytes || bytes > resultBytes - offset) return fail(Status::Invalid);
    auto* cursor = static_cast<uint8_t*>(data);
    while (bytes) {
        const uint32_t chunk = uint32_t(bytes > a::kMaxCopyBytes ? a::kMaxCopyBytes : bytes);
        a::ReadResultRequest request{header(sizeof(request)), fence.sequence, offset, chunk, 0};
        uint8_t output[sizeof(a::DataReply) + a::kMaxCopyBytes]{};
        const size_t outBytes = sizeof(a::DataReply) + chunk;
        auto result = exchange(a::Selector::ReadResult, &request, sizeof(request), output, outBytes);
        if (!result) return result;
        a::DataReply reply{}; memcpy(&reply, output, sizeof(reply));
        if (a::validateHeader(reply.header, outBytes, hello_.header.epoch, hello_.header.minor) != a::Validation::Ok ||
            reply.bytes != chunk || reply.reserved) { lost_ = true; return fail(Status::Protocol); }
        memcpy(cursor, output + sizeof(reply), chunk); bytes -= chunk; offset += chunk; cursor += chunk;
    }
    return ok();
}
Result Client::releaseResult(Fence& fence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_) return fail(Status::NotOpen);
    if (!reusable()) return fail(Status::Unsupported);
    if (!fence.sequence || fence.epoch != hello_.header.epoch) return fail(Status::Invalid);
    auto* retained = findResult(fence); if (!retained) return fail(Status::NotFound);
    a::ReleaseResultRequest request{header(sizeof(request)), fence.sequence, 0};
    a::Header reply{};
    auto result = exchange(a::Selector::ReleaseResult, &request, sizeof(request), &reply, sizeof(reply));
    if (!result) {
        if (result.status != Status::NotFound) lost_ = true;
        return result;
    }
    if (a::validateHeader(reply, sizeof(reply), hello_.header.epoch, hello_.header.minor) != a::Validation::Ok) {
        lost_ = true; return fail(Status::Protocol);
    }
    *retained = {}; fence = {}; return ok();
}
} }
