#!/bin/bash
#
# 用 Command Line Tools 自带的 clang 直接编译 ACPMicPlugin.driver。
#
# 为什么不用 xcodebuild：本机只装了 Command Line Tools（没有完整 Xcode），
# 而且这个插件只有一个 .c 文件，直接调用 clang 更快、更可控。
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SRC="ACPMicPlugin.c"
BUILD_DIR="build"
DRIVER="${BUILD_DIR}/ACPMicPlugin.driver"
EXECUTABLE="${DRIVER}/Contents/MacOS/ACPMicPlugin"
ARCH="$(uname -m)"
MIN_MACOS="12.0"

# 优先用环境变量指定的 SDK，否则用 CLT 的 SDK；两者都没有就报错退出
SDK="${SDK_PATH:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}"
if [ ! -d "$SDK" ]; then
    SDK="$(/usr/bin/xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
fi
if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
    echo "错误：找不到 macOS SDK，请设置 SDK_PATH" >&2
    exit 1
fi

echo "==> clang  : $(/usr/bin/clang --version | head -1)"
echo "==> SDK    : $SDK"
echo "==> 架构   : $ARCH (最低 macOS $MIN_MACOS)"
echo "==> 源码   : $SRC"
echo "==> 输出   : $DRIVER"
echo

# 每次都从干净目录开始，避免上一次的残留文件被打进签名
rm -rf "$BUILD_DIR"
mkdir -p "$DRIVER/Contents/MacOS"
cp Info.plist "$DRIVER/Contents/Info.plist"

echo "==> 编译"
/usr/bin/clang \
    -arch "$ARCH" \
    -mmacosx-version-min="$MIN_MACOS" \
    -isysroot "$SDK" \
    -std=c99 \
    -O2 \
    -Wall -Wextra -Wno-unused-parameter \
    -fno-common \
    -bundle \
    -framework CoreFoundation \
    -framework IOKit \
    -o "$EXECUTABLE" \
    "$SRC"

# coreaudiod 的 AMCP 会以非特权用户读取 Info.plist 做哈希，
# 文件权限过严会导致 errSecCSInfoPlistFailed (-67030)
chmod -R u+rwX,go+rX "$DRIVER"
xattr -cr "$DRIVER" 2>/dev/null || true

echo "==> 校验 Info.plist"
/usr/bin/plutil -lint "$DRIVER/Contents/Info.plist"

echo "==> ad-hoc 签名"
/usr/bin/codesign --force --sign - "$DRIVER"
/usr/bin/codesign --verify --strict --verbose=2 "$DRIVER"

echo
echo "==> 构建成功"
/usr/bin/find "$DRIVER" -type f -exec /bin/ls -l {} \;
echo
/usr/bin/codesign -dv "$DRIVER" 2>&1 | /usr/bin/sed 's/^/    /'
echo
echo "下一步：sudo $SCRIPT_DIR/install.sh"
