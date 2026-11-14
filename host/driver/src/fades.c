// SPDX-License-Identifier: GPL-2.0
// 求解器と AXI DMA SG エンジンをキュー経由で排他管理する。
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

/* AXI DMA の非サイクリック SG。変更しない next ポインターを事前に連結し、
 * 準備済みディスクリプターを dma_wmb + TAILDESC で公開する。回収は COMPLETE 後のみ。
 * XAxiDma_BdRing{Alloc,ToHw,FromHw,Free} と同様に active で所有数を明示管理するため、
 * リングの全エントリーを使用できる。完了バッファは BD の回収後も保持できる。 */
struct fades_bd {
    __le32 next, next_hi, address, address_hi;
    __le32 reserved[2];
    __le32 control, status;
    __le32 app[5], padding[3];
} __aligned(64);

enum slot_state { CPU, QUEUED, ACTIVE, DONE };

struct fades_fifo {
    u32 *indices, capacity, head, count;
};

struct fades_slot {
    enum slot_state state;
    u32 records;
    void *cpu;
    struct page *pages; /* RX: キャッシュ可能なストリーミング DMA 用の割り当て。 */
    dma_addr_t address;
};

struct fades_channel {
    struct fades_bd *bd;
    dma_addr_t bd_address;
    u32 head, tail, active;
    u32 owners[RING_SIZE];
    struct fades_slot *slots;
    u32 count;
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
    bool opened, removed, faulted, running;
};

static u32 fifo_front(struct fades_fifo *f) {
    return f->indices[f->head];
}

static u32 fifo_pop(struct fades_fifo *f) {
    u32 index = fifo_front(f);
    f->head = (f->head + 1) % f->capacity;
    f->count--;
    return index;
}

static void fifo_push(struct fades_fifo *f, u32 index) {
    f->indices[(f->head + f->count++) % f->capacity] = index;
}

static void __iomem *channel_regs(struct fades_dma *d, u32 q) {
    return d->dma + (q == FADES_QUEUE_RX ? 0x30 : 0);
}

static void write_address(void __iomem *reg, dma_addr_t address) {
    /* AXI DMA の拡張アドレスレジスターは下位、上位の順に書き込む。
     * 提供側の lo_hi_writeq と同じ順序で、上位の書き込みでポインターが確定する。 */
    writel(lower_32_bits(address), reg);
    writel(upper_32_bits(address), reg + 4);
}

static void free_buffers(struct fades_dma *d) {
    unsigned int q, i;

    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        for (i = 0; c->slots && i < c->count; i++) {
            struct fades_slot *s = &c->slots[i];
            if (s->pages) {
                dma_free_pages(d->dev, FADES_DMA_SLOT_BYTES, s->pages, s->address,
                               DMA_BIDIRECTIONAL);
            } else if (s->cpu) {
                dma_free_coherent(d->dev, FADES_DMA_SLOT_BYTES, s->cpu, s->address);
            }
        }
        kfree(c->slots);
        kfree(c->pending.indices);
        kfree(c->done.indices);
        c->slots = NULL;
        c->count = 0;
        memset(&c->pending, 0, sizeof(c->pending));
        memset(&c->done, 0, sizeof(c->done));
    }
}

static int configure_buffers(struct fades_dma *d, const struct fades_dma_config *cfg) {
    unsigned int q, i;

    if (d->channels[0].count) {
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
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        c->count = q ? cfg->rx_buffers : cfg->tx_buffers;
        c->slots = kcalloc(c->count, sizeof(*c->slots), GFP_KERNEL);
        c->pending.indices = kcalloc(c->count, sizeof(u32), GFP_KERNEL);
        c->done.indices = kcalloc(c->count, sizeof(u32), GFP_KERNEL);
        c->pending.capacity = c->done.capacity = c->count;
        if (!c->slots || !c->pending.indices || !c->done.indices) {
            goto fail;
        }
        for (i = 0; i < c->count; i++) {
            struct fades_slot *s = &c->slots[i];
            gfp_t flags = GFP_KERNEL | __GFP_ZERO | __GFP_NORETRY | __GFP_NOWARN;
            if (q == FADES_QUEUE_RX) {
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
    free_buffers(d);
    return -ENOMEM;
}

static void fades_free(struct kref *ref) {
    struct fades_dma *d = container_of(ref, struct fades_dma, refs);
    unsigned int q;

    free_buffers(d);
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        if (c->bd) {
            dma_free_coherent(d->dev, sizeof(*c->bd) * RING_SIZE, c->bd, c->bd_address);
        }
    }
    put_device(d->dev);
    kfree(d);
}

/* リセットは AXI DMA の両チャネルに影響する。呼び出し側で mutex を保持すること。 */
static int hardware_reset(struct fades_dma *d) {
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

static int fades_stop(struct fades_dma *d) {
    unsigned int q;
    int ret = hardware_reset(d);

    d->running = false;
    d->faulted = ret != 0;
    if (ret) {
        /* DMA の所有状態が不明なため、メモリーを同期・再利用・解放しない。 */
        wake_up_interruptible(&d->wait);
        return ret;
    }
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        unsigned int index;
        if (q == FADES_QUEUE_RX) {
            for (index = 0; index < c->count; index++) {
                if (c->slots[index].state == ACTIVE) {
                    dma_sync_single_for_cpu(d->dev, c->slots[index].address, FADES_DMA_SLOT_BYTES,
                                            DMA_BIDIRECTIONAL);
                }
            }
        }
        for (index = 0; index < c->count; index++) {
            c->slots[index].state = CPU;
            c->slots[index].records = 0;
        }
        c->pending.head = c->pending.count = 0;
        c->done.head = c->done.count = 0;
        c->head = c->tail = c->active = 0;
    }
    wake_up_interruptible(&d->wait);
    return ret;
}

static int fades_start(struct fades_dma *d) {
    unsigned int q, i;
    int ret;

    if (d->running) {
        return -EBUSY;
    }
    if (d->faulted) {
        return -EIO;
    }
    if (!d->channels[0].count) {
        return -EINVAL;
    }
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
    writel(FADES_RECORDS_PER_SLOT, d->control + 4);
    writel(2, d->control);
    readl(d->control);
    usleep_range(20, 40);
    d->running = true;
    d->faulted = false;
    /* DMA 開始前に、受信バッファ待ちの TX ディスクリプターも含め、受理済みの全入力を
     * 登録する。誤ってアイドルと判定し、出力を途中で区切ることを防ぐ。 */
    for (i = 0; i < d->channels[0].count; i++) {
        if (d->channels[0].slots[i].state == QUEUED) {
            writel(d->channels[0].slots[i].records, d->control + 8);
        }
    }
    readl(d->control + 8);
    return 0;
}

static void submit_bd(struct fades_dma *d, u32 q, u32 index, u32 records) {
    struct fades_channel *c = &d->channels[q];
    struct fades_bd *bd = &c->bd[c->tail];
    dma_addr_t address = c->slots[index].address;

    if (q == FADES_QUEUE_RX) {
        dma_sync_single_for_device(d->dev, address, FADES_DMA_SLOT_BYTES, DMA_BIDIRECTIONAL);
    }
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

/* 全容量の RX ディスクリプターを独立に登録してから TX を供給する。
 * 複数の小さな入力が同じ出力スロットを埋めるため、TX ごとに RX を予約しない。
 * 受信の残り容量を超える入力は AXI の背圧で制御する。 */
static void fades_schedule(struct fades_dma *d) {
    struct fades_channel *tx = &d->channels[0], *rx = &d->channels[1];

    if (!d->running || d->faulted) {
        return;
    }
    while (rx->pending.count && rx->active < RING_SIZE) {
        submit_bd(d, FADES_QUEUE_RX, fifo_pop(&rx->pending), FADES_RECORDS_PER_SLOT);
    }
    while (tx->pending.count && tx->active < RING_SIZE && rx->active) {
        u32 index = fifo_pop(&tx->pending);
        submit_bd(d, FADES_QUEUE_TX, index, tx->slots[index].records);
    }
}

static irqreturn_t fades_irq(int irq, void *arg) {
    struct fades_dma *d = arg;
    unsigned int q;

    mutex_lock(&d->lock);
    if (d->removed) {
        goto out;
    }
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        u32 status = readl(channel_regs(d, q) + DMA_SR);
        writel(status & (DMA_IRQ | BIT(13)), channel_regs(d, q) + DMA_SR);
        if (!d->running) {
            continue;
        }
        if (status & DMA_ERRORS) {
            dev_err(d->dev, "channel %u DMA status %#x\n", q, status);
            d->faulted = true;
        }
        while (c->active && !d->faulted) {
            u32 done = le32_to_cpu(READ_ONCE(c->bd[c->head].status));
            u32 index = c->owners[c->head];
            u32 bytes = done & BD_LENGTH;
            u32 capacity = c->slots[index].records * 64;
            if (!(done & BD_COMPLETE)) {
                break;
            }
            dma_rmb();
            if ((done & BD_ERRORS) || !bytes || bytes % 64 || bytes > capacity ||
                (q == FADES_QUEUE_TX && bytes != capacity)) {
                dev_err(d->dev, "channel %u descriptor %#x expected %u bytes\n", q, done,
                        c->slots[index].records * 64);
                d->faulted = true;
                break;
            }
            if (q == FADES_QUEUE_RX) {
                dma_sync_single_for_cpu(d->dev, c->slots[index].address, bytes, DMA_BIDIRECTIONAL);
            }
            c->slots[index].records = bytes / 64;
            c->slots[index].state = DONE;
            fifo_push(&c->done, index);
            c->head = (c->head + 1) % RING_SIZE;
            c->active--;
        }
    }
    if (d->running && (readl(d->control + 12) & 1)) {
        d->faulted = true;
    }
    if (d->faulted) {
        /* ユーザー空間から復旧できるようにする前に両エンジンを停止する。
         * 部分転送の場合も、STREAMOFF まではスロットの所有権を維持する。 */
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

static int fades_open(struct inode *inode, struct file *file) {
    struct fades_dma *d = container_of(file->private_data, struct fades_dma, misc);
    int ret = 0;

    mutex_lock(&d->lock);
    if (d->removed) {
        ret = -ENODEV;
    } else if (d->opened) {
        ret = -EBUSY;
    } else {
        /* 前回の close 時にリセットが失敗すると、メモリーは再利用禁止のまま残る。
         * 新しいセッションを公開する前に復旧する。 */
        if (d->channels[0].count) {
            ret = fades_stop(d);
            if (!ret) {
                free_buffers(d);
            }
        }
        if (!ret) {
            d->opened = true;
            kref_get(&d->refs);
            file->private_data = d;
        }
    }
    mutex_unlock(&d->lock);
    return ret;
}

static int fades_release(struct inode *inode, struct file *file) {
    struct fades_dma *d = file->private_data;
    int ret;

    mutex_lock(&d->lock);
    ret = d->removed ? (d->faulted ? -EIO : 0) : fades_stop(d);
    if (!ret) {
        free_buffers(d);
    }
    d->opened = false;
    mutex_unlock(&d->lock);
    kref_put(&d->refs, fades_free);
    return 0;
}

/* スナップショット済みの64ビットカウンターを取得する。 */
static u64 read_counter(struct fades_dma *d, unsigned int index) {
    u32 low = readl(d->control + 0x100 + index * 8);
    u32 high = readl(d->control + 0x104 + index * 8);
    return (u64)high << 32 | low;
}

static long fades_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct fades_dma *d = file->private_data;
    void __user *user = (void __user *)arg;
    struct fades_buffer b;
    struct fades_channel *c;
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
            .tx_buffers = d->channels[0].count,
            .rx_buffers = d->channels[1].count,
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
        if (info.queue > 1 || info.index >= d->channels[info.queue].count) {
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
            fades_schedule(d);
        }
        break;
    case FADES_STREAMOFF:
        ret = fades_stop(d);
        break;
    case FADES_QBUF:
    case FADES_DQBUF:
        if (copy_from_user(&b, user, sizeof(b))) {
            ret = -EFAULT;
            break;
        }
        if (b.queue > 1 || b.reserved) {
            ret = -EINVAL;
            break;
        }
        if (d->faulted) {
            ret = -EIO;
            break;
        }
        c = &d->channels[b.queue];
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
            if (c->slots[b.index].state != CPU) {
                ret = -EBUSY;
                break;
            }
            if (b.queue == FADES_QUEUE_TX && d->running) {
                writel(b.records, d->control + 8);
                readl(d->control + 8);
            }
            c->slots[b.index].state = QUEUED;
            c->slots[b.index].records = b.records;
            fifo_push(&c->pending, b.index);
            fades_schedule(d);
        } else {
            if (!c->done.count) {
                ret = -EAGAIN;
                break;
            }
            b.index = fifo_front(&c->done);
            b.records = c->slots[b.index].records;
            /* copy_to_user が失敗しても所有権や完了情報を失わないようにする。 */
            if (copy_to_user(user, &b, sizeof(b))) {
                ret = -EFAULT;
                break;
            }
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
        if (ret) {
            break;
        }
        stats.accepted = read_counter(d, 0);
        stats.emitted = read_counter(d, 1);
        stats.input_stall_cycles = read_counter(d, 2);
        stats.dispatched = read_counter(d, 3);
        stats.collected = read_counter(d, 4);
        stats.queue_empty_cycles = read_counter(d, 5);
        for (i = 0; i < ARRAY_SIZE(stats.core_dispatched); i++) {
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

static __poll_t fades_poll(struct file *file, poll_table *wait) {
    struct fades_dma *d = file->private_data;
    __poll_t events = 0;

    poll_wait(file, &d->wait, wait);
    mutex_lock(&d->lock);
    if (d->removed) {
        events = EPOLLERR | EPOLLHUP;
    } else if (d->faulted) {
        events = EPOLLERR;
    } else {
        if (d->channels[0].done.count) {
            events |= EPOLLOUT | EPOLLWRNORM;
        }
        if (d->channels[1].done.count) {
            events |= EPOLLIN | EPOLLRDNORM;
        }
    }
    mutex_unlock(&d->lock);
    return events;
}

static int fades_mmap(struct file *file, struct vm_area_struct *vma) {
    struct fades_dma *d = file->private_data;
    const unsigned long pages = FADES_DMA_SLOT_BYTES >> PAGE_SHIFT;
    unsigned long number = vma->vm_pgoff / pages;
    unsigned long q = number / FADES_DMA_MAX_BUFFERS;
    unsigned long index = number % FADES_DMA_MAX_BUFFERS;
    struct fades_slot *s;
    int ret;

    if (!(vma->vm_flags & VM_SHARED) || vma->vm_end - vma->vm_start != FADES_DMA_SLOT_BYTES ||
        vma->vm_pgoff % pages || q > 1) {
        return -EINVAL;
    }
    mutex_lock(&d->lock);
    if (d->removed) {
        ret = -ENODEV;
    } else if (index >= d->channels[q].count) {
        ret = -EINVAL;
    } else {
        s = &d->channels[q].slots[index];
        vma->vm_pgoff = 0;
        if (s->pages) {
            ret = dma_mmap_pages(d->dev, vma, FADES_DMA_SLOT_BYTES, s->pages);
        } else {
            ret = dma_mmap_coherent(d->dev, vma, s->cpu, s->address, FADES_DMA_SLOT_BYTES);
        }
    }
    mutex_unlock(&d->lock);
    /* VMA がファイルへの参照を保持するため、バッファも解放されずに維持される。 */
    return ret;
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

static int fades_probe(struct platform_device *pdev) {
    struct fades_dma *d;
    struct clk *clk;
    unsigned long dma_hz, core_hz;
    unsigned int q, requested = 0;
    int ret;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
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
    clk = devm_clk_get_enabled(d->dev, "dma");
    if (IS_ERR(clk)) {
        ret = PTR_ERR(clk);
        goto fail;
    }
    dma_hz = clk_get_rate(clk);
    clk = devm_clk_get_enabled(d->dev, "core");
    if (IS_ERR(clk)) {
        ret = PTR_ERR(clk);
        goto fail;
    }
    core_hz = clk_get_rate(clk);
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
    for (q = 0; q < 2; q++) {
        struct fades_channel *c = &d->channels[q];
        c->bd = dma_alloc_coherent(d->dev, sizeof(*c->bd) * RING_SIZE, &c->bd_address, GFP_KERNEL);
        if (!c->bd) {
            ret = -ENOMEM;
            goto fail;
        }
        d->irq[q] = platform_get_irq(pdev, q);
        if (d->irq[q] < 0) {
            ret = d->irq[q];
            goto fail;
        }
        ret = request_threaded_irq(d->irq[q], NULL, fades_irq, IRQF_ONESHOT, dev_name(d->dev), d);
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
    while (requested) {
        free_irq(d->irq[--requested], d);
    }
    kref_put(&d->refs, fades_free);
    return ret;
}

static void fades_remove(struct platform_device *pdev) {
    struct fades_dma *d = platform_get_drvdata(pdev);
    int ret;

    misc_deregister(&d->misc);
    mutex_lock(&d->lock);
    ret = fades_stop(d);
    d->removed = true;
    wake_up_interruptible(&d->wait);
    mutex_unlock(&d->lock);
    free_irq(d->irq[0], d);
    free_irq(d->irq[1], d);
    if (!ret) {
        kref_put(&d->refs, fades_free);
    } else {
        /* HW がまだバスマスターとして動作している可能性があるため、メモリーを保持する。
         * 別の所有者が再利用したメモリーへの DMA 書き込みを防ぐ。 */
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
