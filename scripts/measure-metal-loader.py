#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fragment after measure-metal-initializers.py's definitions, before report.

Reads code for named discovery exports and at most four code-derived offsets.
Never invokes discovery, Objective-C messages, IOKit or GPU operations. The
local runner embeds EXTRA_OFFSETS and EXPECTED_CODE; stdin leaves no Mac files.
"""
EXTRA_OFFSETS = globals().get("EXTRA_OFFSETS", [])
EXPECTED_CODE = globals().get("EXPECTED_CODE", {})
if len(EXTRA_OFFSETS) > 4 or len(set(EXTRA_OFFSETS)) != len(EXTRA_OFFSETS):
    raise RuntimeError("Loader follow-up bound or duplicate offset")
report = {"system": subprocess.check_output(["sw_vers"], text=True, timeout=10).strip(),
          "methods": {}, "helpers": {}, "images": {},
          "limits": {"named_functions": 2, "extra_functions": 4,
                     "bytes_per_function": 8192, "total_code_bytes": 24576},
          "device_enumerated": False, "device_instantiated": False,
          "user_client_opened": False, "objc_messages_sent": False,
          "gpu_submitted": False, "remote_files_created": False}
layouts = {}
named = {name: c.cast(getattr(metal, name), c.c_void_p).value
         for name in ("MTLCopyAllDevices", "MTLCreateSystemDefaultDevice")}
base = location(named["MTLCopyAllDevices"])["base"]
requests = list(named.items()) + [("loader_at_" + hex(offset), base + offset)
                                for offset in EXTRA_OFFSETS]
total = 0
for name, address in requests:
    loc = location(address)
    if not loc["resolved"] or "/Metal.framework/" not in loc["image"] or loc["base"] != base:
        raise RuntimeError("Unexpected loader image")
    if base not in layouts:
        layouts[base] = image_layout(base)
    layout = layouts[base]
    if layout["uuid"] != "c3efbabe-6b06-33ab-bc45-43fe86297aa1":
        raise RuntimeError("Metal image changed since initializer measurement")
    section = next((s for s in layout["sections"] if s["segment"] == "__TEXT"
                    and s["name"] == "__text"
                    and s["address"] <= address < s["address"] + s["bytes"]), None)
    following = next((f for f in layout["functions"] if f > address), None)
    if not section or address not in layout["functions"] or not following:
        raise RuntimeError("Loader function bounds unavailable")
    end = min(following, section["address"] + section["bytes"])
    count = end - address
    if count > 8192 or total + count > 24576:
        report["helpers"][name] = dict(loc, skipped="code bound", declared_bytes=count)
        continue
    data = c.string_at(address, count)
    digest = hashlib.sha256(data).hexdigest()
    if name in EXPECTED_CODE and digest != EXPECTED_CODE[name]:
        raise RuntimeError("Previously captured loader function changed")
    total += count
    report["helpers"][name] = dict(loc, implementation=address,
        image_uuid=layout["uuid"], bytes=count, sha256=digest,
        function_start_matched=True, function_end_bounded=True,
        truncated=False, code_hex=data.hex())
report["total_code_bytes"] = total
report["images"] = {str(key): {k: v for k, v in value.items() if k != "functions"}
                    for key, value in layouts.items()}
print(json.dumps(report, indent=2, sort_keys=True))
