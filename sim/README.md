# 開発・検証の入口

ここは実機ユーザー用のアプリではなく、RTL を変更するときの試験コード。
個々のテストファイルを直接実行せず、リポジトリルートの Make ターゲットから使う。
CPU 照合には PS ツールと共通の `reference/sudoku.hpp` を使用する。

## 何を確認するか

|目的|コマンド|実機 / 必要ツール|
|---|---|---|
|基本的な推論・分岐・制御の試験|`make test`|実機不要、Icarus / C++20|
|単一コアの全問・ストレス試験|`make core-test`|実機不要、Verilator / C++20|
|コア群と供給・回収系の全体試験|`make stream-test`|実機不要、Verilator / C++20|
|RTL の構造・ラッチ不在|`make lint synth`|実機不要、Verilator / Yosys|
|推論・分岐の等価性証明|`make formal`|実機不要、Yosys|
|本物の XPM 非同期 FIFO|`make fifo-test`|実機不要、Vivado xsim|
|問題集の解の存在・一意性|`make reference-full`|実機不要、独立 CPU 参照|
|Linux インターフェース|`python3 host/tests/check_abi.py`|ロード済み KV260|
|PS ツールの入力形式|`make host-input-test`|実機不要|

`core-test` は全49,158問と追加4,096件を検証する。解・ヒント保持・順序に加え、
探索・矛盾・バックトラック、入出力停止、キュー周回、reset とサイクル集計を確認する。
`stream-test` は非調和な2クロックでID対応・結果順・先行投入上限を確認し、
解なし、不正セル値、FIFO満杯、停止中のデータ保持、部分入力・未回収出力のresetも試験する。
短縮するときは `LIMIT=512`、ビルド並列数は `JOBS=4` 等を指定する。

## テストファイルの役割

|ファイル|呼び出すターゲット / 役割|
|---|---|
|`cpp/core_test.cpp`|`core-test`。単一コアをcycle単位で駆動し、CPU照合とCSV出力|
|`cpp/stream_test.cpp`|`stream-test`。64-byteレコードを送受信してコア群を試験|
|`cpp/reference_test.cpp`|`test` / `reference-full`。CPU参照ソルバーとテキスト形式を試験|
|`tb/tb_inference.sv`、`tb/tb_branch.sv`|`test`。推論規則と候補分割の有向試験|
|`tb/tb_solver_axi.sv`|`test` / `axi-test`。AW/W独立受理、WSTRB、応答停止、設定保護|
|`tb/tb_async_fifo_xpm.sv`|`fifo-test`。実際のXPMでfull/count/周回/停止/resetを試験|
|`formal/formal_branch.sv`|`formal`。分岐選択器を独立した逐次仕様と比較|
|`formal/formal_inference.sv`|`formal`。共通解析・推論・行変化検出の仕様と比較|
|`formal/formal_locked_masks.sv`|`formal`。共有したLocked Candidatesマスクを独立仕様と比較|
|`scripts/core_results.py`|`core-test` / `regression`。CSVの集計・完全回帰比較|
|`scripts/xpm_fifo_test.py`|`fifo-test`。VivadoのXPMライブラリーを使ってxsimを起動|
|`data/17puz49158.txt`、`data/HardestDatabase110626.txt`|BinarySudoku原本の問題集。出所は [data/README](data/README.md)|

Verilator の FIFO は64エントリの抽象モデル。XPM のFWFT段・初期化レイテンシを
cycle単位で再現しないため、実FIFOの性質は `fifo-test` で別に確認する。

## サイクル回帰と単一コア実装評価

`core-test` は `build/core/results.csv`、隣接する `.totals.json`、`summary.json` を生成する。
CSVは問題ごとの完了間隔、totalsは最後の出力排出も含む全体値。
既存の完全実行を基準にする場合:

```sh
make regression BASELINE=/path/to/saved/results.csv
```

49,158問+4,096件のCSVとtotalsの完全一致を確認する。保存時には両ファイルが必要。

```sh
make vivado-core PERIOD=9.090909
```

これは `fpga/kv260/build_core.tcl` で単一コアをOOC配置配線する開発用評価。
生成先は `build/vivado_single_core/`、主な結果は `timing_summary.rpt`、
`utilization.rpt`、`summary.txt`。実機用ビットストリームは生成しない。
実機ロード用は [KV260 のビルド](../fpga/kv260/README.md) を使う。
