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
ctest --preset host -L '^segment_rules$'
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
| `segment_rules` | `segment-model-*`、`segment-core-*`、`lint-segment-core-*` | 固定3セルの局所矛盾・限定Naked Tripleの比較モデル、3モードのRTL照合・コア試験・lint |
| `fades_core` | `fades-core`、`synth-core`、`lint-fades_core` | コア1個のCPU照合・探索・バックトラック・順序・停止・キュー周回・reset・サイクル集計、構造合成、lint |
| `fades` | `fades`、`lint-fades` | `FADES_CORES` 個のコア群と供給・ID管理・回収・非同期キューの統合試験、lint |
| `fades_axi` | `axi`、`lint-fades_axi`、`lint-fades_kv260` | AXI-LiteのAW/W独立受理・WSTRB・応答停止・設定保護・snapshot・未実装コアのカウンター0、AXI境界とボードラッパーのlint |
| `async_queue` | `async-queue-xpm` | Xilinx提供の非同期FIFOモデルでfull/count・周回・停止・resetを検証（Vivado xsim） |
| `reference` | `reference-dataset` | CPU参照実装・入力形式の基本試験と、全問題集の解の存在・一意性 |

コア・統合・CPU参照試験は既定の全49,158問を使い、コア試験には追加4,096件のストレス入力がある。
`fades-core-hard` は難問375問とストレス入力を検証する。小さい問題集では先行投入数を
512件から問題数まで減らす。固定3セル規則の評価方法・結果は
[segment_rules/README.md](segment_rules/README.md) を参照。
`fades-core` のDUTは常に単一の `fades_core`。`fades` のDUTは既定6コアの `fades` 全体で、
`FADES_CORES` でコア数を変更できる。AXI-Lite制御やPS・DMAを含むボード全体は含まない。
統合試験は解なし・不正セル・満杯・出力停止・部分入力と未回収出力のresetも検証する。
Verilator用キューは64エントリの抽象モデル。XPMのFWFT段・初期化レイテンシは
cycle単位では再現しないため、実キューの性質をxsimでも確認する。

`lint`、`synth`、`formal`、`fifo-test` のビルドターゲットから該当検査を実行することもできる。
ホスト試験は [host/README.md](../host/README.md) を参照。

## 出力・サイクル回帰

生成物は選択したビルドディレクトリの `sim/<試験ディレクトリ>/` に置く。
各試験の作業ディレクトリも独立している。

コア試験は `sim/fades_core/` に `results.csv` と
`results.csv.totals.json` を生成する。CSVは問題ごとの完了間隔、totalsは最後の出力排出を含む全体値。

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
