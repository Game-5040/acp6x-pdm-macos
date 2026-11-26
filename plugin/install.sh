#!/bin/bash
#
# 安装 / 卸载 ACPMicPlugin.driver
#
# 用法：
#   sudo ./install.sh            安装并重启 coreaudiod
#   sudo ./install.sh uninstall  卸载并重启 coreaudiod
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/build/ACPMicPlugin.driver"

HAL_DIR="/Library/Audio/Plug-Ins/HAL"
DEST="${HAL_DIR}/ACPMicPlugin.driver"

usage() {
    cat <<EOF
用法：
  sudo $0              安装插件到 ${HAL_DIR} 并重启 coreaudiod
  sudo $0 uninstall    从 ${HAL_DIR} 删除插件并重启 coreaudiod
EOF
}

# 只允许操作这一个固定路径，避免脚本被改坏时误删系统目录
assert_dest_sane() {
    case "$DEST" in
        "/Library/Audio/Plug-Ins/HAL/ACPMicPlugin.driver") ;;
        *)
            echo "内部错误：目标路径异常（${DEST}），已中止" >&2
            exit 1
            ;;
    esac
}

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限。请这样运行："
    echo "    sudo $0 ${1:-}"
    exit 1
fi

restart_coreaudio() {
    echo "==> 重启 coreaudiod 让 HAL 重新扫描插件"
    launchctl kickstart -k system/com.apple.audio.coreaudiod
    sleep 1
}

case "${1:-install}" in
    install)
        if [ ! -d "$SRC" ]; then
            echo "错误：找不到 $SRC"
            echo "请先在 ${SCRIPT_DIR} 里运行 ./build.sh"
            exit 1
        fi

        assert_dest_sane
        if [ -e "$DEST" ]; then
            echo "==> 已存在旧版本，先删除"
            rm -rf "$DEST"
        fi

        echo "==> 复制到 $DEST"
        /usr/bin/ditto "$SRC" "$DEST"
        chown -R root:wheel "$DEST"
        # Info.plist 必须对非特权用户可读，否则 coreaudiod 的代码签名校验会失败
        chmod -R u+rwX,go+rX "$DEST"
        xattr -cr "$DEST" 2>/dev/null || true

        echo "==> 在安装位置重新做 ad-hoc 签名"
        /usr/bin/codesign --force --sign - "$DEST"
        /usr/bin/codesign --verify --strict --verbose=2 "$DEST"

        restart_coreaudio

        cat <<'EOF'

==> 安装完成

设备名：AMD ACP PDM Microphone
在「系统设置 → 声音 → 输入」里选择它；
也可以在「音频 MIDI 设置」里看到它（应为 2 声道、48000 Hz、float32 输入）。

排查命令：
  查看插件日志：
    log show --last 5m --predicate 'eventMessage CONTAINS "ACPMic"' --style compact
  确认插件已加载：
    system_profiler SPAudioDataType | grep -B2 -A6 'AMD ACP PDM'
  确认内核驱动在：
    ioreg -l -w0 | grep -i ACPCapture | head

注意：如果内核驱动 ACPCapture.kext 没加载，设备依然会出现，但录音是静音，
日志里会打印“未找到 IOService "ACPCapture"”。

EOF
        ;;

    uninstall)
        assert_dest_sane
        if [ -e "$DEST" ]; then
            echo "==> 删除 $DEST"
            rm -rf "$DEST"
        else
            echo "==> 未安装，无需删除"
        fi
        restart_coreaudio
        echo "==> 已卸载。若「声音 → 输入」里仍有残留，注销再登录一次即可。"
        ;;

    -h|--help|help)
        usage
        ;;

    *)
        usage
        exit 1
        ;;
esac
