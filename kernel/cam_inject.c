/*
 * cam_inject.c — Kernel-level camera frame injector
 *
 * Hooks vb2_buffer_done via kprobe, defers buffer modification
 * via workqueue, injects a pre-loaded frame into the DMA-BUF.
 *
 * Sysfs interface:
 *   /sys/camera_inject/control    - 0=off, 1=on
 *   /sys/camera_inject/frame      - write raw frame data here
 *   /sys/camera_inject/frame_size - expected frame size in bytes
 *   /sys/camera_inject/pids       - comma-separated target PID list
 *   /sys/camera_inject/status     - read current state
 *
 * Build (inside Android kernel tree):
 *   make -C <KERNEL_SRC> M=$PWD modules
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/dma-buf.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <media/videobuf2-core.h>

/* dma-buf vmap/vunmap compatibility:
 * - Pre-5.11 / GKI 5.10: vmap returns void*, vunmap 2-arg on GKI 5.10
 * - GKI 5.15 / early 5.11: uses struct dma_buf_map (dma-buf-map.h)
 * - Later 5.11+: renamed struct dma_buf_map -> struct iosys_map (iosys-map.h)
 *
 * We use __has_include to detect which header exists at compile time,
 * then alias it to a common ci_map type + helpers.
 */
#if __has_include(<linux/dma-buf-map.h>)
#include <linux/dma-buf-map.h>
#define ci_map_t       struct dma_buf_map
#define ci_map_init(m) do { } while (0)
#elif __has_include(<linux/iosys-map.h>)
#include <linux/iosys-map.h>
#define ci_map_t       struct iosys_map
#define ci_map_init(m) do { } while (0)
#endif

#define DRIVER_NAME "cam_inject"
#define FRAME_MAX_SIZE (4096 * 4096 * 4) /* 4K UHD x 4 bytes */

/* ------------------------------------------------------------------ */
/*  Globals                                                           */
/* ------------------------------------------------------------------ */
static struct kobject *ci_kobj;

static atomic_t            ci_enabled   = ATOMIC_INIT(0);
static DEFINE_MUTEX(ci_frame_lock);
static void               *ci_frame_buf   = NULL;
static size_t              ci_frame_size  = 0;

#define TARGET_PIDS_MAX 64
static pid_t               ci_target_pids[TARGET_PIDS_MAX];
static int                 ci_target_pid_count;
static DEFINE_MUTEX(ci_pid_lock);

/* workqueue for deferred buffer injection */
static struct workqueue_struct *ci_wq;
struct inject_work {
    struct work_struct work;
    struct dma_buf     *dbuf;
    loff_t              dbuf_size;
};
static atomic_t ci_inject_count = ATOMIC_INIT(0);

/* kprobe */
static struct kprobe ci_kp;

/* ------------------------------------------------------------------ */
/*  PID-list helpers                                                  */
/* ------------------------------------------------------------------ */
static bool is_target_pid(pid_t pid)
{
    int i;
    bool match = false;
    mutex_lock(&ci_pid_lock);
    if (ci_target_pid_count == 0) {
        /* empty list -> inject for all */
        match = true;
        goto out;
    }
    for (i = 0; i < ci_target_pid_count; i++) {
        if (ci_target_pids[i] == pid) {
            match = true;
            goto out;
        }
    }
out:
    mutex_unlock(&ci_pid_lock);
    return match;
}

static int parse_pids(const char *buf, size_t count)
{
    char *copy, *tok, *end;
    int n = 0;

    copy = kmemdup(buf, count + 1, GFP_KERNEL);
    if (!copy)
        return -ENOMEM;
    copy[count] = '\0';

    mutex_lock(&ci_pid_lock);
    ci_target_pid_count = 0;
    tok = copy;
    while ((tok = strsep(&copy, ",")) != NULL && n < TARGET_PIDS_MAX) {
        long val;
        val = simple_strtol(tok, &end, 10);
        if (end != tok) {
            ci_target_pids[n++] = (pid_t)val;
        }
    }
    ci_target_pid_count = n;
    mutex_unlock(&ci_pid_lock);

    kfree(copy);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Workqueue: deferred buffer injection                              */
/* ------------------------------------------------------------------ */
static void inject_worker(struct work_struct *work)
{
    struct inject_work *iw = container_of(work, struct inject_work, work);
    struct dma_buf *dmabuf = iw->dbuf;
    void *vaddr = NULL;
#ifdef ci_map_t
    ci_map_t map;
#endif

    if (!atomic_read(&ci_enabled) || !ci_frame_buf || ci_frame_size == 0)
        goto out;

    if (iw->dbuf_size < ci_frame_size) {
        pr_debug_ratelimited(DRIVER_NAME ": buf too small (%lld < %zu)\n",
                             iw->dbuf_size, ci_frame_size);
        goto out;
    }

    /* Ensure cache coherency: invalidate stale device data */
    dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);

    /* vmap the dma-buf into kernel address space */
#ifdef ci_map_t
    if (dma_buf_vmap(dmabuf, &map) != 0) {
        dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
        goto out;
    }
    vaddr = map.vaddr;
#else
    vaddr = dma_buf_vmap(dmabuf);
    if (!vaddr) {
        dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
        goto out;
    }
#endif

    /* Overwrite with our injected frame */
    mutex_lock(&ci_frame_lock);
    memcpy(vaddr, ci_frame_buf, ci_frame_size);
    mutex_unlock(&ci_frame_lock);

    /* Flush CPU writes so userspace sees our data */
    dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);

#ifdef ci_map_t
    dma_buf_vunmap(dmabuf, &map);
#else
    dma_buf_vunmap(dmabuf, vaddr);
#endif

    atomic_inc(&ci_inject_count);
    pr_debug(DRIVER_NAME ": injected %zu bytes into dma-buf %p\n",
             ci_frame_size, dmabuf);

out:
    dma_buf_put(dmabuf);
    kfree(iw);
}

/* ------------------------------------------------------------------ */
/*  kprobe handler                                                    */
/* ------------------------------------------------------------------ */
/*   vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state s)  */
static int handler_pre(struct kprobe *kp, struct pt_regs *regs)
{
    struct vb2_buffer *vb;
    struct dma_buf *dmabuf;
    struct inject_work *iw;
    int i;

    if (!atomic_read(&ci_enabled))
        return 0;

    if (!is_target_pid(current->pid))
        return 0;

    /* On arm64, x0 holds the first argument (vb) */
#if defined(__arm64__) || defined(__aarch64__)
    vb = (struct vb2_buffer *)regs->regs[0];
#elif defined(__arm__)
    vb = (struct vb2_buffer *)regs->uregs[0];
#else
    vb = (struct vb2_buffer *)regs->di; /* x86 fallback */
#endif
    if (!vb)
        return 0;

    /* Iterate planes looking for DMA-BUF */
    for (i = 0; i < vb->num_planes; i++) {
        struct vb2_plane *plane = &vb->planes[i];
        if (!plane->dbuf)
            continue;

        dmabuf = plane->dbuf;
        get_dma_buf(dmabuf);
        /* get_dma_buf returns void on 5.11+, pointer on 5.10 */

        iw = kzalloc(sizeof(*iw), GFP_ATOMIC);
        if (!iw) {
            dma_buf_put(dmabuf);
            break;
        }

        iw->dbuf      = dmabuf;
        iw->dbuf_size = plane->length;

        INIT_WORK(&iw->work, inject_worker);
        queue_work(ci_wq, &iw->work);

        break; /* inject only first plane */
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Sysfs interface                                                   */
/* ------------------------------------------------------------------ */
static ssize_t control_show(struct kobject *kobj,
                            struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&ci_enabled));
}

static ssize_t control_store(struct kobject *kobj,
                             struct kobj_attribute *attr,
                             const char *buf, size_t count)
{
    int val;
    if (kstrtoint(buf, 10, &val) < 0)
        return -EINVAL;
    atomic_set(&ci_enabled, !!val);
    pr_info(DRIVER_NAME ": %s\n", val ? "enabled" : "disabled");
    return count;
}

static ssize_t frame_store(struct kobject *kobj,
                           struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
    if (count > FRAME_MAX_SIZE)
        return -EFBIG;

    mutex_lock(&ci_frame_lock);
    if (!ci_frame_buf) {
        ci_frame_buf = kzalloc(FRAME_MAX_SIZE, GFP_KERNEL);
        if (!ci_frame_buf) {
            mutex_unlock(&ci_frame_lock);
            return -ENOMEM;
        }
    }
    memcpy(ci_frame_buf, buf, count);
    ci_frame_size = count;
    mutex_unlock(&ci_frame_lock);

    pr_debug(DRIVER_NAME ": loaded frame %zu bytes\n", count);
    return count;
}

static ssize_t frame_size_show(struct kobject *kobj,
                               struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%zu\n", ci_frame_size);
}

static ssize_t pids_store(struct kobject *kobj,
                          struct kobj_attribute *attr,
                          const char *buf, size_t count)
{
    return parse_pids(buf, count);
}

static ssize_t pids_show(struct kobject *kobj,
                         struct kobj_attribute *attr, char *buf)
{
    int i, n = 0;
    mutex_lock(&ci_pid_lock);
    for (i = 0; i < ci_target_pid_count; i++)
        n += scnprintf(buf + n, PAGE_SIZE - n, "%d,", ci_target_pids[i]);
    if (n > 0) buf[n - 1] = '\n';
    else      buf[0] = '\n';
    mutex_unlock(&ci_pid_lock);
    return n ?: 1;
}

static ssize_t status_show(struct kobject *kobj,
                           struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE,
                     "enabled:%d\nframe_size:%zu\ntarget_pids:%d\ninject_count:%d\n",
                     atomic_read(&ci_enabled), ci_frame_size,
                     ci_target_pid_count, atomic_read(&ci_inject_count));
}

/* Attributes */
static struct kobj_attribute control_attr   = __ATTR_RW(control);
static struct kobj_attribute frame_attr     = __ATTR_WO(frame);
static struct kobj_attribute frame_size_attr = __ATTR_RO(frame_size);
static struct kobj_attribute pids_attr      = __ATTR_RW(pids);
static struct kobj_attribute status_attr    = __ATTR_RO(status);

static struct attribute *ci_attrs[] = {
    &control_attr.attr,
    &frame_attr.attr,
    &frame_size_attr.attr,
    &pids_attr.attr,
    &status_attr.attr,
    NULL,
};

ATTRIBUTE_GROUPS(ci);

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                */
/* ------------------------------------------------------------------ */
static int __init cam_inject_init(void)
{
    int ret;

    /* Create sysfs kobject */
    ci_kobj = kobject_create_and_add(DRIVER_NAME, kernel_kobj);
    if (!ci_kobj) {
        pr_err(DRIVER_NAME ": failed to create sysfs kobject\n");
        return -ENOMEM;
    }
    ret = sysfs_create_groups(ci_kobj, ci_groups);
    if (ret) {
        kobject_put(ci_kobj);
        return ret;
    }

    /* Create dedicated workqueue */
    ci_wq = alloc_workqueue(DRIVER_NAME, WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!ci_wq) {
        sysfs_remove_groups(ci_kobj, ci_groups);
        kobject_put(ci_kobj);
        return -ENOMEM;
    }

    /* Register kprobe on vb2_buffer_done */
    ci_kp.symbol_name = "vb2_buffer_done";
    ci_kp.pre_handler = handler_pre;
    ret = register_kprobe(&ci_kp);
    if (ret < 0) {
        pr_warn(DRIVER_NAME ": kprobe registration failed (%d) — "
                "vb2_buffer_done may not be exported; try CONFIG_VIDEOBUF2_CORE=y\n", ret);
        /* Non-fatal: module loads but does nothing */
    } else {
        pr_info(DRIVER_NAME ": kprobe registered on vb2_buffer_done\n");
    }

    pr_info(DRIVER_NAME ": loaded.\n");
    return 0;
}

static void __exit cam_inject_exit(void)
{
    unregister_kprobe(&ci_kp);

    if (ci_wq)
        destroy_workqueue(ci_wq);

    sysfs_remove_groups(ci_kobj, ci_groups);
    kobject_put(ci_kobj);

    kfree(ci_frame_buf);
    ci_frame_buf  = NULL;
    ci_frame_size = 0;

    pr_info(DRIVER_NAME ": unloaded.\n");
}

module_init(cam_inject_init);
module_exit(cam_inject_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("pentest-research");
MODULE_DESCRIPTION("Kernel-level camera frame injector via vb2_buffer_done hook");
MODULE_VERSION("0.1.0");
