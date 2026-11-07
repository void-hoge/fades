// Exercise constructor rollback and queue errors without requiring /dev/fades.
#include "fades_stream.hpp"
#include <cstdarg>
#include <iostream>
#include <vector>

namespace {

/** Failure point and resource counts for the wrapped POSIX calls below. */
struct MockDevice {
    bool fail_open = false;
    bool fail_info = false;
    bool incompatible_abi = false;
    unsigned fail_mapping = 0;
    unsigned mapping_attempts = 0;
    unsigned active_mappings = 0;
    unsigned active_descriptors = 0;
    unsigned streamoff_calls = 0;
    int queue_error = 0;
    int dequeue_error = EAGAIN;
    fades_buffer last_enqueued{};
    unsigned enqueue_calls = 0;
    unsigned streamon_calls = 0;
    std::array<void*, 2> mappings{};
    std::array<std::vector<fades_buffer>, 2> completions;
};

MockDevice mock;

/** Fail a test with a diagnostic rather than relying on NDEBUG-sensitive assert. */
void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/** Expect a Device construction error and verify rollback of every resource. */
void expect_constructor_failure() {
    bool failed = false;
    try {
        fades::Device device("/mock/fades");
    } catch (const std::runtime_error&) {
        failed = true;
    }
    expect(failed, "Constructor unexpectedly succeeded");
    expect(mock.active_descriptors == 0, "Failed constructor leaked its fd");
    expect(mock.active_mappings == 0, "Failed constructor leaked a mapping");
    expect(mock.streamoff_calls == 0, "Partial construction tried to stop a stream");
}

} // namespace

extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void*, size_t);

/** Substitute a fake exclusive fd, optionally injecting an open failure. */
extern "C" int __wrap_open(const char*, int, ...) {
    if (mock.fail_open) {
        errno = ENOENT;
        return -1;
    }
    ++mock.active_descriptors;
    return 123;
}

/** Account for closing the fake fd. */
extern "C" int __wrap_close(int) {
    --mock.active_descriptors;
    return 0;
}

/** Supply INFO and controlled queue outcomes, recording ownership operations. */
extern "C" int __wrap_ioctl(int, unsigned long command, ...) {
    if (command == FADES_STREAMOFF) {
        ++mock.streamoff_calls;
        return 0;
    }
    if (command == FADES_STREAMON) {
        ++mock.streamon_calls;
        return 0;
    }
    va_list arguments;
    va_start(arguments, command);
    fades_dma_info* info = nullptr;
    fades_buffer* buffer = nullptr;
    if (command == FADES_DMA_INFO) {
        info = va_arg(arguments, fades_dma_info*);
    } else if (command == FADES_QBUF || command == FADES_DQBUF) {
        buffer = va_arg(arguments, fades_buffer*);
    }
    va_end(arguments);
    if (command == FADES_DMA_INFO) {
        if (mock.fail_info) {
            errno = EIO;
            return -1;
        }
        *info = {
            mock.incompatible_abi ? 1u : FADES_DMA_ABI_VERSION,
            FADES_DMA_SLOTS, FADES_DMA_SLOT_BYTES, 0,
            uint64_t(FADES_DMA_SLOTS) * FADES_DMA_SLOT_BYTES
        };
    } else if (command == FADES_QBUF) {
        if (mock.queue_error) {
            errno = mock.queue_error;
            return -1;
        }
        mock.last_enqueued = *buffer;
        ++mock.enqueue_calls;
    } else if (command == FADES_DQBUF) {
        auto& completions = mock.completions[buffer->queue];
        if (!completions.empty()) {
            *buffer = completions.front();
            completions.erase(completions.begin());
            return 0;
        }
        if (mock.dequeue_error) {
            errno = mock.dequeue_error;
            return -1;
        }
        buffer->index = 3;
        buffer->records = 17;
    }
    return 0;
}

/** Use anonymous memory for successful DMA maps and inject either mmap failure. */
extern "C" void* __wrap_mmap(void*, size_t length, int, int, int, off_t) {
    ++mock.mapping_attempts;
    if (mock.mapping_attempts == mock.fail_mapping) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    void* mapping = __real_mmap(nullptr, length, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping != MAP_FAILED) {
        ++mock.active_mappings;
        mock.mappings[mock.mapping_attempts - 1] = mapping;
    }
    return mapping;
}

/** Unmap the real anonymous allocation and account for its release. */
extern "C" int __wrap_munmap(void* mapping, size_t length) {
    int result = __real_munmap(mapping, length);
    if (result == 0) {
        --mock.active_mappings;
    }
    return result;
}

/** Check constructor rollback, normal RAII, queue failures and encoded ID copies. */
int main() try {
    mock.fail_open = true;
    expect_constructor_failure();
    mock = {};
    mock.fail_info = true;
    expect_constructor_failure();
    mock = {};
    mock.incompatible_abi = true;
    expect_constructor_failure();
    for (unsigned mapping : {1u, 2u}) {
        mock = {};
        mock.fail_mapping = mapping;
        expect_constructor_failure();
    }
    mock = {};
    {
        fades::Device device("/mock/fades");
        expect(device.native_handle() == 123, "Wrong borrowed fd");
        expect(mock.active_mappings == 2, "Constructor did not map both queues");
        expect(device.buffer_data(FADES_QUEUE_TX, 1) - device.buffer_data(FADES_QUEUE_TX, 0) ==
               device.info().slot_bytes, "Incorrect buffer stride");
        device.enqueue(FADES_QUEUE_TX, 2, 10000);
        expect(mock.last_enqueued.index == 2 && mock.last_enqueued.records == 10000,
               "enqueue did not forward buffer arguments");
        mock.queue_error = EBUSY;
        bool enqueue_failed = false;
        try {
            device.enqueue(FADES_QUEUE_TX, 2, 10000);
        } catch (const std::runtime_error&) {
            enqueue_failed = true;
        }
        expect(enqueue_failed, "enqueue swallowed EBUSY");
        fades_buffer buffer{};
        expect(!device.dequeue(FADES_QUEUE_RX, buffer), "EAGAIN was not an empty completion queue");
        mock.dequeue_error = EIO;
        bool dequeue_failed = false;
        try {
            device.dequeue(FADES_QUEUE_RX, buffer);
        } catch (const std::runtime_error&) {
            dequeue_failed = true;
        }
        expect(dequeue_failed, "dequeue swallowed EIO");
        mock.dequeue_error = 0;
        expect(device.dequeue(FADES_QUEUE_RX, buffer) && buffer.index == 3 && buffer.records == 17,
               "dequeue did not return the completion");
    }
    expect(mock.active_descriptors == 0 && mock.active_mappings == 0,
           "Destructor leaked resources");
    expect(mock.streamoff_calls == 1, "Destructor did not stop its stream exactly once");

    mock = {};
    {
        fades::SolverStream solver("/mock/fades");
        expect(solver.capacity() == 16384 && mock.streamon_calls == 1, "Fixed slot stream was not started");
        std::array<fades::Record, 3> inputs{};
        inputs[0][8] = 5;
        expect(solver.try_submit(inputs, 100), "First complete slot was not accepted");
        expect(solver.outstanding() == 3, "Submitted count was not tracked");
        const unsigned first_index = mock.last_enqueued.index;
        for (unsigned index = 1; index < FADES_DMA_SLOTS; ++index) {
            expect(solver.try_submit(inputs, 100 + 3 * index), "Initial free slot was unavailable");
        }
        expect(!solver.try_submit(inputs, 200), "Busy slot accepted a second input");
        const auto* tx = static_cast<const uint8_t*>(mock.mappings[0]) + first_index * FADES_DMA_SLOT_BYTES;
        expect(tx[0] == 100 && tx[8] == 5, "Busy submission changed DMA memory");
        bool oversized_failed = false;
        try {
            std::vector<fades::Record> oversized(solver.capacity() + 1);
            solver.try_submit(oversized);
        } catch (const std::invalid_argument&) {
            oversized_failed = true;
        }
        expect(oversized_failed, "Oversized input did not fail when all slots were busy");
        expect(!solver.try_receive(), "Empty receive queue returned results");
        std::memcpy(mock.mappings[1], inputs.data(), sizeof(inputs));
        mock.completions[FADES_QUEUE_RX].push_back({FADES_QUEUE_RX, 0, 3, 0});
        auto output = solver.try_receive();
        expect(output && output->size() == 3 && output->records()[0][8] == 5,
               "Idle tail was not returned as an RX loan");
        expect(output->records().data() == mock.mappings[1], "Receive copied instead of lending DMA memory");
        const auto enqueue_calls = mock.enqueue_calls;
        auto moved = std::move(*output);
        output.reset();
        expect(mock.enqueue_calls == enqueue_calls, "Moving a loan recycled it early");
        moved.release();
        expect(mock.enqueue_calls == enqueue_calls + 1 && solver.outstanding() == 21 &&
               mock.last_enqueued.queue == FADES_QUEUE_RX, "Loan release did not recycle exactly once");
        moved.release();
        expect(mock.enqueue_calls == enqueue_calls + 1, "Duplicate loan release");
        mock.completions[FADES_QUEUE_RX].push_back({FADES_QUEUE_RX, 1, 1, 0});
        mock.completions[FADES_QUEUE_RX].push_back({FADES_QUEUE_RX, 2, 1, 0});
        auto first_loan = solver.try_receive();
        auto second_loan = solver.try_receive();
        const auto before_assignment = mock.enqueue_calls;
        *first_loan = std::move(*second_loan);
        second_loan.reset();
        expect(mock.enqueue_calls == before_assignment + 1, "Move assignment lost/recycled the wrong loan");
        first_loan.reset();
        expect(mock.enqueue_calls == before_assignment + 2, "Replaced loan was not returned exactly once");
        mock.completions[FADES_QUEUE_TX].push_back({FADES_QUEUE_TX, 0, 3, 0});
        expect(solver.try_submit(inputs), "Completed input slot was not reusable");
        solver.finish_input();
        bool sealed_failed = false;
        try {
            solver.try_submit(inputs);
        } catch (const std::logic_error&) {
            sealed_failed = true;
        }
        expect(sealed_failed, "Sealed stream accepted input");
    }
    expect(!mock.active_descriptors && !mock.active_mappings, "SolverStream leaked Device resources");

    // A loan may outlive its stream: stop DMA, keep mappings, never requeue it.
    mock = {};
    std::optional<fades::SolverStream::OutputSlot> held;
    {
        fades::SolverStream solver("/mock/fades");
        std::array<fades::Record, 1> input{};
        solver.try_submit(input);
        mock.completions[FADES_QUEUE_RX].push_back({FADES_QUEUE_RX, 0, 1, 0});
        held = solver.try_receive();
    }
    expect(mock.active_descriptors == 1 && mock.active_mappings == 2 && mock.streamoff_calls == 1,
           "Stream destruction invalidated a live output view or left DMA running");
    auto enqueue_calls = mock.enqueue_calls;
    held.reset();
    expect(!mock.active_descriptors && !mock.active_mappings && mock.enqueue_calls == enqueue_calls,
           "Late loan destruction requeued stopped DMA or leaked mappings");

    // Automatic recycling cannot throw; its failure must surface on the next operation.
    mock = {};
    {
        fades::SolverStream solver("/mock/fades");
        std::array<fades::Record, 1> input{};
        solver.try_submit(input);
        mock.completions[FADES_QUEUE_RX].push_back({FADES_QUEUE_RX, 0, 1, 0});
        auto output = solver.try_receive();
        mock.queue_error = EIO;
        output.reset();
        bool recycle_failed = false;
        try { solver.try_receive(); } catch (const std::runtime_error&) { recycle_failed = true; }
        expect(recycle_failed, "Destructor hid RX recycling failure");
    }

    std::array<uint8_t, 3 * 64> source{};
    std::array<uint8_t, 3 * 64> destination{};
    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = uint8_t(index);
    }
    constexpr uint64_t first_id = 0xabcdef01fffffffeULL;
    fades::copy_records_with_ids(destination.data(), source.data(), 3, first_id);
    for (unsigned record = 0; record < 3; ++record) {
        uint64_t id = 0;
        for (unsigned byte = 0; byte < 8; ++byte) {
            id |= uint64_t(destination[record * 64 + byte]) << (8 * byte);
        }
        expect(id == first_id + record, "Incorrect copied ID across a 32-bit carry");
        for (unsigned byte = 8; byte < 64; ++byte) {
            expect(destination[record * 64 + byte] == source[record * 64 + byte],
                   "ID copy changed a payload/reserved byte");
        }
    }
    fades::copy_records_with_ids(nullptr, nullptr, 0, 0);
    std::cout << "PASS: constructor rollback, RAII, atomic slots, RX loans/lifetime, queue errors and record copy\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
