/*
 * injector.c — Userspace companion for cam_inject kernel module
 *
 * Two modes:
 *   1) Single frame: injector frame.nv21
 *   2) Video stream: injector -v -s <frame_bytes> < /path/to/raw_frames
 *
 * Build:
 *   aarch64-linux-android-clang -O2 -static -o injector injector.c
 *
 * Video stream prep (on PC):
 *   ffmpeg -i input.mp4 -f rawvideo -pix_fmt nv21 -r 30 frame_%04d.nv21
 *   cat frame_*.nv21 > /sdcard/stream.raw
 *
 * Video stream (on device):
 *   injector -v -s $((640*480*3/2)) < /sdcard/stream.raw
 *
 * Live pipe from PC:
 *   ffmpeg -i input.mp4 -f rawvideo -pix_fmt nv21 -r 30 - | \
 *     adb shell injector -v -s $((640*480*3/2))
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define SYSFS_CTRL   "/sys/camera_inject/control"
#define SYSFS_FRAME  "/sys/camera_inject/frame"
#define SYSFS_PIDS   "/sys/camera_inject/pids"
#define SYSFS_STATUS "/sys/camera_inject/status"
#define CHUNK_SIZE   (1024 * 1024)

static int sysfs_fd = -1;

static int open_sysfs_frame(void)
{
    if (sysfs_fd < 0)
        sysfs_fd = open(SYSFS_FRAME, O_WRONLY);
    if (sysfs_fd < 0) {
        perror("open " SYSFS_FRAME);
        return -1;
    }
    return 0;
}

static int write_sysfs(const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
        ssize_t ret = write(sysfs_fd, p, chunk);
        if (ret <= 0) {
            perror("write");
            return -1;
        }
        p += ret;
        remaining -= ret;
    }
    return 0;
}

static int write_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    const unsigned char *p = data;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
        ssize_t ret = write(fd, p, chunk);
        if (ret <= 0) {
            perror("write");
            close(fd);
            return -1;
        }
        p += ret;
        remaining -= ret;
    }
    close(fd);
    return 0;
}

static int read_status(char *buf, size_t bufsz)
{
    int fd = open(SYSFS_STATUS, O_RDONLY);
    if (fd < 0) {
        perror("open " SYSFS_STATUS);
        return -1;
    }
    ssize_t n = read(fd, buf, bufsz - 1);
    if (n < 0) perror("read");
    else buf[n] = '\0';
    close(fd);
    return (int)n;
}

/* ------------------------------------------------------------------ */
/*  Video stream mode                                                  */
/* ------------------------------------------------------------------ */
static void stream_video(size_t frame_bytes, int fps, int loop)
{
    void *buf = malloc(frame_bytes);
    if (!buf) {
        fprintf(stderr, "malloc(%zu) failed\n", frame_bytes);
        return;
    }

    int usec_per_frame = fps > 0 ? 1000000 / fps : 0;
    struct timespec last_ts = {0, 0};
    unsigned long frame_count = 0;

    fprintf(stderr, "[ ] Streaming %zu-byte frames", frame_bytes);
    if (fps > 0) fprintf(stderr, " @ %d fps", fps);
    if (loop)    fprintf(stderr, " (loop)");
    fprintf(stderr, "\n");

    do {
        size_t off = 0;
        while (off < frame_bytes) {
            ssize_t n = read(STDIN_FILENO, buf + off, frame_bytes - off);
            if (n <= 0) {
                if (n == 0) {
                    /* EOF */
                    if (loop) {
                        /* Rewind stdin if seekable, else break */
                        if (lseek(STDIN_FILENO, 0, SEEK_SET) == 0) {
                            fprintf(stderr, "[ ] Looping...\n");
                            off = 0;
                            continue;
                        }
                    }
                    goto done;
                }
                perror("read");
                goto done;
            }
            off += n;
        }

        /* Rate-limit to match target FPS */
        if (usec_per_frame > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (last_ts.tv_sec || last_ts.tv_nsec) {
                long elapsed = (now.tv_sec - last_ts.tv_sec) * 1000000L +
                               (now.tv_nsec - last_ts.tv_nsec) / 1000L;
                if (elapsed < usec_per_frame)
                    usleep(usec_per_frame - elapsed);
            }
            clock_gettime(CLOCK_MONOTONIC, &last_ts);
        }

        if (write_sysfs(buf, frame_bytes) < 0)
            goto done;

        frame_count++;
        if ((frame_count % 300) == 0)
            fprintf(stderr, "[ ] %lu frames injected\n", frame_count);

    } while (1);

done:
    free(buf);
    fprintf(stderr, "[ ] Total: %lu frames\n", frame_count);
}

/* ------------------------------------------------------------------ */
/*  Single-frame mode                                                  */
/* ------------------------------------------------------------------ */
static int load_frame(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    struct stat st;
    fstat(fd, &st);
    size_t fsize = st.st_size;

    fprintf(stderr, "[ ] Loading %zu bytes ...\n", fsize);

    void *buf = malloc(fsize);
    if (!buf) {
        fprintf(stderr, "malloc(%zu) failed\n", fsize);
        close(fd);
        return -1;
    }

    size_t off = 0;
    while (off < fsize) {
        ssize_t n = read(fd, buf + off, fsize - off);
        if (n <= 0) {
            perror("read");
            free(buf);
            close(fd);
            return -1;
        }
        off += n;
    }
    close(fd);

    int ret = open_sysfs_frame() < 0 ? -1 :
              (write_sysfs(buf, fsize) < 0 ? -1 : 0);
    if (ret == 0)
        fprintf(stderr, "[+] Frame loaded: %zu bytes\n", fsize);
    free(buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [frame_file]\n"
        "\n"
        "Single-frame mode:\n"
        "  %s frame.nv21\n"
        "\n"
        "Video stream mode:\n"
        "  cat stream.raw | %s -v -s <frame_bytes> [-r 30] [-l]\n"
        "  ffmpeg ... - | %s -v -s $((W*H*3/2)) -r 30\n"
        "\n"
        "Options:\n"
        "  -v               Video streaming mode (reads frames from stdin)\n"
        "  -s <bytes>       Frame size in bytes (required for -v)\n"
        "  -r <fps>         Target frame rate (default: no limit = camera native)\n"
        "  -l               Loop the input file on EOF\n"
        "  -p <pids>        Target PIDs: '1234,5678' or empty=all\n"
        "  -e               Enable injection\n"
        "  -d               Disable injection\n"
        "  -S               Print status\n"
        "\n"
        "Raw frame format must match camera output (typically NV21).\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    const char *frame_path = NULL;
    const char *pids = NULL;
    int do_enable = 0, do_disable = 0, do_status = 0;
    int video_mode = 0, loop = 0;
    size_t frame_bytes = 0;
    int fps = 0;
    int opt;

    while ((opt = getopt(argc, argv, "vs:r:lp:edS")) != -1) {
        switch (opt) {
        case 'v': video_mode = 1; break;
        case 's': frame_bytes = atol(optarg); break;
        case 'r': fps = atoi(optarg); break;
        case 'l': loop = 1; break;
        case 'p': pids = optarg; break;
        case 'e': do_enable = 1; break;
        case 'd': do_disable = 1; break;
        case 'S': do_status = 1; break;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (optind < argc)
        frame_path = argv[optind];

    /* Status mode */
    if (do_status) {
        char buf[512];
        if (read_status(buf, sizeof(buf)) > 0)
            printf("STATUS:\n%s\n", buf);
        else
            fprintf(stderr, "Module not loaded?\n");
        return 0;
    }

    /* Set PIDs */
    if (pids) {
        if (write_file(SYSFS_PIDS, pids, strlen(pids)) < 0)
            return 1;
        printf("[+] PIDs set to: %s\n", pids);
    }

    /* Open sysfs frame fd up front for streaming */
    if (video_mode || frame_path) {
        if (open_sysfs_frame() < 0)
            return 1;
    }

    /* Video stream mode */
    if (video_mode) {
        if (frame_bytes == 0) {
            fprintf(stderr, "-s <frame_bytes> is required in video mode\n");
            return 1;
        }
        stream_video(frame_bytes, fps, loop);
        goto finish;
    }

    /* Single frame mode */
    if (frame_path) {
        if (load_frame(frame_path) < 0)
            return 1;
    }

finish:
    if (sysfs_fd >= 0) close(sysfs_fd);

    if (do_enable) {
        if (write_file(SYSFS_CTRL, "1\n", 2) < 0) return 1;
        printf("[+] Injection ENABLED\n");
    }
    if (do_disable) {
        if (write_file(SYSFS_CTRL, "0\n", 2) < 0) return 1;
        printf("[-] Injection DISABLED\n");
    }

    return 0;
}
