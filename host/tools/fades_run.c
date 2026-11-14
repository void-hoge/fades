/* SPDX-License-Identifier: MIT
 * 64 バイトの生レコードを扱う。host/README.md を参照。
 * 指定時間だけ入力コピーと ID 更新を継続し、結果は件数のみを数える。
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

struct fades_device {
    int fd;
    unsigned buffer_counts[2];
    uint8_t **mapped_buffers[2];
};

static void fail(const char *what) {
    perror(what);
    exit(EXIT_FAILURE); /* fd と VMA の解放により DMA が停止し、バッファも解放される。 */
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

static void setup(struct fades_device *a) {
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
        .tx_buffers = a->buffer_counts[FADES_QUEUE_TX],
        .rx_buffers = a->buffer_counts[FADES_QUEUE_RX],
    };
    if (ioctl(a->fd, FADES_DMA_CONFIG, &cfg) < 0) {
        fail("DMA_CONFIG");
    }
    for (enum fades_queue q = FADES_QUEUE_TX; q <= FADES_QUEUE_RX; q++) {
        a->mapped_buffers[q] = calloc(a->buffer_counts[q], sizeof(*a->mapped_buffers[q]));
        if (!a->mapped_buffers[q]) {
            fail("calloc");
        }
        for (unsigned i = 0; i < a->buffer_counts[q]; i++) {
            struct fades_buffer_info b = {.queue = q, .index = i};
            if (ioctl(a->fd, FADES_QUERYBUF, &b) < 0) {
                fail("QUERYBUF");
            }
            a->mapped_buffers[q][i] = mmap(NULL, b.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                                           a->fd, (off_t)b.mmap_offset);
            if (a->mapped_buffers[q][i] == MAP_FAILED) {
                fail("mmap");
            }
        }
    }
}

static void queue(struct fades_device *a, enum fades_queue q, unsigned i, unsigned n) {
    struct fades_buffer b = {.queue = q, .index = i, .records = n};
    if (ioctl(a->fd, FADES_QBUF, &b) < 0) {
        fail("QBUF");
    }
}

static int try_dequeue(struct fades_device *a, enum fades_queue q, struct fades_buffer *b) {
    *b = (struct fades_buffer){.queue = q};
    if (ioctl(a->fd, FADES_DQBUF, b) == 0) {
        return 1;
    }
    if (errno == EAGAIN || errno == EINTR) {
        return 0;
    }
    fail("DQBUF");
    return 0;
}

static void wait_ready(struct fades_device *a, short events) {
    struct pollfd p = {.fd = a->fd, .events = events};
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

/* ストリーミング計測用。各レコードを 1 回だけコピーし、書き込み前に ID を差し替える。
 * キャッシュされない TX 領域を、ID 更新のためにストライドアクセスで再走査せずに済む。 */
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
    struct fades_device a = {
        .fd = -1,
        .buffer_counts = {[FADES_QUEUE_TX] = number(argv[2]), [FADES_QUEUE_RX] = number(argv[3])},
    };
    size_t records;
    uint8_t *source = load_input(argv[1], &records);
    setup(&a);

    // 出力バッファを全てエンキュー
    for (unsigned i = 0; i < a.buffer_counts[FADES_QUEUE_RX]; i++) {
        queue(&a, FADES_QUEUE_RX, i, 0);
    }

    uint64_t submitted = 0, received = 0;
    size_t position = 0;
    // 初回の問題を埋めて全てエンキュー
    for (unsigned i = 0; i < a.buffer_counts[FADES_QUEUE_TX]; i++) {
        fill(a.mapped_buffers[FADES_QUEUE_TX][i], source, records, &position, submitted);
        queue(&a, FADES_QUEUE_TX, i, FADES_RECORDS_PER_SLOT);
        submitted += FADES_RECORDS_PER_SLOT;
    }
    // 計測開始して STREAMON
    double begin = now();
    double deadline = begin + seconds;
    control(a.fd, FADES_STREAMON);
    int feeding = 1;
    while (feeding || received != submitted) {
        // 計測時間が終わったら、出力だけ待つ
        wait_ready(&a, POLLIN | (feeding ? POLLOUT : 0));
        if (feeding && now() >= deadline) {
            feeding = 0;
        }
        struct fades_buffer b;
        // 計測時間内ならば、入力バッファを取り出して埋めてエンキュー
        if (feeding && try_dequeue(&a, FADES_QUEUE_TX, &b)) {
            fill(a.mapped_buffers[FADES_QUEUE_TX][b.index], source, records, &position, submitted);
            queue(&a, FADES_QUEUE_TX, b.index, FADES_RECORDS_PER_SLOT);
            submitted += FADES_RECORDS_PER_SLOT;
        }
        if (try_dequeue(&a, FADES_QUEUE_RX, &b)) {
            received += b.records;
            queue(&a, FADES_QUEUE_RX, b.index, 0);
        }
    }
    // 計測終了
    double elapsed = now() - begin;
    // 集計
    struct fades_solver_stats stats;
    if (ioctl(a.fd, FADES_SOLVER_STATS, &stats) < 0) {
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
    // デバイスの停止
    control(a.fd, FADES_STREAMOFF);
    printf("Stream (no output read): TX/RX=%u/%u, %" PRIu64 " records, %.6f s, %.6f Mproblem/s\n",
           a.buffer_counts[FADES_QUEUE_TX], a.buffer_counts[FADES_QUEUE_RX], received, elapsed,
           received / elapsed / 1e6);
    // 片付け
    for (enum fades_queue q = FADES_QUEUE_TX; q <= FADES_QUEUE_RX; q++) {
        for (unsigned i = 0; i < a.buffer_counts[q]; i++) {
            if (munmap(a.mapped_buffers[q][i], FADES_DMA_SLOT_BYTES)) {
                fail("munmap");
            }
        }
        free(a.mapped_buffers[q]);
    }
    close(a.fd);
    free(source);
    return EXIT_SUCCESS;
}
