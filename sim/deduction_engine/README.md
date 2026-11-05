# セグメント共通解析への再構成：シミュレーション段階の結果

Singlesのみの推論単体・探索込みサイクル測定、Kaggle 100万問と供給系の検討は
[SINGLES_RESULTS.md](SINGLES_RESULTS.md) を参照。

この文書は第1段階（機能等価な共通化）の記録。
続く第2段階では、#9より強い固定領域Pair規則をモード3として追加した。
結果と検証範囲は [Pair評価結果](../segment_rules/PAIR_RESULTS.md) を参照。
以下の「全モード」は第1段階の0/1/2を指す。
後続のモード4の配置配線結果は [実装評価](../segment_rules/PAIR_IMPLEMENTATION.md) を参照。

## 結論と停止位置

**3セルの存在・重複情報を共通化したRTLは、変更前と全モードで1更新ごとに完全等価。**
全問題集のコアシミュレーションでも、結果・サイクル数・イベント記録を維持した。
今回の変更によるサイクル削減は0%。LUT・CLB削減、配線、周波数、搭載コア数への効果は未確認。

ユーザー指示に従い、シミュレーション・形式検証段階で停止する。
指示前に比較元のVivado合成を開始していたが、240秒でタイムアウト終了し、
比較用の合成済みレポート・チェックポイントは未生成。配置配線には進んでいない。
指示前のYosysコア構造チェック `synth-core` は成功した。
指示後は技術マッピングを伴う合成・配置配線を実施していない。

関連: #8（triad移植）、#9（固定3セル規則）。
基準コミット: `e19edbfc88423d0fccec8a729d8554ffc7712fc0`。
実装・検証コードは `11-shared-segment-bivalue-pairs` ブランチで管理する（#11）。

## 再構成した構造

3セルの候補数字マスクをA・B・Cとして、`segment_occurrences` が次を生成する。

```text
E = A | B | C                    数字ごとの候補の存在
M = (A & B) | (A & C) | (B & C) 数字ごとに2セル以上の候補が存在
```

- 横27・縦27の54セグメントで計算。
- 行は横3セグメント、列は縦3セグメント、ブロックは横3セグメントを集約する。
- ユニットの存在・重複情報からHidden Singleと数字候補なしの矛盾を求める。
- 同じEを `inference_rules` へ渡し、Pointing／Claimingと固定Tripleにも利用する。
- 確定数字の出現・重複解析は既存方式を使う。
- 状態幅729 bit、コアの制御、探索・分岐、RAM構成、更新フェーズは従来どおり。

ユニット内の集約は以下。

```text
unit_seen  = E0 | E1 | E2
unit_multi = M0 | M1 | M2 | (E0 & E1) | (E0 & E2) | (E1 & E2)
unique     = unit_seen & ~unit_multi
```

元RTLの候補側解析は27ユニットごとに下位3個＋上位1個、計108個の
9 bit三分岐マージを記述していた。今回は下位54個＋上位27個の計81個へ明示的に共有し、
規則側のセグメントORも同じEへ接続した。
これは**RTL上の構造の数え方であり、LUTが25%減るという意味ではない**。
旧RTLでも合成ツールが重複式を共有していた可能性があり、実際の資源差は未確認。
共有信号のファンアウト・配線距離が増える可能性もある。

## 機能検証

### 変更前RTLとの形式等価検証

`compare_revision.py` で基準コミットのRTLを取り出し、現在のRTLとのmiterを作成する。
全729入力bitを無制約にしたSAT検証で、`SEGMENT_RULES=0/1/2` すべてPASS。
正常な盤面だけでなく、不正・矛盾状態も含む。

比較出力は候補729 bit、完成、矛盾、強制衝突、行変更9 bitのすべて。
この処理はYosysの展開・SATであり、技術マッピングや配置配線は行わない。

さらに既存の独立prefix-scan参照による解析の形式検証、Locked Candidatesの形式検証、
行変更情報の形式検証、有向試験、全モードのRTL／スカラーモデル比較、lintが成功した。
モデル試験はモードごとに8,000状態、54位置×64パターンの正解候補保持、
全54位置の除去先・矛盾を検査する。

### 単一コアRTLシミュレーション

17ヒント49,158問、Hard 1106の375問、それぞれに付随する4,096件のストレス入力を実行。
各モード・各問題集で、変更前の保存済みCSVと総計JSONが完全一致した。
17ヒントは512件、難問は375件を先行投入し、通常データセットは入出力停止なし。
表は通常データセット部分のみ。サイクル数にはRAM待ちと最後の排出を含む。

|モード|17ヒント 総cycle|cycle/問|難問 総cycle|cycle/問|変更前からの差|
|---|---:|---:|---:|---:|---|
|0：Singles＋Locked Candidates|1,281,134|26.0616|319,367|851.6453|すべて0|
|1：固定3セルの局所矛盾を追加|1,280,390|26.0464|319,348|851.5947|すべて0|
|2：固定Tripleの除去も追加|1,167,141|23.7426|298,477|795.9387|すべて0|

解とヒント保持は既存試験の独立CPU参照でも確認する。
比較元は `build/segment-eval/`、今回の結果は `build/segment-sharing/sim/`。
CSV・JSON・SATログはGit管理外の生成物。
今回の変更について新たな多コア実行試験・ボード実機測定は行っていない。

## 再現手順（合成・配置配線なし）

```sh
cmake --preset host -B build/segment-sharing/sim \
    -DFADES_BUILD_HOST=OFF -DFADES_CORES=6 -DFADES_SEGMENT_RULES=0
cmake --build build/segment-sharing/sim --target \
    deduction-engine-build segment_model_test_0 segment_model_test_1 segment_model_test_2 \
    core_test core_test_segment_1 core_test_segment_2 --parallel 4
ctest --test-dir build/segment-sharing/sim --output-on-failure -j 3 \
    -R '^(deduction-engine|formal-deduction-engine|formal-row-changes|formal-locked-masks|segment-model-[012]|segment-core(-hard)?-[12]|fades-core(-hard)?|lint-.*)$'
python3 sim/deduction_engine/compare_revision.py \
    e19edbfc88423d0fccec8a729d8554ffc7712fc0 build/segment-sharing/equivalence
```

17ヒント＋ストレス入力の変更前CSVとの完全一致は、3モードそれぞれについて
`sim/fades_core/core_results.py compare` で確認した。
難問＋ストレス入力はCSVのバイト一致と総計JSONの一致を確認した。

## この時点での判断

- 共有解析を既存規則の共通入力とする、機能等価な再構成の足場ができた。
- 現在の段階では性能改善や追加規則を搭載できる余地を証明していない。
- 面積が減るか、共有によって配線が悪化しないかは物理評価が必要。
- 既存の直接伝播を維持したため、#8のような補助状態経由の更新回数増加は発生しない。
- 次段階では資源・遅延の改善を確認してから、局所Pair／Tripleや帯サポートの
  入れ替えを検討する。ユーザーの再開指示までは合成・配置配線へ進まない。
