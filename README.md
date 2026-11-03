# FADES: FPGA-Accelerated Deductive and Explorative Sudoku Solver

KV260 向けの SystemVerilog 数独ソルバー。Singles / Locked Candidates を
並列適用し、停滞時は候補を二分して深さ優先探索する。
PS の Linux プログラムから DDR / AXI DMA 経由で問題を投入し、ID 付きの結果を回収する。

既定の実機構成は **6 コア × 100 MHz、DMA 200 MHz**。コア数は1〜6で指定できる。
最初の解を返し、解なしも結果を 1 件返す。問題の一意性を証明する処理ではない。

## 1. 実機を使う

FPGA アプリとドライバーをロード済みの KV260 上で実行する。
提供する入口は、時間指定の性能測定と、件数指定の正当性確認の2つ。

```sh
cmake --preset host-kv260                          # KV260上でconfigure
cmake --build --preset host-kv260 --parallel 4
build/host-kv260/host/fades_benchmark sim/data/17puz49158.txt 10
build/host-kv260/host/fades_verify sim/data/17puz49158.txt 8193 6 1
build/host-kv260/host/fades_verify sim/data/HardestDatabase110626.txt 375 1 1
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
# 開発機: CMake 3.25以降、Ninja、Vivado 2024.2 + K26サポート、dtc
cmake --preset fpga-kv260 -DFADES_CORES=6 -DFADES_VIVADO_JOBS=2
cmake --build --preset fpga-kv260 --target kv260-package
```

生成物は `build/fpga-kv260/fpga/kv260/firmware/fades/`。KV260 で `xmutil loadapp fades` と
`insmod fades.ko` を実行すると **`/dev/fades`** が使える。
転送・Arm64 ビルド・ロードの手順と、各ボード用ファイルの役割は
[fpga/kv260/README.md](fpga/kv260/README.md) にまとめる。

## 3. 開発・検証する

```sh
cmake --preset sim -DFADES_CORES=6
cmake --build --preset sim --parallel 4
ctest --preset sim -L quick   # 有向試験、コア・統合短縮試験、lint・構造合成
ctest --preset sim -L full    # 全問題集のコア・統合・CPU参照試験
ctest --preset sim -L formal  # 推論・分岐の形式検証
ctest --preset sim -L xpm     # Vivado xsimによる実XPMキュー試験
```

試験の詳細と回帰比較は [sim/README.md](sim/README.md)。必要ツールは
CMake 3.25以降、Ninja、C++20、Python 3、Icarus Verilog、Verilator、Yosys。
XPM試験にはVivadoが必要。ホストのみのビルドではRTLツールは不要。

### ビルド対象と出力の分離

|プリセット|用途|出力先|
|---|---|---|
|`host-native`|開発機のホストツール・入力試験|`build/host-native/`|
|`host-kv260`|KV260上のネイティブビルド・ドライバー|`build/host-kv260/`|
|`sim`|RTLシミュレーション・検証|`build/sim/`|
|`fpga-kv260`|KV260回路・firmware生成|`build/fpga-kv260/`|

コア数はプリセットとは独立した `-DFADES_CORES=1`〜`6`（既定6）。
ボードトップからコア群まで伝播し、シミュレーションにも同じ値を使う。
ホストABIの6コア分のカウンターレイアウトは固定で、未実装コア分は0になる。
構成変更時は同じビルドディレクトリで再configureしてからビルドする。
複数構成を保持するときは `-B` で出力先を分ける:

```sh
cmake --preset sim -B build/sim-check-1core -DFADES_CORES=1
cmake --build build/sim-check-1core --parallel 4
ctest --test-dir build/sim-check-1core --output-on-failure -L quick
```

ツールのパスなどのローカル設定は `CMakeUserPresets.json` に記述できる（Git管理外）。
一覧は `cmake --list-presets`、試験一覧は `ctest --preset sim -N`。

## ソースを読む順序

|対象|読むファイル|役割|
|---|---|---|
|PS利用|`host/tools/fades_benchmark.cpp` / `fades_verify.cpp`|性能測定 / 解の照合|
|Linux接続|`host/include/fades_ioctl.h` → `host/driver/fades.c`|公開ABI → バッファ・DMA・制御|
|ボード生成|`fpga/kv260/scripts/build.tcl`|PS・DMA・クロックの接続、実装、出力検査|
|PLの入口|`rtl/fades_axi.sv` → `fades.sv`|制御 → 供給・ID管理・結果回収・コア群|
|探索|`rtl/fades_core.sv` → `deduction_engine.sv` / `branch_generator.sv`|状態列・探索 → 推論 / 分岐|
|クロック境界|`rtl/async_queue.sv`|XPM非同期FIFOと試験用モデル|
|CPU照合|`reference/sudoku.hpp`|PSツールとRTL試験が共有する独立参照ソルバー|

`build/` は生成物、`results/` は生ログ・実験履歴、`docs/` はローカル設計メモ。

## 実測

KV260 実機で 17 ヒント 49,158 問と Hard 1106 の 375 問を CPU 参照解と照合済み。
準備済み DDR の 10 秒ストレス測定では、17 ヒント集合 **22.34 Mproblem/s**、
Hard 1106 **0.7032 Mproblem/s**。入力準備と前後の全解検証は計測外。
6コア100 MHz / DMA 200 MHz、131,072件を反復する条件で、件数は延べ投入数。
計測中は DMA 完了・PL件数を検査し、前後のバッチを全件検証する。
入力データの出所は [sim/README.md](sim/README.md#問題集)。
