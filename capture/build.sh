#!/bin/bash
# 构建 ACPCapture.kext（Phase 1：PDM 采集 + PCM 导出）
# 用法：./build.sh        生成 ./ACPCapture.kext
#       ./build.sh clean  清理
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" = "clean" ]; then
    rm -rf ACPCapture.kext build
    echo "cleaned"
    exit 0
fi

SDK="$(xcrun --show-sdk-path)"
KDIR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

rm -rf ACPCapture.kext build
mkdir -p build ACPCapture.kext/Contents/MacOS

CFLAGS=(-std=c++17 -fapple-kext -DKERNEL -D__KERNEL__
        -fno-rtti -fno-exceptions -fno-threadsafe-statics
        -O2 -w -target x86_64-apple-macos11.0
        -I"$KDIR" -I../regs)

clang++ "${CFLAGS[@]}" -c ACPCapture.cpp -o build/ACPCapture.o 2>&1 | head -30
clang++ "${CFLAGS[@]}" -c kmod_info.cpp  -o build/kmod_info.o 2>&1 | head -10

ld -r -o ACPCapture.kext/Contents/MacOS/ACPCapture \
      build/ACPCapture.o build/kmod_info.o -kext
cp Info.plist ACPCapture.kext/Contents/Info.plist

plutil -lint ACPCapture.kext/Contents/Info.plist >/dev/null
echo "built: $(pwd)/ACPCapture.kext"
file ACPCapture.kext/Contents/MacOS/ACPCapture

if nm -m ACPCapture.kext/Contents/MacOS/ACPCapture | grep -q "(__DATA,__data) external _kmod_info"; then
    echo "OK: _kmod_info 已生成"
else
    echo "错误：_kmod_info 缺失，注入必失败" >&2
    exit 1
fi
