# FADES stream protocol v1

全レコードは 64 byte、リトルエンディアン。beat は 128 bit、先頭の
16 byte が beat 0。セルは行優先、先頭セルが下位 nibble。

|入力ビット|内容|
|---|---|
|63:0|request_id|
|387:64|81 個の 4-bit セル。0=空欄、1〜9=ヒント|
|511:388|予約。ホストはゼロを送信、RTL は無視|

|出力ビット|内容|
|---|---|
|63:0|request_id|
|71:64|status: 0=解あり、1=解なし、2=不正セル値|
|395:72|解の 81 セル。status != 0 なら全てゼロ|
|511:396|予約。ゼロ|

各入力に必ず 1 結果。重複ヒントなど通常の数独の矛盾は status=1。
10〜15 のセルを含む入力は status=2。各コア内は入力順、全体は結果を
回収できた順（同時有効時はラウンドロビン）で出力する。

TKEEP は常に 0xffff。入力 TLAST は任意の 4-beat レコード境界で許され、
通常バッチ境界では状態をリセットしない。途中の TLAST、不完全 TKEEP は
sticky framing_error を立てて入力停止。復旧には両ドメイン・全コア・FIFO の
全体リセットが必要。フレーミング異常では途中の入力に結果を保証しない。

出力の total_records と packet_records は実行前のリセット中に設定して保持する。
packet_records 件ごと、および total_records の最後で beat 3 に TLAST を付ける。
入力 TLAST と特定の結果を対応付けない。ホストは受信 descriptor を先行投入する。
実行中の設定変更は禁止。total_records と packet_records はともに 1 以上。

## AXI-Lite

KV260 の基底アドレスは `0xa0010000`。32-bit データ、byte strobe 対応。
AW/W は独立受理し、B/R の停止中は応答を保持する。

|offset|意味|
|---|---|
|0x000|bit0 全体 reset（初期値1）、bit1 探索 enable|
|0x004|total_records。reset=1 の間だけ書き込み可能|
|0x008|packet_records。同上|
|0x00c|bit0 sticky framing error、bit1 snapshot pending|
|0x010|bit0=1 書き込みで snapshot 要求。pending 中は無視|
|0x014|ハードウェア version `0x46414401`（FAD / v1）|
|0x100〜0x25f|44 個の 64-bit snapshot counter、下位32 bit が先|

カウンター順:

|index|内容|領域|
|---|---|---|
|0|入力レコード受理件数|DMA|
|1|出力レコード転送件数|DMA|
|2|入力 valid && !ready cycle|DMA|
|3|コアへ配布した件数|core|
|4|コア結果を回収した件数|core|
|5|共通入力 FIFO が非 valid の cycle|core|
|6〜11|コア別配布件数|core|
|12〜17|コア別回収件数|core|
|18〜23|コア別 busy cycle|core|
|24〜29|コア別 terminal output wait cycle|core|
|30|入力 FIFO write-domain 最大占有数|DMA|
|31|最初の配布から最後の回収まで、両端を含む cycle|core|
|32〜37|コア別 enable && !busy && inflight==0 cycle|core|
|38〜43|コア別最大未完了数|core|

index 31 以外の cycle counter は reset 解除から snapshot までを数える。
core counter は request/ack handshake で core 領域の保持レジスターへ一括コピーし、
同期 ack の後で DMA 領域へ取り込む。DMA counter はその取り込み時に採取する。
動作中の両領域の採取時刻は異なるが、全件排出後の件数照合にはずれがない。
FWFT の先読み・出力段と read pointer の CDC 遅延により、index 30 は RAM 設定深さを
超えうる保守的な占有数となる。

## Linux ioctl ABI v1

公開ヘッダーは `host/include/fades_ioctl.h`。ユーザー空間は `/dev/fades` を開く。

- `FADES_DMA_INFO`: ABI version、slot 数、slot byte 数、mapping byte 数を取得。
- `FADES_DMA_RUN`: slot 当たり byte 数、slot 数、反復 round 数、timeout を指定し、
  全送受信の完了まで待つ。kernel 経過 ns と片方向当たりの総 byte 数を返す。
- `FADES_SOLVER_STATS`: 44 個の 64-bit counter を coherent snapshot として返す。

現行は TX/RX 各 8 MiB = 1 MiB × 8 slot。各 mmap は領域全体を MAP_SHARED で行い、
TX offset は 0、RX offset は `info.map_bytes`。slot 間隔は `info.slot_bytes`。
RUN の byte 数は 64 の正の倍数で 1 MiB 以下、slot 数は 1〜8、round 数は 1〜100,000、
timeout は round ごとに 1〜60,000 ms。転送中の CPU によるバッファアクセスは禁止。

単一 open とし、不正 ioctl / 転送引数 / mmap を拒否する。
RUN は各 round の開始時に総件数と packet 長を設定し、全 RX descriptor を先行投入する。
タイムアウト等で停止したインスタンスは faulted となり、以後の RUN は拒否する。
回復にはモジュールと FPGA アプリの再ロードを行う。
