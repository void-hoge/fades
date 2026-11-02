# FADES: FPGA-Accelerated Deductive and Explorative Sudoku Solver

KV260 向けの SystemVerilog 数独ソルバー。Singles / Locked Candidates を
並列適用し、停滞時は候補を二分して深さ優先探索する。
PS の Linux プログラムから DDR / AXI DMA 経由で問題を投入し、ID 付きの結果を回収する。

現行の実機構成は **6 コア × 100 MHz、DMA 200 MHz**。
最初の解を返し、解なしも結果を 1 件返す。問題の一意性を証明する処理ではない。

## 構成

```text
rtl/
  inference.sv        候補解析・推論・完成/矛盾判定
  branch.sv           分岐セル選択・候補の二分
  core.sv             単一コアの探索・BRAM 状態列
  async_fifo.sv       XPM 非同期 FIFO とシミュレーションモデル
  solver_stream.sv    入出力変換・配布・ID 管理・結果回収・コア群
  solver_axi.sv       AXI-Lite 制御・カウンター snapshot
fpga/
  kv260/              実機ラッパー・overlay・FPGA アプリ定義・操作手順
  constraints/        OOC・CDC・ファン端子の制約
  scripts/            Vivado ビルドとレポート出力
host/
  include/            ユーザー空間 / カーネル共通 ioctl ABI
  driver/             Linux DMAengine クライアント
  tools/              解照合ベンチマーク・時間指定ストレステスト
  tests/              実機 ABI 試験・小規模入力
sim/                  RTL/CPU テスト・形式検証・参照ソルバー・入力データ
docs/                 回路構成・プロトコル・検証手順・測定結果
```

`build/` は生成物、`results/` はローカルの生ログ・実験履歴。
公開する構成説明と測定要約は `docs/` に置く。

## ビルドと検証

必要ツール: GNU Make、Python 3、C++20、Icarus Verilog、Verilator、Yosys。
ボード回路の生成には Vivado 2024.2 と K26 デバイスサポート、overlay には `dtc` を使う。

```sh
make test                        # 推論・分岐・CPU 参照・AXI-Lite の機能試験
make lint                        # core / stream / AXI-Lite / ボードラッパー
make synth                       # 単一コアの RAM 構造・ラッチ不在を確認
make formal                      # 推論・分岐・変化検出の等価性証明
make reference-full              # 49,158 問の解の存在・一意性を CPU で確認
make core-test JOBS=4             # 単一コア全問・ストレス試験
make stream-test JOBS=4           # コア群・非同期入出力・ID・解なし・リセット
make fifo-test                   # Vivado xsim で実際の XPM FIFO を試験
make host                        # このマシン向けの PS ツールを生成
make public-check                # Git公開対象だけのコピーでビルドと文書リンクを確認
```

```sh
make vivado-core PERIOD=9.090909  # 単一コア OOC、110 MHz 目標
make vivado-kv260 JOBS=2          # PS/DMA/コア群を含む実機用回路
make package-kv260               # build/kv260/firmware/fades/ にパッケージ
```

OOC と実機ロード用回路は別ターゲット。実機のコア周波数は 100 MHz であり、
単一コア OOC の 110 MHz とは区別する。
実機への転送・Arm64 ビルド・ロードは [KV260 手順](fpga/kv260/README.md) を参照。

## PS から使う

Linux デバイスは **`/dev/fades`**、モジュールと FPGA アプリの名前は **`fades`**。
TX/RX 各 8 MiB の coherent DMA バッファを `mmap` し、`ioctl` で転送・完了待ちを行う。
問題も結果も 64 byte。結果は回収順に届くため、64-bit request ID で対応付ける。

ボード上での例:

```sh
build/host/fades_verify sim/data/17clues.txt 8193 6 1  # 全49,158問の解を照合
build/host/fades_benchmark sim/data/17clues.txt 10     # 10秒の性能測定
python3 host/tests/check_abi.py
```

[PS ソフトウェア](host/README.md) に引数・検証範囲・計測時間の定義を記載する。
入力テキストは **1 行 81 文字、空欄 `.`、ヒント `1〜9`、セル間の空白なし**。
LF / CRLF に対応し、空行は読み飛ばす。同梱ファイルは BinarySudoku の
`17puz49158.txt` と同じ内容・改行を使用する。

## 実測

KV260 実機で 17 ヒント 49,158 問と Hard 1106 の 375 問を CPU 参照解と照合済み。
準備済み DDR の 10 秒ストレス測定では、17 ヒント集合 **22.34 Mproblem/s**、
Hard 1106 **0.7032 Mproblem/s**。入力準備と前後の全解検証は計測外。
条件・資源量・タイミング・検証範囲は [測定結果](docs/measurements.md) を参照。

## 詳細

- [回路構成とコア設計](docs/architecture.md)
- [ストリーム・AXI-Lite・Linux ABI](docs/protocol.md)
- [検証手順](docs/verification.md)
- [入力データの出所と形式](sim/data/README.md)
