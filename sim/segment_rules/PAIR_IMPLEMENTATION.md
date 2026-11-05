# モード4の6コア・100 MHzボード合成／配置配線結果

本書は合成・配置配線時点の記録。後続の実機試験でモード4の動作と、
#9比で約2.46%／1.15%の実スループット改善を確認した。
最新結果は [PAIR_KV260_RESULTS.md](PAIR_KV260_RESULTS.md) を参照。

## 結論

**モード4はKV260へ6コアを搭載したまま、core 100 MHz / DMA 200 MHzの制約を満たした。**
合成、配置、phys_opt、配線、bitstream生成、既存export検査を完了。
#9のモード2に対して、ボード全体のLUTは **+7,822（+8.06%）**、
CLBは **+202（+1.42%）**、BRAMは不変。

これまで未確認だった「コア数と目標周波数を維持して実装できるか」は、この構成では確認できた。
既存RTLシミュレーションで得た#9比の同クロック速度向上は、17ヒントで約2.79%、難問で約1.15%。
**今回の回路の実機ロード・性能測定は行っておらず、実機スループットの改善は未確認。**

関連: #9、#11。実装ブランチ: `11-shared-segment-bivalue-pairs`。
前段の論理構造分析は [PAIR_FACTORIZATION.md](PAIR_FACTORIZATION.md)、
集計CSVは [pair_implementation.csv](pair_implementation.csv)。

## 条件

- 実行日: 2026-11-05 JST。
- Vivado 2024.2、part `xck26-sfvc784-2LV-c`、KV260 SOM preset 1.4。
- `FADES_CORES=6`、`FADES_SEGMENT_RULES=4`、Vivado jobs=2。
- core `clk_pl_1` は10 ns、DMA `clk_pl_0` は5 nsの配置配線制約。
- PS、AXI DMA、SmartConnect、非同期FIFO、TTC0→EMIO→fan A12を含むボード全体。
- 通常の既存ビルドフローを使用。ビルド中にRTL・制約・スクリプトを変更せず、
  入力11ファイルのSHA256が開始前後で一致した。
- `kv260-bitstream` は終了値0、全体約2,943秒（49分3秒）。追加の再配置・再配線は行っていない。
- 比較元は#9の `build/segment-6core-pl/fpga/kv260/implementation/`。
  同じツール・part・コア数・クロック制約の保存済み結果。

100 / 200 MHzは設計制約であり、実機で計測したクロックではない。
#9の実機測定時のDMAクロック約187.5 MHzとは区別する。

## 資源の比較

### ソルバーIPのOOC合成（同じ集計範囲）

|指標|#9：モード2|今回：モード4|差|
|---|---:|---:|---:|
|LUT|88,303|96,377|+8,074（約+9.14%）|
|FF|24,510|24,510|0|
|BRAM tile|123|123|0|
|CARRY8|611|611|0|

合成段階の増分を6コアで割ると約1,346 LUT／コア。
これはPS・DMA等を含むボード全体の資源量ではない。

### ボード全体の配置配線後

|指標|#9：モード2|今回：モード4|差|
|---|---:|---:|---:|
|LUT|97,018（82.84%）|104,840（89.52%）|+7,822（+8.06%）|
|FF|36,268|36,271|+3|
|CLB|14,217（97.11%）|14,419（98.49%）|+202（+1.42%）|
|BRAM tile|128/144|128/144|0|
|CARRY8|644|644|0|
|F7 Mux|925|925|0|

配置配線後の増分を6コアで割ると約1,304 LUT／コア。
CLBの残りは221個。LUT増加率に比べてCLB増加率は小さいが、配置し直した結果であり、
他構成でも同じように詰められる保証ではない。FFの+3も物理最適化後の差。

この比較にはセグメント共通解析への再構成とPair追加の両方が含まれる。
共通化だけのモード2を今回新たに合成した結果や、完全Pairのモード3との物理比較ではないため、
個々の共有方法のLUT削減量をこの差から分離しない。
Boolean DAGで数えたPair増分約34%削減を、実際のLUT削減率と解釈しない。

## タイミング・配線・CDC・ファン

|指標|#9：モード2|今回：モード4|
|---|---:|---:|
|全体setup WNS|+0.099 ns|**+0.066 ns**|
|core 100 MHz setup WNS|+0.297 ns|**+0.116 ns**|
|DMA 200 MHz setup WNS|+0.099 ns|+0.066 ns|
|全体hold WHS|+0.011 ns|**+0.010 ns**|
|core側hold WHS|+0.011 ns|+0.011 ns|
|pulse width WPWS|+1.000 ns|+1.000 ns|
|snapshot bus skew（5 ns要求）|2.970 ns|2.276 ns|

- 最終 `report_timing_summary -delay_type min_max` で全user timing constraintを満たした。
- setup／hold／pulse widthの違反endpointは0。
- 全154,304本のroutable netが配線済み、routing errorは0。
- 未レビューCritical CDCは0。既存XPM FIFOのCDC-15 Warning 900件は残る。
  snapshot・reset・固定設定に対する既存のレビュー済みwaiverを使用した。
- DRC Errorは0。既存DMA BRAMのcollision advisoryとSmartConnectのno-routable-load Warningが残る。
- fan `A12 / LVCMOS33 / 4 mA / SLOW` とPS TTC0 wave[2]→EMIOの接続を維持し、export検査を通過。
- 配線途中には混雑level 5を報告したが、通常の再配線反復で配線・タイミングを完了した。

配線器の推定WNSは+0.072 nsだったが、上表には最終解析の+0.066 nsを使用した。

### core最悪パス

```text
solvers[1].solver/current_reg[426]
  → Naked Single判定・fixed_digits
  → ユニットの矛盾検査（analyze/bad_unit[11]）
  → cell_contradiction / terminal等の制御
  → current_reg[208]/CE
```

- データパス9.553 ns：論理2.396 ns、配線7.157 ns（約74.9%）。
- LUT 15段。最終CEへ向かうネットのfanoutは729。
- この最悪パスはPairの包含判定・Mマスク生成自体を通っていない。
- #9の最悪パスとはendpoint・配置が異なるため、差をPair回路の直列遅延とみなさない。

## 機能確認

前段の単一コア全問・ストレス試験と形式検証に加え、今回のビルド中に
`FADES_SEGMENT_RULES=4` / 6コアでストリームの全49,158問・難問375問、
AXI試験、ボードトップまでのlintを実行し、7試験すべて成功した。
出力先は `build/bivalue-6core-sim/`。配置配線済みネットリストのシミュレーションや実機試験ではない。

## 生成物と再現手順

出力先: `build/bivalue-6core-pl/fpga/kv260/implementation/`

- `fades.bit.bin`、`fades.xsa`、`verified_routed.dcp`
- `build_complete`、`complete.stamp`、各種レポート
- firmwareパッケージ: `build/bivalue-6core-pl/fpga/kv260/firmware/fades/`
- 全体ログ: `build/bivalue-6core-pl/build.log`
- 入力manifest: `build/bivalue-6core-pl/source_hashes.sha256`

`fades.bit.bin` SHA256:
`813dd2451f90ccfc3b6d02aa2daf51eb38d7121a55e6f18e5e23792ed38e3716`

評価時 `rtl/deduction_engine.sv` SHA256:
`f1f3c7283f4cd56f7e87c6028b3aa689722ec57933f30a689947ba559dbe6543`

```sh
cmake --preset kv260-pl -B build/bivalue-6core-pl \
    -DFADES_CORES=6 -DFADES_SEGMENT_RULES=4 -DFADES_VIVADO_JOBS=2 \
    -DVIVADO_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/vivado
cmake --build build/bivalue-6core-pl --target kv260-bitstream
cmake --build build/bivalue-6core-pl --target kv260-package

cmake --preset host -B build/bivalue-6core-sim \
    -DFADES_BUILD_HOST=OFF -DFADES_CORES=6 -DFADES_SEGMENT_RULES=4
cmake --build build/bivalue-6core-sim --target stream_test axi-build --parallel 2
ctest --test-dir build/bivalue-6core-sim --output-on-failure -j 2 \
    -R '^(fades|fades-hard|axi|lint-fades.*)$'
```

既定モードは0のまま。今回確認したのはモード4を6コア・目標周波数で実装できること。
LUT増加約8.06%に対し、サイクル由来の速度向上は約1〜3%なので、LUT当たり性能の改善ではない。
実機での採用判断には、#9との同条件A/B測定が必要。
