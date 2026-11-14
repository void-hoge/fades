/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef FADES_IOCTL_H
#define FADES_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FADES_DMA_ABI_VERSION 7              ///< ドライバーとユーザー側で照合する ABI バージョン。
#define FADES_DMA_MAX_BUFFERS 4096U          ///< 各方向に設定できるバッファ個数の上限。
#define FADES_DMA_SLOT_BYTES (1024U * 1024U) ///< 1 面のデータ領域と mmap のサイズ（バイト）。
#define FADES_RECORDS_PER_SLOT                                                                     \
    (FADES_DMA_SLOT_BYTES / 64U) ///< 1 面に格納できる64バイトレコード数。

/**
 * @brief ドライバーの ABI とバッファ構成を返す情報。
 * @details DMA_INFO の出力。open 直後は両方向とも未設定で、CONFIG 成功後に個数が反映される。
 * ユーザー側は abi_version と slot_bytes を確認してからバッファを操作する。
 */
struct fades_dma_info {
    __u32 abi_version; ///< 出力: ドライバーが提供する ABI バージョン。
    __u32 slot_bytes;  ///< 出力: 各バッファのサイズ（バイト）。
    __u32 max_buffers; ///< 出力: キューごとの個数上限。この値以下でも確保に失敗する場合がある。
    __u32 tx_buffers;  ///< 出力: 設定済み TX バッファ数。CONFIG 成功前は 0。
    __u32 rx_buffers;  ///< 出力: 設定済み RX バッファ数。CONFIG 成功前は 0。
    __u32 reserved;    ///< 出力: 予約領域。ドライバーは 0 を返す。
};

/**
 * @brief TX/RX のバッファ個数を指定する設定要求。
 * @details DMA_CONFIG の入力。各面を独立に確保し、初期所有権は全て CPU 側になる。
 * open ごとに成功は 1 回のみ。確保失敗時はロールバックし、同じ fd で再試行できる。
 */
struct fades_dma_config {
    __u32 tx_buffers;  ///< 入力: 確保する TX バッファ数（1..FADES_DMA_MAX_BUFFERS）。
    __u32 rx_buffers;  ///< 入力: 確保する RX バッファ数（1..FADES_DMA_MAX_BUFFERS）。
    __u32 reserved[2]; ///< 入力: 予約領域。全要素を 0 にする。
};

/**
 * @brief 指定バッファのユーザー空間マッピング情報。
 * @details QUERYBUF では queue/index を入力し、mmap_offset/map_bytes を受け取る。
 * オフセットは物理/DMA アドレスではない。その位置から正確に map_bytes 分を
 * MAP_SHARED で mmap する。問い合わせ自体では所有権は変わらない。
 */
struct fades_buffer_info {
    __u32 queue;               ///< 入力: enum fades_queue の値で TX/RX を選択する。
    __u32 index;               ///< 入力: 指定方向のバッファ番号（0..設定済み個数-1）。
    __aligned_u64 mmap_offset; ///< 出力: mmap に渡す不透明なバイトオフセット。
    __aligned_u64 map_bytes;   ///< 出力: mmap する正確なバイト数。MAP_SHARED を使う。
};

enum fades_queue {
    FADES_QUEUE_TX = 0, ///< ユーザーが入力を書き、DMA が読む送信側。完了通知は POLLOUT。
    FADES_QUEUE_RX = 1, ///< DMA が結果を書き、ユーザーが回収する受信側。完了通知は POLLIN。
};

/**
 * @brief QBUF/DQBUF で受け渡すバッファ番号と有効件数。
 * @details queue で独立した TX/RX キューを選択する。QBUF は指定面を DMA 側へ渡し、
 * DQBUF は完了面の番号・実件数を返して CPU 所有へ戻す。TX 完了は求解完了とは独立する。
 */
struct fades_buffer {
    __u32 queue;    ///< 入力: QBUF/DQBUF の対象方向。enum fades_queue の値を指定する。
    __u32 index;    ///< QBUF 入力: 投入する面の番号。DQBUF 出力: 回収した面の番号。
    __u32 records;  ///< QBUF 入力: TX は1..16384件、RX は 0。DQBUF 出力: 実際の転送件数。
    __u32 reserved; ///< 入力: 予約領域。必ず 0 にする。
};

/**
 * @brief 求解器のセッション累積統計のスナップショット。
 * @details SOLVER_STATS の出力。コアと DMA の各ドメインは異なる時点でサンプリングされる
 * ため、各段の総件数が一致するのは全件排出後のみ。コア別配列の添字は HW コア番号を表す。
 */
struct fades_solver_stats {
    __aligned_u64 accepted;                   ///< HW の入力受理件数。
    __aligned_u64 emitted;                    ///< HW の出力件数。
    __aligned_u64 input_stall_cycles;         ///< 入力ストールの DMA クロックサイクル数。
    __aligned_u64 dispatched;                 ///< コアへの振り分け件数。
    __aligned_u64 collected;                  ///< コアからの回収件数。
    __aligned_u64 queue_empty_cycles;         ///< 入力キューが空のコアクロックサイクル数。
    __aligned_u64 core_dispatched[6];         ///< コア別の振り分け件数。添字は HW コア番号。
    __aligned_u64 core_collected[6];          ///< コア別の回収件数。
    __aligned_u64 core_busy_cycles[6];        ///< コア別の稼働サイクル数。
    __aligned_u64 core_output_wait_cycles[6]; ///< コア別の結果出力待ちサイクル数。
    __aligned_u64 queue_peak;        ///< 入力キュー書き込み側で観測した最大占有レコード数。
    __aligned_u64 processing_cycles; ///< 初回振り分けから取得時までのコアクロックサイクル数。
    __aligned_u64
        core_input_wait_cycles[6]; ///< コア別の有効かつアイドルかつ処理中要求なしのサイクル数。
    __aligned_u64 core_max_inflight[6]; ///< コア別の最大処理中要求数。
};

// review: FADESIOC_* にしたほうがいい気がする
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
 * poll の POLLOUT/POLLIN は、それぞれ TX/RX の完了バッファがあることを示す。
 * STREAMOFF 成功後は待機方向にかかわらず POLLHUP、異常時は POLLERR を返す。 */
#define FADES_DQBUF _IOWR('F', 4, struct fades_buffer)

// review: 6 が欠番ってのは過去の開発の名残なので、今や 6 があっても良い
/* 開始・停止。入力終了通知は不要で、番号 6 は欠番として再利用しない。 */
/* 呼び出し前にキューへ投入したバッファも含め、DMA と求解を開始する。 */
#define FADES_STREAMON _IO('F', 5)
/* 転送を停止・リセットし、成功時に全バッファを CPU 所有に戻す。
 * バッファ設定とマッピングは維持する。成功後は次の STREAMON 成功まで poll に
 * POLLHUP を返す。再開前に全待機スレッドが停止を確認し終了するよう同期すること。 */
#define FADES_STREAMOFF _IO('F', 7)

/* 統計 */
/* 求解器のセッション累積カウンターを取得する。上記 fades_solver_stats を参照。 */
#define FADES_SOLVER_STATS _IOR('F', 2, struct fades_solver_stats)

#endif
