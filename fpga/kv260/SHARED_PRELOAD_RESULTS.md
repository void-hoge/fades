# 共有入力前処理：115.385 MHzの配置配線・17ヒント実機測定

## 結論（2026-11-06）

**共有前処理＋6コアは、追加post-route物理最適化後にcore 115.385 MHz相当の
タイミング検査を通過し、KV260実機の17ヒント測定は中央値26.511458 Msolve/s。**

既存main115の保存済み中央値25.605369 Msolve/sに対して **+3.54%**。
比較元は2026-11-05の既存測定で、今回再測定したA/B比較ではない。
実クロック、カーネル、PS実行ファイル、ドライバー、入力データのhash、CPU設定は
比較元と同じであることを確認した。

コアのRTLはmain `bd7ce0a` と同一。共有前処理回路を1基追加し、
数字→候補マスク変換と、確定数字による候補除去を2段にパイプライン化した。
シミュレーションと構成は `sim/fades/SHARED_PRELOAD_RESULTS.md` を参照。

## 配置配線・タイミング

- Vivado 2024.2、KV260 revB、`xck26-sfvc784-2LV-c`、6コア。
- 現在の未コミットRTLを `results/shared_preload_115/source/` に固定してビルド。
  元ファイルのSHA256は同ディレクトリ上位の `source_manifest.json` に保存。
- PS IP要求はcore 115.384616 MHz、DMA 187.5 MHz。
  生成されたPS XDCもcore 8.666 ns、DMA 5.333 nsであることを確認。
- リンク・論理最適化済みのボード全体DCPへ、周期を明示して配置配線。
  XPM Gray/FIFOの周期依存CDC数値制約も更新した。
- core **8.666 ns / 115.393 MHz**、DMA **5.333 ns / 187.512 MHz**の検査。
  いずれも実機の周期を1 ps単位で短い側に丸めた条件。

|段階|core WNS ns|core TNS ns|core setup違反endpoint|DMA WNS ns|全体WHS ns|
|---|---:|---:|---:|---:|---:|
|標準配置配線後|−0.165|−18.974|373|+0.193|+0.003|
|追加 `phys_opt_design -directive AggressiveExplore` 後|**+0.000**|**0**|**0**|**+0.193**|**+0.003**|

追加最適化後はTHS=0、pulse-width違反なし、未制約内部endpoint=0。
productionと同じfan A12 / CDC / bus skew検査を通過。
最終checkpointを開き直し、max/min slackが負でないことも再検査した。
**coreのsetup余裕はレポート精度で0.000 nsであり、ほぼ余裕なし。**

標準配線後の最悪パスはコア0の分岐生成→BRAM。
追加最適化後はコア3の以下の検証・制御パスが最悪になった。

```text
current_reg[569]/Q
 → Singleton抽出 → fixed_digits
 → 確定数字重複 → state_contradiction → terminal
 → current入力選択
 → current_reg[340]/D
```

データパス8.446 ns（論理2.453 ns、配線5.993 ns）、14 LUT段。
共有前処理内部の最悪レジスタ間パスは5.343 ns、slack +3.117 ns。
共有前処理の全入力経路を含めた最悪slackは+0.557 ns。

## リソース（追加最適化後、ボード全体）

|リソース|使用量|搭載量|使用率|既存main115使用量|
|---|---:|---:|---:|---:|
|CLB LUT|87,887|117,120|75.04%|89,622|
|FF|37,861|234,240|16.16%|36,273|
|使用CLB|13,876|14,640|94.78%|13,879|
|BRAM 36 Kbタイル換算|128|144|88.89%|128|
|URAM|0|64|0%|0|
|DSP|0|1,248|0%|0|

階層レポート上の共有前処理ブロックは **1,960 LUT / 1,590 FF / BRAM 0**、
インスタンス数は1。全体値と既存main115の差には、再合成・再配置配線による
マッピング差も含むため、差分をそのまま前処理単体の増減とは扱わない。

## 17ヒント実機ベンチマーク

### 条件

- KV260 revB、Ubuntu 24.04、Arm64、kernel `6.8.0-1015-xilinx`。
- 測定前後の実クロック：core **115,384,615 Hz**、DMA **187,499,999 Hz**。
- CPU userspace governor、1,333,333 kHz（測定前後一致）。
- 17ヒント49,158問。独立CPU参照で全問が解ありであることを確認。
- 1バッチ131,072件、8スロット×1 MiBの準備済みDDR反復。
- 各10秒×3回。最終バッチ排出・snapshotまでの実経過時間で算出。
- CPU参照・入力準備・warmup・前後バッチの内容検証は計測外。
  DMA・driver・PS制御・solver・最終排出は計測内。
- benchmarkとverifyは既存main115と同一バイナリ。測定中の入力再充填時間は0。

|回|処理件数|実経過秒|Msolve/s|
|---|---:|---:|---:|
|1|265,289,728|10.001102|26.526050|
|2|265,158,656|10.002277|26.509830|
|3|265,158,656|10.001662|26.511458|
|中央値|||**26.511458**|

|構成|17ヒント Msolve/s（中央値）|
|---|---:|
|既存main115、2026-11-05の測定|25.605369|
|共有前処理版、今回|**26.511458**|
|比率|**+3.538668%**|

### 正当性・動作確認

- ABI、1問、17ヒント全49,158問、3,072件mixed試験PASS。
- 各ベンチマークでwarmupと最終131,072件の全解・ID検証PASS。
- 測定中の各バッチはDMA完了とPL件数を検査し、全3回Errors=0。
- 測定前後のfirmware・ソフト・ドライバー・データSHA256一致。
- 機能試験・ベンチマーク中のdmesg追加0行。
- 終了時は共有前処理版のimageをロードした状態、`/dev/fades`あり。

`fades_verify` の表示する「at 100 MHz」はツールに固定された換算表示なので、
その行を今回のスループットとして使用していない。
上の測定値は `fades_benchmark` の実経過時間からの値。

### SHA256

```text
fades.bit.bin:
6e077fbd133daa5f145a22e20ef54b2121a485a4fdc61d06c32c66461980949c
fades.dtbo:
525e36e3ae916eca4a14eb63be0ff9ec084076cd1abd7a879e65ba392d3728e2
fades_benchmark:
9222fe51da3fbd752b09d01d210ef5be7fdf5dfd328bc5be44682731bd722151
fades_verify:
f3ea0ba1518982f0f62c7249af0ad743e8e28b2c05968ae7abe1745d1dca5fc8
17puz49158.txt:
58ef7d83e8cbac32495161f9745877fef82f5e8b3fe58e3cad4eb3fc004a81b9
```

## 保存物・再現

- 配置配線全体：`results/shared_preload_115/`
  - `run.py`、`link.tcl`、`clock_reimplement.tcl`：ソース固定、合成、リンク、配置配線・追加最適化。
  - `source/`、`source_manifest.json`、`source.patch`：入力ソース。
  - `route/`：標準配線結果。
  - `postopt/`：最終レポート、`verified_routed.dcp`、firmware、DTBO。
  - `audit.tcl`、`postopt/audit.txt`：clock period・slack・前処理インスタンス数の再検査。
  - `board/`：before/after、機能試験、`bench-17-{1,2,3}.log`、`summary.json`。
- 実機：`/root/fades/shared_preload_115/`
  - `firmware/`、`results/`、`board_benchmark.py`。
  - ロード前のfirmwareは `previous_firmware/` に保存。

ベンチマークの各回の実行コマンド：

```sh
/root/fades/bivalue_pairs/build/kv260-ps/host/fades_benchmark \
  /root/fades/bivalue_pairs/sim/data/17puz49158.txt 10
```

最終checkpointの再検査：

```sh
/tools/Xilinx/Vivado/2024.2/bin/vivado -mode batch -nolog -nojournal \
  -source results/shared_preload_115/audit.tcl
```
