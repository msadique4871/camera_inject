#!/system/bin/sh
# service.sh — Late-init actions: ensure module is loaded, set default state

# Reload module if post-fs-data failed (e.g. filesystem not ready)
KODIR=${NVBASE}/modules/camera_inject
KOMOD=${KODIR}/cam_inject.ko

if ! lsmod | grep -q cam_inject; then
    [ -f "$KOMOD" ] && insmod "$KOMOD" 2>/dev/null || true
fi

# Start with injection disabled — user must echo 1 > control
if [ -w /sys/camera_inject/control ]; then
    echo 0 > /sys/camera_inject/control
fi
