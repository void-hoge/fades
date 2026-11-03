# RTL・参照実装の検証

テストは目的別のディレクトリにまとめ、各 `CMakeLists.txt` がソース・生成物・
実行条件を定義する。他の試験のコードや生成物には依存しない。
CPU照合はPSツールと共通の `reference/sudoku.hpp` を使う。

## 実行

必要ツール: CMake 3.25以降、Ninja、C++20、Python 3、Icarus Verilog、Verilator、Yosys。
実XPMの試験にはVivado 2024.2のxsimを使う。

```sh
cmake --preset host -DFADES_CORES=6
cmake --build --preset host
ctest --preset host
ctest --preset host -L '^fades_core$'
ctest --preset host -L '^fades$'
ctest --preset host -L '^triad$'
ctest --preset host -L '^async_queue$'
```

`host` はC++ツールとRTLシミュレーションをまとめてビルドし、ドライバーは生成しない。
ラベルは `sim/` のディレクトリ名と同じで、検証対象を指定すると、その対象の試験をまとめて実行する。
`-L` は正規表現なので `^...$` で完全一致を指定する。
ラベルを省略するとホスト入力試験も含めた登録済みの全試験を実行する。
`ctest --preset host -N` で一覧を表示し、`-R` で個別の試験名を選ぶこともできる。
1コア構成は `-DFADES_CORES=1`。並存させる場合は `-B build/host-1core` 等で
出力先を変え、build/CTestにもそのディレクトリを指定する。
コア数を同じ出力先で変更した場合も、再configure・buildで生成コードを更新する。

`xvlog` がPATH上または `$XILINX_VIVADO/bin` にない場合、XPM試験は登録しない。
`-DXVLOG_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/xvlog` で明示指定できる。
問題集は `-DFADES_DATASET=/absolute/path/to/dataset.txt` で変更可能。

## 試験一覧

| ディレクトリ／ラベル | 試験名 | 検証対象・目的 |
|----------------------|--------|----------------|
| `deduction_engine` | `deduction-engine`、`formal-deduction-engine`、`formal-row-changes`、`formal-locked-masks` | Singles、Locked Candidates、推論ステップ、推論・行変化・共通マスクと独立仕様の等価性 |
| `branch_generator` | `branch-generator`、`formal-branch-generator` | 候補分割の有向試験・独立逐次仕様との等価性 |
| `triad` | `triad-model`、`formal-triad-box`、`formal-triad-branch`、`formal-triad-split` | スカラー仕様とのフェーズ・分岐照合、解候補の保持、均等分割・状態被覆の形式検証、2段選択器の照合 |
| `fades_core` | `fades-core`、`fades-core-hard`、`synth-core`、`lint-fades_core` | コア1個のCPU照合・探索・バックトラック・順序・停止・キュー周回・reset・サイクル集計、構造合成、lint |
| `fades` | `fades`、`lint-fades` | `FADES_CORES` 個のコア群と供給・ID管理・回収・非同期キューの統合試験、lint |
| `fades_axi` | `axi`、`lint-fades_axi`、`lint-fades_kv260` | AXI-LiteのAW/W独立受理・WSTRB・応答停止・設定保護・snapshot・未実装コアのカウンター0、AXI境界とボードラッパーのlint |
| `async_queue` | `async-queue-xpm` | Xilinx提供の非同期FIFOモデルでfull/count・周回・停止・resetを検証（Vivado xsim） |
| `reference` | `reference-dataset` | CPU参照実装・入力形式の基本試験と、全問題集の解の存在・一意性 |

コア・統合・CPU参照試験は既定の全49,158問を使い、コア試験には追加4,096件のストレス入力がある。
`fades-core-hard` は難問375問と4,096件のストレス入力を使う。
コアは512状態の半分を公開入力から事前投入し、SDP書き込みの仲裁も検査する。
探索なしで検出できるtriad個数矛盾と、独立参照で解なしを確認しtriad固定点からも
探索が必要な矛盾を区別して検証する。
`fades-core` のDUTは常に単一の `fades_core`。`fades` のDUTは既定6コアの `fades` 全体で、
`FADES_CORES` でコア数を変更できる。AXI-Lite制御やPS・DMAを含むボード全体は含まない。
統合試験は解なし・不正セル・満杯・出力停止・部分入力と未回収出力のresetも検証する。
Verilator用キューは64エントリの抽象モデル。XPMのFWFT段・初期化レイテンシは
cycle単位では再現しないため、実キューの性質をxsimでも確認する。

`lint`、`synth`、`formal`、`fifo-test` のビルドターゲットから該当検査を実行することもできる。
ホスト試験は [host/README.md](../host/README.md) を参照。

## Triad の性能評価（実装前のゲート）

参照モデルだけで推論スケジュールと分割方法を比較する。

```sh
cmake --build --preset host --target triad_benchmark
build/host/sim/triad/triad_benchmark sim/data/17puz49158.txt
build/host/sim/triad/triad_benchmark sim/data/HardestDatabase110626.txt
build/host/sim/triad/triad_benchmark --probes
# 特定方式だけを比較する場合
build/host/sim/triad/triad_benchmark sim/data/17puz49158.txt forward-recompute-1tick-bound
```

全問について独立CPU参照と解を照合する。`alternating-balanced` は現試作の
2フェーズ、`parallel-*` は同じ入力状態から両規則を並列適用するモデル。
`early` はセル完成時に保存済みのnegative triad候補・帯の配置候補との整合性も検査して終了し、`commit` は
均等二分の代わりに1配置 / 残りへ分割する。

サイクル数は推論・分岐・兄弟枝破棄後の読出しから見積もる。現RTLとの境界差は
数cycleだが、新方式で増えるRAM待ちや検査回路の遅延は含まない。
6コア100 MHz換算値は、面積・周波数・負荷分散が成立すると仮定した見積もりであり、
実機性能の主張ではない。性能改善が見えない案は合成・配置配線へ進めない。

追加の診断ポリシー:

- `parallel-naked` / `parallel-singles` / `parallel-singles-locked` は、現在状態を
  共通に読む直接セル伝播を追加する。回路面積を追加するため採用とは別の診断。
- `seeded` はTdokuと同様にヒントからセル候補・negative triad候補・帯の配置候補を初期化し、1cycleを課金する。
- `lazy` はセル規則が進む間は補助状態を更新せず、停滞時にtriadを適用する。
- `cell` は従来の2候補優先・セル候補均等二分を使用する。
- `legacy-cell-model` は従来ルール・分岐のモデル。17ヒント集合の推論回数・分岐回数は
  保存済みRTL結果と一致し、未モデル化のRAM待ちなどによる総cycle差は約0.05%。
- `forward-*` はブロックで得た確定要求をnegative triad候補の保存を経由せず帯へ直接送る。
  `recompute` は配置削減後のpositive triadから制限を返す。
  いずれも**組み合わせ論理の直列依存がある**。`1tick-bound` は1cycleへ融合できる
  場合の楽観的上限、`2tick` は同じ更新を2cycleとして課金した比較。周波数の保証はない。

直接伝播は強制衝突フラグの登録に必要な1cycleも課金する。
`early` は完成セルと保存済みのnegative triad候補・帯の配置候補の整合性を検査するため、セルだけを見て解を返さない。

今回の評価では、17ヒント集合は従来RTLの26.06 cycle / 問に対し、
2フェーズ66.61、並列更新41.60、並列更新＋早期完成40.60 cycle / 問。
難問は並列更新でも約996 cycle / 問（6コア100 MHz換算約0.60 M問/s）で、
初期試作を採用する根拠が得られていない。ボードビルドは停止している。

直接規則追加では23.50 cycle / 問、早期完成込み21.83だが、5コアへ減ると従来6コアを
上回れない。規則追加なしのforward/recomputeは、1cycleへ融合できる楽観条件で20.67、
2cycleでは39.50。5コアの場合の損益分岐は約95 MHzで、面積・周波数を含む採用条件を
満たしたという結論ではない。詳細な原因・条件はローカルの `docs/triad-evaluation.md`。

## 出力・サイクル回帰

生成物は選択したビルドディレクトリの `sim/<試験ディレクトリ>/` に置く。
各試験の作業ディレクトリも独立している。

コア試験は `sim/fades_core/` に `results.csv` と
`results.csv.totals.json` を生成する。CSVは問題ごとの完了間隔、totalsは最後の出力排出を含む全体値。
難問試験の出力は `hard-results.csv` とその `.totals.json`。

```sh
ctest --preset host -L '^fades_core$'
cmake --build --preset host --target core-summary

cmake --preset host -DFADES_BASELINE=/absolute/path/to/saved/results.csv
cmake --build --preset host --target regression
```

`core-summary` はコア試験のCSVから `summary.json` を生成する。
`regression` はベースラインの指定時に有効になり、49,158問+4,096件のCSVとtotalsの
完全一致を確認する。ベースラインは両ファイルを保存すること。

単一コアのHWはボード実装を `FADES_CORES=1` で生成する。
詳細は [KV260ビルド手順](../fpga/kv260/README.md)。

## 問題集

`data/` の2ファイルはBinarySudokuの原本で、ファイル名・内容・CRLFを保持する。
1行81文字、空欄 `.`、ヒント `1`〜`9`、行優先。

| ファイル                    | 問題数 | 用途                               |
|-----------------------------|-------:|------------------------------------|
| `17puz49158.txt`            | 49,158 | 17ヒント集合、試験と測定の既定入力 |
| `HardestDatabase110626.txt` |    375 | 探索負荷の大きい難問集合           |

出所: [BinarySudoku problems/9x9](https://github.com/void-hoge/binarysudoku/tree/a494907924083cbfc9e14a74c75f614ead752c28/problems/9x9)
（commit `a494907924083cbfc9e14a74c75f614ead752c28`）。`.gitattributes` で原本の改行を維持する。
