# matter-espurna

Matter (connectedhomeip) for ESPurna on two chips, the Beken BK7231N (LibreTiny) and the ESP32 (Arduino-ESP32 3.3,
ESP-IDF 5.5), as a static library per chip: `libMatterApp.a` with On/Off plugs, one per relay (`gn/app/plug.zap`), and
a small C API that ESPurna calls (`espurna_matter.h` of the ESPurna fork). The same connectedhomeip, data model and
application on both chips; only the platform of connectedhomeip under it is the chip's. (Until 2026-11-12 the
repository was `matter-bk7231n`, the library of the BK7231N alone; the ESP32 build of ESPurna had the Matter library of
Arduino-ESP32, esp_matter, with another data model.)

- `gn/` - own GN root, one for both chips (`.gn`; the chip's arguments come with `gn gen`: `chips/bk7231n.gni`,
  `chips/esp32.gni`), `toolchain/` (arm-none-eabi for the BK7231N, xtensa-esp32-elf for the ESP32), the application
  (`app/`: data model, `matter_app.cpp` with the C API, `AppServer`, `ProjectConfig.h`) and the platforms:
  - BK7231N: our port `external_platform/BK7231N/` (derived from the Beken platform of connectedhomeip for the Armino
    SDK, adapted to the BDK / LibreTiny; Wi-Fi, key-value store, events and factory data come from ESPurna through the
    `bk_*` API of `matter_pal.h`; the `__atomic_*` of the ARM968E-S in `bk7231n_port.c`), `bdk/` flags of BDK 3.0.33 /
    LibreTiny 1.14, mbedTLS 2.28 of `mbedtls/`
  - ESP32: connectedhomeip's own `src/platform/ESP32` (what esp_matter has too), compiled with the flags, include folders
    and `sdkconfig.h` of the prebuilt ESP-IDF of Arduino-ESP32 (`chips/esp32_flags.gni`, made by
    `tools/gen_esp32_flags.py`), mbedTLS, lwIP, FreeRTOS and the mdns component of ESP-IDF (`mbedtls_idf/`)
  The identity of the board (factory data or an own development DAC), the pairing codes, the controllers and the pairing
  window are ESPurna's (`matter_identity.cpp`, `matter_app.cpp` of the fork): the library calls
  `matter_identity_install()` before the server init
- `lwip-override/lwipopts.h` - LibreTiny's lwipopts with the sizes Matter needs; ESPurna builds lwIP with it
  (`-I.../lwip-override`) and the library is compiled against the same header
- `tools/` - `build.sh` (WSL: gn + ninja with the Windows arm-none-eabi gcc through `wingcc.sh`, the package
  unzipped in `<out>/pkg` for `ESPURNA_MATTER_LIB_DIR`), `package.py` (the package of both builds), flag generators,
  size reports

connectedhomeip is not part of this repository: commit `b87051a95f` (Matter 1.5, the same commit as esp_matter 1.5.1
of the ESP32 build of ESPurna) at `C:\working\ginhub\connectedhomeip-1.5` with the submodules nlassert, nlio, mbedtls,
jsoncpp and pigweed, linked as `gn/third_party/connectedhomeip` (an NTFS junction: the Windows gcc can not follow a
WSL symlink; `build.sh` makes it). The data model target (`gn/chip-examples/espurna-plug/BUILD.gn`) is copied into
`<connectedhomeip>/examples/espurna-plug` by `build.sh` / `ci_build.sh`: since 1.5 a `chip_data_model` may use
`src/app:required-privileges` only from there. Not committed either: `out/` (build output), `.venv-wsl/` (Python for
the code generators, + `scripts/setup/requirements.build.txt` of connectedhomeip), `tools/gnroot` (gn),
`tools/zap-2025.10.23` (zap-cli v2025.10.23-nightly, the version `scripts/setup/zap.version` asks for).

`gn/app/plug.matter` is generated from `plug.zap` (after a connectedhomeip update: `scripts/tools/zap/convert.py` and
`scripts/tools/zap/generate.py plug.zap -t src/app/zap-templates/matter-idl-server.json -o gn/app` of connectedhomeip).

Build (WSL):

    bash /mnt/c/working/ginhub/matter-espurna/tools/build.sh /mnt/c/working/ginhub/matter-espurna/out/plug-tuned-15

The include paths of LibreTiny / BDK 3.0.33 (`gn/bdk/bdk_flags.gni`) go through `gn/third_party/pio`, a second
junction (to `C:\Users\igoru\.platformio`) that `build.sh` makes.

Configs: `out/plug-tuned-15` (Quectel factory data of the board), `out/plug-test` (SDK test attestation, VID 0xFFF1 /
PID 0x8001, `ProjectConfigTest.h`). A change of the project config or of `lwipopts.h` rebuilds most of the
library (10-15 min). The last build on connectedhomeip v1.4.2.0 is commit `cefb35a`.

Fixes that made the speaker hub keep the device (2026-11-05):
- `lwip-override`: `PBUF_POOL_SIZE` 6 (was 3) - Matter's packet buffers come from this pool; at AddNOC it ran empty,
  the AddNOC reply and the operational mDNS announcement were dropped
- `DiagnosticDataProviderImpl::GetNetworkInterfaces()` - IsOperational, MAC, IPv4 and IPv6 addresses (the hub reads
  them right after CommissioningComplete and removed its fabric when the interface looked down)

## Releases

GitHub Actions (`.github/workflows/release.yml`) build the library after every push to `main` that changes `gn/`,
`lwip-override/`, `tools/` or the workflow (and on "Run workflow"): `tools/ci_build.sh` on Ubuntu 24.04, natively with
the Linux builds of the same compilers (BK7231N: the PlatformIO package `toolchain-gccarmnoneeabi@1.100301.220327`,
GCC 10.3.1; ESP32: crosstool-NG `esp-14.2.0_20260121`, the `toolchain-xtensa-esp-elf` of pioarduino) and the same pinned
connectedhomeip, LibreTiny 1.14.0 / BDK packages and Arduino-ESP32 3.3.12 libraries (all pins are at the top of
`ci_build.sh`). Each build is published as a release `libmatter-<yyyymmdd>-<sha7>` with, per chip (`bk7231n`, `esp32`):

- `matter-espurna-<chip>.zip` - `lib/libMatterApp.a` (debug info stripped) and the headers of the library's API:
  `include/chip/` (connectedhomeip `src` + `src/include`, nlassert, nlio, the generated build config, the project
  config; bk7231n also its platform headers and `include/lwipopts.h`) with `include/chip/flags.txt` (its include
  folders and defines). ESPurna's code on the Matter API is compiled against them (`tools/package.py`)
- `matter-espurna-<chip>.zip.sha256`

The newest 5 of these releases are kept, older ones are deleted with their tags. ESPurna pins one in
`platformio_beken.ini` and `platformio_esp32.ini` (`custom_matter_lib`, checked against its SHA-256,
`scripts/matter_lib.py`). The same build on any
Linux, WSL included (`gn` on PATH):

    bash tools/ci_build.sh <deps_dir> <out_dir>
