#!/bin/bash
# 构建 ACPProbe.kext（Phase 0 探路驱动，只读）
# 用法：./build.sh          生成 stage 0（只读元数据，不碰 MMIO）
#       ./build.sh 1        生成 stage 1（只读 PIN_CONFIG / PGFSM_STATUS）
#       ./build.sh 2|3      寄存器表 / 表 + 全窗扫描
#       ./build.sh clean    清理
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" = "clean" ]; then
    rm -rf ACPProbe.kext *.o
    echo "cleaned"
    exit 0
fi

STAGE="${1:-0}"
case "$STAGE" in 0|1|2|3) ;; *) echo "stage 只能是 0/1/2/3" >&2; exit 1 ;; esac

SDK="$(xcrun --show-sdk-path)"
KDIR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

rm -rf ACPProbe.kext build
mkdir -p build ACPProbe.kext/Contents/MacOS

# 关键编译参数：
#   -DKERNEL    ：Kernel.framework 里 IOPCIDevice 等真正的类声明在 #if defined(KERNEL) 里
#   -fapple-kext：kext C++ 方言（不要 new/delete 语义、无 RTTI）
clang++ -std=c++17 -fapple-kext -DKERNEL -D__KERNEL__ \
        -fno-rtti -fno-exceptions -fno-threadsafe-statics \
        -O2 -w -target x86_64-apple-macos11.0 -DPROBE_STAGE=$STAGE \
        -I"$KDIR" -I../regs \
        -c ACPProbe.cpp -o build/ACPProbe.o

clang++ -std=c++17 -fapple-kext -DKERNEL -D__KERNEL__ \
        -fno-rtti -fno-exceptions -fno-threadsafe-statics \
        -O2 -w -target x86_64-apple-macos11.0 \
        -I"$KDIR" -I../regs \
        -c kmod_info.cpp -o build/kmod_info.o

ld -r -o ACPProbe.kext/Contents/MacOS/ACPProbe build/ACPProbe.o build/kmod_info.o -kext
cp Info.plist ACPProbe.kext/Contents/Info.plist

plutil -lint ACPProbe.kext/Contents/Info.plist >/dev/null
echo "built: $(pwd)/ACPProbe.kext  (PROBE_STAGE=$STAGE)"
file ACPProbe.kext/Contents/MacOS/ACPProbe

# 自检：_kmod_info 必须存在，且在 __DATA,__data 里
if nm -m ACPProbe.kext/Contents/MacOS/ACPProbe | grep -q "(__DATA,__data) external _kmod_info"; then
    echo "OK: _kmod_info 已生成"
    nm -m ACPProbe.kext/Contents/MacOS/ACPProbe | grep "_kmod_info"
else
    echo "错误：_kmod_info 仍然缺失，这个 kext 注入必失败" >&2
    exit 1
fi
