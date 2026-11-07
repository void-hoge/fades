// SPDX-License-Identifier: MIT
#pragma once

#include "fades_device.hpp"
#include "fades_records.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <exception>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <vector>
#include <utility>

namespace fades {

/**
 * Single-threaded fixed-capacity slot interface above Device.
 * Each submit accepts an entire nonempty span into one free TX slot, or returns
 * false without copying. Receive lends one full output slot, or an idle tail,
 * without copying. Its move-only OutputSlot recycles RX on release/destruction.
 * Slots are independent of input groups; match results by request ID.
 * This class owns no benchmark timer, input source, callback or CPU oracle.
 */
class SolverStream {
    /** Keep mappings alive while borrowed output slots exist, even after stream destruction. */
    struct State {
        explicit State(const char* path) : device(path) {}
        Device device;
        bool closed = false;
        std::exception_ptr recycle_error;
    };

public:
    /**
     * Exclusive, move-only loan of a completed cached RX slot. records() is
     * valid until release/destruction; do not retain its references afterward.
     * No allocation/copy occurs per loan. A shared lifetime token keeps mmap
     * valid if the stream is destroyed first; in that case release does not
     * requeue into the stopped stream. The loan retains the exclusive fd too;
     * release it before opening another stream. All operations remain single-threaded.
     */
    class OutputSlot {
    public:
        OutputSlot(const OutputSlot&) = delete;
        OutputSlot& operator=(const OutputSlot&) = delete;

        /** Move ownership of the loan, leaving the source empty. */
        OutputSlot(OutputSlot&& other) noexcept
            : state_(std::move(other.state_)), index_(other.index_), records_(std::exchange(other.records_, {})) {}

        /** Return the previous loan, then take ownership of another loan. */
        OutputSlot& operator=(OutputSlot&& other) noexcept {
            if (this != &other) {
                release_noexcept();
                state_ = std::move(other.state_);
                index_ = other.index_;
                records_ = std::exchange(other.records_, {});
            }
            return *this;
        }

        /** Return RX automatically; a recycle failure is rethrown by the stream's next I/O/statistics call. */
        ~OutputSlot() noexcept { release_noexcept(); }

        /** Borrow the actual result records in this slot, with no CPU copy. */
        std::span<const Record> records() const noexcept { return records_; }

        /** Actual record count; zero after move or release. */
        size_t size() const noexcept { return records_.size(); }

        /**
         * Return the slot explicitly and invalidate its views. Idempotent.
         * @throws std::runtime_error Immediately if QBUF fails; the stream also
         *         remembers the error so automatic cleanup cannot hide it.
         */
        void release() {
            auto state = std::move(state_);
            records_ = {};
            if (state && !state->closed) {
                try {
                    state->device.enqueue(FADES_QUEUE_RX, index_);
                } catch (...) {
                    state->recycle_error = std::current_exception();
                    throw;
                }
            }
        }

    private:
        friend class SolverStream;

        /** Construct a loan only after Device transferred RX ownership to CPU. */
        OutputSlot(std::shared_ptr<State> state, unsigned index, unsigned count)
            : state_(std::move(state)), index_(index),
              records_(reinterpret_cast<const Record*>(state_->device.buffer_data(FADES_QUEUE_RX, index)), count) {}

        /** Defer release errors rather than throwing from a destructor/move assignment. */
        void release_noexcept() noexcept {
            try { release(); } catch (...) {}
        }

        std::shared_ptr<State> state_;
        unsigned index_;
        std::span<const Record> records_;
    };

    /**
     * Open Device, post RX buffers and start one session.
     * Slot capacity is fixed at 16,384 records (1 MiB); each direction has
     * eight buffers. Capacity is not a user-facing runtime parameter.
     * @throws std::runtime_error On setup failure; Device rolls back resources.
     */
    explicit SolverStream(const char* path = "/dev/fades") : state_(std::make_shared<State>(path)) {
        free_inputs_.reserve(FADES_DMA_SLOTS);
        for (unsigned index = 0; index < FADES_DMA_SLOTS; ++index) {
            free_inputs_.push_back(index);
            state_->device.enqueue(FADES_QUEUE_RX, index);
        }
        if (ioctl(state_->device.native_handle(), FADES_STREAMON) != 0) {
            throw std::runtime_error("STREAMON: " + std::string(std::strerror(errno)));
        }
    }

    SolverStream(const SolverStream&) = delete;
    SolverStream& operator=(const SolverStream&) = delete;
    SolverStream(SolverStream&&) = delete;
    SolverStream& operator=(SolverStream&&) = delete;

    /** Stop hardware if loans remain; their mappings stay valid until the last loan is destroyed. */
    ~SolverStream() noexcept {
        state_->closed = true;
        if (state_.use_count() > 1) { ioctl(state_->device.native_handle(), FADES_STREAMOFF); }
    }

    /** Copy all inputs unchanged into one slot; caller supplies unique outstanding IDs. */
    bool try_submit(std::span<const Record> inputs) {
        validate_submission(inputs.size());
        reclaim_inputs();
        if (free_inputs_.empty()) { return false; }
        unsigned index = free_inputs_.back();
        std::memcpy(state_->device.buffer_data(FADES_QUEUE_TX, index), inputs.data(), inputs.size_bytes());
        enqueue_input(index, unsigned(inputs.size()));
        return true;
    }

    /**
     * Accept all inputs or none, assigning IDs first_id..first_id+size-1 using
     * the fused copy. On false, no input memory is copied and no IDs are used.
     * The caller ensures assigned IDs do not wrap or collide.
     */
    bool try_submit(std::span<const Record> inputs, uint64_t first_id) {
        validate_submission(inputs.size());
        reclaim_inputs();
        if (free_inputs_.empty()) { return false; }
        unsigned index = free_inputs_.back();
        copy_records_with_ids(state_->device.buffer_data(FADES_QUEUE_TX, index), inputs[0].data(),
                              unsigned(inputs.size()), first_id);
        enqueue_input(index, unsigned(inputs.size()));
        return true;
    }

    /**
     * Lend one full output slot or completed idle tail without copying. Returns
     * nullopt when none is ready. Read records() before releasing the loan;
     * holding all RX slots applies backpressure until a loan is returned.
     */
    std::optional<OutputSlot> try_receive() {
        check_error();
        fades_buffer buffer{};
        if (!state_->device.dequeue(FADES_QUEUE_RX, buffer)) { return std::nullopt; }
        if (!buffer.records || buffer.records > capacity_) {
            throw std::runtime_error("Invalid RX length");
        }
        received_ += buffer.records;
        return OutputSlot(state_, buffer.index, buffer.records);
    }

    /**
     * Seal further input while keeping receive active. Idle tails flush without
     * this call; sealing also makes wait() ignore irrelevant TX completions.
     */
    void finish_input() {
        check_error();
        if (!sealed_) {
            if (ioctl(state_->device.native_handle(), FADES_END_INPUT) != 0) {
                throw std::runtime_error("END_INPUT: " + std::string(std::strerror(errno)));
            }
            sealed_ = true;
        }
    }

    /** Wait for progress, returning false on timeout. Negative timeout means indefinitely. */
    bool wait(int timeout_ms = -1) {
        reclaim_inputs();
        if (!sealed_ && !free_inputs_.empty()) { return true; }
        return wait_for_events(short(POLLIN | (sealed_ ? 0 : POLLOUT)), timeout_ms);
    }

    /** Wait only for results while leaving input open for later submissions. */
    bool wait_for_results(int timeout_ms = -1) {
        return wait_for_events(POLLIN, timeout_ms);
    }

    /** Wait for progress no later than a monotonic deadline; false means timeout. */
    bool wait_until(std::chrono::steady_clock::time_point deadline) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= remaining.zero()) { return false; }
        auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        return wait(int(std::min<int64_t>(milliseconds, std::numeric_limits<int>::max())));
    }

    /** Maximum input size and full output size, in 64-byte records. */
    unsigned capacity() const noexcept { return capacity_; }

    /** Accepted records whose output loans have not yet been returned to the caller. */
    uint64_t outstanding() const noexcept { return submitted_ - received_; }

    /** Read cumulative hardware counters without stopping the session or validating them. */
    fades_solver_stats statistics() const {
        check_error();
        fades_solver_stats statistics{};
        if (ioctl(state_->device.native_handle(), FADES_SOLVER_STATS, &statistics) != 0) {
            throw std::runtime_error("STATS: " + std::string(std::strerror(errno)));
        }
        return statistics;
    }

private:
    /** Surface a failure from automatic RX recycling at the next observable operation. */
    void check_error() const {
        if (state_->recycle_error) { std::rethrow_exception(state_->recycle_error); }
    }

    /** Poll a selected direction, checking device faults and allowing spurious EINTR wakeups. */
    bool wait_for_events(short events, int timeout_ms) {
        check_error();
        pollfd descriptor{state_->device.native_handle(), events, 0};
        int result = poll(&descriptor, 1, timeout_ms);
        if (result < 0 && errno == EINTR) { return true; }
        if (result < 0) { throw std::runtime_error("poll: " + std::string(std::strerror(errno))); }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error("DMA/solver fault");
        }
        return result != 0;
    }

    /** Reject oversized/empty input before testing availability; never partially accept it. */
    void validate_submission(size_t records) const {
        if (!records || records > capacity_) {
            throw std::invalid_argument("Input does not fit one slot");
        }
        check_error();
        if (sealed_) { throw std::logic_error("Input is already finished"); }
    }

    /** Return every DMA-completed input buffer to the free list. */
    void reclaim_inputs() {
        check_error();
        fades_buffer buffer{};
        while (state_->device.dequeue(FADES_QUEUE_TX, buffer)) { free_inputs_.push_back(buffer.index); }
    }

    /** Transfer a successfully copied TX buffer to Device and account for all its input. */
    void enqueue_input(unsigned index, unsigned records) {
        state_->device.enqueue(FADES_QUEUE_TX, index, records);
        free_inputs_.pop_back();
        submitted_ += records;
    }

    std::shared_ptr<State> state_;
    static constexpr unsigned capacity_ = FADES_RECORDS_PER_SLOT;
    std::vector<unsigned> free_inputs_;
    uint64_t submitted_ = 0;
    uint64_t received_ = 0;
    bool sealed_ = false;
};

} // namespace fades
