#!/bin/bash
# Runs the Windows arm-none-eabi toolchain of PlatformIO/LibreTiny (GCC 10.3.1) from WSL ninja.
#   wingcc.sh gcc|g++|ar|size|nm <args...>
# - /mnt/c/... arguments are rewritten to C:/... (Windows exe can't see WSL paths)
# - ar gets its arguments through a response file (Windows command line limit is 32 KiB)
# - the -MF depfile is rewritten back to /mnt/c/... so ninja can stat the headers
TC='/mnt/c/Users/igoru/.platformio/packages/toolchain-gccarmnoneeabi@1.100301.220327/bin'
export GCC_EXEC_PREFIX="C:/Users/igoru/.platformio/packages/toolchain-gccarmnoneeabi@1.100301.220327/lib/gcc/"
export WSLENV="GCC_EXEC_PREFIX${WSLENV:+:$WSLENV}"   # argv[0] from WSL is useless to gcc.exe for finding its prefix
tool=$1
shift
args=()
mf=""
prev=""
for a in "$@"; do
    a="${a//\/mnt\/c\//C:/}"
    args+=("$a")
    [ "$prev" = "-MF" ] && mf="$a"
    prev="$a"
done
if [ "$tool" = "ar" ]; then
    # ar <flags> <archive> <objects...>
    rsp="${args[1]}.wingcc.rsp"
    printf '%s\n' "${args[@]:2}" > "$rsp"
    "$TC/arm-none-eabi-ar.exe" "${args[0]}" "${args[1]}" "@$rsp"
    rc=$?
    rm -f "$rsp"
    exit $rc
fi
"$TC/arm-none-eabi-$tool.exe" "${args[@]}"
rc=$?
if [ $rc -eq 0 ] && [ -n "$mf" ] && [ -f "$mf" ]; then
    sed -i -e 's#[Cc]:[/\\]#/mnt/c/#g' -e 's#\r$##' "$mf"
fi
exit $rc
