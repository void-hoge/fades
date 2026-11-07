// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace fades {

/** Encoded 64-byte wire record. Input and result layouts are documented in README. */
using Record = std::array<uint8_t, 64>;
static_assert(sizeof(Record) == 64);

/**
 * Copy encoded records to CPU-owned DMA memory while replacing their IDs.
 * Payload/reserved bytes are preserved. Full-width stores fuse ID insertion and
 * copying into a single DMA-memory pass; AArch64 also prefetches the cached source.
 * @param destination Writable CPU-owned TX memory, at least count * 64 bytes.
 * @param source Cached encoded source, at least count * 64 bytes.
 * @param count Number of records, not bytes. Zero performs no memory accesses.
 * @param first_id ID of the first record; subsequent IDs are first_id + index.
 * @pre Source and destination do not overlap. The caller ensures IDs do not wrap
 *      or collide with outstanding requests. Never write driver-owned TX memory.
 */
inline void copy_records_with_ids(uint8_t* destination, const uint8_t* source,
                                  unsigned count, uint64_t first_id) {
    for (unsigned index = 0; index < count; ++index) {
        const uint8_t* input_record = source + size_t(index) * 64;
        uint8_t* output_record = destination + size_t(index) * 64;
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
        Record record;
        std::memcpy(record.data(), input_record, record.size());
        for (unsigned byte = 0; byte < 8; ++byte) {
            record[byte] = uint8_t(request_id >> (8 * byte));
        }
        std::memcpy(output_record, record.data(), record.size());
#endif
    }
}

} // namespace fades
