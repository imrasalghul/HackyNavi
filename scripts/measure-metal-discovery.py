#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded macOS metadata only; no device creation, client open, or GPU calls.

Run by passing this source on stdin to the Mac's python3. It writes JSON only
to stdout and creates no remote files. Export presence is not an ABI contract.
"""
import ctypes as c
import hashlib
import json
from pathlib import Path
import platform
import plistlib
import re
import subprocess


def command(argv):
    return subprocess.check_output(argv, text=True, timeout=10).strip()


if platform.system() != "Darwin":
    raise SystemExit("This metadata reader requires the actual macOS runtime")

sdk = Path(command(["xcrun", "--sdk", "macosx", "--show-sdk-path"]))
report = {
    "system": command(["sw_vers"]), "architecture": platform.machine(),
    "sdk_path": str(sdk),
    "sdk_version": command(["xcrun", "--sdk", "macosx", "--show-sdk-version"]),
    "headers": {}, "runtime": {}, "libraries": {}, "kernel_bundles": {},
    "limits": {"header_bytes": 2 * 1024 * 1024, "binary_bytes": 32 * 1024 * 1024,
               "class_members": 4096, "selected_header_lines": 40},
    "device_enumerated": False, "device_instantiated": False,
    "user_client_opened": False, "gpu_submitted": False,
    "remote_files_created": False,
}

headers = {
    "System/Library/Frameworks/IOKit.framework/Headers/graphics/IOAccelerator.h":
        r"class IOAccelerator|createAccelID|retainAccelID|releaseAccelID",
    "System/Library/Frameworks/IOKit.framework/Headers/graphics/IOAccelClientConnect.h":
        r"kIOAcceleratorClassName|kIOAccel.*ClientType|Private client",
    "System/Library/Frameworks/Kernel.framework/Headers/IOKit/graphics/IOAccelerator.h":
        r"class IOAccelerator|createAccelID|retainAccelID|releaseAccelID",
    "System/Library/Frameworks/Kernel.framework/Headers/IOKit/IOService.h":
        r"newUserClient|registerService|callPlatformFunction",
    "System/Library/Frameworks/Kernel.framework/Headers/IOKit/IOUserClient.h":
        r"initWithTask|clientClose|clientDied|externalMethod|clientMemoryForType",
}
for relative, pattern in headers.items():
    path = sdk / relative
    item = {"exists": path.is_file()}
    if path.is_file():
        item["bytes"] = path.stat().st_size
        if item["bytes"] <= report["limits"]["header_bytes"]:
            data = path.read_bytes()
            item["sha256"] = hashlib.sha256(data).hexdigest()
            item["matches"] = [
                {"line": i + 1, "text": line.strip()[:500]}
                for i, line in enumerate(data.decode().splitlines()) if re.search(pattern, line)
            ][:40]
        else:
            item["skipped"] = "size bound"
    report["headers"][str(path)] = item

loaded = []
for name in ("/System/Library/Frameworks/Foundation.framework/Foundation",
             "/System/Library/Frameworks/Metal.framework/Metal",
             "/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator"):
    try:
        lib = c.CDLL(name)
        loaded.append(lib)
        report["libraries"][name] = {"loaded": True}
    except OSError as error:
        report["libraries"][name] = {"loaded": False, "error": str(error)[:1000]}

objc = c.CDLL("/usr/lib/libobjc.A.dylib")
signatures = {
    "objc_getClass": ([c.c_char_p], c.c_void_p),
    "class_getSuperclass": ([c.c_void_p], c.c_void_p),
    "class_getName": ([c.c_void_p], c.c_char_p),
    "class_getInstanceSize": ([c.c_void_p], c.c_size_t),
    "class_getImageName": ([c.c_void_p], c.c_char_p),
    "class_copyMethodList": ([c.c_void_p, c.POINTER(c.c_uint)], c.POINTER(c.c_void_p)),
    "method_getName": ([c.c_void_p], c.c_void_p),
    "sel_getName": ([c.c_void_p], c.c_char_p),
    "method_getTypeEncoding": ([c.c_void_p], c.c_char_p),
    "class_copyIvarList": ([c.c_void_p, c.POINTER(c.c_uint)], c.POINTER(c.c_void_p)),
    "ivar_getName": ([c.c_void_p], c.c_char_p),
    "ivar_getTypeEncoding": ([c.c_void_p], c.c_char_p),
    "ivar_getOffset": ([c.c_void_p], c.c_ssize_t),
}
for name, (args, result) in signatures.items():
    function = getattr(objc, name)
    function.argtypes, function.restype = args, result
libc = c.CDLL(None)
libc.free.argtypes, libc.free.restype = [c.c_void_p], None


def string(value):
    return value.decode("utf-8", errors="replace") if value else None


selectors = {
    "initWithAcceleratorPort:", "supportLazyInitialization", "lazyInitialize",
    "initLimits", "featureProfile", "llvmVersion", "deviceRef", "sharedRef",
    "acceleratorPort", "acceleratorService", "registryID", "dealloc",
}
ivars = {"_acceleratorPort", "_acceleratorService", "_deviceRef", "_sharedRef",
         "_sharedMemorySize", "_deviceInfo", "_featureProfile"}
for name in ("MTLIOAccelDevice", "_MTLDevice"):
    cls = objc.objc_getClass(name.encode())
    item = {"present": bool(cls), "selected_methods": {}, "selected_ivars": {}}
    if cls:
        item["instance_bytes"] = objc.class_getInstanceSize(cls)
        item["image"] = string(objc.class_getImageName(cls))
        parent = objc.class_getSuperclass(cls)
        item["superclass"] = string(objc.class_getName(parent)) if parent else None
        current, depth = cls, 0
        while current and depth < 8:
            owner = string(objc.class_getName(current))
            for copy, target, wanted, name_of in (
                (objc.class_copyMethodList, item["selected_methods"], selectors,
                 lambda p: string(objc.sel_getName(objc.method_getName(p)))),
                (objc.class_copyIvarList, item["selected_ivars"], ivars,
                 lambda p: string(objc.ivar_getName(p))),
            ):
                count = c.c_uint()
                members = copy(current, c.byref(count))
                try:
                    if count.value > 4096:
                        raise RuntimeError("Unexpected runtime member count")
                    for i in range(count.value):
                        member = members[i]
                        member_name = name_of(member)
                        if member_name in wanted and member_name not in target:
                            if target is item["selected_methods"]:
                                target[member_name] = {"owner": owner, "encoding": string(objc.method_getTypeEncoding(member))}
                            else:
                                target[member_name] = {"owner": owner, "encoding": string(objc.ivar_getTypeEncoding(member)),
                                                       "offset": objc.ivar_getOffset(member)}
                finally:
                    libc.free(members)
            current, depth = objc.class_getSuperclass(current), depth + 1
        item["missing_selected_methods"] = sorted(selectors - item["selected_methods"].keys())
        item["missing_selected_ivars"] = sorted(ivars - item["selected_ivars"].keys())
    report["runtime"][name] = item

# Named export lookup only. No function is invoked and no private signature is guessed.
report["named_exports"] = {}
for name in ("IOAccelDeviceCreate", "IOAccelDeviceDestroy", "IOAccelDeviceGetConfig",
             "IOAccelDeviceGetName", "IOAccelDeviceGetEventMachine",
             "IOAccelSharedCreate", "IOAccelSharedDestroy", "IOAccelContextCreate"):
    present_in = []
    for path, library in zip([p for p, value in report["libraries"].items() if value["loaded"]], loaded):
        try:
            getattr(library, name)
            present_in.append(path)
        except AttributeError:
            pass
    report["named_exports"][name] = {"found_via_handles": present_in,
                                     "declaration_or_signature_verified": False}

for name in ("IOAcceleratorFamily2", "IOGraphicsFamily"):
    bundle = Path("/System/Library/Extensions") / (name + ".kext")
    binary = bundle / "Contents/MacOS" / name
    info = bundle / "Contents/Info.plist"
    item = {"bundle_exists": bundle.is_dir(), "binary_exists": binary.is_file()}
    if info.is_file() and info.stat().st_size <= 256 * 1024:
        data = info.read_bytes()
        parsed = plistlib.loads(data)
        item["info_sha256"] = hashlib.sha256(data).hexdigest()
        item["bundle_id"] = parsed.get("CFBundleIdentifier")
        item["bundle_version"] = parsed.get("CFBundleVersion")
        item["bundle_short_version"] = parsed.get("CFBundleShortVersionString")
    if binary.is_file():
        item["binary_bytes"] = binary.stat().st_size
        if item["binary_bytes"] <= report["limits"]["binary_bytes"]:
            item["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        else:
            item["skipped"] = "size bound"
    report["kernel_bundles"][name] = item

print(json.dumps(report, indent=2, sort_keys=True))
