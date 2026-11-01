# FADES: FPGA-Accelerated Deductive and Explorative Sudoku Solver

KV260 (K26) 向けの単一コア数独ソルバー。RTL は SystemVerilog。
Singles / Locked Candidatesを並列適用し、停滞時は3クラス二分7段の
選択器で2候補セルを優先して選び、候補を二分して深さ優先探索する。

## 構成

```text
rtl/
  inference.sv       共通解析・推論・完成/矛盾判定・行単位変化検出
  branch.sv          3クラス二分7段のセル選択・候補の均等二分
  core.sv            現在状態・BRAM状態列・探索/入出力制御
sim/
  tb/                SystemVerilog機能テスト
  cpp/               独立CPU参照ソルバー・Verilatorテストドライバー
  formal/            等価性証明用の独立参照仕様
  data/              17ヒント49,158問
  scripts/           集計・回帰比較
fpga/
  kv260/top.sv       PL内部接続用の単一コアラッパー
  constraints/       OOC制約
  scripts/           Vivado合成・配置配線・タイミングレポート
```

`build/`（生成物）、`results/`（保存する測定結果）、`docs/`（設計メモ）は
Git管理対象外。必要に応じて作成する。

## アーキテクチャ

- 81セル×9候補 = 729ビット。セル `i = row*9+col` は `[9*i +: 9]`、
  数字 `d` はビット `d-1`。空欄は `9'h1ff`、ヒントはone-hot。
- `inference` の全規則は同じ現在状態を参照する。Naked/Hidden Single、
  Locked CandidatesのPointing/Claimingを実装。
- 解析は推論と判定で共有。9セルの出現集約は三分2段。
- Locked Candidatesの除去マスクは行・列それぞれの3マス区間ごとに合成し、
  同じ区間の3セルで共有する。
- 推論と現在状態の完成/矛盾判定を並列実行。次候補と行別変化9ビット、
  同時強制矛盾フラグを保持し、翌サイクルで停止を判断する。
  停止時にはそのサイクルの推論結果を破棄する。
- セル選択は2ビットの3クラス（2候補、3候補以上、対象外）の順に優先する。
  最下位の1を最大2回除去して分類し、分岐用popcountとは独立に選択する。
  判定は `branch` 側で行い、`branch_select` には81ビットずつの未確定・2候補フラグを渡す。
  選択器のモジュール境界・条件式は、面積を確認した旧式の記述を維持する。
  81セル＋無効47葉を二分7段 `128 → 64 → 32 → 16 → 8 → 4 → 2 → 1` で集約。
  同クラスなら行優先で最初のセルを選び、3～9候補の大小は区別しない。
  選択セルの数字順で前半
  `floor(n/2)` を子A、残りを子Bとし、子Aから探索する。
- 状態列は729ビット×1,024の同期二重ポートBRAM。先頭側に未探索枝、
  末尾側に新規問題を置く。先頭キャッシュはBRAMエントリの複製。
  ポートAは枝保存/先頭補充、ポートBは新規問題の入力。
- 最大324枝と空き判別用1エントリを予約する。未着手問題は最大699件。
  一つの問題の最初の解を返すと残りの枝をまとめて破棄する。
  解なしの場合も結果を1件返し、入力順序を維持する。

## インターフェース

`core` と `fpga/kv260/top.sv` は同じ単一コアのインターフェースを持つ。

| ポート | 意味 |
|---|---|
| `clk`, `reset` | 立ち上がり更新、同期Highリセット |
| `enable` | 探索の実行許可。入力受け入れ・結果転送は独立 |
| `input_valid`, `input_ready`, `input_candidates[728:0]` | 両方Highの立ち上がりで問題を受け取る |
| `result_valid`, `result_ready` | 両方Highの立ち上がりで結果を転送 |
| `result_solved`, `result_candidates[728:0]` | 解の有無と候補行列。解なし時の行列は解ではない |
| `queued[10:0]`, `pending_branches[10:0]` | 状態列占有数と未探索枝数 |
| `busy` | 現在の問題を処理中。結果待ち・列全体の空判定とは別 |
| `event_*` | 立ち上がり直前に観測する計測イベント |

入力側は `valid && !ready` 中にデータを保持する。出力も同様に保持される。
リセットは実行中の問題・状態列・未転送結果を無効化する（RAM本体は初期化しない）。
`event_infer/branch/load/memory_wait/output_wait` は排他的なサイクル分類。
`event_contradiction/finish/skip/wasted_infer` は付加イベントであり排他的ではない。

## 検証

必要ツール: GNU Make、Python 3、C++20対応コンパイラ、Icarus Verilog、
Verilator、Yosys。FPGA実装にはVivadoとK26デバイスサポートが必要。
BinarySudokuなど外部ソルバーへのビルド・実行依存はない。

```sh
make test                    # 推論規則・分岐選択/候補二分・CPU参照の機能テスト
make lint                    # コアとKV260ラッパー
make synth                   # RAMを保持する構造合成・ラッチ不在チェック
make formal                  # 共通解析、除去マスク共有、行変化、3クラス選択の等価性証明
make reference-full          # 49,158問の一意性をCPUで検証
make core-test JOBS=4         # 全49,158問＋4,096件のストレス検証
make core-test LIMIT=512      # 短縮実行（最低512問、性能測定には全問を使う）
make regression BASELINE=/path/to/results.csv
make branch-compare BASELINE=/path/to/other_policy/results.csv
```

参照実装は候補ビット集合、Naked/Hidden Single、Pointing/Claiming、MRV探索を
備える。RTLとは独立に逐次推論し、必要に応じて最大2解まで列挙する。
全問テストは各問題につき1解を生成し、RTLの解と照合する。
出力の数独制約と入力候補保持も独立に検査する。

ストレステストでは完成済み、複数解、解なし、探索による矛盾、強制矛盾、
入出力待ち、実行停止、キュー周回、同時書き込み、探索途中リセットを検証する。
結果は `build/core/results.csv`、`results.csv.totals.json`、`summary.json`。
回帰比較にはCSVと隣接する `.totals.json` が必要。ベースラインはローカルに
保存し、全問・ストレスの完走とCSV/集計の一致を検査する。
分岐方針が異なる場合は `branch-compare` で完走・問題順・列容量を確認し、
サイクル数を比較する（方針間での完全一致は要求しない）。

## FPGA実装

```sh
make vivado-core PERIOD=9.090909                    # コア単体、110MHz目標
make vivado-kv260                                  # 単一コアラッパー込み、110MHz目標
```

対象は `xck26-sfvc784-2LV-c`。OOCフローで合成・配置配線し、資源量・
setup/hold/pulse width・経路別タイミングをレポートする。
729ビット入出力はPL内部接続を想定し、外部I/Oのタイミングはfalse path。
ボードのピン接続・PS/AXI・DMA接続はこのラッパーに含まれない。
出力は `build/vivado_single_core/`。
要求周期で未達の場合は同じ配線を緩い周期で
再解析し、要求時と再解析時の結果を区別して保存する。

詳細な検証・性能測定記録はローカルの `results/` に保存する。
性能は本構成の全問サイクル数と配置配線結果から算出する。
現行の単一コアの測定記録は `results/single_core/`、
`results/six_core/` は過去の6コア実験の履歴。
