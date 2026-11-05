#!/bin/bash
# Sizing link of libMatterApp.a (Git Bash, Windows toolchain): gc-sections from matter_app_start,
# FreeRTOS/lwIP/BDK left unresolved (already in the firmware). Output: out/size/plug.elf + .map
M=/c/working/ginhub/matter-bk7231n
TC='/c/Users/igoru/.platformio/packages/toolchain-gccarmnoneeabi@1.100301.220327/bin'
LIB=${1:-$M/out/plug/lib/libMatterApp.a}
O=$M/out/size; mkdir -p $O
"$TC/arm-none-eabi-g++.exe" -mcpu=arm968e-s -march=armv5te -mthumb -mthumb-interwork -Os \
  --specs=nano.specs --specs=nosys.specs -nostartfiles \
  -Wl,--gc-sections -Wl,-e,matter_app_start -Wl,-u,matter_app_start \
  -Wl,--unresolved-symbols=ignore-all -Wl,--defsym=__dso_handle=0 -Wl,-Map=$O/plug.map \
  -o $O/plug.elf "$LIB" -lstdc++_nano -lsupc++_nano -lc_nano -lm -lgcc 2> $O/link.log
echo "link exit $?"
"$TC/arm-none-eabi-size.exe" -A $O/plug.elf | grep -E "^\.(text|rodata|data|bss|init_array|ARM)|Total"
"$TC/arm-none-eabi-size.exe" $O/plug.elf
