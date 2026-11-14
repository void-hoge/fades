/* SPDX-License-Identifier: MIT
 * Processes raw 64-byte records; see host/README.md.
 * Copies input and updates IDs for the specified duration, counting results without reading them.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include "fades_ioctl.h"

/**
 * @brief Device fd and per-direction buffer mappings used by the tool.
 * @details setup() allocates and maps the requested buffers, indexed by queue direction
 * and buffer number. At shutdown, STREAMOFF is followed by releasing all mappings and
 * arrays and closing the fd.
 */
struct fades_device {
    int fd;
    unsigned buffer_counts[2];
    uint8_t **mapped_buffers[2];
};

static void fail(const char *what) {
    perror(what);
    exit(EXIT_FAILURE); /* Releasing the fd and VMAs stops DMA and frees the buffers. */
}

static void control(int fd, unsigned long cmd) {
    if (ioctl(fd, cmd) < 0) {
        fail("control ioctl");
    }
}

static unsigned number(const char *text) {
    char *end;
    errno = 0;
    unsigned long count = strtoul(text, &end, 10);
    if (errno || !*text || *end || !count || count > FADES_DMA_MAX_BUFFERS) {
        fprintf(stderr, "Buffer count must be 1..%u\n", FADES_DMA_MAX_BUFFERS);
        exit(EXIT_FAILURE);
    }
    return (unsigned)count;
}

static double now(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp)) {
        fail("clock_gettime");
    }
    return (double)timestamp.tv_sec + (double)timestamp.tv_nsec * 1e-9;
}

static void setup(struct fades_device *device) {
    device->fd = open("/dev/fades", O_RDWR | O_CLOEXEC);
    if (device->fd < 0) {
        fail("open /dev/fades");
    }
    struct fades_dma_info info = {0};
    if (ioctl(device->fd, FADESIOC_DMA_INFO, &info) < 0) {
        fail("DMA_INFO");
    }
    if (info.abi_version != FADES_DMA_ABI_VERSION || info.slot_bytes != FADES_DMA_SLOT_BYTES) {
        fprintf(stderr, "Incompatible FADES ABI\n");
        exit(EXIT_FAILURE);
    }
    struct fades_dma_config cfg = {
        .tx_buffers = device->buffer_counts[FADES_QUEUE_TX],
        .rx_buffers = device->buffer_counts[FADES_QUEUE_RX],
    };
    if (ioctl(device->fd, FADESIOC_DMA_CONFIG, &cfg) < 0) {
        fail("DMA_CONFIG");
    }
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        device->mapped_buffers[direction] =
            calloc(device->buffer_counts[direction], sizeof(*device->mapped_buffers[direction]));
        if (!device->mapped_buffers[direction]) {
            fail("calloc");
        }
        for (unsigned index = 0; index < device->buffer_counts[direction]; index++) {
            struct fades_buffer_info buffer = {.queue = direction, .index = index};
            if (ioctl(device->fd, FADESIOC_QUERYBUF, &buffer) < 0) {
                fail("QUERYBUF");
            }
            device->mapped_buffers[direction][index] =
                mmap(NULL, buffer.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, device->fd,
                     (off_t)buffer.mmap_offset);
            if (device->mapped_buffers[direction][index] == MAP_FAILED) {
                fail("mmap");
            }
        }
    }
}

static void queue(struct fades_device *device, enum fades_queue direction, unsigned index,
                  unsigned records) {
    struct fades_buffer buffer = {.queue = direction, .index = index, .records = records};
    if (ioctl(device->fd, FADESIOC_QBUF, &buffer) < 0) {
        fail("QBUF");
    }
}

static int try_dequeue(struct fades_device *device, enum fades_queue direction,
                       struct fades_buffer *buffer) {
    *buffer = (struct fades_buffer){.queue = direction};
    if (ioctl(device->fd, FADESIOC_DQBUF, buffer) == 0) {
        return 1;
    }
    if (errno == EAGAIN || errno == EINTR) {
        return 0;
    }
    fail("DQBUF");
    return 0;
}

static void wait_ready(struct fades_device *device, short events) {
    struct pollfd descriptor = {.fd = device->fd, .events = events};
    int ret;
    do {
        ret = poll(&descriptor, 1, 10000);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        fail("poll");
    }
    if (!ret || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        fprintf(stderr, "DMA timeout/error: revents=%x\n", descriptor.revents);
        exit(EXIT_FAILURE);
    }
}

/* For streaming benchmarks: copy each record once, replacing its ID before writing.
 * This avoids rescanning the uncached TX region with strided accesses to update IDs. */
static void copy_records(uint8_t *dst, const uint8_t *src, unsigned count, uint64_t first) {
    for (unsigned index = 0; index < count; index++) {
        const uint8_t *source_record = src + (size_t)index * 64;
        if (index + 8 < count) {
            __builtin_prefetch(source_record + 512, 0, 3);
        }
#ifdef __aarch64__
        uint8x16_t v0 = vld1q_u8(source_record);
        uint8x16_t v1 = vld1q_u8(source_record + 16);
        uint8x16_t v2 = vld1q_u8(source_record + 32);
        uint8x16_t v3 = vld1q_u8(source_record + 48);
        v0 = vreinterpretq_u8_u64(vsetq_lane_u64(first + index, vreinterpretq_u64_u8(v0), 0));
        vst1q_u8(dst, v0);
        vst1q_u8(dst + 16, v1);
        vst1q_u8(dst + 32, v2);
        vst1q_u8(dst + 48, v3);
#else
        uint8_t record[64];
        memcpy(record, source_record, 64);
        for (unsigned byte = 0; byte < 8; byte++) {
            record[byte] = (uint8_t)((first + index) >> (8 * byte));
        }
        memcpy(dst, record, 64);
#endif
        dst += 64;
    }
}

static void fill(uint8_t *dst, const uint8_t *src, size_t records, size_t *position,
                 uint64_t first) {
    unsigned remaining = FADES_RECORDS_PER_SLOT;
    while (remaining) {
        unsigned count =
            records - *position < remaining ? (unsigned)(records - *position) : remaining;
        copy_records(dst, src + *position * 64, count, first);
        dst += (size_t)count * 64;
        first += count;
        remaining -= count;
        *position += count;
        if (*position == records) {
            *position = 0;
        }
    }
}

static uint8_t *load_input(const char *path, size_t *records) {
    FILE *in = fopen(path, "rb");
    struct stat st;
    if (!in || fstat(fileno(in), &st)) {
        fail("input");
    }
    if (st.st_size <= 0 || st.st_size % 64 || (uint64_t)st.st_size > SIZE_MAX) {
        fprintf(stderr, "Input must contain complete 64-byte records\n");
        exit(EXIT_FAILURE);
    }
    size_t bytes = (size_t)st.st_size;
    uint8_t *source = malloc(bytes);
    if (!source) {
        fail("malloc input");
    }
    if (fread(source, 1, bytes, in) != bytes) {
        fail("read input");
    }
    if (fclose(in)) {
        fail("close input");
    }
    *records = bytes / 64;
    return source;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s INPUT.raw TX_BUFFERS RX_BUFFERS SECONDS\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    double seconds = strtod(argv[4], &end);
    if (!*argv[4] || *end || !(seconds > 0 && seconds <= 3600)) {
        fprintf(stderr, "Duration must satisfy 0 < SECONDS <= 3600\n");
        return EXIT_FAILURE;
    }
    size_t records;
    uint8_t *source = load_input(argv[1], &records);

    struct fades_device device = {
        .fd = -1,
        .buffer_counts = {[FADES_QUEUE_TX] = number(argv[2]), [FADES_QUEUE_RX] = number(argv[3])},
    };
    setup(&device);

    // Enqueue all output buffers.
    for (unsigned index = 0; index < device.buffer_counts[FADES_QUEUE_RX]; index++) {
        queue(&device, FADES_QUEUE_RX, index, 0);
    }

    uint64_t submitted = 0;
    uint64_t received = 0;
    size_t position = 0;
    // Fill and enqueue all initial input buffers.
    for (unsigned index = 0; index < device.buffer_counts[FADES_QUEUE_TX]; index++) {
        fill(device.mapped_buffers[FADES_QUEUE_TX][index], source, records, &position, submitted);
        queue(&device, FADES_QUEUE_TX, index, FADES_RECORDS_PER_SLOT);
        submitted += FADES_RECORDS_PER_SLOT;
    }
    // Start timing and streaming.
    double begin = now();
    double deadline = begin + seconds;
    control(device.fd, FADESIOC_STREAMON);
    int feeding = 1;
    while (feeding || received != submitted) {
        // After the timed interval, wait only for output.
        wait_ready(&device, POLLIN | (feeding ? POLLOUT : 0));
        if (feeding && now() >= deadline) {
            feeding = 0;
        }
        struct fades_buffer buffer;
        // During the timed interval, dequeue, fill and enqueue input buffers.
        if (feeding && try_dequeue(&device, FADES_QUEUE_TX, &buffer)) {
            fill(device.mapped_buffers[FADES_QUEUE_TX][buffer.index], source, records, &position,
                 submitted);
            queue(&device, FADES_QUEUE_TX, buffer.index, FADES_RECORDS_PER_SLOT);
            submitted += FADES_RECORDS_PER_SLOT;
        }
        if (try_dequeue(&device, FADES_QUEUE_RX, &buffer)) {
            received += buffer.records;
            queue(&device, FADES_QUEUE_RX, buffer.index, 0);
        }
    }
    // Stop timing.
    double elapsed = now() - begin;
    // Check statistics.
    struct fades_solver_stats stats;
    if (ioctl(device.fd, FADESIOC_SOLVER_STATS, &stats) < 0) {
        fail("SOLVER_STATS");
    }
    if (stats.accepted != submitted || stats.emitted != submitted ||
        stats.dispatched != submitted || stats.collected != submitted) {
        fprintf(stderr,
                "Stage totals mismatch: accepted=%llu emitted=%llu dispatched=%llu "
                "collected=%llu expected=%" PRIu64 "\n",
                (unsigned long long)stats.accepted, (unsigned long long)stats.emitted,
                (unsigned long long)stats.dispatched, (unsigned long long)stats.collected,
                submitted);
        return EXIT_FAILURE;
    }
    // Stop the device.
    control(device.fd, FADESIOC_STREAMOFF);
    printf("Stream (no output read): TX/RX=%u/%u, %" PRIu64 " records, %.6f s, %.6f Mproblem/s\n",
           device.buffer_counts[FADES_QUEUE_TX], device.buffer_counts[FADES_QUEUE_RX], received,
           elapsed, received / elapsed / 1e6);
    // Clean up.
    for (enum fades_queue direction = FADES_QUEUE_TX; direction <= FADES_QUEUE_RX; direction++) {
        for (unsigned index = 0; index < device.buffer_counts[direction]; index++) {
            if (munmap(device.mapped_buffers[direction][index], FADES_DMA_SLOT_BYTES)) {
                fail("munmap");
            }
        }
        free(device.mapped_buffers[direction]);
    }
    close(device.fd);
    free(source);
    return EXIT_SUCCESS;
}
