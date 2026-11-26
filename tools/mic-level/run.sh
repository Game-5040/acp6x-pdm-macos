#!/bin/bash
# 测默认输入设备 4 秒，打印峰值/RMS。用来判断"这个输入到底有没有信号"。
set -euo pipefail
cd "$(dirname "$0")"
[ -x ./mic-level ] || clang -fobjc-arc -O2 -framework AVFoundation -framework Foundation \
    -framework CoreMedia -framework AudioToolbox mic-level.m -o mic-level
./mic-level
