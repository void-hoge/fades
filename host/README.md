# PS / Linux ソフトウェア

`/dev/fades` を使うユーザープログラムと Linux DMAengine クライアント。
PS は問題の選択・反復と入出力バッファを管理し、PL はストリームを処理する。

## ディレクトリ

- `include/fades_ioctl.h`: ABI バージョン、ioctl、固定幅の共通構造体。
- `driver/fades.c`: `fades.ko`。DMA バッファ、AXI-Lite、排他的 open、転送完了を管理。
- `tools/fades_verify.cpp`: 件数指定の転送・CPU 参照解との照合・診断用の時間表示。
- `tools/fades_benchmark.cpp`: ファイルと秒数を受け取る時間指定の性能測定。
- `tests/check_abi.py`: 二重 open、不正 ioctl / mmap / 転送引数、close/reopen の検査。
- `tests/check_input.py`: 両ツールの Usage と不正な問題テキストの拒否試験。
- `tests/data/stress_mixed.txt`: 完成済み問題と矛盾する問題の小規模入力。

ホストの検証は `sim/cpp/reference_solver.hpp` の独立 CPU ソルバーを共有する。
外部ソルバーの実行・ビルドは不要。

## ビルド

リポジトリのルートから:

```sh
make host                     # build/host/fades_verify と fades_benchmark
make host-module              # 実行中 Linux のヘッダーで fades.ko をビルド
```

モジュールは KV260 上で、その実行中カーネルに対応したヘッダーを使ってビルドする。
Ubuntu / Arm64 では `build-essential` と `linux-headers-$(uname -r)` が必要。
ヘッダーが別の場所なら `KDIR=/path/to/kernel/build` を指定する。

`make -C host` と `make -C host module` も利用でき、既定出力先は同じ。
`OUT=/absolute/output/path`、`CXX=...`、`CXXFLAGS=...` を上書きできる。
回路の転送・ロード・デバイス作成は [KV260 手順](../fpga/kv260/README.md) を参照。

## メモリと転送

INFO ioctl で ABI とスロット構成を確認してから、TX/RX をそれぞれ `mmap` する。

|領域|mmap offset|長さ|
|---|---:|---:|
|TX|0|`info.map_bytes`|
|RX|`info.map_bytes`|`info.map_bytes`|

現行は各 8 MiB、1 MiB × 8 slot。1 レコード 64 byte なので最大 131,072 件。
バッファは DMA デバイス上で `dma_alloc_coherent` し、`dma_mmap_coherent` で公開する。
非コヒーレント HP ポート向けのマッピング属性は Linux が管理する。
ユーザー空間は物理アドレスや手動の cache flush を扱わない。

`FADES_DMA_RUN` は全 RX descriptor を先に準備し、TX とともに起動して全完了まで待つ。
**RUN 中は CPU から TX/RX を読み書きしない。** `valid && !ready` の停止は PL が扱う。
結果は入力 slot の順序に対応しないため、全 RX 領域を request ID で照合する。

各 round は全体を排出してから次を開始し、round ごとに回路をリセットして総件数と
packet 長を設定する。連続 SG リングを途切れなく補充する実装ではない。
異常時は両チャネルを停止し、ドライバーを faulted にする。
タイムアウト等の後はアプリとモジュールを再ロードしてから再開する。

## 共通の問題テキスト形式

1 行に **81 文字**を行優先で並べ、空欄は `.`、ヒントは `1〜9`。
セル間・行頭・行末の空白は付けない。LF / CRLF の両方に対応し、空行は読み飛ばす。
長さの異なる行や `0`・`-`・空白などの不正文字は拒否する。
両プログラムとシミュレーションは共通の `reference::parse` で読み込む。
同梱の `sim/data/17clues.txt` は BinarySudoku の `17puz49158.txt` をそのまま取り込んだもの。

## `fades_verify`

```sh
build/host/fades_verify DATASET RECORDS_PER_PACKET SLOTS ROUNDS [mixed]
build/host/fades_verify sim/data/17clues.txt 8193 6 1
build/host/fades_verify sim/data/17clues.txt 1024 3 1 mixed
```

ファイルを巡回して各 slot に同数の問題を入れる。`8193 6` は 49,158 問を一巡する。
`mixed` は末尾 64 件を矛盾する問題 32 件と不正セル値の問題 32 件に置換する。

計測前に CPU 参照解を生成し、warmup と計測後の全結果について以下を検査する。

- ID の範囲・重複・件数、期待する status。
- 81 セルの数字、行・列・ブロックの制約、入力ヒントの保持。
- CPU 参照ソルバーが選んだ解との一致。
- 予約領域、および解なし・不正入力時のセルがゼロ。
- PL の各段・コア別件数と未完了上限。

参照ソルバーは最初の解を求める。複数解の入力では、別の正しい解でも参照解との
完全一致検査に失敗しうるため、このツールの通常入力には一意解の問題集を使う。
反復測定の中間 round は DMA 完了を検査し、CPU で全解を逐一照合するものではない。

## `fades_benchmark`

```sh
build/host/fades_benchmark PUZZLE_FILE SECONDS
build/host/fades_benchmark sim/data/17clues.txt 10
```

引数は問題ファイルと実行時間だけ。秒数は有限の `0.001〜86400`、小数も指定可能。
解なし・複数解も使用でき、解があれば数独制約と入力ヒントを満たす解を検証する。

1 round は 8 × 1 MiB = 131,072 件。小さいファイルは問題を先頭から循環して
準備済み DDR を充填・反復する。ファイルがバッファより大きい場合はウィンドウを
切り替え、計測中の再充填時間を別表示する。ID は未完了リクエスト間で一意とし、
全件排出済みの round 間では再利用する。

計測前にファイル読み込み、CPU 参照による解の有無の判定、DDR 準備、warmup と全件検証を行う。
計測中は DMA 完了と PL の各段・コア別件数を毎 round 検査し、終了後に最後のバッチを
全件検証する。中間 round の全解は CPU で逐一照合しない。

期限後に新規 RUN を始めず、実行中のバッチを最後まで排出する。
速度は **実経過時間**で計算し、指定時間、期限超過、期限前 / 後のバッチ完了件数を分ける。
期限前件数は whole-batch 単位の保守的な値であり、レコードごとの厳密な完了時刻ではない。
件数は同じ問題の再投入を含む延べ件数。

## 計測範囲

|表示・値|含むもの|
|---|---|
|`fades_verify` の kernel 時間|reset/config、descriptor 準備、DMA、数独処理、出力排出、完了待ち|
|`fades_benchmark` の実時間|上記に加え PS ループ、snapshot と件数検査、大規模ファイルの再充填|
|PL processing cycles|最初の配布から最後の結果回収まで。最後の DMA 出力排出は含まない|
|`fades_verify` の single-round 総合時間|入力準備・コピー、RUN、snapshot、全解検証|

CPU 参照解の事前生成は全ての速度値の区間外。
準備済み DDR の速度と、ホスト側の全件検証を含む速度を区別する。
