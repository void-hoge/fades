# FADES: FPGA-Accelerated Deductive and Explorative Sudoku Solver

KV260 向けの SystemVerilog 数独ソルバー。Singles / Locked Candidates を
並列適用し、停滞時は候補を二分して深さ優先探索する。
PS の Linux プログラムから DDR / AXI DMA 経由で問題を投入し、ID 付きの結果を回収する。

現行の実機構成は **6 コア × 100 MHz、DMA 200 MHz**。
最初の解を返し、解なしも結果を 1 件返す。問題の一意性を証明する処理ではない。

## 1. 実機を使う

FPGA アプリとドライバーをロード済みの KV260 上で実行する。
提供する入口は、時間指定の性能測定と、件数指定の正当性確認の2つ。

```sh
make host                                          # KV260上でPSプログラムをビルド
build/host/fades_benchmark sim/data/17puz49158.txt 10     # 10秒の性能測定
build/host/fades_verify sim/data/17puz49158.txt 8193 6 1  # 全49,158問の解を照合
build/host/fades_verify sim/data/HardestDatabase110626.txt 375 1 1  # 難問375問
```

```text
問題ファイル → PSプログラム → /dev/fades → fades.ko → DMA → FPGAのソルバー群
                     ↑                 結果はDDRへDMAで返す ───────────┘
```

入力は **1行81文字、空欄 `.`、ヒント `1〜9`、セル間の空白なし**。LF/CRLF に対応。
プログラムは計測前後に独立 CPU 参照を使って検証する。
通常の「問題ファイルを解ファイルへ変換する」コマンドは、現在の提供物には含まない。
引数・データ形式・検証範囲は [host/README.md](host/README.md)。

## 2. 実機を準備する

開発機で回路を生成し、KV260 上で PS プログラムとドライバーをビルドしてロードする。

```sh
# 開発機: Vivado 2024.2 + K26サポート、dtc
make vivado-kv260 JOBS=2
make package-kv260
```

生成物は `build/kv260/firmware/fades/`。KV260 で `xmutil loadapp fades` と
`insmod fades.ko` を実行すると **`/dev/fades`** が使える。
転送・Arm64 ビルド・ロードの手順と、各ボード用ファイルの役割は
[fpga/kv260/README.md](fpga/kv260/README.md) にまとめる。

## 3. 開発・検証する

```sh
make test lint            # 推論・分岐・CPU参照・AXI-Liteの基本試験
make core-test            # 単一コアの全問・ストレス試験
make stream-test          # コア群・非同期入出力・ID対応の全体試験
```

形式検証、XPM FIFO、回帰比較、単一コアの面積・タイミング評価は
[sim/README.md](sim/README.md)。必要ツールは GNU Make、C++20、Python 3、
Icarus Verilog、Verilator、Yosys。コマンドの一覧は `make help`。

## ソースを読む順序

|対象|読むファイル|役割|
|---|---|---|
|PS利用|`host/tools/fades_benchmark.cpp` / `fades_verify.cpp`|性能測定 / 解の照合|
|Linux接続|`host/include/fades_ioctl.h` → `host/driver/fades.c`|公開ABI → バッファ・DMA・制御|
|ボード生成|`fpga/kv260/build.tcl`|PS・DMA・クロックの接続、実装、出力検査|
|PLの入口|`rtl/solver_axi.sv` → `solver_stream.sv`|制御 → 供給・ID管理・結果回収・コア群|
|探索|`rtl/core.sv` → `inference.sv` / `branch.sv`|状態列・探索 → 推論 / 分岐|
|クロック境界|`rtl/async_fifo.sv`|XPM非同期FIFOと試験用モデル|
|CPU照合|`reference/sudoku.hpp`|PSツールとRTL試験が共有する独立参照ソルバー|

`build/` は生成物、`results/` は生ログ・実験履歴、`docs/` はローカル設計メモ。

## 実測

KV260 実機で 17 ヒント 49,158 問と Hard 1106 の 375 問を CPU 参照解と照合済み。
準備済み DDR の 10 秒ストレス測定では、17 ヒント集合 **22.34 Mproblem/s**、
Hard 1106 **0.7032 Mproblem/s**。入力準備と前後の全解検証は計測外。
6コア100 MHz / DMA 200 MHz、131,072件を反復する条件で、件数は延べ投入数。
計測中は DMA 完了・PL件数を検査し、前後のバッチを全件検証する。
入力データの出所は [sim/data/README.md](sim/data/README.md)。
