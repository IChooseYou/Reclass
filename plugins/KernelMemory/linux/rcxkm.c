/*
 * rcxkm.c -- Linux kernel module stub for Reclass kernel memory provider.
 *
 * Provides /dev/rcxkm char device with ioctl() dispatch using the same
 * protocol structs as the Windows driver (rcx_drv_protocol.h).
 *
 * Build: make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 *
 * TODO: implement handlers (currently returns -ENOSYS for all IOCTLs).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>

#include "../rcx_drv_protocol.h"

#define DEVICE_NAME "rcxkm"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Reclass");
MODULE_DESCRIPTION("Reclass kernel memory provider (stub)");

/* ── IOCTL dispatch ─────────────────────────────────────────────────── */

static long rcxkm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    (void)filp;
    (void)arg;

    switch (cmd) {
    case IOCTL_RCX_READ_MEMORY:
        /* TODO: find_get_pid(pid) -> get_task_struct -> access_process_vm() */
        return -ENOSYS;

    case IOCTL_RCX_WRITE_MEMORY:
        /* TODO: access_process_vm() with FOLL_WRITE */
        return -ENOSYS;

    case IOCTL_RCX_QUERY_REGIONS:
        /* TODO: walk target mm->mmap via VMA iteration */
        return -ENOSYS;

    case IOCTL_RCX_QUERY_PEB:
        /* N/A on Linux (no PEB); could return mm->start_brk or similar */
        return -ENOSYS;

    case IOCTL_RCX_QUERY_MODULES:
        /* TODO: walk target /proc/pid/maps or mm VMAs */
        return -ENOSYS;

    case IOCTL_RCX_QUERY_TEBS:
        /* N/A on Linux (no TEB) */
        return -ENOSYS;

    case IOCTL_RCX_PING: {
        struct RcxDrvPingResponse resp = {
            .version = RCX_DRV_VERSION,
            .driverBuild = 1,
        };
        if (copy_to_user((void __user *)arg, &resp, sizeof(resp)))
            return -EFAULT;
        return 0;
    }

    case IOCTL_RCX_READ_PHYS:
        /* TODO: ioremap() + memcpy_fromio() */
        return -ENOSYS;

    case IOCTL_RCX_WRITE_PHYS:
        /* TODO: ioremap() + memcpy_toio() */
        return -ENOSYS;

    default:
        return -EINVAL;
    }
}

/* ── File operations ────────────────────────────────────────────────── */

static int rcxkm_open(struct inode *inode, struct file *filp)
{
    (void)inode; (void)filp;
    return 0;
}

static int rcxkm_release(struct inode *inode, struct file *filp)
{
    (void)inode; (void)filp;
    return 0;
}

static const struct file_operations rcxkm_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = rcxkm_ioctl,
    .open           = rcxkm_open,
    .release        = rcxkm_release,
};

static struct miscdevice rcxkm_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &rcxkm_fops,
};

/* ── Module init/exit ───────────────────────────────────────────────── */

static int __init rcxkm_init(void)
{
    int ret = misc_register(&rcxkm_device);
    if (ret) {
        pr_err("rcxkm: failed to register misc device (err=%d)\n", ret);
        return ret;
    }
    pr_info("rcxkm: loaded, device /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit rcxkm_exit(void)
{
    misc_deregister(&rcxkm_device);
    pr_info("rcxkm: unloaded\n");
}

module_init(rcxkm_init);
module_exit(rcxkm_exit);
