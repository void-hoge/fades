// SPDX-License-Identifier: GPL-2.0
// FADES DMAengine client. Owns buffers, solver control and completion handling.
#include <linux/completion.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "fades_ioctl.h"

struct fades_buffer {
    struct device *dev;
    void *cpu;
    dma_addr_t dma;
};

struct fades_dma {
    struct miscdevice misc;
    struct mutex lock;
    struct kref refs;
    struct dma_chan *tx;
    struct dma_chan *rx;
    struct fades_buffer tx_buf;
    struct fades_buffer rx_buf;
    size_t map_bytes;
    bool opened;
    bool removed;
    bool faulted;
    void __iomem *control;
};

struct fades_packet {
    struct completion done;
    dma_cookie_t cookie;
};

static void fades_buffer_free(struct fades_buffer *buf, size_t size)
{
    if (buf->cpu)
        dma_free_coherent(buf->dev, size, buf->cpu, buf->dma);
    if (buf->dev)
        put_device(buf->dev);
}

static void fades_free(struct kref *ref)
{
    struct fades_dma *d = container_of(ref, struct fades_dma, refs);

    fades_buffer_free(&d->rx_buf, d->map_bytes);
    fades_buffer_free(&d->tx_buf, d->map_bytes);
    kfree(d);
}

static void fades_done(void *arg)
{
    struct fades_packet *packet = arg;

    complete(&packet->done);
}

static int fades_submit(struct dma_chan *chan, dma_addr_t addr, u32 bytes,
                        enum dma_transfer_direction dir, struct fades_packet *packet)
{
    struct dma_async_tx_descriptor *desc;

    init_completion(&packet->done);
    desc = dmaengine_prep_slave_single(chan, addr, bytes, dir,
                                      DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
    if (!desc)
        return -EIO;
    desc->callback = fades_done;
    desc->callback_param = packet;
    packet->cookie = dmaengine_submit(desc);
    return dma_submit_error(packet->cookie);
}

static int fades_wait(struct dma_chan *chan, struct fades_packet *packet,
                      unsigned long deadline)
{
    long left, ret;

    /* A completion already delivered at the deadline is still successful. */
    if (!try_wait_for_completion(&packet->done)) {
        left = (long)(deadline - jiffies);
        if (left <= 0)
            return -ETIMEDOUT;
        ret = wait_for_completion_interruptible_timeout(&packet->done, left);
        if (ret < 0)
            return ret;
        if (!ret)
            return -ETIMEDOUT;
    }
    return dma_async_is_tx_complete(chan, packet->cookie, NULL, NULL) == DMA_COMPLETE
        ? 0 : -EIO;
}

/* Caller holds lock. Callbacks refer to this stack only until termination below. */
static int fades_run(struct fades_dma *d, struct fades_dma_run *run)
{
    struct fades_packet tx[FADES_DMA_SLOTS], rx[FADES_DMA_SLOTS];
    u64 start = ktime_get_ns();
    unsigned long deadline;
    u32 round, slot;
    int ret;

    for (round = 0; round < run->rounds; round++) {
        if (signal_pending(current)) {
            ret = -ERESTARTSYS;
            goto stop;
        }
        /* One run is drained completely before the next reset. Hold the
         * reset long enough for both clocks and XPM reset initialization. */
        writel(1, d->control);
        readl(d->control);
        usleep_range(20, 40);
        writel(run->bytes / 64 * run->slots, d->control + 4);
        writel(run->bytes / 64, d->control + 8);
        writel(2, d->control);
        readl(d->control);
        usleep_range(20, 40);
        /* Queue all receive buffers before starting either channel. Each TX
         * descriptor is one packet, with TLAST at the end of run->bytes. */
        for (slot = 0; slot < run->slots; slot++) {
            ret = fades_submit(d->rx, d->rx_buf.dma + slot * FADES_DMA_SLOT_BYTES,
                               run->bytes, DMA_DEV_TO_MEM, &rx[slot]);
            if (ret)
                goto stop;
        }
        for (slot = 0; slot < run->slots; slot++) {
            ret = fades_submit(d->tx, d->tx_buf.dma + slot * FADES_DMA_SLOT_BYTES,
                               run->bytes, DMA_MEM_TO_DEV, &tx[slot]);
            if (ret)
                goto stop;
        }
        deadline = jiffies + msecs_to_jiffies(run->timeout_ms);
        dma_wmb();
        dma_async_issue_pending(d->rx);
        dma_async_issue_pending(d->tx);
        for (slot = 0; slot < run->slots; slot++) {
            ret = fades_wait(d->rx, &rx[slot], deadline);
            if (ret)
                goto stop;
            ret = fades_wait(d->tx, &tx[slot], deadline);
            if (ret)
                goto stop;
        }
        if (readl(d->control + 12) & 1) {
            ret = -EPROTO;
            goto stop;
        }
    }
    dma_rmb();
    run->elapsed_ns = ktime_get_ns() - start;
    run->bytes_per_direction = (u64)run->bytes * run->slots * run->rounds;
    /* COMPLETE can become visible before the callback tasklet has returned.
     * Drain it before the stack holding callback arguments goes away. */
    dmaengine_synchronize(d->rx);
    dmaengine_synchronize(d->tx);
    return 0;

stop:
    dmaengine_terminate_sync(d->tx);
    dmaengine_terminate_sync(d->rx);
    /* A timeout may leave a partial packet in the stream FIFO. Require FPGA
     * reload rather than silently treating the next request as recovered. */
    d->faulted = true;
    return ret;
}

static int fades_open(struct inode *inode, struct file *file)
{
    struct fades_dma *d = container_of(file->private_data, struct fades_dma, misc);
    int ret = 0;

    mutex_lock(&d->lock);
    if (d->removed)
        ret = -ENODEV;
    else if (d->opened)
        ret = -EBUSY;
    else {
        d->opened = true;
        kref_get(&d->refs);
        file->private_data = d;
    }
    mutex_unlock(&d->lock);
    return ret;
}

static int fades_release(struct inode *inode, struct file *file)
{
    struct fades_dma *d = file->private_data;

    mutex_lock(&d->lock);
    d->opened = false;
    mutex_unlock(&d->lock);
    kref_put(&d->refs, fades_free);
    return 0;
}

static long fades_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct fades_dma *d = file->private_data;
    struct fades_dma_info info = {
        .abi_version = FADES_DMA_ABI_VERSION,
        .slots = FADES_DMA_SLOTS,
        .slot_bytes = FADES_DMA_SLOT_BYTES,
        .map_bytes = d->map_bytes,
    };
    struct fades_dma_run run;
    void __user *user = (void __user *)arg;
    int ret = 0;

    if (mutex_lock_interruptible(&d->lock))
        return -ERESTARTSYS;
    if (d->removed) {
        ret = -ENODEV;
        goto out;
    }
    if (cmd == FADES_DMA_INFO) {
        ret = copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
    } else if (cmd == FADES_DMA_RUN) {
        if (d->faulted) {
            ret = -EIO;
            goto out;
        }
        if (copy_from_user(&run, user, sizeof(run))) {
            ret = -EFAULT;
            goto out;
        }
        if (!run.bytes || run.bytes % 64 || run.bytes > FADES_DMA_SLOT_BYTES ||
            !run.slots || run.slots > FADES_DMA_SLOTS ||
            !run.rounds || run.rounds > 100000 ||
            !run.timeout_ms || run.timeout_ms > 60000) {
            ret = -EINVAL;
            goto out;
        }
        run.elapsed_ns = 0;
        run.bytes_per_direction = 0;
        ret = fades_run(d, &run);
        if (!ret && copy_to_user(user, &run, sizeof(run)))
            ret = -EFAULT;
    } else if (cmd == FADES_SOLVER_STATS) {
        struct fades_solver_stats stats;
        u32 status;
        unsigned int i;

        writel(1, d->control + 16);
        ret = readl_poll_timeout(d->control + 12, status, !(status & 2), 1, 100000);
        if (ret)
            goto out;
        for (i = 0; i < ARRAY_SIZE(stats.counters); i++) {
            u32 low = readl(d->control + 0x100 + i * 8);
            u32 high = readl(d->control + 0x104 + i * 8);

            stats.counters[i] = (u64)high << 32 | low;
        }
        ret = copy_to_user(user, &stats, sizeof(stats)) ? -EFAULT : 0;
    } else {
        ret = -ENOTTY;
    }
out:
    mutex_unlock(&d->lock);
    return ret;
}

static int fades_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct fades_dma *d = file->private_data;
    struct fades_buffer *buf;
    unsigned long offset = vma->vm_pgoff;
    int ret;

    if (!(vma->vm_flags & VM_SHARED) || vma->vm_end - vma->vm_start != d->map_bytes)
        return -EINVAL;
    if (offset == 0)
        buf = &d->tx_buf;
    else if (offset == d->map_bytes >> PAGE_SHIFT)
        buf = &d->rx_buf;
    else
        return -EINVAL;
    mutex_lock(&d->lock);
    if (d->removed) {
        ret = -ENODEV;
    } else {
        vma->vm_pgoff = 0;
        ret = dma_mmap_coherent(buf->dev, vma, buf->cpu, buf->dma, d->map_bytes);
    }
    mutex_unlock(&d->lock);
    /* The VMA holds a file reference, keeping the buffers alive until munmap. */
    return ret;
}

static const struct file_operations fades_fops = {
    .owner = THIS_MODULE,
    .open = fades_open,
    .release = fades_release,
    .unlocked_ioctl = fades_ioctl,
    .compat_ioctl = compat_ptr_ioctl,
    .mmap = fades_mmap,
    .llseek = no_llseek,
};

static int fades_buffer_alloc(struct fades_buffer *buf, struct dma_chan *chan, size_t size)
{
    buf->dev = get_device(dmaengine_get_dma_device(chan));
    buf->cpu = dma_alloc_coherent(buf->dev, size, &buf->dma, GFP_KERNEL);
    return buf->cpu ? 0 : -ENOMEM;
}

static int fades_probe(struct platform_device *pdev)
{
    struct fades_dma *d;
    struct clk *clk;
    int ret;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;
    kref_init(&d->refs);
    mutex_init(&d->lock);
    d->map_bytes = FADES_DMA_SLOTS * FADES_DMA_SLOT_BYTES;
    clk = devm_clk_get_enabled(&pdev->dev, "dma");
    if (IS_ERR(clk)) { ret = PTR_ERR(clk); goto free; }
    clk = devm_clk_get_enabled(&pdev->dev, "core");
    if (IS_ERR(clk)) { ret = PTR_ERR(clk); goto free; }
    d->control = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(d->control)) {
        ret = PTR_ERR(d->control);
        goto free;
    }
    if (readl(d->control + 20) != 0x46414401) {
        ret = -ENODEV;
        goto free;
    }
    d->tx = dma_request_chan(&pdev->dev, "tx");
    if (IS_ERR(d->tx)) {
        ret = PTR_ERR(d->tx);
        goto free;
    }
    d->rx = dma_request_chan(&pdev->dev, "rx");
    if (IS_ERR(d->rx)) {
        ret = PTR_ERR(d->rx);
        goto release_tx;
    }
    ret = fades_buffer_alloc(&d->tx_buf, d->tx, d->map_bytes);
    if (ret)
        goto release_rx;
    ret = fades_buffer_alloc(&d->rx_buf, d->rx, d->map_bytes);
    if (ret)
        goto release_rx;
    d->misc.minor = MISC_DYNAMIC_MINOR;
    d->misc.name = "fades";
    d->misc.fops = &fades_fops;
    d->misc.parent = &pdev->dev;
    d->misc.mode = 0600;
    ret = misc_register(&d->misc);
    if (ret)
        goto release_rx;
    platform_set_drvdata(pdev, d);
    dev_info(&pdev->dev, "ready: %u slots x %u bytes per direction\n",
             FADES_DMA_SLOTS, FADES_DMA_SLOT_BYTES);
    return 0;

release_rx:
    dma_release_channel(d->rx);
release_tx:
    dma_release_channel(d->tx);
free:
    kref_put(&d->refs, fades_free);
    return dev_err_probe(&pdev->dev, ret, "FADES setup failed\n");
}

static void fades_remove(struct platform_device *pdev)
{
    struct fades_dma *d = platform_get_drvdata(pdev);

    misc_deregister(&d->misc);
    mutex_lock(&d->lock);
    d->removed = true;
    dmaengine_terminate_sync(d->tx);
    dmaengine_terminate_sync(d->rx);
    dma_release_channel(d->rx);
    dma_release_channel(d->tx);
    mutex_unlock(&d->lock);
    kref_put(&d->refs, fades_free);
}

static const struct of_device_id fades_match[] = {
    { .compatible = "fades,solver-1.0" },
    { }
};
MODULE_DEVICE_TABLE(of, fades_match);

static struct platform_driver fades_driver = {
    .probe = fades_probe,
    .remove_new = fades_remove,
    .driver = {
        .name = "fades",
        .of_match_table = fades_match,
    },
};
module_platform_driver(fades_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FADES FPGA Sudoku solver DMAengine client");
