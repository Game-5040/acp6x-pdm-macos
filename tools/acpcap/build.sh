#!/bin/sh
# acpcap —— 用户态采集/试听工具（纯 clang，不需要 Xcode）
set -e
cd "$(dirname "$0")"
clang -O2 -o acpcap acpcap.c -framework IOKit -framework CoreFoundation
echo "已生成 $(pwd)/acpcap"
