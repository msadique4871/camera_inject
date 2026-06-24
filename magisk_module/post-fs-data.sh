#!/system/bin/sh
# post-fs-data.sh — Load the kernel module early
# This runs before Zygote starts, so injection covers apps from launch.

KODIR=${NVBASE}/modules/camera_inject
KOMOD=${KODIR}/cam_inject.ko

if [ -f "$KOMOD" ]; then
    # Only try once; if it fails (e.g. kernel mismatch), don't spam logs
    if ! lsmod | grep -q cam_inject; then
        insmod "$KOMOD" 2>/dev/null || true
    fi
fi
