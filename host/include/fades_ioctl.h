/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef FADES_IOCTL_H
#define FADES_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FADES_DMA_ABI_VERSION 2
#define FADES_DMA_SLOTS 8
#define FADES_DMA_SLOT_BYTES (1024U * 1024U)

struct fades_dma_info {
    __u32 abi_version;
    __u32 slots;
    __u32 slot_bytes;
    __u32 reserved;
    /* Each mmap covers this many bytes. TX offset=0, RX offset=map_bytes. */
    __aligned_u64 map_bytes;
};

enum fades_queue { FADES_QUEUE_TX = 0, FADES_QUEUE_RX = 1 };

struct fades_buffer {
    __u32 queue;
    __u32 index;
    __u32 records;     /* QBUF: TX count or RX zero. DQBUF: actual count. */
    __u32 reserved;    /* Must be zero. */
};

/* Session-cumulative snapshot. Core and DMA domains are sampled at different
 * instants; stage totals are equal only after drain.
 * 0 accepted, 1 output, 2 input stall, 3 dispatched, 4 collected, 5 queue empty,
 * 6..11 per-core dispatched, 12..17 collected, 18..23 busy, 24..29 output wait,
 * 30 queue write-domain peak, 31 first dispatch through snapshot cycles,
 * 32..37 enabled+idle+no inflight cycles, 38..43 peak inflight.
 */
struct fades_solver_stats {
    __aligned_u64 counters[44];
};

#define FADES_DMA_INFO _IOR('F', 0, struct fades_dma_info)
#define FADES_SOLVER_STATS _IOR('F', 2, struct fades_solver_stats)
#define FADES_QBUF _IOW('F', 3, struct fades_buffer)
#define FADES_DQBUF _IOWR('F', 4, struct fades_buffer)
#define FADES_STREAMON _IO('F', 5)
#define FADES_END_INPUT _IO('F', 6)
#define FADES_STREAMOFF _IO('F', 7)

#endif
