#!/bin/bash
# install-probe.sh — 把只读探针 ACPProbe.kext 装进 OpenCore（Phase 0B）
#
# 用法：
#   sudo ./install-probe.sh            # 安装（自动挂载 ESP、备份 config、ocvalidate）
#   sudo ./install-probe.sh uninstall  # 卸载（移除条目与 kext 目录）
#   sudo ./install-probe.sh status     # 只看当前状态
#
# 装完重启，然后在 macOS 里跑：
#   log show --last boot --predicate 'process == "kernel"' | grep ACPProbe
set -euo pipefail

ESP=${ESP:-/Volumes/SYSTEM}
HERE="$(cd "$(dirname "$0")" && pwd)"
KEXT_SRC="$HERE/../probe/ACPProbe.kext"
OCDIR="$ESP/EFI/OC"
CFG="$OCDIR/config.plist"
KEXT_NAME="ACPProbe.kext"
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
if [ ! -f "$CFG" ]; then
    red "ESP 里没有 config.plist。先手动挂载，再重跑："
    echo "    sudo diskutil mount disk0s1"
    exit 1
fi

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
    echo "已加载：$(kmutil showloaded 2>/dev/null | grep -c ACPProbe || true) 条 ACPProbe"
}

install_probe() {
    [ -d "$KEXT_SRC" ] || die "找不到编译好的 ${KEXT_SRC}，先跑 probe/build.sh"
    [ -f "$KEXT_SRC/Contents/MacOS/ACPProbe" ] || die "$KEXT_NAME 里没有可执行文件"

    local ts; ts=$(date +%Y%m%d-%H%M%S)
    cp -p "$CFG" "$CFG.bak-acpprobe-$ts"
    ylw "已备份 config.plist.bak-acpprobe-$ts"

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
    'Comment': 'ACP6x PDM probe (read-only, Phase 0B)',
    'Enabled': True,
    'ExecutablePath': 'Contents/MacOS/ACPProbe',
    'MaxKernel': '',
    'MinKernel': '25.0.0',
    'PlistPath': 'Contents/Info.plist',
})
plistlib.dump(d, open(cfg, 'wb'))
print(f"Kernel.Add 现在 {len(adds)} 条")
PY

    if validate; then
        grn "安装完成。重启后跑："
        echo "    log show --last boot --predicate 'process == \"kernel\"' | grep ACPProbe"
        echo "  （如果什么都没看到，说明 kext 没被注入：检查 Kernel.Add 里 ACPProbe.kext 是否 Enabled、MinKernel 是否 <= 25.6.0）"
    else
        cp -p "$CFG.bak-acpprobe-$ts" "$CFG"
        rm -rf "$OCDIR/Kexts/$KEXT_NAME"
        die "校验失败，已回滚"
    fi
}

uninstall_probe() {
    local ts; ts=$(date +%Y%m%d-%H%M%S)
    cp -p "$CFG" "$CFG.bak-acpprobe-rm-$ts"
    ylw "已备份 config.plist.bak-acpprobe-rm-$ts"

    python3 - "$CFG" "$KEXT_NAME" <<'PY'
import plistlib, sys
cfg, name = sys.argv[1], sys.argv[2]
d = plistlib.load(open(cfg, 'rb'))
adds = d.setdefault('Kernel', {}).setdefault('Add', [])
before = len(adds)
adds[:] = [e for e in adds if e.get('BundlePath') != name]
plistlib.dump(d, open(cfg, 'wb'))
print("Kernel.Add: %d -> %d" % (before, len(adds)))
cap = [e for e in adds if e.get('BundlePath') == 'ACPCapture.kext']
print("ACPCapture.kext 条目：%s" % (("在，Enabled=%s" % cap[0].get('Enabled')) if cap else "不在！"))
PY

    # 保险：采集 kext 是麦克风的命根子，它要是没了就立刻回滚
    if ! python3 - "$CFG" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
ok = any(e.get('BundlePath') == 'ACPCapture.kext' and e.get('Enabled', True)
         for e in d.get('Kernel', {}).get('Add', []))
sys.exit(0 if ok else 1)
PY
    then
        cp -p "$CFG.bak-acpprobe-rm-$ts" "$CFG"
        die "移除后 ACPCapture.kext 条目不见了，已回滚（麦克风不能丢）"
    fi

    rm -rf "$OCDIR/Kexts/$KEXT_NAME"
    if validate; then
        grn "已卸载 ACPProbe.kext（条目 + Kexts 目录）"
        echo "  Kexts 里 ${KEXT_NAME}：$([ -d "$OCDIR/Kexts/$KEXT_NAME" ] && echo 仍存在 || echo 已删除)"
        echo "  重启后生效；麦克风不受影响（ACPCapture.kext 保留）"
    else
        cp -p "$CFG.bak-acpprobe-rm-$ts" "$CFG"
        die "ocvalidate 失败，已回滚"
    fi
}

usage() {
    cat <<EOF
用法：sudo $0 [install|uninstall|status]

  install    （默认）把 ACPProbe.kext 装进 OpenCore
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
