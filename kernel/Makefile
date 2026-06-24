# SPDX-License-Identifier: GPL-2.0
#
# Makefile — cam_inject kernel module
#
# Usage:
#   1) Set KERNEL_SRC to your Android kernel tree (e.g. common/)
#   2) Set CROSS_COMPILE to your ARM64 toolchain (e.g. aarch64-linux-android-)
#   3) ARCH=arm64 make -C $(KERNEL_SRC) M=$(PWD) modules
#

obj-m := cam_inject.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# Android kernel build — example invocation:
#   ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- \
#   KDIR=~/android-kernel/out/msm-kernel-<target> \
#   make
