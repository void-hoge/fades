// SPDX-License-Identifier: MIT
#pragma once

#include "fades_ioctl.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fades {

/**
 * Exclusive RAII owner of a FADES fd and its TX/RX mmap regions.
 * Construction opens and maps a usable device. Destruction cancels any stream,
 * unmaps memory and closes the fd. There are no worker threads. A numbered
 * buffer is called a slot in the ABI; TX and RX indices are independent.
 */
class Device {
public:
    /**
     * Open, validate INFO and map TX/RX DMA regions. RX is cacheable and the
     * driver synchronizes it at ownership changes; callers never flush caches.
     * @throws std::runtime_error On failure, after releasing acquired resources.
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
    ~Device() noexcept { release_resources(); }

    /** Borrow the fd for poll/ioctl; the caller must not close it. */
    int native_handle() const noexcept { return descriptor_; }

    /** Borrow the validated ABI layout until this Device is destroyed. */
    const fades_dma_info& info() const noexcept { return info_; }

    /**
     * Borrow one mapped buffer, containing info().slot_bytes bytes.
     * @param queue FADES_QUEUE_TX or FADES_QUEUE_RX.
     * @param index Buffer number in [0, info().slots).
     * @pre Read/write this memory only while the buffer is CPU-owned.
     * @throws std::out_of_range For an invalid queue/index.
     */
    uint8_t* buffer_data(unsigned queue, unsigned index) const {
        if (queue > FADES_QUEUE_RX || index >= info_.slots) {
            throw std::out_of_range("Invalid DMA buffer queue/index");
        }
        return (queue == FADES_QUEUE_TX ? tx_mapping_ : rx_mapping_) + size_t(index) * info_.slot_bytes;
    }

    /**
     * Register a CPU-owned buffer without waiting for DMA. On success the
     * driver owns it until dequeue/STREAMOFF. TX records must fit the configured
     * slot capacity; RX records must be zero.
     * @throws std::runtime_error On ioctl failure, with ownership unchanged.
     */
    void enqueue(unsigned queue, unsigned index, unsigned records = 0) {
        fades_buffer buffer{queue, index, records, 0};
        if (ioctl(descriptor_, FADES_QBUF, &buffer) != 0) {
            throw std::runtime_error("QBUF: " + std::string(std::strerror(errno)));
        }
    }

    /**
     * Reclaim one completed buffer without waiting. Returns false for EAGAIN,
     * otherwise returns true with its index and actual count in buffer. TX
     * completion permits memory reuse; it does not mean solving is complete.
     * @throws std::runtime_error For errors other than EAGAIN.
     */
    bool dequeue(unsigned queue, fades_buffer& buffer) {
        buffer = {queue, 0, 0, 0};
        if (ioctl(descriptor_, FADES_DQBUF, &buffer) == 0) { return true; }
        if (errno == EAGAIN) { return false; }
        throw std::runtime_error("DQBUF: " + std::string(std::strerror(errno)));
    }

private:
    /** Release fully or partially acquired resources, including constructor rollback. */
    void release_resources() noexcept {
        if (tx_mapping_ && rx_mapping_) { ioctl(descriptor_, FADES_STREAMOFF); }
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

} // namespace fades
