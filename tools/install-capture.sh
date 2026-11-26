#!/bin/bash
# install-capture.sh — 把采集驱动 ACPCapture.kext 装进 OpenCore（Phase 1）
#
# 用法：
#   sudo ./install-capture.sh            # 安装（自动挂载 ESP、备份 config、ocvalidate）
#   sudo ./install-capture.sh uninstall  # 卸载（移除条目与 kext 目录）
#   sudo ./install-capture.sh status     # 只看当前状态
#
# 装完重启，然后在 macOS 里跑：
#   ioreg -rc ACPCapture -l | head -40
set -euo pipefail

ESP=${ESP:-/Volumes/SYSTEM}
HERE="$(cd "$(dirname "$0")" && pwd)"
KEXT_SRC="$HERE/../capture/ACPCapture.kext"
OCDIR="$ESP/EFI/OC"
CFG="$OCDIR/config.plist"
KEXT_NAME="ACPCapture.kext"
OCVALIDATE="${OCVALIDATE:-$HERE/../../oc-release/Utilities/ocvalidate/ocvalidate}"

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
ylw()  { printf '\033[33m%s\033[0m\n' "$*"; }
die()  { red "错误：$*"; exit 1; }

[ "$(id -u)" -eq 0 ] || die "需要 root：sudo $0 $*"

# ESP 没挂就挂上
if [ ! -f "$CFG" ]; then
    ylw "ESP 未挂载，尝试挂载 disk0s1 ..."
    diskutil mount disk0s1 >/dev/null 2>&1 || true
fi
[ -f "$CFG" ] || die "找不到 ${CFG}（ESP 没挂上？先 sudo diskutil mount disk0s1）"

validate() {
    /usr/bin/plutil -lint "$CFG" >/dev/null || die "config.plist 语法错误"
    if [ -x "$OCVALIDATE" ]; then
        local out; out="$("$OCVALIDATE" "$CFG" 2>&1 || true)"
        if echo "$out" | grep -qi "No issues found"; then
            grn "ocvalidate: No issues found"
        else
            red "ocvalidate 报错："; echo "$out" | sed 's/^/    /'
            return 1
        fi
    else
        ylw "跳过 ocvalidate（没找到 ${OCVALIDATE}）"
    fi
}

status() {
    echo "配置：$CFG"
    python3 - "$CFG" "$KEXT_NAME" <<'PY'
import plistlib, sys
cfg, name = sys.argv[1], sys.argv[2]
d = plistlib.load(open(cfg, 'rb'))
adds = d.get('Kernel', {}).get('Add', [])
hit = [e for e in adds if e.get('BundlePath') == name]
print(f"Kernel.Add: {len(adds)} 条")
if hit:
    e = hit[0]
    print(f"  {name}: Enabled={e.get('Enabled')} MinKernel={e.get('MinKernel','')!r} "
          f"MaxKernel={e.get('MaxKernel','')!r} Comment={e.get('Comment','')!r}")
else:
    print(f"  {name}: 未注册")
PY
    if [ -d "$OCDIR/Kexts/$KEXT_NAME" ]; then
        echo "Kexts 里的 ${KEXT_NAME}：存在（$(du -sh "$OCDIR/Kexts/$KEXT_NAME" | awk '{print $1}')）"
    else
        echo "Kexts 里的 ${KEXT_NAME}：不存在"
    fi
    echo "已加载：$(kmutil showloaded 2>/dev/null | grep -c ACPCapture || true) 条 ACPCapture"
}

install_probe() {
    [ -d "$KEXT_SRC" ] || die "找不到编译好的 ${KEXT_SRC}，先跑 probe/build.sh"
    [ -f "$KEXT_SRC/Contents/MacOS/ACPCapture" ] || die "$KEXT_NAME 里没有可执行文件"

    local ts; ts=$(date +%Y%m%d-%H%M%S)
    cp -p "$CFG" "$CFG.bak-acpcapture-$ts"
    ylw "已备份 config.plist.bak-acpcapture-$ts"

    rm -rf "$OCDIR/Kexts/$KEXT_NAME"
    mkdir -p "$OCDIR/Kexts"
    cp -R "$KEXT_SRC" "$OCDIR/Kexts/$KEXT_NAME"

    # FAT32 上复制偶尔会丢文件（之前 WTLWireless 的插件 Info.plist 就这么丢过），
    # 所以逐个文件比对大小，不一致立刻回滚。
    if ! diff <(cd "$KEXT_SRC" && find . -type f -exec stat -f '%z %N' {} \; | sort) \
              <(cd "$OCDIR/Kexts/$KEXT_NAME" && find . -type f -exec stat -f '%z %N' {} \; | sort) >/dev/null; then
        red "复制不完整，逐项差异："
        diff <(cd "$KEXT_SRC" && find . -type f -exec stat -f '%z %N' {} \; | sort) \
             <(cd "$OCDIR/Kexts/$KEXT_NAME" && find . -type f -exec stat -f '%z %N' {} \; | sort) | sed 's/^/    /' || true
        rm -rf "$OCDIR/Kexts/$KEXT_NAME"
        die "kext 复制校验失败，已清理（配置未改）"
    fi
    grn "已复制 ${KEXT_NAME} -> EFI/OC/Kexts/（逐文件大小校验通过）"

    python3 - "$CFG" "$KEXT_NAME" <<'PY'
import plistlib, sys
cfg, name = sys.argv[1], sys.argv[2]
d = plistlib.load(open(cfg, 'rb'))
adds = d.setdefault('Kernel', {}).setdefault('Add', [])
adds[:] = [e for e in adds if e.get('BundlePath') != name]
adds.append({
    'Arch': 'Any',
    'BundlePath': name,
    'Comment': 'ACP6x PDM capture (Phase 1)',
    'Enabled': True,
    'ExecutablePath': 'Contents/MacOS/ACPCapture',
    'MaxKernel': '',
    'MinKernel': '25.0.0',
    'PlistPath': 'Contents/Info.plist',
})
plistlib.dump(d, open(cfg, 'wb'))
print(f"Kernel.Add 现在 {len(adds)} 条")
PY

    if validate; then
        grn "安装完成。重启后跑："
        echo "    ioreg -rc ACPCapture -l | head -40"
        echo "  （然后跑 tools/acpcap/acpcap 录 3 秒看有没有峰值）"
    else
        cp -p "$CFG.bak-acpcapture-$ts" "$CFG"
        rm -rf "$OCDIR/Kexts/$KEXT_NAME"
        die "校验失败，已回滚"
    fi
}

uninstall_probe() {
    local ts; ts=$(date +%Y%m%d-%H%M%S)
    cp -p "$CFG" "$CFG.bak-acpcapture-rm-$ts"
    python3 - "$CFG" "$KEXT_NAME" <<'PY'
import plistlib, sys
cfg, name = sys.argv[1], sys.argv[2]
d = plistlib.load(open(cfg, 'rb'))
adds = d.get('Kernel', {}).get('Add', [])
before = len(adds)
adds[:] = [e for e in adds if e.get('BundlePath') != name]
plistlib.dump(d, open(cfg, 'wb'))
print(f"Kernel.Add: {before} -> {len(adds)}")
PY
    rm -rf "$OCDIR/Kexts/$KEXT_NAME"
    validate || true
    grn "已卸载（备份 config.plist.bak-acpcapture-rm-${ts}）"
}

usage() {
    cat <<EOF
用法：sudo $0 [install|uninstall|status]

  install    （默认）把 ACPCapture.kext 装进 OpenCore
  uninstall  移除条目与 kext 目录
  status     只看当前状态，不改任何东西
EOF
}

echo "参数：共 $# 个 [${*:-无}]"

case "${1:-}" in
    "")                install_probe ;;
    install)           install_probe ;;
    uninstall|remove)  uninstall_probe ;;
    status|show)       status ;;
    -h|--help|help)    usage ;;
    *)
        red "错误：不认识的参数 '$1'"
        echo
        usage
        exit 2
        ;;
esac
