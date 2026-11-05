# mainベース・Singlesのみ：1/2コアの合成・配置配線による資源測定

## 結論

**1・2コアとも、ボード全体の配置配線を完了し、core 100 MHz / DMA 200 MHz制約で
setup / hold / CDC / bus skew / fan検査を通過した。**

配置配線後のコア規模は、2コア構成の平均で **10,572 LUT、2,255 FF、
20.5 BRAMタイル、2,068 occupied CLB/コア**。
ボード全体の1→2コア増分は **11,246 LUT、2,637 FF、20.5 BRAMタイル、1,930 CLB**。
推論エンジン階層だけなら約4.1k LUTであり、探索・RAMアクセス・状態制御も大きい。

**Locked Candidatesを無効化しただけの現行コアを20個複製する構成は、
LUTとBRAMの両方でKV260の資源を超える。**

## 測定条件

- 基準：main / origin/main `bd7ce0aff7e94eb5b7960996e87e24280258285b`。
- 実験ブランチ：`13-singles-core-area`、mainから直接分岐。
- 作業worktree：元リポジトリから `build/singles-main/`。
- 測定日：2026-11-05、Vivado 2024.2 build 5239630、`xck26-sfvc784-2LV-c`。
- `FADES_LOCKED_CANDIDATES=0`：Naked / Hidden Singleのみ。mainにある
  Pointing / Claimingを定数パラメーターで無効化し、合成時に除去する。
- #11側の共有セグメント解析・Pair / Triple回路は持ち込んでいない。
- **探索・分岐・729 bit × 1024段のコアRAMはmainのまま保持**。
  これは探索を持たない推論専用エンジンの測定ではない。
- PS、AXI DMA SG、128-bit入出力、FIFO、AXI制御とカウンターを含むボード全体。
- クロック制約は最終timing reportでもcore 10.000 ns / 100 MHz、
  DMA 5.000 ns / 200 MHzであることを確認。実機クロック・実機性能の測定値ではない。
- ファン：TTC0 wave[2] → EMIO → fan_en_b → A12を保持・検査。
- 1コア、2コアの順に逐次ビルド、Vivado jobs=2。

## 1. ボード全体

### 合成後

|構成|LUT|FF|RAMB36|RAMB18|BRAMタイル|
|---|---:|---:|---:|---:|---:|
|1コア|20,626|20,981|24|3|25.5|
|2コア|31,224|23,608|44|4|46.0|
|2−1 増分|10,598|2,627|20|1|20.5|

CLBは実配置に依存するため、合成後は記載しない。

### 配置配線後

|構成|LUT|LUT使用率|FF|BRAMタイル|BRAM使用率|CLB|CLB使用率|
|---|---:|---:|---:|---:|---:|---:|---:|
|1コア|20,237|17.28%|20,205|25.5|17.71%|4,760|32.51%|
|2コア|31,483|26.88%|22,842|46.0|31.94%|6,690|45.70%|
|2−1 増分|**11,246**|—|**2,637**|**20.5**|—|**1,930**|—|

両構成ともURAM=0、DSP=0。
KV260の全資源は117,120 LUT、234,240 FF、144 BRAMタイル、14,640 CLB。
BRAMタイル相当は `RAMB36 + RAMB18 / 2`。

## 2. コア階層ごとの内訳

|段階|構成・階層|LUT|FF|RAMB36|RAMB18|occupied CLB|
|---|---|---:|---:|---:|---:|---:|
|合成後|1コア：core 0|9,800|2,255|20|1|—|
|合成後|2コア：core 0|10,213|2,255|20|1|—|
|合成後|2コア：core 1|9,729|2,255|20|1|—|
|配置配線後|1コア：core 0|10,346|2,255|20|1|2,111|
|配置配線後|2コア：core 0|10,686|2,255|20|1|2,134|
|配置配線後|2コア：core 1|10,458|2,255|20|1|2,002|
|配置配線後|2コアの平均|**10,572**|**2,255**|**20**|**1**|**2,068**|

### 推論エンジン階層 `infer`

|段階|構成・階層|LUT|occupied CLB|
|---|---|---:|---:|
|合成後|全3インスタンス共通|3,983|—|
|配置配線後|1コア：core 0 / infer|4,181|1,048|
|配置配線後|2コア：core 0 / infer|4,081|990|
|配置配線後|2コア：core 1 / infer|4,139|912|

`infer`は組み合わせ回路で、階層内FF・BRAM・URAM・DSPは0。
現在状態・後続状態の選択・探索制御等は親コアに属するため、
推論エンジン階層の数だけで独立コア全体の規模を見積もることはできない。
分岐回路などはVivadoの再構成で親階層に取り込まれており、親のLUTを全てRAM制御と
解釈することもできない。

### CLBと増分の解釈

occupied CLBは対象階層のprimitiveが実配置されているSLICEL / SLICEMサイトのユニーク数。
UltraScale+では1 CLBに1 SLICEが対応する。`report_utilization -cells` のコアCLB数とも一致。
ボード全体についてもこのカウントが通常のutilization reportの4,760 / 6,690と一致する。

異なる階層が同じCLBを共有する場合があり、階層別CLB数は加算できない。
また、1→2コア増分は、追加コアだけでなくdispatch・collect・カウンターの増分、
既存コアと周辺の配置・物理最適化の変化も含む。
そのため1,930 CLBがどの構成でも固定の「追加1コア面積」になるわけではない。
階層別LUTもLUT combiningと階層間最適化の影響があり、内訳の厳密な加算は避ける。

## 3. タイミング・検査

|構成|core WNS ns|DMA WNS ns|全体WHS ns|TNS / THS|最終判定|
|---|---:|---:|---:|---|---|
|1コア|+1.548|+0.553|+0.010|ともに0|PASS|
|2コア|+0.636|+0.270|+0.011|ともに0|PASS|

最終exportのfan、CDC、bus skew、setup / hold検査も通過し、
verified routed checkpoint・bitstream・XSA・firmware packageを生成した。
このWNSから20コア時の到達クロックや最大周波数を推定したものではない。

機能確認はSingles構成の17ヒント49,158問＋4,096件ストレスの単一コア試験、
2コアの全17ヒント統合試験、AXI、4トップのlint、Yosys RAM構造チェックの8件がPASS。
1コアのボードトップも別途lint PASS。今回、実機ロードは行っていない。

補足：mainの既存`core_test`は先行投入512問以上を要求するため、Hard 1106の375問を
そのまま指定した追加試行は開始前の入力件数チェックで拒否された。
この試行を難問の検証成功件数には含めない。

## 4. 20コアに向けた判断

- 現状コアを20個複製すると、コア部分だけで約211k LUT、410 BRAMタイル。
  周辺を含める前にKV260の117k LUT / 144 BRAMタイルを超える。
- BRAMは1→2コアの増分でも各コア内訳でも20.5タイルで一致。
  現在の周辺5タイル相当と合わせると `5 + 20.5 × N`。
  このRAM構成のままなら、容量上は6コアまでで、7コアは148.5タイルになる。
- 推論専用エンジンを多数置く構成なら、親コアにある分岐・状態選択・RAMを切り離す必要がある。
  推論階層は約4.1k LUTだが、独立エンジン用の状態レジスター・制御・供給系・fallbackも加算し、
  CLBへの詰め込みと多コア配置のタイミングを実際に確認する必要がある。

## 5. 保存先と再現

この文書内のパスは実験worktree `build/singles-main/` を基準にする。

- 集計CSV：[singles_area.csv](singles_area.csv)
- 全体の実行ログ・開始終了日時・RTL/TclのSHA256：`results/singles_area/`
- 1コア出力：`build/singles-1core-pl/fpga/kv260/implementation/`
- 2コア出力：`build/singles-2core-pl/fpga/kv260/implementation/`
- 合成後：`synth_utilization.rpt`、`synth_hierarchy.rpt`
- 配置配線後：`utilization.rpt`、`hierarchy.rpt`、`timing_summary.rpt`
- CLB内訳：`clb_footprint.csv`、`core0_utilization.rpt`、`core1_utilization.rpt`（2コアのみ）
- checkpoint：`verified_routed.dcp`

```sh
# mainから分岐した実験worktreeで実行
python3 fpga/kv260/scripts/singles_area_experiment.py --jobs 2

# 配置済みの階層別CLB数を抽出
/tools/Xilinx/Vivado/2024.2/bin/vivado -mode batch -nojournal \
  -source fpga/kv260/scripts/singles_clb_detail.tcl \
  -tclargs build/singles-1core-pl/fpga/kv260/implementation
/tools/Xilinx/Vivado/2024.2/bin/vivado -mode batch -nojournal \
  -source fpga/kv260/scripts/singles_clb_detail.tcl \
  -tclargs build/singles-2core-pl/fpga/kv260/implementation
python3 fpga/kv260/scripts/singles_area_summary.py
```

比較スクリプトは両ビルドの完了と測定開始時からのRTL/Tclハッシュ一致を検査する。
個別ビルドの場合は次のconfigureを使い、`FADES_CORES=2`と別の`-B`でも同条件で実行する。

```sh
cmake --preset kv260-pl -B build/singles-1core-pl \
  -DFADES_CORES=1 -DFADES_LOCKED_CANDIDATES=0 -DFADES_VIVADO_JOBS=2 \
  -DVIVADO_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/vivado
cmake --build build/singles-1core-pl --target kv260-package
```
