# 固定3セルの集合規則：シミュレーション評価

基準はmainの `bd7ce0a`。triad移植ではなく、従来のセル候補729 bit、
Singles / Locked Candidates、セル候補二分探索に小さい規則を追加して比較する。
前半にモデル・RTLシミュレーション、後半に配置配線と実機A/B測定の条件・結果を記録し、
サイクル数、タイミング制約、実行時のクロック、実機スループットを区別する。

## 比較条件

`SEGMENT_RULES` はRTLの実験パラメーターで、CMakeの `FADES_SEGMENT_RULES` から
ボードトップ・AXI・ストリーム・コア・推論エンジンへ伝播する。
既定値は0で、通常のビルドは従来動作を使う。

|値|規則|
|---|---|
|0|従来のSingles＋Pointing / Claiming|
|1|上記＋固定3セルの候補和が3種類未満なら矛盾|
|2|上記＋候補和がちょうど3種類なら、同じ行・列・ブロックの対象外セルからその3数字を除去|

対象はブロック内の横3セル27組・縦3セル27組。既存の `row_segment` /
`col_segment` を再利用する。値2はNaked Tripleをこの54組に限定したもの。
確定済みセルも含めて候補和を調べ、未確定3セルの一般的な組み合わせ探索は行わない。
選択した3セル自身からは除去せず、行・列・ブロックの制約を共有する他セルへ適用する。

全規則・矛盾検査は同じ現在状態を読む。推論結果を別の推論へその場で渡さず、
従来と同じ1更新 / clockの構造で比較する。分岐方法、状態幅、RAM深さは同じ。

## 実行方法

```sh
cmake --preset host -B build/segment-eval -DFADES_CORES=6 -DFADES_BUILD_HOST=OFF
cmake --build build/segment-eval --target segment_benchmark \
    segment_model_test_0 segment_model_test_1 segment_model_test_2 \
    core_test core_test_segment_1 core_test_segment_2 --parallel 4

# 規則・モデル照合、両問題集のコア試験、実験モードのlint
ctest --test-dir build/segment-eval \
    -R '^(segment-|lint-segment-|fades-core)' --output-on-failure -j 3

# 同じステップ・探索方針のスカラーモデル。独立参照解と全問照合する。
build/segment-eval/sim/segment_rules/segment_benchmark sim/data/17puz49158.txt
build/segment-eval/sim/segment_rules/segment_benchmark sim/data/HardestDatabase110626.txt
```

モデルは `segment_model.hpp`。従来規則は数字の位置を逐次走査して表現し、
RTLのマスクネットワークとは独立に記述する。8,000状態の比較に加え、
全54位置 × 64候補パターンで既知の正解候補の保持を検査し、局所矛盾と除去先も調べる。
各モードのRTL出力・完成・矛盾・強制衝突をモデルと照合する。

モデルのcycle見積もりは推論・分岐・兄弟枝破棄後の読み出しを含むが、
全RAM待ちは含まない。最終的な性能比較には、以下のクロック単位RTL試験の結果を使う。

## RTLシミュレーション結果

同じ入力順、入力・出力停止なし、単一コア。解とヒント保持は独立CPU参照で検査。
17ヒントは先行投入512件、難問375問は全375件を先行投入し、各問題集内では3モードで
同じ条件にする。総cycleは推論・探索・RAM待ちと最後の結果排出を含む。
4,096件のストレス試験は別シナリオであり、下表には含めない。

### 17ヒント49,158問

|モード|総cycle|cycle / 問|推論cycle|分岐回数|同クロック速度比|
|---|---:|---:|---:|---:|---:|
|従来|1,281,134|26.0616|1,210,220|47,550|1.0000|
|局所矛盾のみ|1,280,390|26.0464|1,209,428|47,511|1.0006|
|限定Naked Tripleも適用|1,167,141|23.7426|1,106,617|39,029|1.0977|

従来モードの全49,158問＋4,096件のストレス入力は、保存済みの
`results/single_core/results.csv` およびtotalsと完全一致した。

### Hard 1106・375問

|モード|総cycle|cycle / 問|推論cycle|分岐回数|同クロック速度比|
|---|---:|---:|---:|---:|---:|
|従来|319,367|851.6453|285,877|32,189|1.0000|
|局所矛盾のみ|319,348|851.5947|285,850|32,189|1.0001|
|限定Naked Tripleも適用|298,477|795.9387|266,795|30,436|1.0700|

## 新規則が実際に効いた回数

以下はRTLと同じ更新・分岐をするスカラーモデルでの集計。
推論結果が破棄される停止cycleは除き、実際に採用した更新だけを数える。
「追加除去」は、その更新で従来規則が除去する分を除外し、重複するセル・数字を1回と数える。

|限定Naked Tripleの指標|17ヒント|Hard 1106|
|---|---:|---:|
|候補和が3種類だった組の延べ数|17,808,844|3,362,946|
|従来規則に追加の除去があった組の延べ数|178,533|30,856|
|追加除去があった更新回数|152,256|26,131|
|追加で除去した候補ビット数|825,256|82,722|

既に完成した3セルも候補和は3種類になるため、単なる条件成立数と有効除去数は大きく異なる。

## 問題別の変動と限界

CSVは前問の完了から当該問の完了までの区間を記録する。キュー遷移を含み、
各問題だけを孤立して実行したレイテンシとは異なる。

|限定Naked Tripleと従来の比較|17ヒント|Hard 1106|
|---|---:|---:|
|完了間隔が短縮した問題数|20,893|326|
|不変|28,075|38|
|悪化|190|11|
|95パーセンタイル：従来 → 新規則|47 → 42 cycle|3,073 → 2,828 cycle|
|最大完了間隔：従来 → 新規則|9,767 → 6,325 cycle|9,819 → 9,672 cycle|

強い推論で分岐位置が変わるため、個別には遅くなる例もある。
17ヒントの `dataset_38775` は27 → 291 cycle、分岐1 → 30回。
難問の `dataset_270` は146 → 259 cycle、分岐14 → 27回。

比較用JSONは次のように生成できる。

```sh
python3 sim/segment_rules/compare.py \
    build/segment-eval/sim/fades_core/results.csv \
    build/segment-eval/sim/segment_rules/core-1.csv \
    build/segment-eval/sim/segment_rules/core-2.csv
python3 sim/segment_rules/compare.py \
    build/segment-eval/sim/fades_core/hard-results.csv \
    build/segment-eval/sim/segment_rules/core-hard-1.csv \
    build/segment-eval/sim/segment_rules/core-hard-2.csv
```

## 判断

- **局所矛盾検出だけは、今回の問題集ではほぼ性能に寄与しない。**
- **限定Naked Tripleにはサイクル削減の効果がある。** 同じコア数・周波数なら、
  17ヒントで約9.77%、難問で約7.00%の速度向上となる。
- ただし6 → 5コアへ減れば、全体性能は従来比で約0.915倍／0.892倍となり、利益が消える。
- 6コアを維持する場合でも、両問題集で改善を残すには、動作周波数を従来の約93.46%以上に
  保つ必要がある。従来100 MHzなら約93.5 MHzが損益分岐。
- 以下にボード全体の配置配線結果と実機A/B測定を記録する。

## 6コア・100 MHzのボード配置配線を評価する

限定Naked Tripleを有効化したボード全体を、従来と同じcore 100 MHz / DMA 200 MHzで
生成する。PS、AXI DMA、非同期FIFO、ファンのTTC0 → EMIO → A12配線を含む。

```sh
cmake --preset kv260-pl -B build/segment-6core-pl \
    -DFADES_CORES=6 -DFADES_SEGMENT_RULES=2 -DFADES_VIVADO_JOBS=2 \
    -DVIVADO_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/vivado
cmake --build build/segment-6core-pl --target kv260-bitstream
```

`build/segment-6core-pl/fpga/kv260/configuration.txt` と、BDから出力する
`implementation/solver_configuration.txt` でモード・コア数を確認できる。
通常の出力先とは分け、レポートは `implementation/` 配下に保存する。
合成・配置配線中はRTL・制約・ビルドスクリプトを固定する。

有効化した6コアの統合シミュレーションは以下で実行する。

```sh
cmake --preset host -B build/segment-enabled-sim \
    -DFADES_CORES=6 -DFADES_SEGMENT_RULES=2 -DFADES_BUILD_HOST=OFF
cmake --build build/segment-enabled-sim --target core_test stream_test axi-build \
    segment_model_test_2 --parallel 4
ctest --test-dir build/segment-enabled-sim \
    -R '^(fades|fades-hard|fades-core|fades-core-hard|axi|segment-model-2|lint-fades.*)$' \
    --output-on-failure -j 3
```

この構成で、全49,158問・難問375問のコア／統合試験、AXI、モデル照合、
ボードトップまでのlintの10試験が成功している。

### 配置配線結果（2026-11-04）

Vivado 2024.2、KV260 / `xck26-sfvc784-2LV-c`、6コア、core 100 MHz / DMA 200 MHz、
`FADES_SEGMENT_RULES=2`、Vivado jobs=2。通常の合成・配置・phys_opt・配線・bitstream生成を
完了し、既存のexport検査を通過した。RTL・制約・スクリプトはビルド中に固定した。
比較元は `build/kv260-pl/fpga/kv260/implementation/` の従来6コア回路の配置配線済みレポート。

|配置配線後の資源|従来|限定Naked Triple有効|差分|
|---|---:|---:|---:|
|LUT|86,133（73.54%）|97,018（82.84%）|+10,885|
|FF|36,273|36,268|−5|
|CLB|13,823（94.42%）|14,217（97.11%）|+394|
|BRAM tile|128 / 144|128 / 144|0|
|CARRY8|644|644|0|

配置配線後のLUT増加をコア数で割ると約1,814 LUT / core。
合成段階のソルバーIP単体では78,393 → 88,303 LUT（約1,652 LUT / core増）。
両者は合成／配置配線の段階と集計範囲が異なる。FFの小さい差も含め、物理最適化の影響がある。

|タイミング指標|限定Naked Triple有効|
|---|---:|
|全体setup WNS|+0.099 ns|
|core 100 MHzのsetup WNS|+0.297 ns|
|DMA 200 MHzのsetup WNS|+0.099 ns|
|hold WHS|+0.011 ns|
|pulse width WPWS|+1.000 ns|
|setup / holdの違反endpoint|0 / 0|

全user timing constraintを満たした。未レビューのCritical CDCは0、
snapshot bus skewは5 ns要求に対し2.970 ns。ファンのA12 / LVCMOS33 / 4 mA / SLOWも
export時の検査を通過した。bitstream、XSA、`verified_routed.dcp`、`build_complete`を生成済み。
この配置配線評価と、後述する実機ベンチマークは測定範囲を分けて記録する。

### 現在の最長パス

core側の最悪setupパスは以下。

```text
solvers[1].solver/current_reg[450]
  → 候補が1数字かの判定
  → 列内の確定数字の重複検出（fixed_multi）
  → cell_contradiction
  → terminal / finish等のコア制御
  → 状態列の占有数更新の加減算
  → solvers[1].solver/count_reg[9]
```

この `count_reg` は状態列の占有数で、popcountの出力ではない。
新しい3個判定そのものを通る経路ではなく、従来からある矛盾検査を通っている。

- データパス遅延：9.397 ns。
- 論理遅延：2.339 ns、配線遅延：7.058 ns（約75%）。
- 論理段数：17（CARRY8×3、LUT×14）。
- WNS：+0.297 ns。

従来のcore最悪パスは、候補 → 確定数字重複 → 矛盾制御 → `current_reg/CE`、
14段、論理2.008 ns＋配線7.262 ns、WNS +0.267 nsだった。
最悪endpointが変わっているため、段数の差をそのまま追加規則の直列段数とは解釈しない。
今回も配線遅延の寄与が大きい。

全体の最悪パスは別の200 MHz側で、`total_records_reg[9]` からDMAのreadyレジスターへ
至る経路。coreの高周波化と、DMA側の制約は分けて考える必要がある。

### 次の最適化を考える際の注意

- 現在の最悪coreパスにある、遅く到着する終了判定から占有数の加減算への直列依存が候補。
  カウンター更新候補の先行計算などは、推論サイクルを増やさず検討できる可能性がある。
- 9 bitの個数判定は、数値の全4 bitが必要なのか、`==3` / `<3`だけなのかを区別する。
  専用のBoolean回路と、GPCで全popcountを作る回路の両方を比較する必要がある。
- 現在はCLB 97.11%、BRAM 128/144。論理を縮めても、同じRAM構成で7コアにすると、
  状態RAM約20.5 tile / core × 7＋周辺5 tile＝約148.5 tileとなり144 tileを超える。
  コア追加には状態メモリーの構成も別途見直す必要がある。
- `../../advgpcgen-rs/` のGPCを調べ、3:2圧縮＋`(1,1,7;4)`、または3組の3:2圧縮＋
  `(1,3,5;4)`で9 bit popcountを作れることを、CARRY8機能モデルを使い全512入力で確認した。
  同プロジェクトのHDLはCARRY4向けなので、CARRY8の2×4モード、LUT出力とS/DIの接続、
  2カウンターの同一スライスへの収納は実装上の追加検討事項。機能一致だけで面積・速度の
  改善を主張しない。現回路へのGPC置換は行っていない。

## KV260実機A/Bベンチマーク（2026-11-04 JST）

同じKV260へ従来回路と限定Naked Triple有効回路を順にロードし、同じネイティブビルドの
PSプログラム・ドライバーで比較した。`xmutil listapps`、デバイスの存在、firmwareの
SHA256、機能試験で切り替えを確認した。測定後は新回路をロードした状態。

### 実クロックと測定範囲

- 両回路とも6コア。Ubuntu 24.04.5 LTS / Arm64、カーネル `6.8.0-1015-xilinx`。
- Linux `clk_summary` の実測値は、両回路・測定前後とも **core 99,999,999 Hz、
  DMA 187,499,999 Hz**。配置配線の制約・設定目標は100 / 200 MHzだが、今回の実行時の
  DMAクロックは約187.5 MHzであり、200 MHzの測定値とは扱わない。
- CPU governorはuserspace、CPU周波数1,333,333 kHzで両回路共通。
- 1バッチ131,072件、8スロット × 1 MiB、1レコード64 byte。小さい問題集は先頭から
  循環してDDRを埋め、準備済みDDRを反復する。
- 各問題集について10秒測定を3回。CPU参照生成、入力準備、warmupと最終全件検証は
  計測外。計測中はreset/config、descriptor準備、DMA、ソルバー、排出・完了待ち、
  PSループ、snapshot・件数検査を含む。
- 最後のバッチを中断せず排出し、**実経過時間**でスループットを計算する。
  件数は同じ問題の再投入を含む延べ件数で、異なる問題の件数ではない。
- ファンPWM 15 / enable 1。前後の温度センサー値は約31〜33 ℃。

### 10秒×3回の結果

単位はM問/s。中央値同士で改善率を計算する。

|問題集|回路|1回目|2回目|3回目|中央値|
|---|---|---:|---:|---:|---:|
|17ヒント49,158問|従来|22.293479|22.323317|22.351494|22.323317|
|17ヒント49,158問|限定Naked Triple|24.362756|24.460315|24.403257|**24.403257**|
|Hard 1106・375問|従来|0.703226|0.703227|0.703219|0.703226|
|Hard 1106・375問|限定Naked Triple|0.752190|0.752238|0.752164|**0.752190**|

**改善率は17ヒント集合で+9.317%、Hard 1106で+6.963%。**
これは従来の別環境の公表値との比較ではなく、同じ実機・同じ実クロックでの再測定比較。
12回すべてエラー0、前後各131,072件の全結果検証PASS、全コアの投入・回収件数一致、
最大未完了数2、FIFO write-domain peak 65を確認した。計測中の全結果をCPUで逐一
照合したものではなく、各roundではDMA完了とPL件数を検査している。

各回の完了件数・排出込み時間・速度・実クロックは
[`kv260_benchmark.csv`](kv260_benchmark.csv) に保存する。時間表示は小数6桁へ丸めているため、
CSVの速度はプログラムが丸め前の時間で計算して表示した値を保持する。

### 機能確認と単一バッチの測定

両回路でABI検査・close/reopen、全49,158問、難問375問をwarmup／測定後の2回ずつ
独立CPU参照と照合した。mixed入力3,072件も解あり3,008・解なし32・不正セル32を確認。

|1巡のPL区間：最初の配布〜最後の結果回収|従来|限定Naked Triple|
|---|---:|---:|
|17ヒント49,158問|217,732 core cycle|196,927 core cycle|
|Hard 1106・375問|56,139 core cycle|53,538 core cycle|

このPL区間は最後のDMA排出・PS処理を含まず、上の10秒ベンチマークとは範囲が異なる。
小さい難問集合1巡と、131,072件を反復する測定では、末尾の負荷偏りの影響も異なる。

再現には、この節の実クロックを確認した上で、回路ごとに次を実行する。

```sh
python3 host/tests/check_abi.py
build/kv260-ps/host/fades_verify sim/data/17puz49158.txt 8193 6 1
build/kv260-ps/host/fades_verify sim/data/HardestDatabase110626.txt 375 1 1
build/kv260-ps/host/fades_verify sim/data/17puz49158.txt 1024 3 1 mixed
for run in 1 2 3; do
    build/kv260-ps/host/fades_benchmark sim/data/17puz49158.txt 10
    build/kv260-ps/host/fades_benchmark sim/data/HardestDatabase110626.txt 10
done
```

今回のbitstream SHA256:

- 従来: `86bdc415a5eaf52f902ec780582808af9442ca25078583cdcd22d098d80588f3`
- 限定Naked Triple: `428393bf1c67347783b40be972f4571e2bb3f7da9e21538e7638a8fe6845f144`

ローカルの生ログ・環境・時計・集計は `results/segment_rules_kv260/`、実機作業場所は
`/root/fades/segment_rules/`。`results/` はGit管理外のため、比較に必要な数値は上記CSVと
本文にも記録した。初回ロード時には起動時overlayの残存を解消してから両測定を行い、
異常転送やDMAタイムアウトは発生していない。
