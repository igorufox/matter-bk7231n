#!/usr/bin/env python3
# The package of one chip ESPurna takes (tools/ci_build.sh for the release, tools/build.sh for a local build):
#
#   tools/package.py <chip> <matter-espurna> <connectedhomeip> <gn out dir> <libMatterApp.a> <package dir> [<zip>]
#
# <chip>: bk7231n or esp32
#   lib/libMatterApp.a
#   include/lwipopts.h          bk7231n: lwIP options the library was built with (ESPurna builds the SDK's lwIP with
#                               them); the ESP32 takes lwIP as ESP-IDF has it
#   include/chip/<root>/...     the headers of the library's API, each <root> one include folder:
#     src, include              connectedhomeip src/ and src/include/ (headers only)
#     app-common                connectedhomeip zzz_generated/app-common (the generated cluster headers:
#                               app-common/zap-generated/cluster-enums.h ...)
#     nlassert, nlio            the two header libraries of connectedhomeip's third_party
#     gen                       the build configuration headers GN wrote (gen/include of the build)
#     gn                        app/*.h (the project config) and, bk7231n, external_platform/BK7231N/*.h of this
#                               repository (the ESP32 platform headers are connectedhomeip's own, in src)
#   include/chip/flags.txt      "-I<root>" lines (relative to include/chip) and the "-D" defines of the library's code
#
# ESPurna's code on the Matter API (the identity of the board, matter_identity.cpp of the fork: one code with the
# ESP32) is compiled against exactly these headers and defines, so it sees the classes as the library was built.
# The zip has a fixed member order and timestamps: the same bytes for the same inputs.

import os
import shutil
import sys
import zipfile

HEADERS = (".h", ".hpp", ".ipp", ".inc", ".def")

# what the headers of the API need (the build configuration headers of gen/); the rest of the library's defines
# (minimal mDNS, the BDK's CONFIG_IPV6) only concern its own sources
DEFINES = ("CHIP_HAVE_CONFIG_H=1",)


def copy_headers(src, dst):
    for root, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(files):
            if name.endswith(HEADERS):
                path = os.path.join(root, name)
                target = os.path.join(dst, os.path.relpath(path, src))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(path, target)


def main(target, m, chip, out, lib, pkg, archive=None):
    if target not in ("bk7231n", "esp32"):
        raise SystemExit(f"package.py: unknown chip {target}")

    shutil.rmtree(pkg, ignore_errors=True)
    os.makedirs(os.path.join(pkg, "lib"))
    shutil.copyfile(lib, os.path.join(pkg, "lib", "libMatterApp.a"))
    os.makedirs(os.path.join(pkg, "include"))
    if target == "bk7231n":
        shutil.copyfile(os.path.join(m, "lwip-override", "lwipopts.h"), os.path.join(pkg, "include", "lwipopts.h"))

    headers = os.path.join(pkg, "include", "chip")
    roots = {
        "src": os.path.join(chip, "src"),
        "include": os.path.join(chip, "src", "include"),
        "app-common": os.path.join(chip, "zzz_generated", "app-common"),
        "nlassert": os.path.join(chip, "third_party", "nlassert", "repo", "include"),
        "nlio": os.path.join(chip, "third_party", "nlio", "repo", "include"),
        "gen": os.path.join(out, "gen", "include"),
    }
    for name, src in roots.items():
        copy_headers(src, os.path.join(headers, name))

    gn = os.path.join(headers, "gn")
    copy_headers(os.path.join(m, "gn", "app"), os.path.join(gn, "app"))
    if target == "bk7231n":
        copy_headers(os.path.join(m, "gn", "external_platform", "BK7231N"),
                     os.path.join(gn, "external_platform", "BK7231N"))

    with open(os.path.join(headers, "flags.txt"), "w", newline="\n") as f:
        for name in (*roots.keys(), "gn"):
            f.write(f"-I{name}\n")
        for define in DEFINES:
            f.write(f"-D{define}\n")

    if not archive:
        return

    names = []
    for root, dirs, files in os.walk(pkg):
        dirs.sort()
        for name in sorted(files):
            names.append(os.path.relpath(os.path.join(root, name), pkg).replace(os.sep, "/"))

    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in names:
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            with open(os.path.join(pkg, name), "rb") as f:
                z.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


if __name__ == "__main__":
    main(*sys.argv[1:])
