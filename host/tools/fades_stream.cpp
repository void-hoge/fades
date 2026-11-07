// Minimal single-thread ioctl example: packed 64-byte input records -> results.
// Results are completion ordered; use request IDs, not file position, to match.
#include "fades_stream.hpp"
#include <fstream>
#include <iostream>

/** Stream packed records between files using explicit ioctls and one poll fd. */
int main(int argc, char** argv) try {
    if (argc != 3 && argc != 4) {
        throw std::runtime_error("Usage: fades_stream INPUT_RECORDS OUTPUT_RECORDS [RECORDS_PER_BUFFER]");
    }
    unsigned records_per_buffer = argc == 4 ? std::stoul(argv[3]) : 10000;
    if (!records_per_buffer || records_per_buffer > 16384) {
        throw std::runtime_error("Invalid records per buffer");
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::ofstream output(argv[2], std::ios::binary);
    if (!input || !output) {
        throw std::runtime_error("Cannot open input/output");
    }
    // Construction opens, validates INFO and mmaps both regions. All streaming
    // operations below are explicit ioctls on the borrowed fd.
    fades::Device device;
    int fd = device.native_handle();

    // Transfer one CPU-owned buffer to the driver; records is zero for RX.
    auto enqueue_buffer = [&](unsigned queue, unsigned index, unsigned records) {
        fades_buffer buffer{queue, index, records, 0};
        if (ioctl(fd, FADES_QBUF, &buffer) != 0) {
            throw std::runtime_error("QBUF: " + std::string(std::strerror(errno)));
        }
    };
    // Reclaim one completion. An empty queue (EAGAIN) is not an error.
    auto dequeue_buffer = [&](unsigned queue, fades_buffer& buffer) {
        buffer = {queue, 0, 0, 0};
        if (ioctl(fd, FADES_DQBUF, &buffer) == 0) {
            return true;
        }
        if (errno == EAGAIN) {
            return false;
        }
        throw std::runtime_error("DQBUF: " + std::string(std::strerror(errno)));
    };
    uint64_t submitted_records = 0;
    uint64_t completed_records = 0;
    bool input_finished = false;

    // Fill a CPU-owned TX buffer from the file, or seal input on EOF.
    auto submit_next = [&](unsigned index) {
        input.read(reinterpret_cast<char*>(device.buffer_data(FADES_QUEUE_TX, index)),
                   size_t(records_per_buffer) * 64);
        auto bytes = input.gcount();
        if (input.bad() || bytes % 64 != 0) {
            throw std::runtime_error("Invalid/truncated input record file");
        }
        if (bytes == 0) {
            if (ioctl(fd, FADES_END_INPUT) != 0) {
                throw std::runtime_error("END_INPUT: " + std::string(std::strerror(errno)));
            }
            input_finished = true;
            return;
        }
        enqueue_buffer(FADES_QUEUE_TX, index, unsigned(bytes / 64));
        submitted_records += bytes / 64;
    };
    for (unsigned index = 0; index < device.info().slots; ++index) {
        enqueue_buffer(FADES_QUEUE_RX, index, 0);
    }
    if (ioctl(fd, FADES_STREAMON) != 0) {
        throw std::runtime_error("STREAMON: " + std::string(std::strerror(errno)));
    }
    for (unsigned index = 0; index < device.info().slots && !input_finished; ++index) {
        submit_next(index);
    }
    while (!input_finished || completed_records != submitted_records) {
        pollfd poll_descriptor{fd, short(POLLIN | (input_finished ? 0 : POLLOUT)), 0};
        int poll_result = poll(&poll_descriptor, 1, 60000);
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result <= 0 || (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            throw std::runtime_error("poll error/timeout");
        }
        fades_buffer buffer{};
        while (dequeue_buffer(FADES_QUEUE_RX, buffer)) {
            output.write(reinterpret_cast<char*>(device.buffer_data(FADES_QUEUE_RX, buffer.index)),
                         size_t(buffer.records) * 64);
            if (!output) {
                throw std::runtime_error("Output write failed");
            }
            completed_records += buffer.records;
            enqueue_buffer(FADES_QUEUE_RX, buffer.index, 0);
        }
        if (!input_finished) {
            while (dequeue_buffer(FADES_QUEUE_TX, buffer)) {
                submit_next(buffer.index);
                if (input_finished) {
                    break;
                }
            }
        }
    }
    if (ioctl(fd, FADES_STREAMOFF) != 0) {
        throw std::runtime_error("STREAMOFF: " + std::string(std::strerror(errno)));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Output close failed");
    }
    std::cout << "Completed " << completed_records << " records\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
