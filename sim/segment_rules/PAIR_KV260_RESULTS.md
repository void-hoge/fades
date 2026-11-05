# モード4のKV260実機動作・#9とのA/B測定

## 結論

**モード4は実機で正常動作し、今回同条件で再測定した#9のモード2に対して、
17ヒントで約2.46%、難問で約1.15%の実スループット改善を確認した。**
測定は10秒×各3回、排出込みの実経過時間から求めた速度の中央値で比較する。

|問題集|#9：モード2 M問/s|モード4 M問/s|改善率|
|---|---:|---:|---:|
|17ヒント49,158問|24.343633|**24.941565**|**+2.4562%**|
|Hard 1106・375問|0.752210|**0.760858**|**+1.1497%**|

過去の別測定の#9の値を流用した比較ではない。
全12回でErrors=0、各回のwarmup・最終バッチ131,072件の全結果検証が成功した。
測定後はモード4を再ロードし、短い機能試験を通した状態で終了した。

関連: #9、#11。測定日は2026-11-05。
配置配線結果は [PAIR_IMPLEMENTATION.md](PAIR_IMPLEMENTATION.md)、
12回分の数値は [kv260_pair_benchmark.csv](kv260_pair_benchmark.csv)。

## 実機・ソフトウェア・クロック

- KV260 revB、6コア。Ubuntu 24.04.5 LTS / Arm64。
- 実行中カーネルは **`6.8.0-1015-xilinx`**。今回の実機で確認した値を使用する。
- `/root/fades/bivalue_pairs/` に独立した作業場所を作り、同じソースからPSツール・
  カーネルモジュールを実機ネイティブビルド。ホスト入力試験も成功。
- 両回路とも同じ実行ファイル・ドライバー・問題ファイルを使用。
  SHA256が回路間および測定前後で一致することを確認した。
- Linux `clk_summary` の実行時rateは、両回路・測定前後とも
  **core 99,999,999 Hz / DMA 187,499,999 Hz**。
- 配置配線制約の100 / 200 MHzと区別する。今回のDMA実行時rateは約187.5 MHz。
- CPU governorはuserspace、CPU周波数1,333,333 kHzで両回路共通。
- ファンPWM15 / enable1。測定前後の温度入力は約31.7〜34.2℃。

ロード時は `/dev/fades` を使用するプロセスがないことを確認してからクライアントモジュールを外し、
`xmutil unloadapp` → firmware配置 → `xmutil loadapp fades` → ドライバーロードを行った。
`xmutil listapps`、`/dev/fades`、firmware SHA256、機能試験で切り替えを確認した。
両回路ともPS TTC0 wave[2]→EMIO→fan A12の配線を持つ。

## 転送条件と計測範囲

- 1バッチ **131,072件**、8スロット×1 MiB、1レコード64 byte。
- 各問題集を先頭から循環して準備済みDDRを埋め、同じ内容を反復する。
  延べ処理件数であり、異なる問題の件数ではない。
- CPU参照生成、入力準備、warmupと前後の全件検証は計測外。
- 計測中はreset/config、descriptor準備、DMA、数独処理、排出・完了待ち、
  PSループ、snapshotとPL件数検査を含む。
- 指定時間後に実行中の最終バッチを打ち切らず、排出を完了して実経過時間で速度を計算。
  難問では約10.11〜10.16秒になっており、指定の10秒で割ってはいない。
- 計測中の全結果をCPUで逐一照合する測定ではない。
  中間roundはDMA完了とPL件数、内容はwarmupと最終バッチを全件検証する。

## 各回の結果

単位はM問/s。

|問題集|回路|1回目|2回目|3回目|中央値|
|---|---|---:|---:|---:|---:|
|17ヒント|モード2|24.350543|24.290280|24.343633|24.343633|
|17ヒント|モード4|24.996170|24.941565|24.899725|24.941565|
|難問|モード2|0.752210|0.752064|0.752225|0.752210|
|難問|モード4|0.760828|0.760858|0.761016|0.760858|

全回で以下を確認した。

- Errors=0、全6コアの投入件数と回収件数が一致。
- 各コアの最大未完了数2、FIFO write-domain peak 65。
- 入力の計測中再充填時間0、準備済みDDR反復。
- warmup・最終バッチのID重複／欠落なし、解・ヒント保持・予約領域の検査成功。
- 機能試験後から測定後までの `dmesg` に追加メッセージなし。DMAエラーやタイムアウトなし。

実行順はモード4の機能試験・測定、モード2の機能試験・測定、その後モード4を再ロード。
モード4の初回SSH待機が180秒で終了したため、5本の測定の最終検証完了と
実行中プロセスがないことを確認し、未実施の難問3回目だけを別の呼び出しで実行した。
各測定の内部区間は最後まで完了しており、途中で打ち切られた測定は集計していない。
CPU参照生成等を含む全測定手順は10秒×6より長いので、再現時のSSH待機には十分な時間を取る。

## 機能確認と単一バッチのPL区間

両回路でABI・排他的open・不正ioctl/mmap・close/reopen、1件転送、
17ヒント全49,158問、難問375問、mixed 3,072件を検証した。
通常問題集・mixedはいずれもwarmupと最終roundの2回ずつ全結果を独立CPU参照と照合。
mixedの内訳は解あり3,008、解なし32、不正セル32。

|1巡のPL区間：最初の配布〜最後の回収|モード2|モード4|
|---|---:|---:|
|17ヒント49,158問|196,925 core cycle|191,497 core cycle|
|難問375問|53,538 core cycle|50,974 core cycle|

これは最後のDMA排出やPS処理を含まない別区間。
小さい難問集合1巡では末尾の負荷偏りの影響があるため、
上の131,072件反復測定の改善率とは区別する。

## 判断

- コア数・目標周波数を維持して配置配線できるだけでなく、実機でも性能改善を確認できた。
- 同クロック単一コアRTLの#9比 +2.79%／+1.15%に対し、今回の実機測定は+2.46%／+1.15%。
  実機値はDMA・PS等を含むので、RTLの速度比との完全一致を前提としない。
- LUTは#9比+8.06%、CLBは+1.42%。LUT当たり性能を改善したわけではないが、
  同じデバイスの6コアを維持して全体スループットを上げられた。
- CLB使用率98.49%、core setup WNS +0.116 nsで余裕は小さい。
  今回の結果から、追加規則をさらに載せられるとは判断しない。

## 保存物と再現手順

生ログ・環境・集計スクリプトは開発機の `results/bivalue_pairs_kv260/` に保存。
実機では `/root/fades/bivalue_pairs/` 配下。`results/` はGit管理外なので、
比較に必要な12回分の数値を上記CSVと本文にも保存した。
実装・検証・評価文書は `11-shared-segment-bivalue-pairs` ブランチで管理する（#11）。

firmware SHA256:

- モード2: `428393bf1c67347783b40be972f4571e2bb3f7da9e21538e7638a8fe6845f144`
- モード4: `813dd2451f90ccfc3b6d02aa2daf51eb38d7121a55e6f18e5e23792ed38e3716`

測定に使ったPS実行ファイルSHA256:
`9222fe51da3fbd752b09d01d210ef5be7fdf5dfd328bc5be44682731bd722151`（fades_benchmark）。

回路ごとにモジュール・アプリを切り替え、実クロックとCPU設定が同じことを確認してから、
rootで以下を実行する。

```sh
python3 /root/fades/bivalue_pairs/host/tests/check_abi.py
/root/fades/bivalue_pairs/build/kv260-ps/host/fades_verify \
    /root/fades/bivalue_pairs/sim/data/17puz49158.txt 8193 6 1
/root/fades/bivalue_pairs/build/kv260-ps/host/fades_verify \
    /root/fades/bivalue_pairs/sim/data/HardestDatabase110626.txt 375 1 1
/root/fades/bivalue_pairs/build/kv260-ps/host/fades_verify \
    /root/fades/bivalue_pairs/sim/data/17puz49158.txt 1024 3 1 mixed
for run in 1 2 3; do
    /root/fades/bivalue_pairs/build/kv260-ps/host/fades_benchmark \
        /root/fades/bivalue_pairs/sim/data/17puz49158.txt 10
    /root/fades/bivalue_pairs/build/kv260-ps/host/fades_benchmark \
        /root/fades/bivalue_pairs/sim/data/HardestDatabase110626.txt 10
done
```
