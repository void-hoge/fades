# 新規問題ロード時のNaked Single先行伝播

この文書は最初のコア内先行伝播実験の記録。
現在の実装は共有入力パイプラインへ移行済み。
構成・検証・再現は `sim/fades/SHARED_PRELOAD_RESULTS.md` を参照。

## 結果（2026-11-06）

main `bd7ce0a` を比較元に、新規問題を `head_cache` から `current` へ
ロードする既存サイクルで、確定数字による候補除去を1回先行する実験を行った。
通常の推論は従来どおりSingles＋Locked Candidates。

**17ヒント49,158問、HardestDatabase110626の375問とも、全問で
ちょうど1 cycle・1 inference減少。探索イベントは全問で変更なし。**

|問題集|問題数|main 総cycle|先行伝播 総cycle|main cycle/問|先行伝播 cycle/問|サイクル削減率|同クロックでの速度向上相当|
|---|---:|---:|---:|---:|---:|---:|---:|
|17ヒント|49,158|1,281,134|1,231,976|26.061557|25.061557|3.8371%|3.9902%|
|Hard 1106|375|319,367|318,992|851.645333|850.645333|0.1174%|0.1176%|

上記の総cycleは最終結果の排出までを含む単一コアRTLシミュレーション。
初期投入は17ヒント512問、難問375問。初期投入は計測外、通常測定中は
enable=1・出力ready=1で、入力供給・出力の外的停止なし。
BRAM待ち・分岐・盤面ロードは計測内。

|問題集|inference main → 先行伝播|branch（両者同じ）|contradiction（両者同じ）|
|---|---:|---:|---:|
|17ヒント|1,210,220 → 1,161,062|47,550|36,230|
|Hard 1106|285,877 → 285,502|32,189|30,094|

通常データセットの各問題について、CSVの `cycles` と `inferences` は
それぞれ必ず−1。`branches`、`contradictions`、`discarded`、
`wasted_inferences` はすべて変更前と一致した。
総load数・memory waitも不変。実行時間の短縮は、通常推論の有効更新を
既存の新規問題ロードに1回分先取りできたことによる。

## 最初の実験の実装範囲

- `rtl/deduction_engine.sv` に `naked_single_preload` を追加。
- 候補が1個のセルから行・列・ブロックの確定数字集合を作り、他セルから除去。
- 新たにできたSingletonは同じ組み合わせ回路内で再伝播しない。
- 既存Singleton自身は変更せず、重複した確定数字も残す。
  不正なヒントが事前処理で消えて矛盾を見失うことを避ける。
- Hidden Singleによる強制は行わないため、新しいforced-conflictメタデータは不要。
- `fades_core` の `take` で、`pop_new` のときだけ先行伝播した盤面をロード。
  保存済みDFS枝への復帰、`child_a` での継続は従来の経路。
- `last_row_changed`、`last_forced_conflict` の初期値は従来どおり。
- 難問375問を測れるよう、テストベンチの初期投入数を
  `min(512, 問題数)` に変更し、`fades-core-hard` を追加。

## 検証

- 変更前・変更後とも、17ヒント全49,158問と難問全375問の独立CPU参照解に一致。
- 各問題集に付随する4,096件のストレス試験PASS。
  複数解入力、候補なし、確定数字重複、探索が必要な解なし、
  forced-conflictメタデータ、入出力バックプレッシャー、enable停止、
  dequeのwrap・同時書き込み・探索中resetを含む。
- 6コアstream統合シミュレーション `fades` PASS。
- `deduction-engine` および `lint-fades_core/fades/fades_axi/fades_kv260` PASS。

これはサイクル数の評価。先行伝播回路とロードMUXの追加後の面積・Fmax、
115 MHzでの成立、実機スループットは今回の測定からは判断しない。

## 保存先

- 変更前：`build/preload-baseline/sim/fades_core/`
- 変更後：`build/preload-singles/sim/fades_core/`
- 両方に `results.csv`、`hard-results.csv` と各 `.totals.json`。

`build/preload-baseline` はRTL変更前に生成・実行したもの。
現在のソースからこのディレクトリを再ビルドすると、変更後RTLに更新される点に注意。

データSHA256：

```text
17puz49158.txt:
58ef7d83e8cbac32495161f9745877fef82f5e8b3fe58e3cad4eb3fc004a81b9
HardestDatabase110626.txt:
ae3bce96647b90d9c3b6eec14080f9513173f562d4b6df65b1a70a8de11a0afe
```

## 現在のソースでサイクル差を再現

現在はコアをmain相当に戻し、テストラッパーで前処理済み入力を与える。
同一ビルドで未処理入力と前処理済み入力を比較できる。

```sh
cmake --preset host -B build/shared-preload -DFADES_BUILD_HOST=OFF
cmake --build build/shared-preload --target core_test core_preprocessed_test --parallel 4
ctest --test-dir build/shared-preload --output-on-failure -j 2 -R '^fades-core.*$'
```

## 比較元の再生成

独立したmainの作業ツリーへ、375問対応のテストベンチだけを渡す。

```sh
git worktree add --detach build/preload-reference bd7ce0a
cp sim/fades_core/core_test.cpp build/preload-reference/sim/fades_core/
cmake -S build/preload-reference -B build/preload-baseline-repro -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DFADES_BUILD_SIM=ON -DFADES_BUILD_HOST=OFF
cmake --build build/preload-baseline-repro --target core_test --parallel 4
ctest --test-dir build/preload-baseline-repro --output-on-failure -j 2 \
  -R '^fades-core$'
build/preload-baseline-repro/sim/fades_core/core_test \
  sim/data/HardestDatabase110626.txt \
  build/preload-baseline-repro/sim/fades_core/hard-results.csv 0
```
