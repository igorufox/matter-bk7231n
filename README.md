# matter-bk7231n

Matter (connectedhomeip) for the Beken BK7231N, as a static library for ESPurna on LibreTiny: `libMatterApp.a`
with an On/Off plug (one endpoint, `gn/app/plug.zap`) and a small C API that ESPurna calls
(`code/espurna/arch/beken/matter_beken.cpp` in the ESPurna fork, env `matter-ipv6-test` of `platformio_beken.ini`).

- `gn/` - own GN root: build args for the ARM968E-S (`args.gni`, `toolchain/`, `bdk/` flags of BDK 3.0.33 /
  LibreTiny 1.14), mbedTLS 2.28 config, the application (`app/`: data model, `matter_app.cpp` with the C API,
  project config) and the platform port (`external_platform/BK7231N/`, derived from the Beken platform of
  connectedhomeip for the Armino SDK, adapted to the BDK / LibreTiny; Wi-Fi, key-value store, events and factory data
  come from ESPurna through the `bk_*` API of `matter_pal.h`)
- `lwip-override/lwipopts.h` - LibreTiny's lwipopts with the sizes Matter needs; ESPurna builds lwIP with it
  (`-I.../lwip-override`) and the library is compiled against the same header
- `tools/` - `build.sh` (WSL: gn + ninja with the Windows arm-none-eabi gcc through `wingcc.sh`), flag generators,
  size reports

connectedhomeip is not part of this repository: a checkout of tag `v1.4.2.0` at `C:\working\ginhub\connectedhomeip`,
linked as `gn/third_party/connectedhomeip`. Not committed either: `out/` (build output), `.venv-wsl/` (Python for the
code generators), `tools/gnroot` (gn), `tools/zap` (zap-cli).

Build (WSL):

    bash /mnt/c/working/ginhub/matter-bk7231n/tools/build.sh /mnt/c/working/ginhub/matter-bk7231n/out/plug-tuned

Configs: `out/plug-tuned` (Quectel factory data of the board), `out/plug-test` (SDK test attestation, VID 0xFFF1 /
PID 0x8001, `BK7231NProjectConfigTest.h`). A change of the project config or of `lwipopts.h` rebuilds most of the
library (10-15 min).

Fixes that made the speaker hub keep the device (2026-11-05):
- `lwip-override`: `PBUF_POOL_SIZE` 6 (was 3) - Matter's packet buffers come from this pool; at AddNOC it ran empty,
  the AddNOC reply and the operational mDNS announcement were dropped
- `DiagnosticDataProviderImpl::GetNetworkInterfaces()` - IsOperational, MAC, IPv4 and IPv6 addresses (the hub reads
  them right after CommissioningComplete and removed its fabric when the interface looked down)
