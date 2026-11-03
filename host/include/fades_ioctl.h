/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef FADES_IOCTL_H
#define FADES_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FADES_DMA_ABI_VERSION 1
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

struct fades_dma_run {
    __u32 bytes;       /* Per slot; multiple of 64, <= slot_bytes. */
    __u32 slots;       /* Outstanding packets per round, 1..slots. */
    __u32 rounds;      /* Repeat the same mapped input buffers. */
    __u32 timeout_ms;  /* Per round, 1..60000 ms. */
    __aligned_u64 elapsed_ns;
    __aligned_u64 bytes_per_direction;
};

/* One coherent snapshot, from the last round (not accumulated).
 * 0 accepted, 1 output, 2 input stall, 3 dispatched, 4 collected, 5 queue empty,
 * 6..11 per-core dispatched, 12..17 collected, 18..23 busy, 24..29 output wait,
 * 30 queue write-domain peak, 31 first dispatch through last collection cycles,
 * 32..37 enabled+idle+no inflight cycles, 38..43 peak inflight.
 */
struct fades_solver_stats {
    __aligned_u64 counters[44];
};

#define FADES_DMA_INFO _IOR('F', 0, struct fades_dma_info)
#define FADES_DMA_RUN _IOWR('F', 1, struct fades_dma_run)
#define FADES_SOLVER_STATS _IOR('F', 2, struct fades_solver_stats)

#endif
