install -D -m 755 ../../build-alpine/src/dllama        $ROOTFS/usr/bin/dllama
install -D -m 755 ../../build-alpine/src/dllama-api    $ROOTFS/usr/bin/dllama-api
install -D -m 755 entrypoint.sh              $ROOTFS/entrypoint.sh

