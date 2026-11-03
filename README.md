# FADES: FPGA-Accelerated Deductive and Explorative Sudoku Solver

KV260 向けの SystemVerilog 数独ソルバー。セル・triad・帯の配置候補の制約を
並列に伝播し、停滞時は配置候補を均等二分して深さ優先探索する。
PS の Linux プログラムから DDR / AXI DMA 経由で問題を投入し、ID 付きの結果を回収する。

ビルドの既定構成は **6 コア × 100 MHz、DMA 200 MHz**。コア数は1〜6で指定できる。
末尾の実機性能値は従来の Singles / Locked Candidates・セル分岐回路の測定値。
triad 回路は性能評価中の試作。現シミュレーションでは従来回路より遅いため、
ボードビルドは停止し、モデルで性能改善を確認してから実装を進める。
最初の解を返し、解なしも結果を 1 件返す。問題の一意性を証明する処理ではない。

ビルド・検証の基本操作は **configure → build → test**。
以下のコマンドは、対象の開発機またはKV260上のリポジトリルートで実行する。

## 1. 実機を使う

FPGA アプリとドライバーをロード済みの KV260 上で実行する。
提供する入口は、時間指定の性能測定と、件数指定の正当性確認の2つ。

```sh
cmake --preset kv260-ps                            # KV260上でconfigure
cmake --build --preset kv260-ps                     # C++ツールとドライバーを生成
ctest --preset kv260-ps                            # 入力形式・不正入力の拒否試験

# 10秒の性能測定
build/kv260-ps/host/fades_benchmark sim/data/17puz49158.txt 10
# 全49,158問の正当性確認
build/kv260-ps/host/fades_verify sim/data/17puz49158.txt 8193 6 1
# 難問375問の正当性確認
build/kv260-ps/host/fades_verify sim/data/HardestDatabase110626.txt 375 1 1
```

引数は次のとおり。

```text
fades_benchmark 問題ファイル 測定秒数
fades_verify 問題ファイル 1パケットの問題数 DMAスロット数 反復回数 [mixed]
```

**`8193 6 1` の `6` はDMAスロット数であり、コア数ではない。**
コア数は回路生成時の `FADES_CORES` で指定する。
`mixed` を付けると、解なし・不正セルを含む入力も検証する。

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
cmake --preset kv260-pl -DFADES_CORES=6 -DFADES_VIVADO_JOBS=2
cmake --build --preset kv260-pl
```

通常のbuildで、回路生成・出力検査からfirmwareパッケージ生成まで実行する。
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
ctest --preset kv260-ps
```

通常のbuildでC++ツールと `build/kv260-ps/host/driver/fades.ko` を生成する。
実機自身のコンパイラーと実行中カーネルのヘッダーを使う。
firmwareを配置し、`xmutil loadapp fades` と `insmod fades.ko` を実行すると **`/dev/fades`** が使える。
転送・Arm64 ビルド・ロードの手順と、各ボード用ファイルの役割は
[fpga/kv260/README.md](fpga/kv260/README.md) にまとめる。

## 3. 開発・検証する

```sh
cmake --preset host -DFADES_CORES=6
cmake --build --preset host
ctest --preset host                         # 全試験（ホスト入力試験も含む）
ctest --preset host -L '^fades_core$'        # コア単体：全問題集・ストレス・構造合成・lint
ctest --preset host -L '^fades$'             # 指定コア数で供給・回収を含む統合試験・lint
ctest --preset host -L '^deduction_engine$'  # 推論の有向試験・形式検証
ctest --preset host -L '^triad$'             # triad推論・配置分岐のモデル照合・形式検証
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
| `host` | 開発機 | C++ツール、CPU参照テスト、RTLシミュレーター・テストベンチ | `build/host/` |
| `kv260-pl` | 開発機 | FPGA回路・検査レポート・firmwareパッケージ | `build/kv260-pl/` |
| `kv260-ps` | KV260 | 実機用C++ツール・カーネルドライバー | `build/kv260-ps/` |

各プリセットは `cmake --build --preset <名前>` で必要なものをまとめてビルドする。
テストの実行は `ctest --preset host` または `ctest --preset kv260-ps` で行う。

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

開発機用のC++実行ファイルは `build/host/host/`、実機用は `build/kv260-ps/host/` に生成する。
ビルドの並列数を制限したい場合は `--parallel 4` などを追加できる。
これはビルドジョブ数で、FPGAのコア数とは独立。省略時はNinjaの既定の並列度を使う。

ツールのパスなどのローカル設定は `CMakeUserPresets.json` に記述できる（Git管理外）。
一覧は `cmake --list-presets`、試験一覧は `ctest --preset host -N`。

## ソースを読む順序

| 対象         | 読むファイル                                                        | 役割                                        |
|--------------|---------------------------------------------------------------------|---------------------------------------------|
| PS利用       | `host/tools/fades_benchmark.cpp` / `fades_verify.cpp`               | 性能測定 / 解の照合                         |
| Linux接続    | `host/include/fades_ioctl.h` → `host/driver/fades.c`                | 公開ABI → バッファ・DMA・制御               |
| ボード生成   | `fpga/kv260/scripts/build.tcl`                                      | PS・DMA・クロックの接続、実装、出力検査     |
| PLの入口     | `rtl/fades_axi.sv` → `fades.sv`                                     | 制御 → 供給・ID管理・結果回収・コア群       |
| 探索         | `rtl/fades_core.sv` → `triad_engine.sv` | 状態列・探索 → triad推論 / 配置分岐                  |
| クロック境界 | `rtl/async_queue.sv`                                                | XPM非同期FIFOと試験用モデル                 |
| CPU照合      | `reference/sudoku.hpp`                                              | PSツールとRTL試験が共有する独立参照ソルバー |

`build/` は生成物、`results/` は生ログ・実験履歴、`docs/` はローカル設計メモ。

## Triad コア

探索状態は次の1,539 bitで、セル候補だけでは表現できない配置間の制約も保存する。

|ビット範囲|内容|
|---|---|
|`[728:0]`|Cell candidates：81セル × 9数字のセル候補|
|`[1214:729]`|Negative triad candidates：9ブロック × 横縦6 triad × 9数字の不在命題の候補|
|`[1538:1215]`|Configuration candidates：横縦6帯 × 6配置 × 9数字の配置候補|

triad はブロック内の横または縦の3セル。negative 候補は「その数字が3セルに
入らない」という命題がまだ成立可能であることを表す。帯では、各数字の
3ブロックにわたる配置を6通りの順列で表す。positive triad は配置候補から生成する。
negative 候補のビットが1でも、不在が確定したという意味ではない。
以前の説明のC/N/Pはこの3種類の状態の略記であり、推論規則の
Claiming / Naked Single / Pointingの略ではない。

推論はブロック側と帯側を交互に更新する。両フェーズで全状態が不変になったら
固定点とし、完成なら結果を返し、それ以外は配置候補の少ない帯と数字を選ぶ。
選んだ最大6配置を前半 `floor(n/2)` と残りへ二分し、前半から探索する。
帯・数字の選択は推論中に2段のレジスターを通して先行計算する。

状態列は **1,539 bit × 512状態 / core** の同期SDP BRAM。探索枝保存と入力を
1つの書き込みポートで仲裁し、探索枝保存を優先する。均等配置分岐により、
最大深さは `6 × 9 × ceil(log2(6)) = 162`。兄弟枝162件と空き判別1件を予約する。

`reference/triad.hpp` は推論・分岐の検証用スカラーモデル。
解の独立照合には引き続き `reference/sudoku.hpp` を使う。
`rtl/deduction_engine.sv` と `rtl/branch_generator.sv` の旧推論・セル分岐は
局所試験と比較用に残し、共通セル検査 `analysis` は新コアからも使用する。

## 実測

以下は従来回路の比較基準。

KV260 実機で 17 ヒント 49,158 問と Hard 1106 の 375 問を CPU 参照解と照合済み。
準備済み DDR の 10 秒ストレス測定では、17 ヒント集合 **22.34 Mproblem/s**、Hard 1106 **0.7032 Mproblem/s**。入力準備と前後の全解検証は計測外。
6コア100 MHz / DMA 200 MHz、131,072件を反復する条件で、件数は延べ投入数。
計測中は DMA 完了・PL件数を検査し、前後のバッチを全件検証する。
入力データの出所は [sim/README.md](sim/README.md#問題集)。
