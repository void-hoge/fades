// SPDX-License-Identifier: GPL-2.0
// Exclusive queued management of the solver and AXI DMA SG engine.
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

static_assert(sizeof(struct fades_dma_info) == 24);
static_assert(sizeof(struct fades_dma_config) == 16);
static_assert(sizeof(struct fades_buffer_info) == 24);
static_assert(sizeof(struct fades_buffer) == 16);
static_assert(sizeof(struct fades_solver_stats) == 352);
static_assert(offsetof(struct fades_solver_stats, accepted) == 0 * 8);
static_assert(offsetof(struct fades_solver_stats, emitted) == 1 * 8);
static_assert(offsetof(struct fades_solver_stats, input_stall_cycles) == 2 * 8);
static_assert(offsetof(struct fades_solver_stats, dispatched) == 3 * 8);
static_assert(offsetof(struct fades_solver_stats, collected) == 4 * 8);
static_assert(offsetof(struct fades_solver_stats, queue_empty_cycles) == 5 * 8);
static_assert(offsetof(struct fades_solver_stats, core_dispatched) == 6 * 8);
static_assert(offsetof(struct fades_solver_stats, core_collected) == 12 * 8);
static_assert(offsetof(struct fades_solver_stats, core_busy_cycles) == 18 * 8);
static_assert(offsetof(struct fades_solver_stats, core_output_wait_cycles) == 24 * 8);
static_assert(offsetof(struct fades_solver_stats, queue_peak) == 30 * 8);
static_assert(offsetof(struct fades_solver_stats, processing_cycles) == 31 * 8);
static_assert(offsetof(struct fades_solver_stats, core_input_wait_cycles) == 32 * 8);
static_assert(offsetof(struct fades_solver_stats, core_max_inflight) == 38 * 8);

#ifndef FADES_RING_SIZE
#define FADES_RING_SIZE 32
#endif
#define RING_SIZE FADES_RING_SIZE
static_assert(RING_SIZE >= 2);
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

/* 64-byte transfer descriptor read and written by AXI DMA. */
struct fades_bd {
    __le32 next;       /* Low DMA address of the next BD; immutable after initialization. */
    __le32 next_hi;    /* High DMA address of the next BD. */
    __le32 address;    /* Low DMA address of the data buffer. */
    __le32 address_hi; /* High DMA address of the data buffer. */
    __le32 reserved[2];
    __le32 control;    /* Transfer length and TX packet boundary flags. */
    __le32 status;     /* DMA completion, error status and actual transfer length. */
    __le32 app[5];     /* Unused. */
    __le32 padding[3]; /* Padding to a 64-byte boundary. */
} __aligned(64);

enum buffer_state {
    CPU,    /* Owned by userspace. */
    QUEUED, /* Waiting for DMA submission. */
    ACTIVE, /* Submitted to DMA. */
    DONE,   /* DMA complete; waiting for userspace dequeue. */
};

/* FIFO of buffer indices awaiting DMA submission or userspace dequeue. */
struct fades_fifo {
    u32 *indices;
    u32 capacity;
    u32 head;
    u32 count; /* Distinguishes empty from full, allowing every entry to be used. */
};

/* Addresses and ownership state of a 1 MiB data buffer. */
struct fades_data_buffer {
    enum buffer_state state;
    u32 records; /* TX input count; RX capacity at submission or actual count at completion. */
    void *cpu;   /* Kernel virtual address. */
    struct page *pages; /* RX DMA allocation; NULL for TX. */
    dma_addr_t address; /* DMA address of the data buffer. */
};

/* Buffer pool, pending queues and DMA ring for one TX or RX channel. */
struct fades_channel {
    struct fades_bd *bd;   /* BD ring. */
    dma_addr_t bd_address; /* DMA address of the ring. */
    u32 head;              /* Next BD to check for completion. */
    u32 tail;              /* Next BD to submit. */
    u32 active;            /* Number of submitted BDs not yet reclaimed. */
    u32 owners[RING_SIZE]; /* Corresponding buffers index for each BD. */
    struct fades_data_buffer *buffers;
    u32 count;                 /* Number of allocated buffers. */
    struct fades_fifo pending; /* Waiting for DMA submission. */
    struct fades_fifo done;    /* Waiting for userspace dequeue. */
};

/* Device state for the solver and its dedicated AXI DMA. */
struct fades_device {
    struct miscdevice misc;
    struct device *dev;
    struct mutex lock;                /* Serializes buffer, queue and hardware operations. */
    struct kref refs;                 /* References held by the device and open file. */
    wait_queue_head_t wait;           /* Notifies poll waiters of completion, errors and stop. */
    struct fades_channel channels[2]; /* TX/RX channels indexed by fades_queue. */
    void __iomem *control;            /* Solver control registers. */
    void __iomem *dma;                /* AXI DMA registers. */
    int irq[2];                       /* TX/RX IRQ numbers. */
    bool opened;                      /* Exclusive open remains held while VMAs exist. */
    bool removed;
    bool faulted;
    bool running;
    bool stopped; /* After successful STREAMOFF, poll reports HUP until STREAMON. */
};

/* Get the AXI DMA register block for the selected direction. */
static void __iomem *channel_regs(struct fades_device *d, enum fades_queue direction) {
    return d->dma + (direction == FADES_QUEUE_RX ? 0x30 : 0);
}

/* Free data buffers and queues after DMA stops, under exclusion; partial allocations are supported.
 */
static void free_buffers(struct fades_device *d) {
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        struct fades_channel *c = &d->channels[direction];
        for (unsigned int i = 0; c->buffers && i < c->count; i++) {
            struct fades_data_buffer *s = &c->buffers[i];
            if (s->pages) {
                dma_free_pages(d->dev, FADES_DMA_SLOT_BYTES, s->pages, s->address,
                               DMA_BIDIRECTIONAL);
            } else if (s->cpu) {
                dma_free_coherent(d->dev, FADES_DMA_SLOT_BYTES, s->cpu, s->address);
            }
        }
        kfree(c->buffers);
        kfree(c->pending.indices);
        kfree(c->done.indices);
        c->buffers = NULL;
        c->count = 0;
        memset(&c->pending, 0, sizeof(c->pending));
        memset(&c->done, 0, sizeof(c->done));
    }
}

/* Reset both DMA channels and the solver under exclusion; failure does not guarantee DMA stopped.
 */
static int hardware_reset(struct fades_device *d) {
    writel(0, channel_regs(d, FADES_QUEUE_TX) + DMA_CR);
    writel(0, channel_regs(d, FADES_QUEUE_RX) + DMA_CR);
    writel(DMA_RESET, d->dma + DMA_CR);
    u32 status;
    int ret = readl_poll_timeout(d->dma + DMA_CR, status, !(status & DMA_RESET), 1, 100000);

    writel(1, d->control);
    readl(d->control);
    usleep_range(20, 40);
    return ret;
}

/* Stop transfers under the mutex; on success, return all buffers to CPU ownership. */
static int fades_stop(struct fades_device *d) {
    int ret = hardware_reset(d);

    d->running = false;
    d->faulted = ret != 0;
    if (ret) {
        /* DMA ownership is uncertain; do not sync, reuse or free its memory. */
        wake_up_interruptible(&d->wait);
        return ret;
    }
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        struct fades_channel *c = &d->channels[direction];
        if (direction == FADES_QUEUE_RX) {
            for (unsigned int index = 0; index < c->count; index++) {
                if (c->buffers[index].state == ACTIVE) {
                    dma_sync_single_for_cpu(d->dev, c->buffers[index].address, FADES_DMA_SLOT_BYTES,
                                            DMA_BIDIRECTIONAL);
                }
            }
        }
        for (unsigned int index = 0; index < c->count; index++) {
            c->buffers[index].state = CPU;
            c->buffers[index].records = 0;
        }
        c->pending.head = c->pending.count = 0;
        c->done.head = c->done.count = 0;
        c->head = c->tail = c->active = 0;
    }
    wake_up_interruptible(&d->wait);
    return ret;
}

/* Open the device exclusively and give the file a reference. */
static int fades_open(struct inode *inode, struct file *file) {
    struct fades_device *d = container_of(file->private_data, struct fades_device, misc);
    int ret = 0;

    mutex_lock(&d->lock);
    if (d->removed) {
        ret = -ENODEV;
    } else if (d->opened) {
        ret = -EBUSY;
    } else {
        /* A failed reset on the previous close leaves memory quarantined.
         * Recover before exposing a new session. */
        if (d->channels[FADES_QUEUE_TX].count) {
            ret = fades_stop(d);
            if (!ret) {
                free_buffers(d);
            }
        }
        if (!ret) {
            d->opened = true;
            d->stopped = false;
            kref_get(&d->refs);
            file->private_data = d;
        }
    }
    mutex_unlock(&d->lock);
    return ret;
}

/* Free the buffers, rings and state when the last device reference is released. */
static void fades_free(struct kref *ref) {
    struct fades_device *d = container_of(ref, struct fades_device, refs);
    free_buffers(d);
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        struct fades_channel *c = &d->channels[direction];
        if (c->bd) {
            dma_free_coherent(d->dev, sizeof(*c->bd) * RING_SIZE, c->bd, c->bd_address);
        }
    }
    put_device(d->dev);
    kfree(d);
}

/* Stop transfers and release exclusive ownership on the last file reference. */
static int fades_release(struct inode *inode, struct file *file) {
    struct fades_device *d = file->private_data;
    mutex_lock(&d->lock);
    /* After removal, use the previous stop result without touching registers. */
    int ret = d->removed ? (d->faulted ? -EIO : 0) : fades_stop(d);
    if (!ret) {
        free_buffers(d);
    }
    d->opened = false;
    mutex_unlock(&d->lock);
    kref_put(&d->refs, fades_free);
    return 0;
}

/* Map a QUERYBUF-selected buffer into userspace. */
static int fades_mmap(struct file *file, struct vm_area_struct *vma) {
    struct fades_device *d = file->private_data;
    const unsigned long pages = FADES_DMA_SLOT_BYTES >> PAGE_SHIFT;
    unsigned long number = vma->vm_pgoff / pages;
    unsigned long queue_number = number / FADES_DMA_MAX_BUFFERS;
    unsigned long index = number % FADES_DMA_MAX_BUFFERS;
    /* Validate the untrusted offset in a wide integer before converting it to an enum. */
    if (!(vma->vm_flags & VM_SHARED) || vma->vm_end - vma->vm_start != FADES_DMA_SLOT_BYTES ||
        vma->vm_pgoff % pages || queue_number > FADES_QUEUE_RX) {
        return -EINVAL;
    }
    enum fades_queue direction = (enum fades_queue)queue_number;
    int ret;
    mutex_lock(&d->lock);
    if (d->removed) {
        ret = -ENODEV;
    } else if (index >= d->channels[direction].count) {
        ret = -EINVAL;
    } else {
        struct fades_data_buffer *s = &d->channels[direction].buffers[index];
        vma->vm_pgoff = 0;
        if (s->pages) {
            ret = dma_mmap_pages(d->dev, vma, FADES_DMA_SLOT_BYTES, s->pages);
        } else {
            ret = dma_mmap_coherent(d->dev, vma, s->cpu, s->address, FADES_DMA_SLOT_BYTES);
        }
    }
    mutex_unlock(&d->lock);
    /* The VMA holds a file reference, keeping the buffers alive. */
    return ret;
}

/* Inspect the front of a nonempty queue under exclusion. */
static u32 fifo_front(struct fades_fifo *f) {
    return f->indices[f->head];
}

/* Pop the front of a nonempty queue under exclusion. */
static u32 fifo_pop(struct fades_fifo *f) {
    u32 index = fifo_front(f);
    f->head = (f->head + 1) % f->capacity;
    f->count--;
    return index;
}

/* Push under exclusion; the caller checks capacity and duplicates. */
static void fifo_push(struct fades_fifo *f, u32 index) {
    f->indices[(f->head + f->count++) % f->capacity] = index;
}

/* Allocate TX/RX buffers and queues under the mutex; roll back on failure. */
static int configure_buffers(struct fades_device *d, const struct fades_dma_config *cfg) {
    if (d->channels[FADES_QUEUE_TX].count) {
        return -EBUSY;
    }
    if (!cfg->tx_buffers || cfg->tx_buffers > FADES_DMA_MAX_BUFFERS || !cfg->rx_buffers ||
        cfg->rx_buffers > FADES_DMA_MAX_BUFFERS || cfg->reserved[0] || cfg->reserved[1]) {
        return -EINVAL;
    }
    if ((u64)(cfg->tx_buffers + cfg->rx_buffers) * FADES_DMA_SLOT_BYTES >
        ((u64)totalram_pages() << PAGE_SHIFT)) {
        return -ENOMEM;
    }
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        struct fades_channel *c = &d->channels[direction];
        c->count = direction == FADES_QUEUE_RX ? cfg->rx_buffers : cfg->tx_buffers;
        c->buffers = kcalloc(c->count, sizeof(*c->buffers), GFP_KERNEL);
        c->pending.indices = kcalloc(c->count, sizeof(u32), GFP_KERNEL);
        c->done.indices = kcalloc(c->count, sizeof(u32), GFP_KERNEL);
        c->pending.capacity = c->done.capacity = c->count;
        if (!c->buffers || !c->pending.indices || !c->done.indices) {
            goto fail;
        }
        for (unsigned int i = 0; i < c->count; i++) {
            struct fades_data_buffer *s = &c->buffers[i];
            gfp_t flags = GFP_KERNEL | __GFP_ZERO | __GFP_NORETRY | __GFP_NOWARN;
            if (direction == FADES_QUEUE_RX) {
                s->pages = dma_alloc_pages(d->dev, FADES_DMA_SLOT_BYTES, &s->address,
                                           DMA_BIDIRECTIONAL, flags);
                if (s->pages) {
                    s->cpu = page_address(s->pages);
                    dma_sync_single_for_cpu(d->dev, s->address, FADES_DMA_SLOT_BYTES,
                                            DMA_BIDIRECTIONAL);
                }
            } else {
                s->cpu = dma_alloc_coherent(d->dev, FADES_DMA_SLOT_BYTES, &s->address, flags);
            }
            if (!s->cpu) {
                goto fail;
            }
        }
    }
    return 0;
fail:
    /* Nothing has been exposed to DMA, so partial allocations can be freed. */
    free_buffers(d);
    return -ENOMEM;
}

/* Set an AXI DMA 64-bit address under exclusion. */
static void write_address(void __iomem *reg, dma_addr_t address) {
    /* AXI DMA extended-address registers use low-then-high writes, matching
     * the provider's lo_hi_writeq; the high write commits the pointer. */
    writel(lower_32_bits(address), reg);
    writel(upper_32_bits(address), reg + 4);
}

/* Initialize DMA and the solver under the mutex, registering prequeued input counts. */
static int fades_start(struct fades_device *d) {
    if (d->running) {
        return -EBUSY;
    }
    if (d->faulted) {
        return -EIO;
    }
    if (!d->channels[FADES_QUEUE_TX].count) {
        return -EINVAL;
    }
    int ret = hardware_reset(d);
    if (ret) {
        d->faulted = true;
        return ret;
    }
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        struct fades_channel *c = &d->channels[direction];
        memset(c->bd, 0, sizeof(*c->bd) * RING_SIZE);
        c->head = c->tail = c->active = 0;
        for (unsigned int i = 0; i < RING_SIZE; i++) {
            dma_addr_t next = c->bd_address + ((i + 1) % RING_SIZE) * sizeof(*c->bd);
            c->bd[i].next = cpu_to_le32(lower_32_bits(next));
            c->bd[i].next_hi = cpu_to_le32(upper_32_bits(next));
        }
        write_address(channel_regs(d, direction) + DMA_CUR, c->bd_address);
        writel(DMA_IRQ | BIT(16) | BIT(0), channel_regs(d, direction) + DMA_CR);
    }
    dma_wmb();
    writel(FADES_RECORDS_PER_SLOT, d->control + 4);
    writel(2, d->control);
    readl(d->control);
    usleep_range(20, 40);
    d->running = true;
    d->faulted = false;
    /* Register all accepted input before DMA starts, including TX waiting for
     * receive capacity, to prevent false idle detection and premature output cuts. */
    for (unsigned int i = 0; i < d->channels[FADES_QUEUE_TX].count; i++) {
        if (d->channels[FADES_QUEUE_TX].buffers[i].state == QUEUED) {
            writel(d->channels[FADES_QUEUE_TX].buffers[i].records, d->control + 8);
        }
    }
    readl(d->control + 8);
    return 0;
}

/* Submit a dequeued pending buffer under the mutex; the DMA ring must have space. */
static void submit_bd(struct fades_device *d, enum fades_queue direction, u32 index, u32 records) {
    struct fades_channel *c = &d->channels[direction];
    struct fades_bd *bd = &c->bd[c->tail];
    dma_addr_t address = c->buffers[index].address;

    if (direction == FADES_QUEUE_RX) {
        dma_sync_single_for_device(d->dev, address, FADES_DMA_SLOT_BYTES, DMA_BIDIRECTIONAL);
    }
    bd->address = cpu_to_le32(lower_32_bits(address));
    bd->address_hi = cpu_to_le32(upper_32_bits(address));
    bd->control = cpu_to_le32(records * 64 | (direction == FADES_QUEUE_TX ? BIT(27) | BIT(26) : 0));
    WRITE_ONCE(bd->status, 0);
    c->owners[c->tail] = index;
    c->buffers[index].records = records;
    c->buffers[index].state = ACTIVE;
    /* Finalize the BD and bookkeeping before publishing them via the tail address. */
    dma_wmb();
    write_address(channel_regs(d, direction) + DMA_TAIL, c->bd_address + c->tail * sizeof(*bd));
    c->tail = (c->tail + 1) % RING_SIZE;
    c->active++;
}

/* Submit pending RX then TX under the mutex; do not reserve one RX per TX. */
static void fades_schedule(struct fades_device *d) {
    struct fades_channel *tx = &d->channels[FADES_QUEUE_TX];
    struct fades_channel *rx = &d->channels[FADES_QUEUE_RX];

    if (!d->running || d->faulted) {
        return;
    }
    /* Publish receive capacity first, and submit input only while capacity exists. */
    while (rx->pending.count && rx->active < RING_SIZE) {
        submit_bd(d, FADES_QUEUE_RX, fifo_pop(&rx->pending), FADES_RECORDS_PER_SLOT);
    }
    while (tx->pending.count && tx->active < RING_SIZE && rx->active) {
        u32 index = fifo_pop(&tx->pending);
        submit_bd(d, FADES_QUEUE_TX, index, tx->buffers[index].records);
    }
}

/* Read a snapshot 64-bit counter under the mutex. */
static u64 read_counter(struct fades_device *d, unsigned int index) {
    u32 low = readl(d->control + 0x100 + index * 8);
    u32 high = readl(d->control + 0x104 + index * 8);
    return (u64)high << 32 | low;
}

/* Validate ioctl requests and serialize buffer and hardware operations. */
static long fades_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct fades_device *d = file->private_data;
    void __user *user = (void __user *)arg;
    int ret = 0;

    if (mutex_lock_interruptible(&d->lock)) {
        return -ERESTARTSYS;
    }
    if (d->removed) {
        ret = -ENODEV;
        goto out;
    }
    switch (cmd) {
    case FADES_DMA_INFO: {
        struct fades_dma_info info = {
            .abi_version = FADES_DMA_ABI_VERSION,
            .slot_bytes = FADES_DMA_SLOT_BYTES,
            .max_buffers = FADES_DMA_MAX_BUFFERS,
            .tx_buffers = d->channels[FADES_QUEUE_TX].count,
            .rx_buffers = d->channels[FADES_QUEUE_RX].count,
        };
        ret = copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
        break;
    }
    case FADES_DMA_CONFIG: {
        struct fades_dma_config cfg;
        if (copy_from_user(&cfg, user, sizeof(cfg))) {
            ret = -EFAULT;
            break;
        }
        ret = configure_buffers(d, &cfg);
        break;
    }
    case FADES_QUERYBUF: {
        struct fades_buffer_info info;
        if (copy_from_user(&info, user, sizeof(info))) {
            ret = -EFAULT;
            break;
        }
        if (info.queue > FADES_QUEUE_RX || info.index >= d->channels[info.queue].count) {
            ret = -EINVAL;
            break;
        }
        info.mmap_offset =
            ((u64)info.queue * FADES_DMA_MAX_BUFFERS + info.index) * FADES_DMA_SLOT_BYTES;
        info.map_bytes = FADES_DMA_SLOT_BYTES;
        ret = copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
        break;
    }
    case FADES_STREAMON:
        ret = fades_start(d);
        if (!ret) {
            d->stopped = false;
            fades_schedule(d);
        }
        break;
    case FADES_STREAMOFF:
        ret = fades_stop(d);
        if (!ret) {
            d->stopped = true;
        }
        break;
    case FADES_QBUF:
    case FADES_DQBUF: {
        struct fades_buffer b;
        if (copy_from_user(&b, user, sizeof(b))) {
            ret = -EFAULT;
            break;
        }
        if (b.queue > FADES_QUEUE_RX || b.reserved) {
            ret = -EINVAL;
            break;
        }
        if (d->faulted) {
            ret = -EIO;
            break;
        }
        struct fades_channel *c = &d->channels[b.queue];
        if (!c->count) {
            ret = -EINVAL;
            break;
        }
        if (cmd == FADES_QBUF) {
            if (b.index >= c->count ||
                (b.queue == FADES_QUEUE_TX ? !b.records || b.records > FADES_RECORDS_PER_SLOT
                                           : b.records != 0)) {
                ret = -EINVAL;
                break;
            }
            if (c->buffers[b.index].state != CPU) {
                ret = -EBUSY;
                break;
            }
            /* Register the count before DMA submission to prevent premature partial output. */
            if (b.queue == FADES_QUEUE_TX && d->running) {
                writel(b.records, d->control + 8);
                readl(d->control + 8);
            }
            c->buffers[b.index].state = QUEUED;
            c->buffers[b.index].records = b.records;
            fifo_push(&c->pending, b.index);
            fades_schedule(d);
        } else {
            if (!c->done.count) {
                ret = -EAGAIN;
                break;
            }
            b.index = fifo_front(&c->done);
            b.records = c->buffers[b.index].records;
            /* Preserve ownership and completion information if copy_to_user fails. */
            if (copy_to_user(user, &b, sizeof(b))) {
                ret = -EFAULT;
                break;
            }
            fifo_pop(&c->done);
            c->buffers[b.index].state = CPU;
        }
        break;
    }
    case FADES_SOLVER_STATS: {
        struct fades_solver_stats stats;
        u32 status;
        writel(1, d->control + 16);
        ret = readl_poll_timeout(d->control + 12, status, !(status & 2), 1, 100000);
        if (ret) {
            break;
        }
        stats.accepted = read_counter(d, 0);
        stats.emitted = read_counter(d, 1);
        stats.input_stall_cycles = read_counter(d, 2);
        stats.dispatched = read_counter(d, 3);
        stats.collected = read_counter(d, 4);
        stats.queue_empty_cycles = read_counter(d, 5);
        for (unsigned int i = 0; i < ARRAY_SIZE(stats.core_dispatched); i++) {
            stats.core_dispatched[i] = read_counter(d, 6 + i);
            stats.core_collected[i] = read_counter(d, 12 + i);
            stats.core_busy_cycles[i] = read_counter(d, 18 + i);
            stats.core_output_wait_cycles[i] = read_counter(d, 24 + i);
            stats.core_input_wait_cycles[i] = read_counter(d, 32 + i);
            stats.core_max_inflight[i] = read_counter(d, 38 + i);
        }
        stats.queue_peak = read_counter(d, 30);
        stats.processing_cycles = read_counter(d, 31);
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

/* Report TX/RX completion, stop and errors to poll without consuming completions. */
static __poll_t fades_poll(struct file *file, poll_table *wait) {
    struct fades_device *d = file->private_data;
    __poll_t events = 0;

    /* Register the wait queue before checking state to avoid missing a completion. */
    poll_wait(file, &d->wait, wait);
    mutex_lock(&d->lock);
    if (d->removed) {
        events = EPOLLERR | EPOLLHUP;
    } else if (d->faulted) {
        events = EPOLLERR;
    } else if (d->stopped) {
        events = EPOLLHUP;
    } else {
        if (d->channels[FADES_QUEUE_TX].done.count) {
            events |= EPOLLOUT | EPOLLWRNORM;
        }
        if (d->channels[FADES_QUEUE_RX].done.count) {
            events |= EPOLLIN | EPOLLRDNORM;
        }
    }
    mutex_unlock(&d->lock);
    return events;
}

/* Reclaim completions on both channels in a threaded IRQ and advance transfers and poll. */
static irqreturn_t fades_irq(int irq, void *arg) {
    struct fades_device *d = arg;
    mutex_lock(&d->lock);
    if (d->removed) {
        goto out;
    }
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        struct fades_channel *c = &d->channels[direction];
        u32 status = readl(channel_regs(d, direction) + DMA_SR);
        writel(status & (DMA_IRQ | BIT(13)), channel_regs(d, direction) + DMA_SR);
        if (!d->running) {
            continue;
        }
        if (status & DMA_ERRORS) {
            dev_err(d->dev, "channel %u DMA status %#x\n", direction, status);
            d->faulted = true;
        }
        while (c->active && !d->faulted) {
            u32 done = le32_to_cpu(READ_ONCE(c->bd[c->head].status));
            u32 index = c->owners[c->head];
            u32 bytes = done & BD_LENGTH;
            u32 capacity = c->buffers[index].records * 64;
            if (!(done & BD_COMPLETE)) {
                break;
            }
            /* After observing completion, make DMA writes visible and validate length and errors.
             */
            dma_rmb();
            if ((done & BD_ERRORS) || !bytes || bytes % 64 || bytes > capacity ||
                (direction == FADES_QUEUE_TX && bytes != capacity)) {
                dev_err(d->dev, "channel %u descriptor %#x expected %u bytes\n", direction, done,
                        c->buffers[index].records * 64);
                d->faulted = true;
                break;
            }
            if (direction == FADES_QUEUE_RX) {
                dma_sync_single_for_cpu(d->dev, c->buffers[index].address, bytes,
                                        DMA_BIDIRECTIONAL);
            }
            c->buffers[index].records = bytes / 64;
            c->buffers[index].state = DONE;
            fifo_push(&c->done, index);
            c->head = (c->head + 1) % RING_SIZE;
            c->active--;
        }
    }
    if (d->running && (readl(d->control + 12) & 1)) {
        d->faulted = true;
    }
    if (d->faulted) {
        /* Stop both engines before userspace recovery. Retain buffer ownership
         * until STREAMOFF, including for partially completed transfers. */
        writel(0, channel_regs(d, FADES_QUEUE_TX) + DMA_CR);
        writel(0, channel_regs(d, FADES_QUEUE_RX) + DMA_CR);
    } else {
        fades_schedule(d);
    }
    wake_up_interruptible(&d->wait);
out:
    mutex_unlock(&d->lock);
    return IRQ_HANDLED;
}

static const struct file_operations fades_fops = {
    .owner = THIS_MODULE,
    .open = fades_open,
    .release = fades_release,
    .unlocked_ioctl = fades_ioctl,
    .compat_ioctl = compat_ptr_ioctl,
    .mmap = fades_mmap,
    .poll = fades_poll,
    .llseek = no_llseek,
};

/* Initialize the Device Tree device and expose /dev/fades. */
static int fades_probe(struct platform_device *pdev) {
    unsigned int requested = 0;
    int ret;

    struct fades_device *d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d) {
        return -ENOMEM;
    }
    kref_init(&d->refs);
    mutex_init(&d->lock);
    init_waitqueue_head(&d->wait);
    d->dev = get_device(&pdev->dev);
    ret = dma_set_mask_and_coherent(d->dev, DMA_BIT_MASK(40));
    if (ret) {
        goto fail;
    }
    /* Enable DMA and core clocks before accessing registers. */
    struct clk *clk = devm_clk_get_enabled(d->dev, "dma");
    if (IS_ERR(clk)) {
        ret = PTR_ERR(clk);
        goto fail;
    }
    unsigned long dma_hz = clk_get_rate(clk);
    clk = devm_clk_get_enabled(d->dev, "core");
    if (IS_ERR(clk)) {
        ret = PTR_ERR(clk);
        goto fail;
    }
    unsigned long core_hz = clk_get_rate(clk);

    d->control = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(d->control)) {
        ret = PTR_ERR(d->control);
        goto fail;
    }
    d->dma = devm_platform_ioremap_resource(pdev, 1);
    if (IS_ERR(d->dma)) {
        ret = PTR_ERR(d->dma);
        goto fail;
    }
    if (readl(d->control + 20) != 0x46414404) {
        ret = -ENODEV;
        goto fail;
    }
    ret = hardware_reset(d);
    if (ret) {
        goto fail;
    }
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        struct fades_channel *c = &d->channels[direction];
        c->bd = dma_alloc_coherent(d->dev, sizeof(*c->bd) * RING_SIZE, &c->bd_address, GFP_KERNEL);
        if (!c->bd) {
            ret = -ENOMEM;
            goto fail;
        }
        d->irq[direction] = platform_get_irq(pdev, direction);
        if (d->irq[direction] < 0) {
            ret = d->irq[direction];
            goto fail;
        }
        ret = request_threaded_irq(d->irq[direction], NULL, fades_irq, IRQF_ONESHOT,
                                   dev_name(d->dev), d);
        if (ret) {
            goto fail;
        }
        requested++;
    }
    d->misc.minor = MISC_DYNAMIC_MINOR;
    d->misc.name = "fades";
    d->misc.fops = &fades_fops;
    d->misc.parent = d->dev;
    d->misc.mode = 0600;
    ret = misc_register(&d->misc);
    if (ret) {
        goto fail;
    }
    platform_set_drvdata(pdev, d);
    dev_info(d->dev, "ABI v7: variable 1 MiB buffers, %u SG entries per channel\n", RING_SIZE);
    dev_info(d->dev, "actual clocks: core %lu Hz, DMA %lu Hz\n", core_hz, dma_hz);
    return 0;
fail:
    /* Wait for registered IRQs to finish before releasing partially initialized state. */
    while (requested) {
        free_irq(d->irq[--requested], d);
    }
    kref_put(&d->refs, fades_free);
    return ret;
}

/* Block new users, stop DMA and IRQs, and release the device-owned reference. */
static void fades_remove(struct platform_device *pdev) {
    struct fades_device *d = platform_get_drvdata(pdev);
    misc_deregister(&d->misc);
    mutex_lock(&d->lock);
    int ret = fades_stop(d);
    d->removed = true;
    wake_up_interruptible(&d->wait);
    mutex_unlock(&d->lock);
    /* Wait until running IRQs no longer reference the state before releasing it. */
    free_irq(d->irq[FADES_QUEUE_TX], d);
    free_irq(d->irq[FADES_QUEUE_RX], d);
    if (!ret) {
        kref_put(&d->refs, fades_free);
    } else {
        /* Hardware might still bus-master. Keep memory pinned to prevent DMA
         * writes into storage reused by another owner. */
        dev_err(d->dev, "DMA reset failed on remove; retaining DMA allocations\n");
    }
}

static const struct of_device_id fades_match[] = {{.compatible = "fades,solver-3.0"}, {}};

MODULE_DEVICE_TABLE(of, fades_match);

static struct platform_driver fades_driver = {
    .probe = fades_probe,
    .remove_new = fades_remove,
    .driver = {.name = "fades", .of_match_table = fades_match},
};

module_platform_driver(fades_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FADES queued solver and dedicated AXI DMA SG driver");
