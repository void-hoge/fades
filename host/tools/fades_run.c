/* SPDX-License-Identifier: MIT
 * Raw 64-byte records; see host/README.md. Finite mode queues all input,
 * collects completion metadata without reading output, then writes results.
 * Timed mode continuously copies input/updates IDs and counts results only.
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

struct app {
    int fd;
    unsigned counts[2];
    uint8_t **maps[2];
};

static void fail(const char *what) {
    perror(what);
    exit(EXIT_FAILURE); /* Closing fd + VMAs stops DMA and releases buffers. */
}

static void control(int fd, unsigned long cmd) {
    if (ioctl(fd, cmd) < 0) {
        fail("control ioctl");
    }
}

static unsigned number(const char *s) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || !n || n > FADES_DMA_MAX_BUFFERS) {
        fprintf(stderr, "Buffer count must be 1..%u\n", FADES_DMA_MAX_BUFFERS);
        exit(EXIT_FAILURE);
    }
    return (unsigned)n;
}

static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        fail("clock_gettime");
    }
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void setup(struct app *a) {
    a->fd = open("/dev/fades", O_RDWR | O_CLOEXEC);
    if (a->fd < 0) {
        fail("open /dev/fades");
    }
    struct fades_dma_info info = {0};
    if (ioctl(a->fd, FADES_DMA_INFO, &info) < 0) {
        fail("DMA_INFO");
    }
    if (info.abi_version != FADES_DMA_ABI_VERSION || info.slot_bytes != FADES_DMA_SLOT_BYTES) {
        fprintf(stderr, "Incompatible FADES ABI\n");
        exit(EXIT_FAILURE);
    }
    struct fades_dma_config cfg = {
        .tx_buffers = a->counts[0],
        .rx_buffers = a->counts[1],
    };
    if (ioctl(a->fd, FADES_DMA_CONFIG, &cfg) < 0) {
        fail("DMA_CONFIG");
    }
    for (unsigned q = 0; q < 2; q++) {
        a->maps[q] = calloc(a->counts[q], sizeof(*a->maps[q]));
        if (!a->maps[q]) {
            fail("calloc");
        }
        for (unsigned i = 0; i < a->counts[q]; i++) {
            struct fades_buffer_info b = {.queue = q, .index = i};
            if (ioctl(a->fd, FADES_QUERYBUF, &b) < 0) {
                fail("QUERYBUF");
            }
            a->maps[q][i] = mmap(NULL, b.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd,
                                 (off_t)b.mmap_offset);
            if (a->maps[q][i] == MAP_FAILED) {
                fail("mmap");
            }
        }
    }
}

static void queue(struct app *a, unsigned q, unsigned i, unsigned n) {
    struct fades_buffer b = {.queue = q, .index = i, .records = n};
    if (ioctl(a->fd, FADES_QBUF, &b) < 0) {
        fail("QBUF");
    }
}

static int dequeue(struct app *a, unsigned q, struct fades_buffer *b) {
    *b = (struct fades_buffer){.queue = q};
    for (;;) {
        if (!ioctl(a->fd, FADES_DQBUF, b)) {
            return 1;
        }
        if (errno == EAGAIN) {
            return 0;
        }
        if (errno != EINTR) {
            fail("DQBUF");
        }
    }
}

static void wait_ready(struct app *a, int input) {
    struct pollfd p = {.fd = a->fd, .events = POLLIN | (input ? POLLOUT : 0)};
    int ret;
    do {
        ret = poll(&p, 1, 10000);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        fail("poll");
    }
    if (!ret || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        fprintf(stderr, "DMA timeout/error: revents=%x\n", p.revents);
        exit(EXIT_FAILURE);
    }
}

/* Streaming benchmark helper. Copies each complete record once, replacing
 * the ID before its stores; avoids a second strided pass over uncached TX. */
static void copy_records(uint8_t *dst, const uint8_t *src, unsigned count, uint64_t first) {
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *s = src + (size_t)i * 64;
        if (i + 8 < count) {
            __builtin_prefetch(s + 512, 0, 3);
        }
#ifdef __aarch64__
        uint8x16_t v0 = vld1q_u8(s);
        uint8x16_t v1 = vld1q_u8(s + 16);
        uint8x16_t v2 = vld1q_u8(s + 32);
        uint8x16_t v3 = vld1q_u8(s + 48);
        v0 = vreinterpretq_u8_u64(vsetq_lane_u64(first + i, vreinterpretq_u64_u8(v0), 0));
        vst1q_u8(dst, v0);
        vst1q_u8(dst + 16, v1);
        vst1q_u8(dst + 32, v2);
        vst1q_u8(dst + 48, v3);
#else
        uint8_t record[64];
        memcpy(record, s, 64);
        for (unsigned j = 0; j < 8; j++) {
            record[j] = (uint8_t)((first + i) >> (8 * j));
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
        unsigned n = records - *position < remaining ? (unsigned)(records - *position) : remaining;
        copy_records(dst, src + *position * 64, n, first);
        dst += (size_t)n * 64;
        first += n;
        remaining -= n;
        *position += n;
        if (*position == records) {
            *position = 0;
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) {
        fprintf(stderr,
                "Usage: %s INPUT.raw OUTPUT.raw TX_BUFFERS RX_BUFFERS\n"
                "       %s INPUT.raw - TX_BUFFERS RX_BUFFERS SECONDS\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    double seconds = argc == 6 ? strtod(argv[5], &end) : 0;
    if (argc == 6 &&
        (!*argv[5] || *end || !(seconds > 0 && seconds <= 3600) || strcmp(argv[2], "-"))) {
        fprintf(stderr, "Timed mode requires OUTPUT=- and 0 < SECONDS <= 3600\n");
        return EXIT_FAILURE;
    }
    struct app a = {.fd = -1, .counts = {number(argv[3]), number(argv[4])}};
    FILE *in = fopen(argv[1], "rb");
    struct stat st;
    if (!in || fstat(fileno(in), &st)) {
        fail("input");
    }
    if (st.st_size <= 0 || st.st_size % 64 || (uint64_t)st.st_size > SIZE_MAX) {
        fprintf(stderr, "Input must contain complete 64-byte records\n");
        return EXIT_FAILURE;
    }
    size_t bytes = (size_t)st.st_size, records = bytes / 64;
    if (!seconds && ((uint64_t)bytes > (uint64_t)a.counts[0] * FADES_DMA_SLOT_BYTES ||
                     (uint64_t)bytes > (uint64_t)a.counts[1] * FADES_DMA_SLOT_BYTES)) {
        fprintf(stderr, "Finite mode requires enough TX and RX capacity for all input\n");
        return EXIT_FAILURE;
    }
    uint8_t *source = malloc(bytes);
    if (!source) {
        fail("malloc input");
    }
    if (fread(source, 1, bytes, in) != bytes) {
        fail("read input");
    }
    fclose(in);
    setup(&a);
    unsigned *valid = calloc(a.counts[1], sizeof(*valid));
    unsigned *order = calloc(a.counts[1], sizeof(*order));
    if (!valid || !order) {
        fail("calloc completions");
    }
    for (unsigned i = 0; i < a.counts[1]; i++) {
        queue(&a, FADES_QUEUE_RX, i, 0);
    }

    uint64_t submitted = 0, received = 0;
    size_t position = 0;
    unsigned completed = 0;
    double begin = now(); /* Timed mode includes initial input copies. */
    for (unsigned i = 0; i < a.counts[0]; i++) {
        unsigned n;
        if (seconds) {
            n = FADES_RECORDS_PER_SLOT;
            fill(a.maps[0][i], source, records, &position, submitted);
        } else {
            if (submitted == records) {
                break;
            }
            n = records - submitted > FADES_RECORDS_PER_SLOT ? FADES_RECORDS_PER_SLOT
                                                             : (unsigned)(records - submitted);
            memcpy(a.maps[0][i], source + submitted * 64, n * 64);
        }
        queue(&a, FADES_QUEUE_TX, i, n);
        submitted += n;
    }
    control(a.fd, FADES_STREAMON);
    int feeding = seconds > 0;
    if (!feeding) {
        control(a.fd, FADES_END_INPUT);
    }
    while (feeding || received != submitted) {
        if (feeding && now() - begin >= seconds) {
            control(a.fd, FADES_END_INPUT);
            feeding = 0;
        }
        struct fades_buffer b;
        while (dequeue(&a, FADES_QUEUE_RX, &b)) {
            received += b.records;
            if (seconds) {
                queue(&a, FADES_QUEUE_RX, b.index, 0);
            } else {
                valid[b.index] = b.records;
                order[completed++] = b.index;
            }
        }
        if (feeding) {
            while (dequeue(&a, FADES_QUEUE_TX, &b)) {
                if (now() - begin >= seconds) {
                    control(a.fd, FADES_END_INPUT);
                    feeding = 0;
                    break;
                }
                fill(a.maps[0][b.index], source, records, &position, submitted);
                queue(&a, FADES_QUEUE_TX, b.index, FADES_RECORDS_PER_SLOT);
                submitted += FADES_RECORDS_PER_SLOT;
            }
        }
        if (feeding || received != submitted) {
            wait_ready(&a, feeding);
        }
    }
    double elapsed = now() - begin;
    struct fades_solver_stats stats;
    if (ioctl(a.fd, FADES_SOLVER_STATS, &stats) < 0) {
        fail("SOLVER_STATS");
    }
    for (unsigned i = 0; i < 6; i++) {
        if ((i == 0 || i == 1 || i == 3 || i == 4) && stats.counters[i] != submitted) {
            fprintf(stderr, "Stage %u mismatch: %llu vs %" PRIu64 "\n", i,
                    (unsigned long long)stats.counters[i], submitted);
            return EXIT_FAILURE;
        }
    }
    control(a.fd, FADES_STREAMOFF);
    printf("%s: TX/RX=%u/%u, %" PRIu64 " records, %.6f s, %.6f Mproblem/s\n",
           seconds ? "Stream (no output read)" : "Finite (output read deferred)", a.counts[0],
           a.counts[1], received, elapsed, received / elapsed / 1e6);
    if (strcmp(argv[2], "-")) {
        FILE *out = fopen(argv[2], "wb");
        if (!out) {
            fail("output");
        }
        for (unsigned i = 0; i < completed; i++) {
            unsigned index = order[i];
            size_t n = (size_t)valid[index] * 64;
            if (fwrite(a.maps[1][index], 1, n, out) != n) {
                fail("write output");
            }
        }
        if (fclose(out)) {
            fail("close output");
        }
    }
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned i = 0; i < a.counts[q]; i++) {
            if (munmap(a.maps[q][i], FADES_DMA_SLOT_BYTES)) {
                fail("munmap");
            }
        }
        free(a.maps[q]);
    }
    close(a.fd);
    free(source);
    free(valid);
    free(order);
    return EXIT_SUCCESS;
}
