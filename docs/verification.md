# 検証

## ローカル

```sh
make test
make lint
make synth
make formal
make reference-full
make core-test JOBS=4
make stream-test JOBS=4
make fifo-test
make host
make host-input-test
make public-check
```

`test` は推論規則・分岐・CPU 参照の有向試験に加え、AXI-Lite の独立 AW/W、WSTRB、
停止中の応答保持、実行中の設定保護、snapshot handshake を試験する。
`axi-test` だけを実行することもできる。

`public-check` は Git の追跡対象と未追跡・非無視ファイルだけを `build/` 内の一時コピーへ
展開し、相対 Markdown リンクと lint/test/synth/host、ホスト入力形式を確認する。
ローカル専用メモや生成物に依存しないことを検証し、コピーは終了時に削除する。

`core-test` は独立 CPU 参照解、入力ヒント、数独の制約を検査し、49,158 問に加えて
4,096 件の完成済み・複数解・解なし・矛盾・バックトラック・キュー周回・停止・reset を試験する。
結果は `build/core/results.csv` と集計 JSON。短縮時は `LIMIT=512` 等を指定する。

`stream-test` は全入力を 64 byte にエンコードして非調和な DMA/core クロックで送受信し、
回収順によらず ID で照合する。追加 322 件では不正セル値 10〜15、解なし、完成済み、
空の複数解問題、入力/出力停止、FIFO の空・満杯・周回、未完了上限、端数 packet を検査する。
出力停止中のデータ保持、未回収結果・部分レコード・探索中の reset と新しいバッチの
整合性、不完全 TKEEP / 途中 TLAST の sticky fault も試験する。

Verilator では抽象 FIFO モデルを使用する。`fifo-test` は Vivado の xvlog/xelab/xsim で
実際の XPM FIFO を使い、非同期クロック、full、pointer 周回、停止中保持、reset/reuse、
write-data-count が full 占有数へ到達することを検査する。
ログは `build/fifo_xpm/test.log`。

既存基準とのサイクル回帰は `make regression BASELINE=/path/to/results.csv`。
CSV と隣接する `.totals.json` を用意する。

## 実機

回路とドライバーのビルド・ロード後、root で以下を実行する。

```sh
python3 host/tests/check_abi.py
build/host/fades_verify sim/data/17clues.txt 1 1 1
build/host/fades_verify sim/data/17clues.txt 1024 3 1 mixed
build/host/fades_verify sim/data/17clues.txt 8193 6 1
build/host/fades_benchmark host/tests/data/stress_mixed.txt 0.05
build/host/fades_benchmark sim/data/17clues.txt 10
```

`check_abi.py` は二重 open、不正引数、mmap の長さ/offset/MAP_SHARED、close/reopen を検査する。
`fades_verify` の検証範囲と `fades_benchmark` の時間定義は [host/README](../host/README.md) を参照。
特に、反復測定の中間 round の全解を CPU で毎回照合しているわけではない。
共通パーサーは LF / CRLF、セルの順序、空欄、文字種・長さの拒否を有向試験する。
`host-input-test` は両実行ファイルの Usage と不正テキストを、デバイスを開く前に拒否することを確認する。

## 外部の難問集合

Hard 1106 を使う場合は Tdoku の `data.zip` 内の
`data/puzzles6_forum_hardest_1106`（375 問）を用意する。
両ツールは空欄 `.`・数字 `1〜9` の 81 文字表記を直接読める。

BinarySudoku の空白区切り版と元データの一致は次で確認できる。

```sh
python3 sim/scripts/compare_tdoku_dataset.py /path/to/hardestdatabase.txt /path/to/tdoku/data.zip --output /path/to/hardest81.txt
```

元データの照合用に空白・空欄記号を正規化し、問題集合と順序の一致、SHA256 を出力する。
`--output` は正規化済みの 81 文字表記を保存する。
`fades_verify /path/to/hardest81.txt 375 1 1` で一巡を検証できる。
