# SPDX-License-Identifier: MIT
"""Second-stage fragment for measure-metal-initializers.py's verified globals.

The local runner embeds a bounded REQUESTS list and EXPECTED method hashes.
This never calls the discovered function pointers. It reads named image data
and asks only Objective-C/dyld metadata APIs to describe those references.
"""
if len(REQUESTS) > 128:
    raise RuntimeError("Too many explicit references")
for name, expected in EXPECTED.items():
    actual = report["methods"][name]
    if actual["sha256"] != expected["sha256"] or actual["image_uuid"] != expected["image_uuid"]:
        raise RuntimeError("Actual method/image changed since disassembly")
objc.sel_getName.argtypes, objc.sel_getName.restype = [c.c_void_p], c.c_void_p
objc.class_getName.argtypes, objc.class_getName.restype = [c.c_void_p], c.c_void_p
system.mach_vm_region.argtypes = [c.c_uint, c.POINTER(c.c_uint64), c.POINTER(c.c_uint64),
                                 c.c_int, c.POINTER(c.c_int), c.POINTER(c.c_uint), c.POINTER(c.c_uint)]
system.mach_vm_region.restype = c.c_int
system.mach_port_deallocate.argtypes, system.mach_port_deallocate.restype = [c.c_uint, c.c_uint], c.c_int
own_task = c.c_uint.in_dll(system, "mach_task_self_").value


def bounded_pointer_region(address):
    # Public mach_vm_region ABI; VM_REGION_BASIC_INFO_64 is 9 and its
    # pack(4) 36-byte structure has 9 integer words (mach/vm_region.h).
    start, size, count, object_name = c.c_uint64(address), c.c_uint64(), c.c_uint(9), c.c_uint()
    info = (c.c_int * 9)()
    kr = system.mach_vm_region(own_task, c.byref(start), c.byref(size), 9,
                               info, c.byref(count), c.byref(object_name))
    released = None
    if object_name.value:
        released = system.mach_port_deallocate(own_task, object_name.value)
    result = {"query_status": kr, "object_name_release_status": released,
              "address": start.value, "bytes": size.value, "info_words": count.value}
    readable = (kr == 0 and count.value == 9 and info[0] & 1
                and start.value <= address and address + 8 <= start.value + size.value)
    if kr == 0:
        result["protection"] = info[0]
    if readable:
        result["word"] = struct.unpack("<Q", c.string_at(address, 8))[0]
    return result


def bounded_cstring(pointer, maximum=192):
    # Only known runtime-returned names enter this function; stop at the NUL,
    # so a valid name ending at a page boundary never reads the next page.
    if not pointer:
        return {"text": None, "terminated": False}
    result = bytearray()
    for i in range(maximum):
        value = c.string_at(pointer + i, 1)
        if value == b"\0":
            return {"text": result.decode("utf-8", errors="replace"), "terminated": True}
        result.extend(value)
    return {"text": result.decode("utf-8", errors="replace"), "terminated": False}


def described(address):
    loc = location(address)
    if not loc["resolved"]:
        return loc, None, None
    base = loc["base"]
    if base not in layouts:
        if len(layouts) >= 16:
            return dict(loc, skipped="image count bound"), None, None
        layouts[base] = image_layout(base)
    layout = layouts[base]
    loc["image_uuid"] = layout["uuid"]
    section = next((s for s in layout["sections"] if s["address"] <= address < s["address"] + s["bytes"]), None)
    segment = next((s for s in layout["segments"] if s["address"] <= address < s["address"] + s["bytes"]), None)
    if section:
        loc["section"] = section["name"]
        loc["segment"] = section["segment"]
    return loc, section, segment


def reference(address, allow_stub=True):
    loc, section, segment = described(address)
    result = {"address": address, "location": loc}
    if not section or not segment or not (segment["initprot"] & 1):
        # The dyld shared cache coalesces some GOT slots outside individual
        # images. Read only this explicit code-derived 8-byte location after
        # checking this process's actual readable VM region; never enumerate.
        region = bounded_pointer_region(address)
        result["vm_region"] = region
        if "word" in region:
            result["word"] = region["word"]
            if region["word"]:
                result["pointee_location"] = location(region["word"])
        else:
            result["skipped"] = "No verified readable 8-byte region"
        return result
    remaining = section["address"] + section["bytes"] - address
    if section["name"] == "__stubs" and allow_stub:
        data = c.string_at(address, min(remaining, 16))
        result["stub_hex"] = data.hex()
        if data[:2] == b"\xff\x25" and len(data) >= 6:
            target = address + 6 + int.from_bytes(data[2:6], "little", signed=True)
            result["stub_pointer"] = reference(target, False)
        return result
    if section["name"] in ("__cstring", "__objc_methname", "__objc_classname"):
        data = c.string_at(address, min(remaining, 192))
        value, separator, ignored = data.partition(b"\0")
        result["string"] = {"text": value.decode("utf-8", errors="replace"), "terminated": bool(separator)}
        return result
    pointer_sections = {"__got", "__auth_got", "__objc_ivar", "__objc_selrefs", "__objc_classrefs",
                        "__objc_superrefs", "__nl_symbol_ptr", "__la_symbol_ptr", "__const"}
    if section["name"] not in pointer_sections or remaining < 8:
        result["skipped"] = "Reference is not a selected pointer/string section"
        return result
    pointer = struct.unpack("<Q", c.string_at(address, 8))[0]
    result["word"] = pointer
    if section["name"] == "__objc_ivar":
        result["kind"] = "ivar offset value; not dereferenced"
    elif pointer:
        result["pointee_location"] = location(pointer)
        if section["name"] == "__objc_selrefs":
            result["selector"] = bounded_cstring(objc.sel_getName(pointer))
        elif section["name"] in ("__objc_classrefs", "__objc_superrefs"):
            result["class_name"] = bounded_cstring(objc.class_getName(pointer))
    return result


if "EXPECTED_HELPERS" in globals():
    for name, expected in EXPECTED_HELPERS.items():
        actual = report["helpers"][name]
        if actual["sha256"] != expected["sha256"] or actual["image_uuid"] != expected["image_uuid"]:
            raise RuntimeError("Actual helper/image changed since disassembly")
metal_base = (report["helpers"][REFERENCE_HELPER]["base"] if "REFERENCE_HELPER" in globals()
              else report["methods"]["initWithAcceleratorPort:"]["base"])
report["explicit_references"] = {
    str(item["target_image_offset"]): reference(metal_base + item["target_image_offset"])
    for item in REQUESTS
}
report["reference_limits"] = {"request_count": len(REQUESTS), "max_requests": 128,
                               "max_images": 16, "pointer_bytes": 8, "string_bytes": 192,
                               "max_stub_depth": 1, "stub_bytes": 16}
report["referenced_images"] = {
    str(base): {"uuid": value["uuid"], "header_sha256": value["header_sha256"]}
    for base, value in layouts.items()
}
print(json.dumps(report, indent=2, sort_keys=True))
