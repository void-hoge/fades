# PS / Linux ソフトウェア

入力用と出力用の2つのバッファキューを1つの poll fd で管理する。
`SolverStream` は固定容量のスロットで問題を一括投入・回収する、1スレッド用のクラス。

## ビルド

KV260 のリポジトリルートで:

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
ctest --preset kv260-ps
```

CMake 3.25以降、Ninja、C++20、Python 3、build-essential、実行中カーネルのヘッダーが必要。
`-DFADES_KERNEL_BUILD=/path/to/headers` でヘッダーを指定できる。
開発機のビルド・テストは `host` プリセット。実機用は Cortex-A53 向けにコンパイルする。

## ユーザー向け API: SolverStream

```cpp
#include "fades_stream.hpp"

fades::SolverStream solver;           // 16,384問 × 入出力8面に固定

// inputs は通常メモリー上のエンコード済み問題。1〜capacity()問。
bool accepted = solver.try_submit(inputs);
// または、コピー時にIDを first_id, first_id+1, ... に置換する。
bool accepted_with_ids = solver.try_submit(inputs, first_id);

// 完成した1スロットを借りる。未完成なら nullopt。出力コピーはしない。
if (auto output = solver.try_receive()) {
    consume(output->records());       // span<const Record> を読む
}                                    // RAIIでRXへ返却する
```

### スロットの契約

slot は循環させるバッファ1面の意味。容量 N は16,384問（1 MiB）固定、入出力各8面。

- **入力**: 1〜N 問を空きスロットへ全件コピーし、投入して true を返す。
  空きがなければ入力をコピー・投入せず false。一部だけ受け付けることはない。
  N 問を超える入力・空入力は、空きの有無によらずエラー。
- **出力**: N 件でフルになったスロット、または受け付け済みの処理がすべて完了した時点の
  端数スロットを返す。それ以外は nullopt。
- `try_receive()` はキャッシュ可能な RX スロットの **move-only な `OutputSlot`** を返す。
  追加コピーは行わない。`records()` は実件数分の読み取り専用 span、`size()` は件数。
  オブジェクト破棄または `release()` で RX に再登録する。返却後の参照は使わない。
  長期保持したい結果だけ、呼び出し側で通常メモリーへコピーする。
- 複数スロットを保持できるが、全 RX を保持すれば返却まで背圧がかかる。
  Stream を先に破棄した場合は DMA を停止し、借りたスロットがなくなるまで fd と mmap を保持する。
  その間は排他的 open も維持する。全操作は1スレッドで使う。
- `release()` は再登録失敗を例外で返す。デストラクタは例外を投げず、失敗を次の Stream の
  I/O・統計取得時に通知する。セルのデコード・解検証はこのクラスでは行わない。
- 入出力の区切りは独立。例えば6千問＋6千問を処理継続中に投入すれば、出力は1万件＋端数2千件。
  ID で結果を対応付ける。入力バッファ番号と出力バッファ番号は対応しない。
- 「処理完了」は、ホストや DMA 内で送信待ちの入力も含めた全登録件数に対して判定する。
  入力 FIFO が一時的に空でも端数にはしない。端数境界を確定した後に追加入力されても境界は保持する。
- アイドルの端数を取り出した後も入力を追加できる。reset や `finish_input()` は不要。

### 待機・終了

- `wait(timeout_ms)` は入力の空き／完了か出力到着を待つ。負値は無期限。
- `wait_until(deadline)` は steady_clock の期限まで待つ。
- `wait_for_results(timeout_ms)` は出力だけを待つ。入力は後から追加できる。
- `finish_input()` は以後の投入を締め切る。端数を返すための操作ではない。
  締め切り後の `wait` は出力だけを待ち、TX 完了による busy loop を避ける。
- `outstanding()` は投入件数 − アプリへスロットとして渡した結果件数。保持中のスロットは含まない。
- デストラクタで停止・キャンセル・資源解放する。
- `statistics()` は累積 PL カウンターを返す。内容の検査・時間計測は呼び出し側の責務。

典型的な有限入力のループ:

```cpp
size_t next = 0;
while (next < problems.size() || solver.outstanding()) {
    auto output = solver.try_receive();
    bool received = output.has_value();
    if (output) {
        consume(output->records());
        output->release();            // 返却エラーをその場で確認したい場合
    }

    bool accepted = false;
    if (next < problems.size()) {
        auto inputs = std::span(problems).subspan(next);
        inputs = inputs.first(std::min(inputs.size(), size_t(solver.capacity())));
        accepted = solver.try_submit(inputs, next);
        if (accepted) next += inputs.size();
        if (next == problems.size()) solver.finish_input();
    }
    if (!received && !accepted) solver.wait();
}
```

## ファイルの責務

|ファイル|責務|
|---|---|
|`include/fades_ioctl.h`|カーネルと共有する ABI 構造体・ioctl 番号|
|`include/fades_device.hpp`|fd・mmap の RAII、バッファアドレス、QBUF/DQBUF|
|`include/fades_records.hpp`|64 byteレコード型、ID 更新付き高速コピー|
|`include/fades_stream.hpp`|`SolverStream`。全件受付・RXのゼロコピー貸出／RAII返却・待機|
|`tools/fades_benchmark.cpp`|問題集・CPU参照・warmup・測定ループ・期限・統計検査・表示|
|`tools/fades_verify.cpp`|有限件数の投入と全結果の逐次照合|
|`tools/fades_stream.cpp`|低レベル ioctl を直接示す、packed record のファイル変換例|
|`driver/fades.c`|バッファ所有権、DMA SG、IRQ、poll、FPGA制御|

`run_streaming_session` とコールバックによる共通実行ループは廃止した。
測定方針はベンチマークの `.cpp` にあり、SolverStream はファイルや測定時間、CPU参照を知らない。
入力の高速コピーは cached な source の先読みと NEON 全幅 store によって、ID 更新とコピーを
1回の DMA メモリー書き込みに融合する。source/destination は重ねない。

## 低レベル ioctl ABI v3

`Device` はコンストラクタで `/dev/fades` を排他的 open、INFO 検証、TX/RX mmap まで行う。
失敗時は取得済み資源を解放。`native_handle()` / `info()` で fd と構成を参照する。
`buffer_data(queue, index)` はバッファ先頭、`enqueue` / `dequeue` は QBUF / DQBUF のラッパー。

|領域|mmap offset|長さ|
|---|---:|---:|
|TX|0|`info.map_bytes`|
|RX|`info.map_bytes`|`info.map_bytes`|

各領域は1 MiB × 8面。`mapping + index * info.slot_bytes` が各バッファの先頭。
MAP_SHARED で領域全体を mmap。
TX は `dma_alloc_coherent` / `dma_mmap_coherent` で、書き込み向けのマッピングを使う。
RX は `dma_alloc_pages` / `dma_mmap_pages` でキャッシュ可能にする。
ドライバーが DMA 開始前・完了後・中断後の所有権移動で Linux DMA API の同期を行い、
CPU が手動 cache flush を行う必要はない。descriptor 自体は coherent メモリーに置く。
結果はこの cached mapping を直接読む。ベンチマークの読み出しでは NEON と先読みを使う。

```c
struct fades_buffer {
    __u32 queue;       /* FADES_QUEUE_TX / FADES_QUEUE_RX */
    __u32 index;       /* 0..7 */
    __u32 records;
    __u32 reserved;    /* 0 */
};
```

|操作|意味|
|---|---|
|INFO|ABI・mmap 構成を取得|
|QBUF(TX)|1〜16,384 問を一括登録。STREAMON 前にも登録可能|
|QBUF(RX)|records=0 で空の受信バッファを登録。実際の受信容量は N 問|
|DQBUF(TX)|入力メモリーを再利用可能にする。解の完成は意味しない|
|DQBUF(RX)|完成した full/idle-tail スロットと実件数を取得|
|STREAMON|容量16,384問を設定して初期化・開始。引数なし|
|END_INPUT|新規 TX を締め切る。以後 QBUF(TX) は EPIPE。RX は回収・再登録可能|
|STREAMOFF|両 DMA と solver を停止・reset し、全バッファを CPU 所有へ戻す|
|STATS|累積カウンターの snapshot|

所有権は `CPU → QBUF → driver → DQBUF → CPU`。
QBUF 後から DQBUF まで、そのバッファを CPU から読み書きしない。
二重登録は EBUSY、不正引数は EINVAL。DQBUF は常に非待機で、空なら EAGAIN。
DQBUF の copy_to_user 失敗では完了を失わず再試行できる。

同じ fd の poll は **POLLIN=RX完了、POLLOUT=TX完了、POLLERR=異常**。
POLLOUT は空き容量ではなく、TX 完了キューが非空という意味。
close も停止処理を行う。mmap が file を保持するため、最後の mapping を閉じてから
排他的 open が解放される。異常後は STREAMOFF で再初期化してから再開する。

DMA は RX を N 件の容量で独立に先行登録する。TX の件数は開始前／投入時に全件を HW に
通知し、その後 DMA へ送る。HW が N 件または最後の結果で TLAST を付ける。
ドライバーは短い RX の実 byte 数を確認して records を返す。
専用 AXI DMA の MMIO/IRQ は FADES が所有し、32要素の事前リンク済み SG ring の tail を延長する。

## レコード形式

64 byte、リトルエンディアン。128 bit × 4 beat、TKEEP は全有効。

|フィールド|入力bit|出力bit|
|---|---|---|
|request ID|63:0|63:0|
|status|71:64|71:64|
|81セル × 4 bit|395:72|395:72|
|予約|511:396|511:396|

セルは行優先、先頭セルが下位 nibble。入力0は空欄、1〜9はヒント、予約領域はゼロ。
入出力ともbyte 8がstatus、セルはbyte 9から。
入力statusは0を設定し、HWは無視する。入力予約領域もHWは無視する。
status は0=解あり、1=解なし、2=不正セル。status != 0 の出力セルと出力予約領域はゼロ。
HW versionは `0x46414404`。旧HWとは入力セルの位置が異なる。
未完了要求の ID は一意にする。

## ツールと測定範囲

```sh
build/kv260-ps/host/fades_stream input.bin output.bin 10000
build/kv260-ps/host/fades_verify sim/data/17puz49158.txt 8193 6 3
build/kv260-ps/host/fades_verify sim/data/17puz49158.txt 1024 3 3 mixed
build/kv260-ps/host/fades_verify sim/data/HardestDatabase110626.txt 375 1 1
build/kv260-ps/host/fades_benchmark sim/data/17puz49158.txt 10
```

- `fades_stream`: packed record を一度ずつ処理して完了順にファイル出力する ioctl 例。
  呼び出し側が入力 ID を割り当てる。任意引数は1回の入力件数で、出力容量は16,384問固定。
  EOF 後は全件回収して終了。
- `fades_verify`: 総件数は `RECORDS_PER_PACKET × SLOTS × ROUNDS`。
  この SLOTS は総件数を定める論理値で、SolverStream の物理バッファは常に8面。
  round 境界で排出せず、warmup と測定の全結果を CPU 参照解・ID・status・予約領域と照合する。
  mixed は論理 round の末尾64問を解なし32問・不正入力32問に置換する。
  一意解問題集を使う。速度は入力準備・借りたスロット上の全解検証・返却込み。
- `fades_benchmark`: 8面 × 16,384問で問題集を循環し、期限まで一括投入を続け、最後に一度排出。
  **初回からの入力コピー・一意 ID 更新・全結果の64 byteを加算チェックサムへ読み込む処理・
  RX返却／再登録・最終排出**を計測する。出力の追加コピーは行わない。
  デバイス準備、テキスト読み込み・エンコード・CPU参照、独立した計測前後各131,072件の全解検証は計測外。
  測定中は DMA 長・最終 PL 件数を検査し全結果を読むが、数独内容の逐一照合は行わない。
  チェックサムは読み出しを観測可能にするために表示する。ファイル入出力時間は含まない。

テキスト問題は1行81文字、空欄 `.`、ヒント `1〜9`、セル間空白なし。LF/CRLF に対応。
空行は読み飛ばし、不正文字・長さは拒否する。
過去の「DMA受信はするが、結果をCPUでは全件読まない」測定とは条件が異なる。
測定結果は [STREAMING_RESULTS.md](STREAMING_RESULTS.md)。

## 実機試験

対応する v3 回路・overlay・ドライバーをセットでロードする。
ロード手順は [KV260](../fpga/kv260/README.md)。

```sh
python3 host/tests/check_abi.py
python3 host/tests/check_stream.py
python3 host/tests/check_stream_cli.py build/kv260-ps/host/fades_stream
build/kv260-ps/host/solver_stream_board_test
```

CTest はパーサー・RAII失敗注入・全件受付／未受付・貸出の移動／寿命／返却エラー試験を実行する。
上記の実機試験は、固定容量／端数・入力結合・RX枯渇からの再開・SG周回・キャンセル・再開始を検査する。
