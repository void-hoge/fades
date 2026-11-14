/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef FADES_IOCTL_H
#define FADES_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FADES_DMA_ABI_VERSION 7
#define FADES_DMA_MAX_BUFFERS 4096U
#define FADES_DMA_SLOT_BYTES (1024U * 1024U)
#define FADES_RECORDS_PER_SLOT (FADES_DMA_SLOT_BYTES / 64U)

struct fades_dma_info {
    __u32 abi_version;
    __u32 slot_bytes;
    __u32 max_buffers; /* キューごとの上限。この値以下でも確保に失敗する場合がある。 */
    __u32 tx_buffers;  /* CONFIG が成功するまでは 0。 */
    __u32 rx_buffers;
    __u32 reserved;
};

/* open ごとに設定成功は 1 回のみ。失敗時はロールバックし、再試行できる。 */
struct fades_dma_config {
    __u32 tx_buffers;
    __u32 rx_buffers;
    __u32 reserved[2];
};

/* queue/index を入力する。オフセットは不透明な値で、物理/DMA アドレスではない。
 * mmap_offset の位置から、正確に map_bytes 分を MAP_SHARED で mmap すること。 */
struct fades_buffer_info {
    __u32 queue;
    __u32 index;
    __aligned_u64 mmap_offset;
    __aligned_u64 map_bytes;
};

enum fades_queue { FADES_QUEUE_TX = 0, FADES_QUEUE_RX = 1 };

struct fades_buffer {
    __u32 queue;
    __u32 index;
    __u32 records;  /* QBUF: TX は入力件数、RX は 0。DQBUF: 実際の転送件数。 */
    __u32 reserved; /* 必ず 0 を指定すること。 */
};

/* セッション累積値のスナップショット。コアと DMA の各ドメインは異なる時点で
 * サンプリングされるため、各段の総件数が一致するのは全件排出後のみ。 */
struct fades_solver_stats {
    __aligned_u64 accepted;                   /* 入力受理件数。 */
    __aligned_u64 emitted;                    /* 出力件数。 */
    __aligned_u64 input_stall_cycles;         /* 入力ストールのサイクル数。 */
    __aligned_u64 dispatched;                 /* コアへの振り分け件数。 */
    __aligned_u64 collected;                  /* コアからの回収件数。 */
    __aligned_u64 queue_empty_cycles;         /* 入力キューが空のサイクル数。 */
    __aligned_u64 core_dispatched[6];         /* コア別の振り分け件数。 */
    __aligned_u64 core_collected[6];          /* コア別の回収件数。 */
    __aligned_u64 core_busy_cycles[6];        /* コア別の稼働サイクル数。 */
    __aligned_u64 core_output_wait_cycles[6]; /* コア別の出力待ちサイクル数。 */
    __aligned_u64 queue_peak;                 /* 入力キュー書き込み側の最大占有数。 */
    __aligned_u64 processing_cycles;          /* 初回振り分けから取得時までのサイクル数。 */
    __aligned_u64
        core_input_wait_cycles[6]; /* コア別の有効かつアイドルかつ処理中要求なしのサイクル数。 */
    __aligned_u64 core_max_inflight[6]; /* コア別の最大処理中要求数。 */
};

/* 設定・マッピング */
/* ABI バージョン、バッファサイズ・個数上限、設定済み TX/RX バッファ数を取得する。 */
#define FADES_DMA_INFO _IOR('F', 0, struct fades_dma_info)
/* TX/RX の独立したバッファプールを確保する。open ごとに成功は 1 回のみ。
 * 確保失敗時はロールバックし、設定を再試行できる。初期所有権は全て CPU 側。 */
#define FADES_DMA_CONFIG _IOW('F', 8, struct fades_dma_config)
/* 指定バッファの不透明な mmap オフセットと必要なマッピングサイズを取得する。 */
#define FADES_QUERYBUF _IOWR('F', 9, struct fades_buffer_info)

/* 投入・回収 */
/* CPU 所有のバッファを DMA に渡す。TX は records=1..16384、RX は records=0。
 * DQBUF で返却されるか STREAMOFF が成功するまで、バッファにアクセスしないこと。 */
#define FADES_QBUF _IOW('F', 3, struct fades_buffer)
/* 指定キューの完了バッファをノンブロッキングで回収する。完了なしは EAGAIN。
 * index と実際の records を返し、所有権を CPU に戻す。
 * TX 完了はバッファの再利用が可能という意味であり、求解完了を意味しない。
 * poll の POLLOUT/POLLIN は、それぞれ TX/RX の完了バッファがあることを示す。 */
#define FADES_DQBUF _IOWR('F', 4, struct fades_buffer)

/* 開始・停止。入力終了通知は不要で、番号 6 は欠番として再利用しない。 */
/* 呼び出し前にキューへ投入したバッファも含め、DMA と求解を開始する。 */
#define FADES_STREAMON _IO('F', 5)
/* 転送を停止・リセットし、成功時に全バッファを CPU 所有に戻す。
 * バッファ設定とマッピングは維持する。 */
#define FADES_STREAMOFF _IO('F', 7)

/* 統計 */
/* 求解器のセッション累積カウンターを取得する。上記 fades_solver_stats を参照。 */
#define FADES_SOLVER_STATS _IOR('F', 2, struct fades_solver_stats)

#endif
