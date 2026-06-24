#!/system/bin/sh
# customize.sh — Magisk module installer
# Called by Magisk during flash; copies .ko and injector to module directory

set -e
SKIPMOUNT=false
PROPFILE=false
POSTFSDATA=true
LATESTARTSERVICE=true

on_install() {
    # Copy kernel module
    ui_print "- Installing cam_inject.ko"
    cp "${MODPATH}/cam_inject.ko" "${MODPATH}/cam_inject.ko"

    # Place userspace injector in /system/bin
    ui_print "- Installing injector binary"
    mkdir -p "${MODPATH}/system/bin"
    cp "${MODPATH}/injector" "${MODPATH}/system/bin/injector"
    chmod 0755 "${MODPATH}/system/bin/injector"
}

set_permissions() {
    set_perm_recursive ${MODPATH} 0 0 0755 0644
    set_perm ${MODPATH}/cam_inject.ko 0 0 0644
    set_perm ${MODPATH}/system/bin/injector 0 0 0755
}
