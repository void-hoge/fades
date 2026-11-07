// SPDX-License-Identifier: MIT
#pragma once

#include "fades_ioctl.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace fades {

/** Monotonic clock used for streaming deadlines and elapsed-time measurements. */
using Clock = std::chrono::steady_clock;

/**
 * Copy encoded input records to a CPU-owned TX buffer and replace their IDs.
 *
 * Each record is 64 bytes. Payload and reserved bytes are copied unchanged;
 * bytes 0..7 become the little-endian ID first_id + record_index. On AArch64,
 * NEON full-width stores and bounded prefetch avoid a second staging copy and
 * narrow writes to the noncached coherent DMA mapping.
 *
 * @param target Writable destination containing at least count * 64 bytes.
 * @param source Cached encoded source containing at least count * 64 bytes.
 * @param count Number of records, not bytes. Zero performs no memory accesses.
 * @param first_id ID for the first record; the caller ensures IDs do not wrap
 *                or collide with outstanding requests.
 * @pre Source and destination do not overlap. The destination TX buffer is
 *      CPU-owned: it has not been enqueued, or has been dequeued since enqueue.
 */
inline void copy_records_with_ids(uint8_t* target, const uint8_t* source,
                                  unsigned count, uint64_t first_id) {
    for (unsigned index = 0; index < count; ++index) {
        const uint8_t* input_record = source + size_t(index) * 64;
        uint8_t* output_record = target + size_t(index) * 64;
        uint64_t request_id = first_id + index;
#if defined(__aarch64__)
        if (index + 8 < count) {
            __builtin_prefetch(source + size_t(index + 8) * 64, 0, 3);
        }
        uint64x2_t first_vector = vreinterpretq_u64_u8(vld1q_u8(input_record));
        first_vector = vsetq_lane_u64(request_id, first_vector, 0);
        const uint8x16_t second_vector = vld1q_u8(input_record + 16);
        const uint8x16_t third_vector = vld1q_u8(input_record + 32);
        const uint8x16_t fourth_vector = vld1q_u8(input_record + 48);
        vst1q_u8(output_record, vreinterpretq_u8_u64(first_vector));
        vst1q_u8(output_record + 16, second_vector);
        vst1q_u8(output_record + 32, third_vector);
        vst1q_u8(output_record + 48, fourth_vector);
#else
        std::array<uint8_t, 64> record;
        std::memcpy(record.data(), input_record, record.size());
        for (unsigned byte = 0; byte < 8; ++byte) {
            record[byte] = uint8_t(request_id >> (8 * byte));
        }
        std::memcpy(output_record, record.data(), record.size());
#endif
    }
}

/** Return the signed elapsed seconds from begin to end on the monotonic clock. */
inline double seconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}

/**
 * Exclusive RAII owner of a FADES fd and its TX/RX mmap regions.
 *
 * Construction opens and maps a usable ABI-v2 device; there is no separate
 * open step or public partially initialized state. Destruction cancels any
 * stream, unmaps memory and closes the fd. Copy and move are disabled.
 *
 * The ABI calls a numbered reusable buffer a "slot": each direction currently
 * has eight 1 MiB buffers. TX and RX buffer indices belong to separate queues;
 * matching indices do not imply matching requests. Results carry request IDs.
 * This class does not track CPU/driver ownership or create worker threads.
 */
class Device {
public:
    /**
     * Open the device, validate INFO and map both coherent DMA regions.
     * @param path Device node to open exclusively, normally /dev/fades.
     * @throws std::runtime_error If open, INFO, ABI validation or mmap fails.
     *         Already acquired mappings/fd are released before rethrowing.
     */
    explicit Device(const char* path = "/dev/fades") {
        descriptor_ = open(path, O_RDWR | O_CLOEXEC);
        if (descriptor_ < 0) {
            throw std::runtime_error("Cannot open " + std::string(path) + ": " + std::strerror(errno));
        }
        try {
            if (ioctl(descriptor_, FADES_DMA_INFO, &info_) != 0) {
                throw std::runtime_error("INFO: " + std::string(std::strerror(errno)));
            }
            if (info_.abi_version != FADES_DMA_ABI_VERSION || info_.slots != FADES_DMA_SLOTS ||
                info_.slot_bytes != FADES_DMA_SLOT_BYTES ||
                info_.map_bytes != uint64_t(info_.slots) * info_.slot_bytes) {
                throw std::runtime_error("Unsupported DMA ABI");
            }
            void* mapping = mmap(nullptr, info_.map_bytes, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, descriptor_, 0);
            if (mapping == MAP_FAILED) {
                throw std::runtime_error("TX mmap: " + std::string(std::strerror(errno)));
            }
            tx_mapping_ = static_cast<uint8_t*>(mapping);
            mapping = mmap(nullptr, info_.map_bytes, PROT_READ | PROT_WRITE,
                           MAP_SHARED, descriptor_, info_.map_bytes);
            if (mapping == MAP_FAILED) {
                throw std::runtime_error("RX mmap: " + std::string(std::strerror(errno)));
            }
            rx_mapping_ = static_cast<uint8_t*>(mapping);
        } catch (...) {
            release_resources();
            throw;
        }
    }

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    /** Best-effort STREAMOFF, munmap and close; cleanup never throws. */
    ~Device() noexcept {
        release_resources();
    }

    /** Borrow the fd for poll/ioctl; the caller must not close it. */
    int native_handle() const noexcept {
        return descriptor_;
    }

    /** Borrow the validated ABI layout, valid until this Device is destroyed. */
    const fades_dma_info& info() const noexcept {
        return info_;
    }

    /**
     * Return the mapped start address of one numbered buffer (ABI "slot").
     * @param queue FADES_QUEUE_TX or FADES_QUEUE_RX.
     * @param index Buffer number in that queue, in [0, info().slots).
     * @return Borrowed pointer to info().slot_bytes bytes; valid for this
     *         Device's lifetime. The pointer does not transfer ownership.
     * @pre Read/write this memory only while the buffer is CPU-owned.
     * @throws std::out_of_range If the queue or index is invalid.
     */
    uint8_t* buffer_data(unsigned queue, unsigned index) const {
        if (queue > FADES_QUEUE_RX || index >= info_.slots) {
            throw std::out_of_range("Invalid DMA buffer queue/index");
        }
        return (queue == FADES_QUEUE_TX ? tx_mapping_ : rx_mapping_) + size_t(index) * info_.slot_bytes;
    }

    /**
     * Register a CPU-owned buffer with QBUF, without waiting for DMA.
     * @param queue FADES_QUEUE_TX for input or FADES_QUEUE_RX for empty output.
     * @param index Buffer number within that queue.
     * @param records TX input count (1..slot_bytes/64); zero for RX.
     * @post On success the driver owns this buffer until dequeue or STREAMOFF.
     * @throws std::runtime_error On ioctl failure; ownership stays with CPU.
     */
    void enqueue(unsigned queue, unsigned index, unsigned records = 0) {
        fades_buffer buffer{queue, index, records, 0};
        if (ioctl(descriptor_, FADES_QBUF, &buffer) != 0) {
            throw std::runtime_error("QBUF: " + std::string(std::strerror(errno)));
        }
    }

    /**
     * Try to reclaim one completed buffer with nonblocking DQBUF.
     * @param queue Completion queue to inspect: FADES_QUEUE_TX or FADES_QUEUE_RX.
     * @param buffer On success, receives the buffer index and actual count.
     *               Its contents are unspecified when false is returned.
     * @return True if ownership transferred to CPU, false for EAGAIN only.
     *         TX completion permits input reuse; it does not mean solving ended.
     * @throws std::runtime_error For any ioctl error other than EAGAIN.
     */
    bool dequeue(unsigned queue, fades_buffer& buffer) {
        buffer = {queue, 0, 0, 0};
        if (ioctl(descriptor_, FADES_DQBUF, &buffer) == 0) {
            return true;
        }
        if (errno == EAGAIN) {
            return false;
        }
        throw std::runtime_error("DQBUF: " + std::string(std::strerror(errno)));
    }

private:
    /** Release fully or partially acquired resources, including on constructor failure. */
    void release_resources() noexcept {
        if (tx_mapping_ && rx_mapping_) {
            ioctl(descriptor_, FADES_STREAMOFF);
        }
        if (rx_mapping_) {
            munmap(rx_mapping_, info_.map_bytes);
            rx_mapping_ = nullptr;
        }
        if (tx_mapping_) {
            munmap(tx_mapping_, info_.map_bytes);
            tx_mapping_ = nullptr;
        }
        if (descriptor_ >= 0) {
            close(descriptor_);
            descriptor_ = -1;
        }
    }

    int descriptor_ = -1;
    uint8_t* tx_mapping_ = nullptr;
    uint8_t* rx_mapping_ = nullptr;
    fades_dma_info info_{};
};

/** Counts, timings and final hardware snapshot from one drained streaming session. */
struct StreamingResult {
    uint64_t submitted_records = 0; ///< Records successfully enqueued as input.
    uint64_t completed_records = 0; ///< Result records returned through DQBUF(RX).
    uint64_t completed_packets = 0; ///< Completed RX buffers, not problem count.
    uint64_t records_observed_before_deadline = 0; ///< Whole RX counts observed by deadline; 0 for untimed runs.
    double elapsed_seconds = 0; ///< STREAMON through final consumption/recycling, excluding final STATS/STREAMOFF.
    double drain_seconds = 0; ///< END_INPUT return through final consumption/recycling.
    double refill_seconds = 0; ///< Input callback time after initial DDR preparation.
    fades_solver_stats solver_stats{}; ///< Session-cumulative snapshot taken after all results were consumed.
};

/**
 * Run one continuous, single-threaded input/refill/result/drain loop.
 *
 * Register empty RX buffers and prepare initial TX buffers, STREAMON once,
 * then use one poll fd to recycle completed buffers. Stop new input at the
 * record limit or deadline, END_INPUT, consume all submitted results, snapshot
 * counters, and STREAMOFF. There is no drain/reset between buffers.
 *
 * @tparam WriteInputBuffer Callable void(uint8_t* data, unsigned records,
 *                         uint64_t first_record). It writes exactly records
 *                         encoded inputs into the CPU-owned TX buffer. The
 *                         zero-based first_record is an ordinal, not an ID
 *                         assigned by this function. The callback assigns IDs.
 * @tparam ReadOutputBuffer Callable void(unsigned index, unsigned records).
 *                         It reads/validates records via device.buffer_data(RX,
 *                         index). RX is re-enqueued immediately after return;
 *                         do not retain data references past that return.
 * @param device Open device with all buffers CPU-owned, e.g. after construction
 *               or STREAMOFF. The device remains open on return.
 * @param records_per_buffer Maximum input records per packet; final packet can be shorter.
 * @param buffer_count Number of buffers to use per direction, 1..info().slots.
 * @param record_limit Maximum total input count; zero runs an empty session.
 * @param duration_seconds Positive deadline measured from before STREAMON;
 *                         zero disables the deadline. Callers provide a finite,
 *                         nonnegative duration representable by Clock.
 * @param write_input_buffer Input-producing callback described above.
 * @param read_output_buffer Result-consuming callback described above.
 * @return Session counts, timings and validated final stage/per-core totals.
 * @throws std::runtime_error For invalid dimensions, ioctl/poll failures,
 *         60 seconds without a completion, or inconsistent final PL counts.
 *         Callback exceptions propagate. On failure, destroy Device or call
 *         STREAMOFF before reusing it.
 * @note Both callbacks execute synchronously on the calling thread. Initial
 *       input preparation is excluded from elapsed/refill times; later callback
 *       work and final drain are included. A refill crossing the deadline is
 *       discarded without QBUF. Timed completion counts use RX observation time.
 */
template<class WriteInputBuffer, class ReadOutputBuffer>
StreamingResult run_streaming_session(Device& device, unsigned records_per_buffer,
                                      unsigned buffer_count, uint64_t record_limit,
                                      double duration_seconds, WriteInputBuffer write_input_buffer,
                                      ReadOutputBuffer read_output_buffer) {
    if (!records_per_buffer || records_per_buffer > device.info().slot_bytes / 64 ||
        !buffer_count || buffer_count > device.info().slots) {
        throw std::runtime_error("Invalid streaming dimensions");
    }
    StreamingResult result;
    bool input_sealed = false;
    Clock::time_point input_closed_at;
    Clock::time_point deadline = Clock::time_point::max();
    auto prepare_input_buffer = [&](unsigned index) {
        unsigned records = unsigned(std::min<uint64_t>(records_per_buffer,
                                                       record_limit - result.submitted_records));
        if (!records) {
            return;
        }
        auto refill_begin = Clock::now();
        write_input_buffer(device.buffer_data(FADES_QUEUE_TX, index), records, result.submitted_records);
        result.refill_seconds += seconds(refill_begin, Clock::now());
        if (duration_seconds > 0 && Clock::now() >= deadline) {
            return;
        }
        device.enqueue(FADES_QUEUE_TX, index, records);
        result.submitted_records += records;
    };
    for (unsigned index = 0; index < buffer_count; ++index) {
        device.enqueue(FADES_QUEUE_RX, index);
    }
    for (unsigned index = 0; index < buffer_count; ++index) {
        prepare_input_buffer(index);
    }
    result.refill_seconds = 0;
    const auto session_begin = Clock::now();
    deadline = session_begin + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(duration_seconds));
    if (ioctl(device.native_handle(), FADES_STREAMON) != 0) {
        throw std::runtime_error("STREAMON: " + std::string(std::strerror(errno)));
    }
    auto seal_input = [&] {
        if (ioctl(device.native_handle(), FADES_END_INPUT) != 0) {
            throw std::runtime_error("END_INPUT: " + std::string(std::strerror(errno)));
        }
        input_sealed = true;
        input_closed_at = Clock::now();
    };
    if (result.submitted_records == record_limit) {
        seal_input();
    }
    while (!input_sealed || result.completed_records != result.submitted_records) {
        if (!input_sealed && duration_seconds > 0 && Clock::now() >= deadline) {
            seal_input();
        }
        fades_buffer buffer{};
        while (device.dequeue(FADES_QUEUE_RX, buffer)) {
            if (!buffer.records || buffer.records > records_per_buffer) {
                throw std::runtime_error("Invalid RX length");
            }
            result.completed_records += buffer.records;
            ++result.completed_packets;
            if (duration_seconds > 0 && Clock::now() <= deadline) {
                result.records_observed_before_deadline += buffer.records;
            }
            read_output_buffer(buffer.index, buffer.records);
            device.enqueue(FADES_QUEUE_RX, buffer.index);
        }
        if (!input_sealed) {
            while (device.dequeue(FADES_QUEUE_TX, buffer)) {
                if (duration_seconds > 0 && Clock::now() >= deadline) {
                    seal_input();
                    break;
                }
                prepare_input_buffer(buffer.index);
                if (result.submitted_records == record_limit) {
                    seal_input();
                    break;
                }
            }
        }
        if (input_sealed && result.completed_records == result.submitted_records) {
            break;
        }
        pollfd poll_descriptor{device.native_handle(), short(POLLIN | (input_sealed ? 0 : POLLOUT)), 0};
        int timeout_ms = 60000;
        if (!input_sealed && duration_seconds > 0) {
            double remaining_seconds = seconds(Clock::now(), deadline);
            timeout_ms = std::clamp(int(std::ceil(std::max(0.0, remaining_seconds) * 1000)), 0, 60000);
        }
        int poll_result = poll(&poll_descriptor, 1, timeout_ms);
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result < 0) {
            throw std::runtime_error("poll: " + std::string(std::strerror(errno)));
        }
        if (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error("DMA/solver fault");
        }
        if (!poll_result && !(!input_sealed && duration_seconds > 0 && Clock::now() >= deadline)) {
            throw std::runtime_error("DMA progress timeout");
        }
    }
    auto session_end = Clock::now();
    result.elapsed_seconds = seconds(session_begin, session_end);
    result.drain_seconds = seconds(input_closed_at, session_end);
    if (ioctl(device.native_handle(), FADES_SOLVER_STATS, &result.solver_stats) != 0) {
        throw std::runtime_error("STATS: " + std::string(std::strerror(errno)));
    }
    if (ioctl(device.native_handle(), FADES_STREAMOFF) != 0) {
        throw std::runtime_error("STREAMOFF: " + std::string(std::strerror(errno)));
    }
    for (unsigned index : {0u, 1u, 3u, 4u}) {
        if (result.solver_stats.counters[index] != result.submitted_records) {
            throw std::runtime_error("PL stage count mismatch");
        }
    }
    uint64_t dispatched_records = 0;
    uint64_t collected_records = 0;
    for (unsigned core = 0; core < 6; ++core) {
        dispatched_records += result.solver_stats.counters[6 + core];
        collected_records += result.solver_stats.counters[12 + core];
        if (result.solver_stats.counters[38 + core] > 2) {
            throw std::runtime_error("PL inflight limit exceeded");
        }
    }
    if (dispatched_records != result.submitted_records || collected_records != result.submitted_records) {
        throw std::runtime_error("PL per-core count mismatch");
    }
    return result;
}

} // namespace fades
