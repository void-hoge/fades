// Exercise constructor rollback and queue errors without requiring /dev/fades.
#include "fades_stream.hpp"
#include <cstdarg>
#include <iostream>

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
    } else if (command == FADES_DQBUF) {
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
    std::cout << "PASS: constructor rollback, RAII, queue errors and record copy\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
