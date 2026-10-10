# SPDX-License-Identifier: MIT
"""Capture only the actual C helpers identified in the two initializer bodies.

Execute after measure-metal-initializers.py without its final print. SYMBOLS
and EXPECTED are embedded by the local runner and bounded here.
"""
if not 1 <= len(SYMBOLS) <= 6:
    raise RuntimeError("Helper count bound")
for name, expected in EXPECTED.items():
    actual = report["methods"][name]
    if actual["sha256"] != expected["sha256"] or actual["image_uuid"] != expected["image_uuid"]:
        raise RuntimeError("Actual method/image changed since disassembly")
report["helpers"] = {}
for name in SYMBOLS:
    address = c.cast(getattr(ioaccel, name), c.c_void_p).value
    loc = location(address)
    if not loc["resolved"] or "/IOAccelerator.framework/" not in loc["image"] or loc["symbol_offset"] != 0:
        raise RuntimeError("Unexpected helper symbol/image")
    base = loc["base"]
    if base not in layouts:
        layouts[base] = image_layout(base)
    layout = layouts[base]
    section = next((s for s in layout["sections"] if s["segment"] == "__TEXT" and s["name"] == "__text"
                    and s["address"] <= address < s["address"] + s["bytes"]), None)
    if not section:
        raise RuntimeError("Helper is outside named executable text section")
    matched = address in layout["functions"]
    following = next((f for f in layout["functions"] if f > address), None)
    if not matched or not following:
        raise RuntimeError("Cannot establish exact helper function bounds")
    end = min(following, section["address"] + section["bytes"])
    data = c.string_at(address, min(end - address, 8192))
    report["helpers"][name] = dict(loc, implementation=address, image_uuid=layout["uuid"],
                                   bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                                   function_start_matched=matched, function_end_bounded=True,
                                   truncated=len(data) < end - address, code_hex=data.hex())
report["helper_images"] = {
    str(base): {key: value for key, value in layout.items() if key != "functions"}
    for base, layout in layouts.items()
}
report["helper_limits"] = {"functions": 6, "bytes_per_function": 8192}
print(json.dumps(report, indent=2, sort_keys=True))
