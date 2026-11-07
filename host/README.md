# PS / Linux ソフトウェア

ABI v2 は V4L2 風の **QBUF / DQBUF と1つの poll fd** で数独を継続処理する。
入力と出力は独立した mmap バッファキュー。1スレッドで使える。

## ビルド

KV260 のリポジトリルートで:

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
ctest --preset kv260-ps
```

必要ツールは CMake 3.25以降、Ninja、C++20、Python 3、build-essential と
実行中カーネルのヘッダー。`-DFADES_KERNEL_BUILD=/path/to/headers` で変更できる。
開発機では `host` プリセットを使う。ドライバーは実機でネイティブビルドする。

## ioctl で書く

公開定義は [`include/fades_ioctl.h`](include/fades_ioctl.h)。
[`tools/fades_stream.cpp`](tools/fades_stream.cpp) は実行可能な1スレッドの ioctl 例。
open/INFO/mmap/終了処理の RAII だけを共通ヘルパーに任せ、QBUF/DQBUF/poll は明示している。

### 1. open・INFO・mmap

`/dev/fades` を O_RDWR で open し、`FADES_DMA_INFO` を取得する。
ABI version は **2**。旧 RUN ABI は廃止した。

|領域|mmap offset|長さ|
|---|---:|---:|
|TX|0|`info.map_bytes`|
|RX|`info.map_bytes`|`info.map_bytes`|

各領域は 1 MiB × 8 slot = 8 MiB。各 slot の先頭は
`mapping + index * info.slot_bytes`。1レコード64 byte、1 slot 最大16,384問。
ここで **slot は循環させるバッファ1面**の意味で、コア内の処理枠ではない。
例えば「TX の index=3」は4番目の入力バッファ、「RX の index=3」は
4番目の結果バッファを指す。両者が同じ問題群を含むという意味ではない。
MAP_SHARED で領域全体を mmap する。CPU の手動 cache flush は不要。
DMA バッファは非コヒーレント HP ポート向けの coherent DMA mapping で公開する。

### 2. キュー操作

```c
struct fades_buffer {
    __u32 queue;       /* FADES_QUEUE_TX=0 / FADES_QUEUE_RX=1 */
    __u32 index;       /* 0..info.slots-1 */
    __u32 records;
    __u32 reserved;    /* 0 */
};
```

|操作|records|意味|
|---|---|---|
|`FADES_QBUF` TX|1〜16,384|問題を書き込んだ入力バッファを登録|
|`FADES_QBUF` RX|0|空の出力バッファを登録|
|`FADES_DQBUF` TX|返り値|転送済み入力バッファと転送件数を取得|
|`FADES_DQBUF` RX|返り値|結果バッファと実際の結果件数を取得|

DQBUF の入力は queue と reserved=0。index/records は出力値。
**DQBUF は常に非ブロッキング**で、完了がなければ EAGAIN。
QBUF は登録だけを行い、DMA 完了を待たない。STREAMON 前にも登録できる。
RX が不足すればドライバーが TX の実行を待機させる。

所有権は `CPU → QBUF → driver → DQBUF → CPU`。
**QBUF 後から DQBUF まで、その slot を CPU から読み書きしない。**
二重 QBUF は EBUSY。不正な queue/index/件数/reserved は EINVAL。
DQBUF の copy_to_user が失敗した場合、完了は失われず再試行できる。

TX 完了は入力メモリーの再利用許可であり、数独の完了通知ではない。
入力と結果の順序は異なり、**TX slot と RX slot は対応しない**。
各結果の request ID で対応付け、未完了の問題間では ID を一意にする。

### 3. 開始・poll・循環

`FADES_STREAMON` は両 DMA と solver を初期化し、キューを動かす。
通常のバッファ境界ではリセットせず、稼働中も追加 QBUF できる。

|poll event|意味|
|---|---|
|POLLIN|DQBUF(RX) できる完了がある|
|POLLOUT|DQBUF(TX) できる完了がある|
|POLLERR|DMA / solver 異常。STREAMOFF で停止する|
|POLLERR + POLLHUP|デバイスが取り外された|

POLLOUT は「空き容量」ではなく **TX 完了キューが非空**という契約。
初期の未使用 slot はユーザーが番号を把握して直接 QBUF する。
完了キューが非空の間は level-triggered で通知する。

```c
/* INFO、TX/RX の mmap は済んでいるものとする。エラー処理は省略。 */
for (unsigned i = 0; i < info.slots; ++i) {
    struct fades_buffer b = { FADES_QUEUE_RX, i, 0, 0 };
    ioctl(fd, FADES_QBUF, &b);
}
ioctl(fd, FADES_STREAMON);
/* 初期TXに問題を詰めてQBUFし、submittedに件数を加える。 */

while (!eof || received != submitted) {
    struct pollfd p = {fd, POLLIN | (eof ? 0 : POLLOUT), 0};
    poll(&p, 1, timeout_ms);
    /* EINTR・timeout・POLLERR/POLLHUP/POLLNVAL を処理する。 */

    struct fades_buffer b = { FADES_QUEUE_RX, 0, 0, 0 };
    while (ioctl(fd, FADES_DQBUF, &b) == 0) {
        consume(rx + b.index * info.slot_bytes, b.records);
        received += b.records;
        b.records = 0;
        ioctl(fd, FADES_QBUF, &b);
    } /* EAGAIN 以外はエラー */

    b = (struct fades_buffer){ FADES_QUEUE_TX, 0, 0, 0 };
    while (!eof && ioctl(fd, FADES_DQBUF, &b) == 0) {
        b.records = fill_next(tx + b.index * info.slot_bytes, 10000);
        if (b.records) {
            ioctl(fd, FADES_QBUF, &b);
            submitted += b.records;
        } else {
            ioctl(fd, FADES_END_INPUT);
            eof = true;
        }
    }
}
ioctl(fd, FADES_STREAMOFF);
```

### 4. 終了・再開始

`FADES_END_INPUT` は新規 TX を締め切り、待たずに戻る。
以後の QBUF(TX) は EPIPE。登録済み入力は引き続き処理し、RX は再登録できる。
投入件数と受け取った結果件数が一致するまで RX を回収する。
入力終了後は poll の POLLOUT を外し、未回収 TX 完了による busy loop を避ける。
空入力でも STREAMON → END_INPUT → STREAMOFF で終了できる。

`FADES_STREAMOFF` は処理を停止・キャンセルし、未回収完了を含む全 slot を
CPU 所有に戻す。次回は必要な RX/TX を再登録して STREAMON する。
異常後も両 DMA と全 solver/FIFO をリセットしてから再開始する。
close も停止処理を行う。mmap は file 参照を保持するため、最後の mapping も閉じて
から排他的 open が解放される。待機 timeout はユーザーの poll で指定する。

### C++ の共通ヘルパー

`include/fades_stream.hpp` の `Device` は fd と mapping を所有する RAII クラス。
**コンストラクタで open・INFO 検証・TX/RX mmap まで完了**し、途中で失敗すれば
取得済みの資源を解放して例外を送出する。デストラクタは処理停止・unmap・close を行う。
内部状態は private とし、`native_handle()` / `info()` で fd と構成を参照する。

- `buffer_data(queue, index)`: 指定したバッファ1面の先頭を取得する。CPU 所有時だけアクセスする。
- `enqueue(queue, index, records)`: QBUF で登録する。
- `dequeue(queue, buffer)`: DQBUF で完了を回収する。EAGAIN だけ false、それ以外の失敗は例外。

verify/benchmark 共通の `run_streaming_session` は、初期登録から開始・補充・結果回収・
入力締め切り・最終排出・統計取得・停止までを、1スレッドで実行する関数。
次の2つのコールバックを呼び出す。

```cpp
// CPU所有のTX領域へ count 問を必ず書く。first_record は通算0始まりの番号。
// request ID の割り当てはこのコールバックの責任。
void write_input_buffer(uint8_t* data, unsigned count, uint64_t first_record);

// 完了したRXバッファの count 件を読む。Device::buffer_data(RX, index)で参照。
// 戻った直後に再登録されるため、参照を後まで保持しない。
void read_output_buffer(unsigned index, unsigned count);
```

型パラメーターは、この署名に合うラムダ／関数オブジェクトの型を受け取るためのもの。
コールバックはどちらも呼び出し元スレッドで同期実行される。
戻り値 `StreamingResult` は投入／完了レコード数、完了パケット数、秒単位の時間、
最終 PL 統計を持つ値オブジェクト。各メンバーの単位・計測範囲はヘッダーの docstring を参照。

## レコード形式

64 byte、リトルエンディアン。128 bit × 4 beat、TKEEP は全有効。

|フィールド|入力bit|出力bit|
|---|---|---|
|request ID|63:0|63:0|
|status|なし|71:64|
|81セル × 4 bit|387:64|395:72|
|予約|511:388|511:396|

セルは行優先、先頭セルが下位 nibble。入力0は空欄、1〜9はヒント。
入力予約領域はゼロ。status は0=解あり、1=解なし、2=不正セル。
status != 0 の出力セルと、全出力の予約領域はゼロ。

## ツール

```sh
# packed 64-byte records をファイル間でストリーム処理
build/kv260-ps/host/fades_stream input.bin output.bin 10000

# 既存の問題テキストを使う正当性検証
build/kv260-ps/host/fades_verify sim/data/17puz49158.txt 8193 6 3
build/kv260-ps/host/fades_verify sim/data/17puz49158.txt 1024 3 3 mixed
build/kv260-ps/host/fades_verify sim/data/HardestDatabase110626.txt 375 1 1

# 時間指定の継続投入ベンチマーク
build/kv260-ps/host/fades_benchmark sim/data/17puz49158.txt 10
```

`fades_stream` は packed record ファイルを扱い、出力は完了順。
入力 ID の割り当ては呼び出し側が行う。入力は64 byte の整数倍でなければ拒否する。

問題テキストは1行81文字、空欄 `.`、ヒント `1〜9`、セル間空白なし。
LF/CRLF に対応。空行は読み飛ばし、不正文字・長さは拒否する。

### verify

引数は `DATASET RECORDS_PER_PACKET SLOTS ROUNDS [mixed]`。
総件数は `RECORDS_PER_PACKET × SLOTS × ROUNDS` だが、**round 境界で待たずに**
1つのセッションとして処理する。問題ファイルは循環して使う。
warmup と測定セッションの**全結果**を CPU 参照解・ID 重複・status・予約領域と照合する。
mixed は各論理 round の末尾64件を解なし32件・不正入力32件に置換する。
複数解の入力では別の正しい解でも CPU 参照解との一致に失敗し得るため、
通常入力は一意解の問題集を使う。速度には逐次の全解検証も含む。

### benchmark

引数は `PUZZLE_FILE SECONDS`。秒数は有限の0.001〜86400。
8面 × 10,000問を使い、全ファイルを循環して、空いた入力へ次の問題を詰める。
ID はセッション内で単調増加し、未完了要求との重複を避ける。
期限で新規投入を止め、一度だけ最終排出する。

計測前: ファイル読み込み、独立 CPU 参照、初期 DDR 準備、warmup 全件検証。
計測内: STREAMON、入力再充填・コピー・ID 更新、QBUF/DQBUF/poll、DMA、計算、最終排出。
計測後: 各 RX slot に最後に格納された結果を全件検証、重複 ID と累積 PL 件数を検査。
中間の全解は CPU で逐一照合しない。解なし・複数解の入力も使用可能。

高速な入力供給では、エンコード済み問題を通常の cached メモリーに保持し、
`fades_stream.hpp` の `copy_records_with_ids(tx_slot, source, count, first_id)` を使える。
ID を付けながら DMA mapping へ一度だけ書き、AArch64 では NEON の全幅 store と
先読みを使う。source と target は重ねず、CPU 所有の TX slot にだけ書く。
KV260 ネイティブビルドでは Cortex-A53 向けにコンパイルする。

実時間による排出込み速度、期限内に観測した packet 完了件数と速度、最終排出時間、
再充填時間を表示する。期限内件数は通知回収時刻による packet 単位の保守的な値。
件数には同一問題の再投入を含む。準備済み DDR をコピーせず反復する旧 RUN 測定とは
条件が異なり、速度を直接比較しない。

## ドライバーと実機試験

専用 AXI DMA の MMIO と IRQ は FADES が排他的に所有する。
32要素の事前リンク済み SG descriptor ring を各方向に置き、稼働中は tail を延長する。
同時所有バッファは各方向8面なので、再利用 descriptor と HW の先読み位置には余裕がある。
descriptor 完了ごとに通知でき、全チェーンの排出を待ってから次を起動する方式ではない。
Device Tree に別の Xilinx DMA provider ノードは作らない。

ロード後に実機で実行:

```sh
python3 host/tests/check_abi.py
python3 host/tests/check_stream.py
python3 host/tests/check_stream_cli.py build/kv260-ps/host/fades_stream
```

前者は ABI・不正引数・所有権・mmap、後者は可変長・SG ring 周回・ID/解の照合・
RX 枯渇からの再開・中断/再開始を検査する。
回路・overlay・モジュールは対応する v2 をまとめてロードする。
ロード手順は [KV260](../fpga/kv260/README.md)。
