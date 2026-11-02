# 実測結果

## 条件

2026-11-02、KV260 revB / K26、Vivado 2024.2、Ubuntu 24.04.5 Arm64、
Linux `6.8.0-1036-xilinx`。コア 100 MHz × 6、DMA 200 MHz、未完了上限 2 / core。
Linux clock summary の実値は core 99,999,999 Hz、DMA 199,999,998 Hz。

以下の性能値は名称整理前の同一回路構成で測定した記録。
公開版のアプリ名・デバイス名は `fades` / `/dev/fades`、ハードウェア version は
`0x46414401`。現在のビルド手順とコマンド名はリポジトリ内の README に従う。
ローカルの生ログは `results/` に保存し、このファイルに公開する測定条件と要約を載せる。

## 機能検証

- 17 ヒント 49,158 問を独立 CPU 参照解と全件照合。
- Tdoku Hard 1106 の 375 問も、一巡の実機結果を CPU 参照解と全件照合。
- ID の欠落・重複なし、status、数独の制約、ヒント保持、予約領域を確認。
- 解なし・不正入力、FIFO 満杯・周回、非同期クロック、入力/出力停止、
  部分入力・未回収出力を含む reset をシミュレーションと実機試験で確認。
- ユーザー空間 ABI、排他的 open、不正 ioctl / mmap / 転送引数を実機で確認。

反復測定では warmup と最終バッチを全件検証する。
中間 round は DMA 完了、時間指定ストレスでは加えて PL の各段・コア別件数を検査する。
計測中の全ての結果を CPU で逐一照合しているわけではない。

## 10 秒ストレス測定

1 round は 8 × 1 MiB = 131,072 件。問題を循環して準備した DDR バッファを反復する。
入力準備・CPU 参照・前後の全件検証は計測外。
計測中は各 round の reset/config、descriptor 準備、DMA、数独処理、出力排出、
PS の完了待ちと snapshot / 件数検査を含む。

|問題集|ファイル内件数|指定時間|排出込み実時間|完了件数|Mproblem/s|
|---|---:|---:|---:|---:|---:|
|17clues|49,158|10 s|10.002298 s|223,477,760|**22.342642**|
|Hard 1106|375|10 s|10.064976 s|7,077,888|**0.703220**|

期限前にバッチ全体の完了を確認できた件数は、それぞれ 223,346,688 件、6,946,816 件。
最終バッチ 131,072 件は期限後に完了して排出した。期限前件数は保守的なバッチ単位の値で、
各レコードの厳密な完了時刻ではない。速度の分母は排出込みの実経過時間。

17clues の DDR バッチはファイル 2 周 + 先頭 32,756 問、Hard 1106 は 349 周 + 先頭 197 問。
件数は再投入を含む延べ件数。どちらもエラー 0、FIFO write-domain peak 65、
各コアの最大未完了数 2。round の境界では全体を排出してから再設定する。

## その他の計測範囲

|条件|結果|
|---|---:|
|17clues 全問、6 × 8,193 件 × 100 rounds、kernel 時間|20.5667 Mproblem/s|
|17clues 巡回、8 × 16,384 件 × 100 rounds、kernel 時間|22.4681 Mproblem/s|
|17clues 全問の first dispatch → last collection|217,682 core cycle / 22.5825 Mproblem/s|
|17clues 全問、入力準備・コピー・RUN・全解検証込み|0.178562 Mproblem/s|
|Hard 1106 全375問一巡、kernel 時間|0.698519 ms / 0.536850 Mproblem/s|

ホスト総合値にも CPU 参照解の事前生成時間は含まない。
PL の区間は最後の結果を回収するまでであり、最後の DMA 出力排出は含まない。

## 初回統合回路の配置配線

|資源|使用量|容量|使用率|
|---|---:|---:|---:|
|LUT|85,508|117,120|73.01%|
|FF|36,273|234,240|15.49%|
|CLB|13,906|14,640|94.99%|
|BRAM tile|128|144|88.89%|
|URAM|0|64|0%|

signoff setup WNS +0.225 ns、hold WHS +0.010 ns、pulse-width WPWS +1.000 ns。
未制約内部 endpoint は 0、全 user timing constraint を満たした。
snapshot skew は 5 ns 要求に対して 2.035 ns。
単一コア OOC の 110 MHz 成功と、実機統合の 100 MHz 成功は別の測定である。

## 入力の出所

- 17clues は [sim/data/README](../sim/data/README.md) に出所と SHA256 を記載。
- Hard 1106 は [Tdoku](https://github.com/t-dillon/tdoku) の
  `data.zip` 内 `data/puzzles6_forum_hardest_1106`。
  BinarySudoku の `hardestdatabase.txt` と 375 問・順序まで一致することを正規化後に確認。
  正規化 sequence SHA256 は
  `fb88ac7cfcc462154a41c62262192ccd40255c39db9bd827e5199f1e1660cd17`。

公開 CPU 値との比較では、CPU 型番・スレッド数・最初の解か一意性確認か・
入力のランダム変形・計測範囲を揃える。ここに示すスループットは単一問題のレイテンシではない。

## 公開用名称での再ビルド・再検証

同日、公開ソースから `make vivado-kv260 JOBS=2` で新規に合成・配置配線し、
`fades` アプリ、`fades.ko`、`/dev/fades` の組み合わせで再検証した。
ハードウェア identity `0x46414401` をドライバーの probe で確認した。

|資源|使用量|使用率|
|---|---:|---:|
|LUT|86,133|73.54%|
|FF|36,273|15.49%|
|CLB|13,823|94.42%|
|BRAM tile|128 / 144|88.89%|
|URAM|0 / 64|0%|

最終 signoff setup WNS **+0.068 ns**（core 領域 +0.267 ns）、
hold WHS **+0.012 ns**、pulse-width WPWS **+1.000 ns**。
未制約内部 endpoint は 0、全 user timing constraint と bus skew を満たす。
snapshot skew は 5 ns 要求に対し 2.236 ns。
レビュー後の Critical CDC は 0。残る Warning 900 件は XPM FIFO の 388+512 data bit。
ファンの A12 / LVCMOS33 / 4 mA / SLOW を export 時に検査した。

- ABI 拒否試験、3,072 件の mixed（解あり 3,008・解なし 32・不正入力 32）: PASS。
- 全 49,158 問の CPU 参照解照合: PASS。processing window は以前と同じ 217,682 cycle。
- Hard 1106 の全 375 問照合: PASS。processing window は以前と同じ 56,138 cycle。
- 時間指定ツール（現在の `fades_benchmark`）で 17clues を 10 秒実行: **223,084,544 件 / 10.001994 秒 =
  22.304007 Mproblem/s**、エラー 0。
  期限前にバッチ完了確認できた件数は 222,953,472、最後の 131,072 件は排出分。
  warmup と最後の各 131,072 件を全件検証し、中間は DMA / PL 件数を確認。
- fan PWM 15 / enable 1、実測後の温度は約 31.4〜33.4 ℃。

新しいレポートは `build/kv260/`、ローカル再検証ログは `results/public_cleanup/` に保存。

## テキスト形式とプログラム名の統一後

同梱入力を `17puz49158.txt` と同一の 81 文字・ドット表記へ差し替えた。
49,158 問の内容と順序に変更はなく、単一コア全問の 1,281,134 cycle、
実機の全問 processing window 217,682 cycle、難問一巡 56,138 cycle も一致した。
両ツールの共通パーサー、LF/CRLF、形式拒否試験と全問照合に成功。

現在の時間指定ツール名は `fades_benchmark`、件数指定の照合ツール名は `fades_verify`。
`fades_benchmark sim/data/17clues.txt 10` の再測定は
**223,215,616 件 / 10.004934 秒 = 22.310553 Mproblem/s**、エラー 0。
期限前のバッチ完了件数は 223,084,544 件、最後の 131,072 件は排出分。
入力準備と前後の全解検証は区間外で、計測中は DMA 完了と PL 件数を確認する。
ローカル生ログと差し替えの同一性検証は `results/input_format/` に保存。
