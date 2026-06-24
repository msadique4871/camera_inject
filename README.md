# CamInject — Kernel-level Android Camera Frame Injector

Injects arbitrary frames into the camera pipeline by hooking
`vb2_buffer_done()` (the kernel V4L2 buffer completion callback)
via kprobe. Operates at DMA-BUF level — completely invisible to
userspace apps, HAL, and framework layers.

Tested concept: **Android 10–16** (kernel 5.10 / 5.15 / 6.1).

## How it works

```
Userspace app          Kernel
    |                    |
    |  DQBUF (ioctl)     |
    |------------------->|
    |                    |  vb2_buffer_done(vb)  ← kprobe fires here
    |                    |    |
    |                    |  workqueue schedules inject_worker()
    |                    |    |
    |                    |  dma_buf_vmap() → memcpy(injected_frame) → vunmap
    |                    |    |
    |  <-- gets modified buffer --|
    |  (sees injected frame)      |
```

## Project structure

```
camera_inject/
├── kernel/
│   ├── Makefile           # ARM64 cross-compile Makefile
│   └── cam_inject.c       # LKM: kprobe + DMA-BUF + sysfs
├── userspace/
│   ├── Makefile
│   └── injector.c         # CLI tool: load frame + enable injection
└── magisk_module/
    ├── module.prop
    ├── customize.sh
    ├── post-fs-data.sh
    ├── service.sh
    └── (cam_inject.ko + injector binary go here at build time)
```

## Building

### Option 1: GitHub Actions (recommended if you only have a phone)

Push this repo to GitHub, go to **Actions** → **Build cam_inject.ko** → **Run workflow**.
Download the Magisk zip for your kernel version from the artifacts.

No local build environment needed.

### Option 2: Cross-compile on PC

```bash
# Kernel module
cd kernel
ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- \
  KDIR=/path/to/android-kernel/out/make ...

# Userspace injector
cd userspace
aarch64-linux-android-clang -O2 -static -o injector injector.c
```

### Option 3: On-device build (Termux, advanced)

If your kernel has kheaders support:
```bash
# Check if kheaders are available
ls -l /sys/kernel/kheaders.tar.xz

# In Termux with proot-distro ubuntu:
tar xf /sys/kernel/kheaders.tar.xz
make -C /path/to/kernel_src M=$(pwd)/kernel modules
```

If kheaders is unavailable, check `uname -r` and download a matching GKI
prebuilt from https://ci.android.com/builds/branches/aosp_kernel-common/branch

## Installation (Magisk module)

1. Place `cam_inject.ko` and `injector` binary in `magisk_module/`
2. Zip it: `cd magisk_module && zip -r ../cam_inject_magisk.zip .`
3. Flash via Magisk Manager
4. Reboot

## Usage

### Single frame (static image injection)

```bash
# On device (as root):
cat /sys/camera_inject/status          # check module loaded
echo "1234,5678" > /sys/camera_inject/pids   # target PIDs (empty = all)
injector /sdcard/frame.nv21            # load a raw frame
echo 1 > /sys/camera_inject/control    # enable injection
# Launch camera app — it sees your frame
echo 0 > /sys/camera_inject/control    # disable
```

### Video stream (MP4 → live camera view)

```bash
# --- On PC: pre-decode MP4 to raw NV21 frames ---
ffmpeg -i input.mp4 -f rawvideo -pix_fmt nv21 -r 30 frame_%04d.nv21
cat frame_*.nv21 > stream.raw

# --- Push to device ---
adb push stream.raw /sdcard/

# --- On device: stream it ---
injector -v -s $((WIDTH*HEIGHT*3/2)) -r 30 < /sdcard/stream.raw

# --- Or live-pipe from PC (no temp file) ---
ffmpeg -i input.mp4 -f rawvideo -pix_fmt nv21 -r 30 - | \
  adb shell injector -v -s $((WIDTH*HEIGHT*3/2)) -r 30
```

Video stream options:
| Flag | Meaning |
|------|---------|
| `-v`  | Video stream mode (reads raw frames from stdin) |
| `-s N` | Frame size in bytes (W×H×1.5 for NV21) |
| `-r N` | Target frame rate (optional; default = camera native) |
| `-l`  | Loop the input on EOF |
| `-p PIDS` | Target PIDs |

### Generating test frames
```bash
# Raw gray frame (NV21 640x480):
python3 -c "
w, h = 640, 480
y = b'\x80' * (w * h)
uv = b'\x00' * (w * h // 2)
import sys; sys.stdout.buffer.write(y + uv)
" > /sdcard/frame.nv21
```

## Limitations

- Requires kernel built with `CONFIG_VIDEOBUF2_CORE=y` and
  `CONFIG_KPROBES=y` (both common on Android GKI kernels).
- Only injects into the first DMA-BUF plane (sufficient for most
  YUV/NV12/NV21 formats; multi-planar formats not yet supported).
- DMA-BUF API changed at 5.11 (iosys_map) — the module handles both.
- Does not inject into JPEG still-capture paths (different code path);
  for photo spoofing, hook vb2_buffer_done with JPEG buffers too.
