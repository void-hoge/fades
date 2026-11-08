# FADES: FPGA-Accelerated Deductive and Explorative Sudoku Solver

KV260 向けの SystemVerilog 数独ソルバー。Singles / Locked Candidates を並列適用し、停滞時は候補を二分して深さ優先探索する。
PS の Linux プログラムから DDR / AXI DMA 経由で問題を投入し、ID 付きの結果を回収する。

既定の実機構成は **6 コア × 約115.385 MHz、DMA 187.5 MHz**。コア数は1〜6で指定できる。
最初の解を返し、解なしも結果を 1 件返す。問題の一意性を証明する処理ではない。

入力の候補マスク変換・確定数字の先行伝播は、全コア共有の2段パイプラインで行う。

ビルド・検証の基本操作は **configure → build → test**。
以下のコマンドは、対象の開発機またはKV260上のリポジトリルートで実行する。

## 1. ホスト側

ホストC++実装は削除した。要求仕様と、verify／streamが確認していたことは
[host/README.md](host/README.md) にまとめている。
Linuxドライバー・ioctl ABI・Pythonによる実機試験は `host/` にある。

## 2. 実機を準備する

開発機で回路を生成し、KV260 上で PS プログラムとドライバーをビルドしてロードする。

```sh
# 開発機: CMake 3.25以降、Ninja、Vivado 2024.2 + K26サポート、dtc
cmake --preset kv260-pl -DFADES_CORES=6 -DFADES_VIVADO_JOBS=2
cmake --build --preset kv260-pl
```

通常のbuildで、回路生成・出力検査からfirmwareパッケージ生成まで実行する。
合成・配置配線にはcore 8.666 ns / DMA 5.333 nsの制約を適用し、
配線後の `AggressiveExplore` 物理最適化と最終タイミング検査も自動実行する。
Vivadoのパスを指定する場合は、configure時に
`-DVIVADO_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/vivado` を追加する。
`FADES_VIVADO_JOBS` はVivado内部の並列数。

| ターゲット        | 処理                                             |
|-------------------|--------------------------------------------------|
| `kv260-bd`        | BD生成・検証                                     |
| `kv260-synth`     | 合成まで                                         |
| `kv260-bitstream` | 配置配線・bitstream生成・タイミング／CDC等の検査 |
| `kv260-package`   | firmwareパッケージまで                           |

生成物は `build/kv260-pl/fpga/kv260/firmware/fades/`。
Vivadoプロジェクトと検査レポートは `build/kv260-pl/fpga/kv260/implementation/` に生成する。
ソースとfirmwareはユーザーが任意の方法で実機へコピーする。
ビルド処理は接続先や実機上の配置先を仮定せず、自動転送もしない。

KV260上で必要なパッケージを入れ、ソースを配置したリポジトリルートでビルドする。

```sh
sudo apt install build-essential cmake ninja-build python3 "linux-headers-$(uname -r)"
cmake --preset kv260-ps
cmake --build --preset kv260-ps
```

通常のbuildで `build/kv260-ps/host/driver/fades.ko` を生成する。
実機自身のコンパイラーと実行中カーネルのヘッダーを使う。
firmwareを配置し、`xmutil loadapp fades` と `insmod fades.ko` を実行すると **`/dev/fades`** が使える。
転送・Arm64 ビルド・ロードの手順と、各ボード用ファイルの役割は
[fpga/kv260/README.md](fpga/kv260/README.md) にまとめる。

## 3. 開発・検証する

```sh
cmake --preset host -DFADES_CORES=6
cmake --build --preset host
ctest --preset host                         # CPU参照・RTLの全試験
ctest --preset host -L '^fades_core$'        # コア単体：全問題集・ストレス・構造合成・lint
ctest --preset host -L '^fades$'             # 指定コア数で供給・回収を含む統合試験・lint
ctest --preset host -L '^deduction_engine$'  # 推論の有向試験・形式検証
ctest --preset host -L '^async_queue$'       # 実装に使うXPM非同期FIFOの試験
```

ラベルは検証対象の `sim/` ディレクトリ名。対象を選ぶと関連する検証をまとめて実行する。
`-L` は正規表現なので、対象名の完全一致には `^...$` を付ける。
コア・統合試験は全49,158問を使い、単一コア試験には追加4,096件のストレス入力がある。
`-N` で試験一覧を表示し、個別の試験は `-R` でも選択できる。

```sh
ctest --preset host -N
ctest --preset host -R '^axi$'
ctest --preset host -L '^branch_generator$'
ctest --preset host -L '^reference$'
```

XPM試験は `xvlog` が見つかった場合に登録する。パスを明示する場合:

```sh
cmake --preset host -DXVLOG_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/xvlog
cmake --build --preset host
ctest --preset host -L '^async_queue$'
```

試験の詳細と回帰比較は [sim/README.md](sim/README.md)。必要ツールは
CMake 3.25以降、Ninja、C++20、Python 3、Icarus Verilog、Verilator、Yosys。
XPM試験にはVivadoが必要。`kv260-ps` ではRTL検証ツール・Vivado・カーネルのクロスビルド環境は不要。

### ビルド対象と出力の分離

| プリセット | 実行場所 | 通常のbuildで生成するもの | 出力先 |
|------------|----------|--------------------------|--------|
| `host` | 開発機 | CPU参照テスト、RTLシミュレーター・テストベンチ | `build/host/` |
| `kv260-pl` | 開発機 | FPGA回路・検査レポート・firmwareパッケージ | `build/kv260-pl/` |
| `kv260-ps` | KV260 | カーネルドライバー | `build/kv260-ps/` |

各プリセットは `cmake --build --preset <名前>` で必要なものをまとめてビルドする。
CPU参照・RTL試験は `ctest --preset host`、実機試験は `host/tests/` のPythonを使う。

コア数はプリセットとは独立した `-DFADES_CORES=1`〜`6`（既定6）。
ボードトップからコア群まで伝播し、シミュレーションにも同じ値を使う。
ホストABIの6コア分のカウンターレイアウトは固定で、未実装コア分は0になる。
構成変更時は同じビルドディレクトリで再configureしてからビルドする。

```sh
cmake --preset host -DFADES_CORES=1
cmake --build --preset host
ctest --preset host -L '^fades$'
```

複数構成を保持するときは `-B` で出力先を分ける:

```sh
cmake --preset host -B build/host-1core -DFADES_CORES=1
cmake --build build/host-1core
ctest --test-dir build/host-1core --output-on-failure -L '^fades$'
```

**`-B` で出力先を変えた場合は、build・testでもそのディレクトリを指定する。**

ビルドの並列数を制限したい場合は `--parallel 4` などを追加できる。
これはビルドジョブ数で、FPGAのコア数とは独立。省略時はNinjaの既定の並列度を使う。

ツールのパスなどのローカル設定は `CMakeUserPresets.json` に記述できる（Git管理外）。
一覧は `cmake --list-presets`、試験一覧は `ctest --preset host -N`。

## ソースを読む順序

| 対象         | 読むファイル                                                        | 役割                                        |
|--------------|---------------------------------------------------------------------|---------------------------------------------|
| PS要求仕様   | `host/README.md`                                                  | 要求仕様と削除前の確認内容                  |
| Linux接続    | `host/include/fades_ioctl.h` → `host/driver/fades.c`                | 公開ABI → バッファ・DMA・制御               |
| ボード生成   | `fpga/kv260/scripts/build.tcl`                                      | PS・DMA・クロックの接続、実装、出力検査     |
| PLの入口     | `rtl/fades_axi.sv` → `fades.sv`                                     | 制御 → 供給・ID管理・結果回収・コア群       |
| 探索         | `rtl/fades_core.sv` → `deduction_engine.sv` / `branch_generator.sv` | 状態列・探索 → 推論 / 分岐                  |
| クロック境界 | `rtl/async_queue.sv`                                                | XPM非同期FIFOと試験用モデル                 |
| CPU照合      | `reference/sudoku.hpp`                                              | RTL試験用の独立参照ソルバー                 |

`build/` は生成物、`results/` は生ログ・実験履歴、`docs/` はローカル設計メモ。

## 実測

以下は削除前のホストC++実装による測定記録。

ABI v5 はTX/RX双方が可変のBufferPool API。既存HWを変更せず、入力コピー・ID更新・
RAII投入／返却・最終排出込み、出力内容のCPU読み出しなしで **27.618329 Mproblem/s**
（TX 8 / RX 64、10秒×3回の中央値）。TX 8 / RX 8では **27.512729 Mproblem/s**。
全6回Errors=0、前後各131,072件の全解・ID検証PASS。
TX/RX 65/65など、両方向がSGリング長を超える構成も実機で検証済み。

ABI v4 は入力8面固定・出力面数可変。**出力内容をCPUで読まない**ベンチマークでは、
入力コピー・一意ID更新・RX返却・最終排出込みで **27.618074 Mproblem/s**
（RX 256面、10秒×3回の中央値）。RX 8面では **27.570465 Mproblem/s**。
既存回路をそのまま使い、6回ともErrors=0、前後各131,072件の全解・ID検証PASS。
条件と可変RX／背圧試験は [host/STREAMING_RESULTS.md](host/STREAMING_RESULTS.md) を参照。

ABI v3 の固定スロット・ゼロコピー出力では、**全結果の64 byteをCPUで読む処理込み**で
**18.753654 Mproblem/s**（10秒の最終確認、187,554,154件、Errors=0）。
16,384問 × 入出力8面、6コア約115.385 MHz / DMA 187.5 MHz、入力コピー・RX返却・最終排出を含む。
回路は WNS +0.037 ns / WHS +0.011 ns、BRAM 増加なしで配置配線・実機検証を通過した。

従来の ABI v2 の継続投入は、入力コピー・ID 更新込みで **27.620716 Mproblem/s**
（10秒×3回の中央値）。180秒で **4,971,960,000件**を処理し Errors=0。
1万問 × 8面、6コア約115.385 MHz / DMA 187.5 MHz、最終排出を含む。
測定範囲と検証結果は [host/STREAMING_RESULTS.md](host/STREAMING_RESULTS.md)。
この旧測定は全出力の CPU 読み出しを含まないため、測定条件が異なる。

以下は旧 ABI v1 の準備済み DDR 反復測定で、測定条件が異なる。

KV260 実機で 17 ヒント 49,158 問と Hard 1106 の 375 問を CPU 参照解と照合済み。
2026-11-07、通常の `kv260-pl` ビルド（Vivado 2024.2、6コア、並列数4）で生成した回路を使用。
実クロックはcore **115,384,615 Hz** / DMA **187,499,999 Hz**、kernel `6.8.0-1015-xilinx`。
準備済みDDRの17ヒント集合を10秒×3回測定し、**26.590488 / 26.597169 / 26.597688 Msolve/s**、
中央値 **26.597169 Msolve/s**。131,072件、8 DMAスロット×1 MiBを反復し、最終排出を計測に含む。
CPU参照・入力準備・計測前後の全解検証は計測外。件数は延べ投入数。
全測定Errors=0、各回の計測前後131,072件の全解・ID検証PASS、測定中のdmesg追加0行。
ABI・全49,158問・mixed入力・難問375問の機能試験もPASS。
最終タイミングはcore WNS +0.055 ns / DMA WNS +0.042 ns、全体WHS +0.010 ns、TNS / THS 0。
入力データの出所は [sim/README.md](sim/README.md#問題集)。
