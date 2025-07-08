#!/bin/sh

OUTPUT="${1:-dllama-rootfs.tar.gz}"

./alpine-make-rootfs \
 --branch v3.20 \
 --packages 'libstdc++ libgcc gdb' \
 -m 'https://mirrors.ustc.edu.cn/alpine' \
 $OUTPUT \
 copy-files.sh

