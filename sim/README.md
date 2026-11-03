# RTL・参照実装の検証

テストは目的別のディレクトリにまとめ、各 `CMakeLists.txt` がソース・生成物・
実行条件を定義する。他の試験のコードや生成物には依存しない。
CPU照合はPSツールと共通の `reference/sudoku.hpp` を使う。

## 実行

必要ツール: CMake 3.25以降、Ninja、C++20、Python 3、Icarus Verilog、Verilator、Yosys。
実XPMの試験にはVivado 2024.2のxsimを使う。

```sh
cmake --preset sim -DFADES_CORES=6
cmake --build --preset sim --parallel 4
ctest --preset sim -L quick
ctest --preset sim -L full
ctest --preset sim -L formal
ctest --preset sim -L xpm
```

ラベルを省略すると登録済みの全試験を実行する。`-R` で名前による選択もできる。
1コア構成は `-DFADES_CORES=1`。並存させる場合は `-B build/sim-check-1core` 等で
出力先を変え、build/CTestにもそのディレクトリを指定する。
コア数を同じ出力先で変更した場合も、再configure・buildで生成コードを更新する。

`xvlog` がPATH上または `$XILINX_VIVADO/bin` にない場合、XPM試験は登録しない。
`-DXVLOG_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/xvlog` で明示指定できる。
問題集は `-DFADES_DATASET=/absolute/path/to/dataset.txt` で変更可能。

## 試験一覧

|ディレクトリ|試験名・ラベル|目的|
|---|---|---|
|`deduction_engine/`|`deduction-engine` / quick|Singles、Locked Candidates、既知問題の推論ステップ|
|同上|`formal-deduction-engine`、`formal-row-changes`、`formal-locked-masks` / formal|推論・行変化・共通マスクと独立仕様の等価性|
|`branch_generator/`|`branch-generator` / quick、`formal-branch-generator` / formal|候補分割の有向試験・独立逐次仕様との等価性|
|`fades_core/`|`core-quick`、`core-full` / quick・full|単一コアのCPU照合、順序、探索・矛盾・バックトラック、入出力停止、キュー周回、reset、サイクル集計|
|同上|`synth-core` / quick・synth|RAMを保持する構造合成とラッチ不在|
|`fades/`|`stream-quick`、`stream-full` / quick・full|コア群・非同期クロック・ID対応、無解・不正セル、満杯・出力停止、部分入力・未回収出力のreset|
|`fades_axi/`|`axi` / quick|AW/W独立受理、WSTRB、応答停止、設定保護、snapshot、未実装コアのカウンター0|
|`async_queue/`|`xpm-queue` / xpm|実XPMのfull/count・周回・停止・reset|
|`reference/`|`reference-quick`、`reference-full` / quick・full|CPU参照実装・入力形式、全問題集の解の存在・一意性|
|全体|`lint-fades_core`、`lint-fades`、`lint-fades_axi`、`lint-fades_kv260` / quick・lint|各階層のVerilator lint|

quickのコア・統合試験は問題集の先頭512件を使い、ストレス試験も実行する。
fullでは既定の全49,158問を使い、コア試験には追加4,096件のストレス入力がある。
Verilator用キューは64エントリの抽象モデル。XPMのFWFT段・初期化レイテンシは
cycle単位では再現しないため、実キューの性質をxsimでも確認する。

`lint`、`synth`、`formal`、`fifo-test` のビルドターゲットから該当検査を実行することもできる。
ホスト試験は [host/README.md](../host/README.md) を参照。

## 出力・サイクル回帰

生成物は選択したビルドディレクトリの `sim/<試験ディレクトリ>/` に置く。
各試験の作業ディレクトリも独立している。

コア試験は `sim/fades_core/quick/` と `full/` に、それぞれ `results.csv` と
`results.csv.totals.json` を生成する。CSVは問題ごとの完了間隔、totalsは最後の出力排出を含む全体値。

```sh
ctest --preset sim -R '^core-full$'
cmake --build --preset sim --target core-summary

cmake --preset sim -DFADES_BASELINE=/absolute/path/to/saved/results.csv
cmake --build --preset sim --target regression
```

`core-summary` はfullのCSVから `summary.json` を生成する。
`regression` はベースラインの指定時に有効になり、49,158問+4,096件のCSVとtotalsの
完全一致を確認する。ベースラインは両ファイルを保存すること。

単一コアのHWはボード実装を `FADES_CORES=1` で生成する。
詳細は [KV260ビルド手順](../fpga/kv260/README.md)。

## 問題集

`data/` の2ファイルはBinarySudokuの原本で、ファイル名・内容・CRLFを保持する。
1行81文字、空欄 `.`、ヒント `1`〜`9`、行優先。

|ファイル|問題数|用途|
|---|---:|---|
|`17puz49158.txt`|49,158|17ヒント集合、試験と測定の既定入力|
|`HardestDatabase110626.txt`|375|探索負荷の大きい難問集合|

出所: [BinarySudoku problems/9x9](https://github.com/void-hoge/binarysudoku/tree/a494907924083cbfc9e14a74c75f614ead752c28/problems/9x9)
（commit `a494907924083cbfc9e14a74c75f614ead752c28`）。`.gitattributes` で原本の改行を維持する。
