# 共有入力パイプラインでの確定数字先行伝播

## 構成（2026-11-06）

コアごとの先行伝播実験を、全コア共通の入力処理へ移した。

```text
入力async FIFO（388 bit：ID＋81セルの数字）
    ↓
候補マスクへデコード
    ↓ レジスタ：候補729 bit＋invalid/ID 65 bit＋valid
確定数字による候補除去を1回（naked_single_preload ×1）
    ↓ レジスタ：候補729 bit＋invalid/ID 65 bit＋valid
既存のコア選択・配送
    ↓
各コアのBRAM／head_cache → current → 通常推論
```

- `rtl/problem_preprocessor.sv` がcoreクロック上の2段elastic pipeline。
- 2段ともvalid/readyで停止・同時受け渡しが可能。
  停止なしでは初期レイテンシ2 cycle、以後1問/cycleで出力できる。
- 先行伝播回路は `fades.preprocess.singles` の1基。
  本来各コアにある通常のSingles＋Locked Candidates推論は維持。
- `rtl/fades_core.sv` はmain `bd7ce0a` と同一に戻した。
  各コアのロード経路に追加の推論回路・選択MUXはない。
- IDとinvalidフラグは盤面と一緒に両段を通し、コアへの実際のdispatch時に
  IDキューへ保存する。入力FIFOを取り出した時点ではinflightを増やさない。
- invalid入力は第2段出力で全候補0にし、従来どおりステータス2で返す。
  確定数字の重複は先行伝播で消さず、コア側の矛盾判定に渡す。
- resetは両段のvalidを消す。無効時に参照しないpayload FFはリセット不要。
- `enable=0` でも従来どおり入力バッファ／コアキューには取り込み可能。
  コアの探索実行だけがenableに従う。
- CMakeとKV260のVivadoスクリプトの両方に新しいRTLファイルを追加。

## サイクル数

共有回路が作った盤面を受け取るコアについて、既存のcycle-accurate
テストベンチを同じ入出力条件で実行した。

|問題集|問題数|未処理入力の総cycle|先行伝播済み入力の総cycle|未処理 cycle/問|先行伝播済み cycle/問|
|---|---:|---:|---:|---:|---:|
|17ヒント|49,158|1,281,134|1,231,976|26.061557|25.061557|
|Hard 1106|375|319,367|318,992|851.645333|850.645333|

コア前処理用のテストラッパー `sim/fades_core/preprocessed.sv` は、共有回路と
同じ `naked_single_preload` の出力を、変更していないコアの入力に渡す。
この表はコアにおける処理サイクル数であり、共有パイプラインの初期2 cycle、
FIFO／DMAの転送、コア選択待ちは含まない。初期投入は計測外、最終排出は計測内。

検証した一致関係：

- 未処理入力の全問題＋各4,096件ストレス試験のCSV・totals JSONは、
  変更前mainの保存物とバイト一致。
- 前処理済み入力の全問題＋各4,096件ストレス試験のCSV・totals JSONは、
  先のコア内ロード時先行伝播実験とバイト一致。
- 通常問題全問で、未処理入力に比べてcycle数・通常推論回数がちょうど1減る。
  分岐・矛盾・枝破棄・無駄な推論回数は変わらない。

## 共有パイプラインと統合試験

### 単体試験 `problem-preprocessor`

独立したセル対セルの参照処理を使い、各256問で次を検証した。

- 候補除去、ID、不正入力フラグの一致。
- 連続入出力：256問を258 cycleで出力、出力開始後のバブルなし。
- 入力ギャップ・出力停止：valid中のpayload保持、両段満杯での入力停止。
- 両段満杯の状態でreset後、古い盤面・IDを出力しない。
- 完成盤面、空盤面、不正数字、重複した確定数字を含む。

### 1コア・6コアstream統合試験

両構成で17ヒント49,158問と難問375問の全解を検査した。
通常測定に加え、322件のmixed入力、2通りのDMA/coreクロック比、
入出力停止、enable停止、未処理データを残したreset、1問、
フレーミングエラーを検証。IDの欠落・重複やinvalid/unsatの取り違えなし。

6コア・通常入力の記録：

|問題集|シミュレーションtime step|processing_cycles|全コアbusy cycle合計|
|---|---:|---:|---:|
|17ヒント49,158問|2,096,420|209,630|1,209,153|
|Hard 1106・375問|550,208|55,009|317,874|

この表はDMA/core半周期3/5 time stepの全体実行で、コア単体の平均表とは
測定範囲が異なる。processing_cyclesは既存仕様どおり最初のdispatchから
最後のcollectまでを計測する。time stepは入力開始・出力排出も含む。

AXI-Lite統合試験と、core／stream／AXI／KV260ラッパーのlintもPASS。

その後の物理評価・実機測定では、追加post-route最適化により115.385 MHz相当の
タイミング検査を通過し、17ヒントの実機中央値は26.511458 Msolve/sだった。
面積・タイミング・測定条件は `fpga/kv260/SHARED_PRELOAD_RESULTS.md` を参照。

## 再現

```sh
cmake --preset host -B build/shared-preload -DFADES_BUILD_HOST=OFF
cmake --build build/shared-preload --target \
  core_test core_preprocessed_test stream_test problem-preprocessor-build axi-build --parallel 4
ctest --test-dir build/shared-preload --output-on-failure -j 2 \
  -R '^(problem-preprocessor|lint-.*|axi|fades-core.*|fades(-hard)?)$'

cmake --preset host -B build/shared-preload-1core -DFADES_BUILD_HOST=OFF -DFADES_CORES=1
cmake --build build/shared-preload-1core --target stream_test --parallel 4
ctest --test-dir build/shared-preload-1core --output-on-failure -j 2 \
  -R '^(fades(-hard)?|lint-fades)$'
```

単一コアの比較CSV・totals JSONは `build/shared-preload/sim/fades_core/`：

- main相当：`results.csv`、`hard-results.csv`
- 先行伝播済み入力：`preprocessed-results.csv`、`preprocessed-hard-results.csv`

元のロード時先行伝播実験とデータSHA256は
`sim/fades_core/PRELOAD_RESULTS.md` を参照。
