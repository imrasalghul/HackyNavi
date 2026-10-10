#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual function-group acquisition evidence with real kernel headers."""
import importlib.util
from pathlib import Path

helper = Path(__file__).with_name("check-psp-display-ta-native-local.py")
spec = importlib.util.spec_from_file_location("native_header_validation", helper)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.SOURCES = ["kext/HackyNavi/" + name + ".cpp" for name in
                  ("HackyNaviFunctionGroup", "HackyNaviDeviceEngines")]

if __name__ == "__main__":
    module.main()
