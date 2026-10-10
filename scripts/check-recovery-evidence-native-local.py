#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual immutable recovery evidence against real kernel headers.

No IOKit objects, recovery commands, or graphics operations are executed.
"""
import importlib.util
from pathlib import Path

helper = Path(__file__).with_name("check-psp-display-ta-native-local.py")
spec = importlib.util.spec_from_file_location("native_header_validation", helper)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.SOURCES = ["kext/HackyNavi/" + name + ".cpp" for name in
                  ("HackyNaviResetController", "HackyNaviLifecycleController", "HackyNaviAtomController")]

if __name__ == "__main__":
    module.main()
