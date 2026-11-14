#!/usr/bin/env python3
"""The changes of the library to the connectedhomeip tree (both build scripts run it, it may run again).

- src/platform/ESP32/BUILD.gn: with chip_mdns = "platform" the ESP32 platform brings its DNS-SD on the mdns
  component of ESP-IDF (DnssdImpl.cpp, ESP32DnssdImpl.cpp). The library's own platform DNS-SD (gn/platform/
  EspurnaDnssd.cpp: ESPurna's one mDNS responder) takes its place on both chips: those sources go.

  python3 tools/chip_patch.py <connectedhomeip>
"""

import os
import re
import sys


def esp32(chip):
    path = os.path.join(chip, "src", "platform", "ESP32", "BUILD.gn")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    patched = re.sub(r'\n\s*if \(chip_mdns == "platform"\) \{\s*sources \+= \[ "DnssdImpl\.cpp" \]\s*\}', "\n", text)
    patched = re.sub(r'\n\s*if \(chip_mdns == "platform"\) \{\s*sources \+= \[\s*"ESP32DnssdImpl\.cpp",\s*'
                     r'"ESP32DnssdImpl\.h",\s*\]\s*\}', "\n", patched)
    if "DnssdImpl.cpp" in patched:
        raise SystemExit(f"{path}: the DNS-SD sources of the ESP32 platform are still there (a new layout?)")
    if patched != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(patched)
        print(f"chip_patch: {path}: the DNS-SD of ESP-IDF removed")


def main():
    esp32(sys.argv[1])


if __name__ == "__main__":
    main()
