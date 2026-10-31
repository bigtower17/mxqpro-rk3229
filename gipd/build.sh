#!/bin/sh
# Cross-compile gipd for ARMv7 (Cortex-A7) with Zig as a static binary.
# No Android NDK needed. Install Zig (https://ziglang.org) and run this.
set -e
zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -static -Os -Wall -o gipd gipd.c
echo "built: $(ls -l gipd)"
# Deploy (device with ADB root):
#   adb push gipd /data/local/tmp/gipd && adb shell 'su -c "chmod 755 /data/local/tmp/gipd"'
#   run with:  gipd 5 1 -d /data/local/tmp/gipd.log
