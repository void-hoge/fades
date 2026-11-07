// SPDX-License-Identifier: GPL-2.0
// Exclusive queued owner of the solver and its AXI DMA SG engine.
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "fades_ioctl.h"

#define RING_SIZE 32
#define DMA_CR 0x00
#define DMA_SR 0x04
#define DMA_CUR 0x08
#define DMA_TAIL 0x10
#define DMA_RESET BIT(2)
#define DMA_IRQ (BIT(12) | BIT(14))
#define DMA_ERRORS GENMASK(10, 4)
#define BD_COMPLETE BIT(31)
#define BD_ERRORS GENMASK(30, 28)
#define BD_LENGTH GENMASK(22, 0)

/* AXI DMA PG021: 64-byte aligned, little-endian SG descriptors. Prelink
 * a ring larger than the maximum eight active descriptors. Only extend the
 * hardware tail; never rewrite an active/prefetched next pointer. */
struct fades_bd {
    __le32 next, next_hi, address, address_hi;
    __le32 reserved[2];
    __le32 control, status;
    __le32 app[5], padding[3];
} __aligned(64);

enum slot_state { CPU, QUEUED, ACTIVE, DONE };
struct fades_fifo { u32 indices[FADES_DMA_SLOTS], head, count; };
struct fades_slot { enum slot_state state; u32 records; };
struct fades_channel {
    void *cpu;
    dma_addr_t address;
    struct fades_bd *bd;
    dma_addr_t bd_address;
    u32 head, tail, active;
    u32 owners[RING_SIZE];
    struct fades_slot slots[FADES_DMA_SLOTS];
    struct fades_fifo pending, done;
};
struct fades_dma {
    struct miscdevice misc;
    struct device *dev;
    struct mutex lock;
    struct kref refs;
    wait_queue_head_t wait;
    struct fades_channel channels[2];
    void __iomem *control, *dma;
    int irq[2];
    bool opened, removed, faulted, running, sealed;
    size_t map_bytes;
};

static u32 fifo_front(struct fades_fifo *f) { return f->indices[f->head]; }
static u32 fifo_pop(struct fades_fifo *f)
{
    u32 index = fifo_front(f);
    f->head = (f->head + 1) % FADES_DMA_SLOTS;
    f->count--;
    return index;
}
static void fifo_push(struct fades_fifo *f, u32 index)
{
    f->indices[(f->head + f->count++) % FADES_DMA_SLOTS] = index;
}
static void __iomem *channel_regs(struct fades_dma *d, u32 q)
{
    return d->dma + (q == FADES_QUEUE_RX ? 0x30 : 0);
}
static void write_address(void __iomem *reg, dma_addr_t address)
{
    /* AXI DMA extended-address registers use low-then-high writes, matching
     * the provider's lo_hi_writeq (the MSB write commits the pointer). */
    writel(lower_32_bits(address), reg);
    writel(upper_32_bits(address), reg + 4);
}

static void fades_free(struct kref *ref)
{
    struct fades_dma *d = container_of(ref, struct fades_dma, refs);
    unsigned int q;

    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        if (c->bd)
            dma_free_coherent(d->dev, sizeof(*c->bd) * RING_SIZE, c->bd, c->bd_address);
        if (c->cpu)
            dma_free_coherent(d->dev, d->map_bytes, c->cpu, c->address);
    }
    put_device(d->dev);
    kfree(d);
}

/* Reset affects BOTH AXI DMA channels. Caller holds the mutex. */
static int hardware_reset(struct fades_dma *d)
{
    u32 status;
    int ret;

    writel(0, channel_regs(d, 0) + DMA_CR);
    writel(0, channel_regs(d, 1) + DMA_CR);
    writel(DMA_RESET, d->dma + DMA_CR);
    ret = readl_poll_timeout(d->dma + DMA_CR, status, !(status & DMA_RESET), 1, 100000);
    writel(1, d->control);
    readl(d->control);
    usleep_range(20, 40);
    return ret;
}

static int fades_stop(struct fades_dma *d)
{
    unsigned int q;
    int ret = hardware_reset(d);

    d->running = false;
    d->sealed = false;
    d->faulted = ret != 0;
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        memset(c->slots, 0, sizeof(c->slots));
        memset(&c->pending, 0, sizeof(c->pending));
        memset(&c->done, 0, sizeof(c->done));
        c->head = c->tail = c->active = 0;
    }
    wake_up_interruptible(&d->wait);
    return ret;
}

static int fades_start(struct fades_dma *d)
{
    unsigned int q, i;
    int ret;

    if (d->running)
        return -EBUSY;
    ret = hardware_reset(d);
    if (ret) {
        d->faulted = true;
        return ret;
    }
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        memset(c->bd, 0, sizeof(*c->bd) * RING_SIZE);
        c->head = c->tail = c->active = 0;
        for (i = 0; i < RING_SIZE; i++) {
            dma_addr_t next = c->bd_address + ((i + 1) % RING_SIZE) * sizeof(*c->bd);
            c->bd[i].next = cpu_to_le32(lower_32_bits(next));
            c->bd[i].next_hi = cpu_to_le32(upper_32_bits(next));
        }
        write_address(channel_regs(d, q) + DMA_CUR, c->bd_address);
        writel(DMA_IRQ | BIT(16) | BIT(0), channel_regs(d, q) + DMA_CR);
    }
    dma_wmb();
    writel(2, d->control);
    readl(d->control);
    usleep_range(20, 40);
    d->running = true;
    d->sealed = false;
    d->faulted = false;
    return 0;
}

static void submit_bd(struct fades_dma *d, u32 q, u32 index, u32 records)
{
    struct fades_channel *c = &d->channels[q];
    struct fades_bd *bd = &c->bd[c->tail];
    dma_addr_t address = c->address + index * FADES_DMA_SLOT_BYTES;

    bd->address = cpu_to_le32(lower_32_bits(address));
    bd->address_hi = cpu_to_le32(upper_32_bits(address));
    bd->control = cpu_to_le32(records * 64 | (q == FADES_QUEUE_TX ? BIT(27) | BIT(26) : 0));
    WRITE_ONCE(bd->status, 0);
    c->owners[c->tail] = index;
    c->slots[index].records = records;
    c->slots[index].state = ACTIVE;
    dma_wmb();
    write_address(channel_regs(d, q) + DMA_TAIL, c->bd_address + c->tail * sizeof(*bd));
    c->tail = (c->tail + 1) % RING_SIZE;
    c->active++;
}

/* Reserve an RX descriptor and a framing command before issuing its TX.
 * RX packet CONTENT is completion ordered, not associated with this TX. */
static void fades_schedule(struct fades_dma *d)
{
    struct fades_channel *tx = &d->channels[0], *rx = &d->channels[1];

    while (d->running && !d->faulted && tx->pending.count && rx->pending.count) {
        u32 t, r, records;
        if (!(readl(d->control + 12) & BIT(2)))
            break;
        t = fifo_pop(&tx->pending);
        r = fifo_pop(&rx->pending);
        records = tx->slots[t].records;
        submit_bd(d, FADES_QUEUE_RX, r, records);
        writel(records, d->control + 8);
        readl(d->control + 12);
        submit_bd(d, FADES_QUEUE_TX, t, records);
    }
}

static irqreturn_t fades_irq(int irq, void *arg)
{
    struct fades_dma *d = arg;
    unsigned int q;

    mutex_lock(&d->lock);
    if (d->removed)
        goto out;
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        u32 status = readl(channel_regs(d, q) + DMA_SR);
        writel(status & (DMA_IRQ | BIT(13)), channel_regs(d, q) + DMA_SR);
        if (!d->running)
            continue;
        if (status & DMA_ERRORS) {
            dev_err(d->dev, "channel %u DMA status %#x\n", q, status);
            d->faulted = true;
        }
        while (c->active && !d->faulted) {
            u32 done = le32_to_cpu(READ_ONCE(c->bd[c->head].status));
            u32 index = c->owners[c->head];
            if (!(done & BD_COMPLETE))
                break;
            dma_rmb();
            if ((done & BD_ERRORS) || (done & BD_LENGTH) != c->slots[index].records * 64) {
                dev_err(d->dev, "channel %u descriptor %#x expected %u bytes\n", q, done, c->slots[index].records * 64);
                d->faulted = true;
                break;
            }
            c->slots[index].state = DONE;
            fifo_push(&c->done, index);
            c->head = (c->head + 1) % RING_SIZE;
            c->active--;
        }
    }
    if (d->running && (readl(d->control + 12) & 1))
        d->faulted = true;
    if (d->faulted) {
        /* Quiesce both engines before allowing userspace to recover. Keep
         * slot ownership until STREAMOFF, including on partial transfers. */
        writel(0, channel_regs(d, 0) + DMA_CR);
        writel(0, channel_regs(d, 1) + DMA_CR);
    } else {
        fades_schedule(d);
    }
    wake_up_interruptible(&d->wait);
out:
    mutex_unlock(&d->lock);
    return IRQ_HANDLED;
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
    if (!d->removed)
        fades_stop(d);
    d->opened = false;
    mutex_unlock(&d->lock);
    kref_put(&d->refs, fades_free);
    return 0;
}

static long fades_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct fades_dma *d = file->private_data;
    void __user *user = (void __user *)arg;
    struct fades_buffer b;
    struct fades_channel *c;
    int ret = 0;

    if (mutex_lock_interruptible(&d->lock))
        return -ERESTARTSYS;
    if (d->removed) { ret = -ENODEV; goto out; }
    switch (cmd) {
    case FADES_DMA_INFO: {
        struct fades_dma_info info = {
            .abi_version = FADES_DMA_ABI_VERSION, .slots = FADES_DMA_SLOTS,
            .slot_bytes = FADES_DMA_SLOT_BYTES, .map_bytes = d->map_bytes,
        };
        ret = copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
        break;
    }
    case FADES_STREAMON:
        ret = fades_start(d);
        if (!ret) fades_schedule(d);
        break;
    case FADES_STREAMOFF:
        ret = fades_stop(d);
        break;
    case FADES_END_INPUT:
        if (!d->running) ret = -EINVAL;
        else if (d->faulted) ret = -EIO;
        else d->sealed = true;
        break;
    case FADES_QBUF:
    case FADES_DQBUF:
        if (copy_from_user(&b, user, sizeof(b))) { ret = -EFAULT; break; }
        if (b.queue > 1 || b.reserved) { ret = -EINVAL; break; }
        if (d->faulted) { ret = -EIO; break; }
        c = &d->channels[b.queue];
        if (cmd == FADES_QBUF) {
            if (b.index >= FADES_DMA_SLOTS ||
                (b.queue == FADES_QUEUE_TX ? !b.records || b.records > FADES_DMA_SLOT_BYTES / 64 : b.records != 0)) {
                ret = -EINVAL; break;
            }
            if (b.queue == FADES_QUEUE_TX && d->sealed) { ret = -EPIPE; break; }
            if (c->slots[b.index].state != CPU) { ret = -EBUSY; break; }
            c->slots[b.index].state = QUEUED;
            c->slots[b.index].records = b.records;
            fifo_push(&c->pending, b.index);
            fades_schedule(d);
        } else {
            if (!c->done.count) { ret = -EAGAIN; break; }
            b.index = fifo_front(&c->done);
            b.records = c->slots[b.index].records;
            /* Do not lose ownership/completion if copy_to_user fails. */
            if (copy_to_user(user, &b, sizeof(b))) { ret = -EFAULT; break; }
            fifo_pop(&c->done);
            c->slots[b.index].state = CPU;
        }
        break;
    case FADES_SOLVER_STATS: {
        struct fades_solver_stats stats;
        u32 status;
        unsigned int i;
        writel(1, d->control + 16);
        ret = readl_poll_timeout(d->control + 12, status, !(status & 2), 1, 100000);
        if (ret) break;
        for (i = 0; i < ARRAY_SIZE(stats.counters); i++) {
            u32 low = readl(d->control + 0x100 + i * 8);
            u32 high = readl(d->control + 0x104 + i * 8);
            stats.counters[i] = (u64)high << 32 | low;
        }
        ret = copy_to_user(user, &stats, sizeof(stats)) ? -EFAULT : 0;
        break;
    }
    default:
        ret = -ENOTTY;
    }
out:
    mutex_unlock(&d->lock);
    return ret;
}

static __poll_t fades_poll(struct file *file, poll_table *wait)
{
    struct fades_dma *d = file->private_data;
    __poll_t events = 0;

    poll_wait(file, &d->wait, wait);
    mutex_lock(&d->lock);
    if (d->removed) events = EPOLLERR | EPOLLHUP;
    else if (d->faulted) events = EPOLLERR;
    else {
        if (d->channels[0].done.count) events |= EPOLLOUT | EPOLLWRNORM;
        if (d->channels[1].done.count) events |= EPOLLIN | EPOLLRDNORM;
    }
    mutex_unlock(&d->lock);
    return events;
}

static int fades_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct fades_dma *d = file->private_data;
    unsigned long offset = vma->vm_pgoff;
    struct fades_channel *c;
    int ret;

    if (!(vma->vm_flags & VM_SHARED) || vma->vm_end - vma->vm_start != d->map_bytes)
        return -EINVAL;
    if (offset == 0) c = &d->channels[0];
    else if (offset == d->map_bytes >> PAGE_SHIFT) c = &d->channels[1];
    else return -EINVAL;
    mutex_lock(&d->lock);
    if (d->removed) ret = -ENODEV;
    else {
        vma->vm_pgoff = 0;
        ret = dma_mmap_coherent(d->dev, vma, c->cpu, c->address, d->map_bytes);
    }
    mutex_unlock(&d->lock);
    /* The VMA keeps the file, and therefore the buffers, alive. */
    return ret;
}

static const struct file_operations fades_fops = {
    .owner = THIS_MODULE, .open = fades_open, .release = fades_release,
    .unlocked_ioctl = fades_ioctl, .compat_ioctl = compat_ptr_ioctl,
    .mmap = fades_mmap, .poll = fades_poll, .llseek = no_llseek,
};

static int fades_probe(struct platform_device *pdev)
{
    struct fades_dma *d;
    struct clk *clk;
    unsigned int q, requested = 0;
    int ret;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d) return -ENOMEM;
    kref_init(&d->refs);
    mutex_init(&d->lock);
    init_waitqueue_head(&d->wait);
    d->dev = get_device(&pdev->dev);
    d->map_bytes = FADES_DMA_SLOTS * FADES_DMA_SLOT_BYTES;
    ret = dma_set_mask_and_coherent(d->dev, DMA_BIT_MASK(40));
    if (ret) goto fail;
    clk = devm_clk_get_enabled(d->dev, "dma");
    if (IS_ERR(clk)) { ret = PTR_ERR(clk); goto fail; }
    clk = devm_clk_get_enabled(d->dev, "core");
    if (IS_ERR(clk)) { ret = PTR_ERR(clk); goto fail; }
    d->control = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(d->control)) { ret = PTR_ERR(d->control); goto fail; }
    d->dma = devm_platform_ioremap_resource(pdev, 1);
    if (IS_ERR(d->dma)) { ret = PTR_ERR(d->dma); goto fail; }
    if (readl(d->control + 20) != 0x46414402) { ret = -ENODEV; goto fail; }
    ret = hardware_reset(d);
    if (ret) goto fail;
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        c->cpu = dma_alloc_coherent(d->dev, d->map_bytes, &c->address, GFP_KERNEL);
        c->bd = dma_alloc_coherent(d->dev, sizeof(*c->bd) * RING_SIZE, &c->bd_address, GFP_KERNEL);
        if (!c->cpu || !c->bd) { ret = -ENOMEM; goto fail; }
        d->irq[q] = platform_get_irq(pdev, q);
        if (d->irq[q] < 0) { ret = d->irq[q]; goto fail; }
        ret = request_threaded_irq(d->irq[q], NULL, fades_irq, IRQF_ONESHOT, dev_name(d->dev), d);
        if (ret) goto fail;
        requested++;
    }
    d->misc.minor = MISC_DYNAMIC_MINOR;
    d->misc.name = "fades";
    d->misc.fops = &fades_fops;
    d->misc.parent = d->dev;
    d->misc.mode = 0600;
    ret = misc_register(&d->misc);
    if (ret) goto fail;
    platform_set_drvdata(pdev, d);
    dev_info(d->dev, "ABI v2: %u slots x %u bytes, direct AXI DMA SG\n", FADES_DMA_SLOTS, FADES_DMA_SLOT_BYTES);
    return 0;
fail:
    while (requested) free_irq(d->irq[--requested], d);
    kref_put(&d->refs, fades_free);
    return ret;
}

static void fades_remove(struct platform_device *pdev)
{
    struct fades_dma *d = platform_get_drvdata(pdev);

    misc_deregister(&d->misc);
    mutex_lock(&d->lock);
    fades_stop(d);
    d->removed = true;
    wake_up_interruptible(&d->wait);
    mutex_unlock(&d->lock);
    free_irq(d->irq[0], d);
    free_irq(d->irq[1], d);
    kref_put(&d->refs, fades_free);
}

static const struct of_device_id fades_match[] = {
    { .compatible = "fades,solver-2.0" }, { }
};
MODULE_DEVICE_TABLE(of, fades_match);
static struct platform_driver fades_driver = {
    .probe = fades_probe, .remove_new = fades_remove,
    .driver = { .name = "fades", .of_match_table = fades_match },
};
module_platform_driver(fades_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FADES queued solver and dedicated AXI DMA SG driver");
