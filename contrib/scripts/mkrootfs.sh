#!/bin/sh

OUTPUT="${1:-dllama-rootfs.tar.gz}"

./alpine-make-rootfs \
 --branch v3.20 \
 --packages 'libstdc++ libgcc' \
 $OUTPUT \
 copy-files.sh

