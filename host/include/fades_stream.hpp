// SPDX-License-Identifier: MIT
// Thin ioctl helpers shared by the tools. No worker threads or hidden replay.
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
#include <limits>
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
using Clock = std::chrono::steady_clock;
inline void require(bool value, const std::string& text) {
    if (!value) throw std::runtime_error(text);
}
inline void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}
// Copy cached, already encoded 64-byte records to a coherent TX mapping while
// replacing their IDs. Full-width stores avoid a second staging copy and slow
// narrow writes to the noncached DMA mapping. Source and destination must not
// overlap. Call only for CPU-owned TX slots.
inline void copy_records_with_ids(uint8_t* target, const uint8_t* source,
                                  unsigned count, uint64_t first_id) {
    for (unsigned j=0;j<count;++j) {
        const uint8_t* src=source+size_t(j)*64;
        uint8_t* dst=target+size_t(j)*64;
        uint64_t id=first_id+j;
#if defined(__aarch64__)
        if(j+8<count)__builtin_prefetch(source+size_t(j+8)*64,0,3);
        uint64x2_t a=vreinterpretq_u64_u8(vld1q_u8(src));
        a=vsetq_lane_u64(id,a,0);
        const uint8x16_t b=vld1q_u8(src+16),c=vld1q_u8(src+32),d=vld1q_u8(src+48);
        vst1q_u8(dst,vreinterpretq_u8_u64(a));
        vst1q_u8(dst+16,b);vst1q_u8(dst+32,c);vst1q_u8(dst+48,d);
#else
        std::array<uint8_t,64> r;
        std::memcpy(r.data(),src,64);
        for(unsigned b=0;b<8;++b)r[b]=uint8_t(id>>(8*b));
        std::memcpy(dst,r.data(),64);
#endif
    }
}
inline void control(int fd, unsigned long cmd, void* arg = nullptr) {
    if (ioctl(fd, cmd, arg) < 0)
        throw std::runtime_error("fades ioctl: " + std::string(std::strerror(errno)));
}
inline double seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b-a).count();
}
struct Device {
    int fd = -1;
    uint8_t *tx = nullptr, *rx = nullptr;
    fades_dma_info info{};
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() {
        if (fd >= 0) ioctl(fd, FADES_STREAMOFF);
        if (rx) munmap(rx, info.map_bytes);
        if (tx) munmap(tx, info.map_bytes);
        if (fd >= 0) close(fd);
    }
    void open_device() {
        fd = open("/dev/fades", O_RDWR | O_CLOEXEC);
        require(fd >= 0, "Cannot open /dev/fades: " + std::string(std::strerror(errno)));
        control(fd, FADES_DMA_INFO, &info);
        require(info.abi_version == FADES_DMA_ABI_VERSION && info.slots == FADES_DMA_SLOTS &&
                info.slot_bytes == FADES_DMA_SLOT_BYTES &&
                info.map_bytes == uint64_t(info.slots)*info.slot_bytes, "Unsupported DMA ABI");
        void* p = mmap(nullptr, info.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        require(p != MAP_FAILED, "TX mmap failed"); tx = static_cast<uint8_t*>(p);
        p = mmap(nullptr, info.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, info.map_bytes);
        require(p != MAP_FAILED, "RX mmap failed"); rx = static_cast<uint8_t*>(p);
    }
    uint8_t* slot(unsigned queue, unsigned index) {
        return (queue == FADES_QUEUE_TX ? tx : rx) + size_t(index)*info.slot_bytes;
    }
    void queue(unsigned queue, unsigned index, unsigned records = 0) {
        fades_buffer b{queue,index,records,0}; control(fd,FADES_QBUF,&b);
    }
    bool dequeue(unsigned queue, fades_buffer& b) {
        b = {queue,0,0,0};
        if (ioctl(fd,FADES_DQBUF,&b) == 0) return true;
        if (errno == EAGAIN) return false;
        throw std::runtime_error("DQBUF: " + std::string(std::strerror(errno)));
    }
};
struct Run {
    uint64_t submitted = 0, received = 0, packets = 0, within = 0;
    double elapsed = 0, drain = 0, refill = 0;
    fades_solver_stats stats{};
};

// fill(ptr, count, first_request), consume(index, count). A finite limit or
// duration ends input without stopping the pipeline. All buffers remain owned
// by their respective side until DQBUF/STREAMOFF.
template<class Fill, class Consume>
Run pump(Device& dev, unsigned per, unsigned slots, uint64_t limit,
         double duration, Fill fill, Consume consume) {
    require(per && per <= dev.info.slot_bytes/64 && slots && slots <= dev.info.slots,
            "Invalid streaming dimensions");
    Run run;
    bool sealed = false;
    Clock::time_point closed;
    Clock::time_point deadline = Clock::time_point::max();
    auto prepare = [&](unsigned index) {
        unsigned n = unsigned(std::min<uint64_t>(per,limit-run.submitted));
        if (!n) return;
        auto a = Clock::now();
        fill(dev.slot(FADES_QUEUE_TX,index),n,run.submitted);
        run.refill += seconds(a,Clock::now());
        // A refill may cross the deadline; do not enqueue that buffer.
        if (duration > 0 && Clock::now() >= deadline) return;
        dev.queue(FADES_QUEUE_TX,index,n); run.submitted += n;
    };
    for (unsigned i=0;i<slots;++i) dev.queue(FADES_QUEUE_RX,i);
    for (unsigned i=0;i<slots;++i) prepare(i);
    run.refill = 0; // Initial DDR preparation is outside the measured interval.
    const auto start = Clock::now();
    deadline = start + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(duration));
    control(dev.fd,FADES_STREAMON);
    auto seal = [&] {
        control(dev.fd,FADES_END_INPUT); sealed = true; closed = Clock::now();
    };
    if (run.submitted == limit) seal();
    while (!sealed || run.received != run.submitted) {
        if (!sealed && duration > 0 && Clock::now() >= deadline) seal();
        fades_buffer b{};
        while (dev.dequeue(FADES_QUEUE_RX,b)) {
            require(b.records && b.records<=per, "Invalid RX length");
            run.received += b.records; ++run.packets;
            if (duration > 0 && Clock::now() <= deadline) run.within += b.records;
            consume(b.index,b.records);
            dev.queue(FADES_QUEUE_RX,b.index);
        }
        if (!sealed) {
            while (dev.dequeue(FADES_QUEUE_TX,b)) {
                if (duration > 0 && Clock::now() >= deadline) { seal(); break; }
                prepare(b.index);
                if (run.submitted == limit) { seal(); break; }
            }
        }
        if (sealed && run.received == run.submitted) break;
        pollfd pfd{dev.fd,short(POLLIN | (sealed ? 0 : POLLOUT)),0};
        int timeout = 60000;
        if (!sealed && duration > 0) {
            double left = seconds(Clock::now(),deadline);
            timeout = std::clamp(int(std::ceil(std::max(0.0,left)*1000)),0,60000);
        }
        int ret = poll(&pfd,1,timeout);
        if (ret < 0 && errno == EINTR) continue;
        require(ret >= 0,"poll failed");
        require(!(pfd.revents & (POLLERR|POLLHUP|POLLNVAL)),"DMA/solver fault");
        require(ret || (!sealed && duration > 0 && Clock::now()>=deadline),"DMA progress timeout");
    }
    auto end = Clock::now();
    run.elapsed = seconds(start,end); run.drain = seconds(closed,end);
    control(dev.fd,FADES_SOLVER_STATS,&run.stats);
    control(dev.fd,FADES_STREAMOFF);
    for (unsigned i : {0u,1u,3u,4u})
        require(run.stats.counters[i]==run.submitted,"PL stage count mismatch");
    uint64_t sent=0,got=0;
    for(unsigned g=0;g<6;++g) {
        sent+=run.stats.counters[6+g];got+=run.stats.counters[12+g];
        require(run.stats.counters[38+g]<=2,"PL inflight limit exceeded");
    }
    require(sent==run.submitted && got==run.submitted,"PL per-core count mismatch");
    return run;
}
} // namespace fades
