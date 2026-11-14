/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef FADES_IOCTL_H
#define FADES_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FADES_DMA_ABI_VERSION 8              ///< ABI version checked by the driver and userspace.
#define FADES_DMA_MAX_BUFFERS 4096U          ///< Maximum number of buffers per direction.
#define FADES_DMA_SLOT_BYTES (1024U * 1024U) ///< Data area and mmap size of one buffer in bytes.
#define FADES_RECORDS_PER_SLOT                                                                     \
    (FADES_DMA_SLOT_BYTES / 64U) ///< Number of 64-byte records per buffer.

/**
 * @brief Driver ABI and buffer configuration information.
 * @details DMA_INFO output. Both directions are unconfigured immediately after open;
 * buffer counts are populated after CONFIG succeeds. Userspace must check abi_version
 * and slot_bytes before accessing buffers.
 */
struct fades_dma_info {
    __u32 abi_version; ///< Output: ABI version provided by the driver.
    __u32 slot_bytes;  ///< Output: Size of each buffer in bytes.
    __u32 max_buffers; ///< Output: Limit per queue; allocations below this limit may still fail.
    __u32 tx_buffers;  ///< Output: Configured TX buffer count, or 0 before CONFIG succeeds.
    __u32 rx_buffers;  ///< Output: Configured RX buffer count, or 0 before CONFIG succeeds.
    __u32 reserved;    ///< Output: Reserved; the driver returns 0.
};

/**
 * @brief Configuration request specifying TX and RX buffer counts.
 * @details DMA_CONFIG input. Buffers are allocated independently and initially owned
 * by the CPU. Configuration can succeed only once per open. Allocation failures are
 * rolled back, allowing a retry on the same fd.
 */
struct fades_dma_config {
    __u32 tx_buffers;  ///< Input: TX buffer count (1..FADES_DMA_MAX_BUFFERS).
    __u32 rx_buffers;  ///< Input: RX buffer count (1..FADES_DMA_MAX_BUFFERS).
    __u32 reserved[2]; ///< Input: Reserved; set all elements to 0.
};

/**
 * @brief Userspace mapping information for a buffer.
 * @details QUERYBUF takes queue/index and returns mmap_offset/map_bytes. The offset
 * is not a physical or DMA address. Use MAP_SHARED to mmap exactly map_bytes bytes
 * at that offset. Querying a buffer does not change its ownership.
 */
struct fades_buffer_info {
    __u32 queue;               ///< Input: TX or RX, selected using enum fades_queue.
    __u32 index;               ///< Input: Buffer index in the selected queue (0..count-1).
    __aligned_u64 mmap_offset; ///< Output: Opaque byte offset for mmap.
    __aligned_u64 map_bytes;   ///< Output: Exact byte count for mmap with MAP_SHARED.
};

enum fades_queue {
    FADES_QUEUE_TX = 0, ///< Userspace writes input for DMA to read; completion uses POLLOUT.
    FADES_QUEUE_RX = 1, ///< DMA writes results for userspace to collect; completion uses POLLIN.
};

/**
 * @brief Buffer index and record count exchanged by QBUF/DQBUF.
 * @details queue selects an independent TX or RX queue. QBUF transfers the selected
 * buffer to DMA; DQBUF returns a completed buffer's index and actual record count and
 * transfers ownership back to the CPU. TX completion is independent of solver completion.
 */
struct fades_buffer {
    __u32 queue;    ///< Input: QBUF/DQBUF direction, using enum fades_queue.
    __u32 index;    ///< QBUF input: Buffer to submit. DQBUF output: Buffer returned.
    __u32 records;  ///< QBUF input: 1..16384 for TX, 0 for RX. DQBUF output: Actual count.
    __u32 reserved; ///< Input: Reserved; must be 0.
};

/**
 * @brief Snapshot of cumulative solver session statistics.
 * @details SOLVER_STATS output. The core and DMA domains are sampled at different
 * times, so stage totals match only after all records are drained. Per-core array
 * indices identify hardware cores.
 */
struct fades_solver_stats {
    __aligned_u64 accepted;                   ///< Input records accepted by hardware.
    __aligned_u64 emitted;                    ///< Output records emitted by hardware.
    __aligned_u64 input_stall_cycles;         ///< DMA clock cycles stalled on input.
    __aligned_u64 dispatched;                 ///< Records dispatched to cores.
    __aligned_u64 collected;                  ///< Records collected from cores.
    __aligned_u64 queue_empty_cycles;         ///< Core clock cycles with an empty input queue.
    __aligned_u64 core_dispatched[6];         ///< Records dispatched per hardware core.
    __aligned_u64 core_collected[6];          ///< Records collected per core.
    __aligned_u64 core_busy_cycles[6];        ///< Busy cycles per core.
    __aligned_u64 core_output_wait_cycles[6]; ///< Output wait cycles per core.
    __aligned_u64 queue_peak;        ///< Peak record occupancy observed at the input queue writer.
    __aligned_u64 processing_cycles; ///< Core clock cycles from first dispatch to sampling.
    __aligned_u64
        core_input_wait_cycles[6];      ///< Cycles per core enabled, idle and without requests.
    __aligned_u64 core_max_inflight[6]; ///< Maximum in-flight requests per core.
};

#define FADESIOC_DMA_INFO _IOR('F', 0, struct fades_dma_info)
#define FADESIOC_SOLVER_STATS _IOR('F', 2, struct fades_solver_stats)
#define FADESIOC_QBUF _IOW('F', 3, struct fades_buffer)
#define FADESIOC_DQBUF _IOWR('F', 4, struct fades_buffer)
#define FADESIOC_STREAMON _IO('F', 5)
#define FADESIOC_STREAMOFF _IO('F', 6)
#define FADESIOC_DMA_CONFIG _IOW('F', 8, struct fades_dma_config)
#define FADESIOC_QUERYBUF _IOWR('F', 9, struct fades_buffer_info)

#endif
