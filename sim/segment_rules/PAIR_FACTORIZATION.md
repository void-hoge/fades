# Pair軽量化：セル特徴とセグメント重複情報を再利用する

本書は合成前の分析記録。後続のユーザー指示により、2026-11-05にモード4を
6コア・core 100 MHz / DMA 200 MHzで配置配線し、タイミングとexport検査を通過した。
実際の資源量は [PAIR_IMPLEMENTATION.md](PAIR_IMPLEMENTATION.md) を参照。
後続の実機測定は [PAIR_KV260_RESULTS.md](PAIR_KV260_RESULTS.md) に記録。

## 結論

**モード4：同じ2候補を持つセル対に限定し、セル特徴と既存の重複情報Mを共有する方法が有望。**
前回のモード3（全2セル部分集合）と比べ、全問題集の総サイクル増加は0.20%／0.13%にとどまり、
両問題集とも各問題の分岐回数はすべて一致した。

固定した式展開・共有方法によるBoolean DAG比較では、Pair追加部分の2入力ANDノードを
10,206 → 6,750（約33.86%減）にできた。
**この数値はLUT・CLBの削減率ではない。** 実際の資源・配線・周波数は未確認。
ユーザー指示に従い、今回も合成・配置配線・実機評価には進んでいない。

関連: #8、#9、#11。実装ブランチ: `11-shared-segment-bivalue-pairs`。

## 1. 最初に検討した完全等価な共有化

候補集合X・Yの和集合が2種類になる条件は、次の和で表せる。

- Xが2候補でYがXの部分集合。
- Yが2候補でXがYの部分集合。
- X・Yが異なるSingleton。

空集合・矛盾状態を含む全入力について、元の和集合の個数判定と完全等価であることをSATで証明した。
セルごとの2候補判定を共有できるが、双方向の包含判定、Singleton条件、マスク統合が増える。
今回のBoolean DAG比較では、対象範囲全体で16,767 → 20,007 ANDノードへ約19.32%増えた。
論理深さも17 → 19となり、この表現を本体へ採用する根拠は得られなかった。
候補は形式検証ファイル内だけに残し、通常のモード3は元の和集合判定を維持した。

## 2. 高価なケースを分離する

モード3の2セル集合規則には、次が混ざっている。

|対象2セル|役割|
|---|---|
|{1,2} と {1,2}|Singlesだけでは出ない、新しいPair制約|
|{1} と {1,2}|Singlesの反復で得る結論を短縮する経路|
|{1} と {2}|同じ更新で既存Singlesが除去する数字|
|空集合を含む|既存検査が入力状態の矛盾を検出する|

そこでモード4は最初のケースだけを専用回路にする。モード3と1更新ごとの出力は異なる。
省いたSingleton＋2候補のケースについては、**合法な相異割当が存在する任意の3セル候補集合で、
Singles＋同一2候補Pairの局所更新2回が、完全Pair＋Singlesの1回の除去を包含する**ことをSATで証明した。
対象は3セル内の除去と、その行・ブロック等を共有する外部への予約数字。
この証明は局所的な伝播関係であり、ソルバー全体の実行が全入力で等価という主張ではない。

## 3. 具体的な共有回路

### セルの2候補判定を共有

```text
rest = candidates & (candidates - 1)
one  = candidates != 0 && rest == 0
two  = rest != 0 && (rest & (rest - 1)) == 0
```

Naked Singleで使う最初の `rest` を、2候補判定でも利用する。
81セルで計算した `two` は、それぞれ横・縦の計4個の固定セル対に使う。
モード3の「162組それぞれの和集合に対する2候補判定」を置き換える構造。
モード0/1/2/3ではこの新しい2候補特徴は推論結果に使用しない。

### 集合一致を片方向の包含判定へ置き換え

```text
pair_AB = two_A && two_B && ((A & ~B) == 0)
```

両方の集合の大きさが2と分かっているため、AがBの部分集合ならA=B。
一般的な9 bit一致比較の代わりに、この条件を使える。
任意の9 bit候補集合について、元の「同じ2候補」という条件との完全等価をSATで証明した。

### 外部へのマスクを、Hidden Single用のMから生成

3セルA・B・Cの、数字ごとの重複情報は既存の共通解析にある。

```text
M = (A & B) | (A & C) | (B & C)
```

もしA=B=Xなら、

```text
M = X | (X & C) = X
```

となる。他のセル対でも同様で、Cの内容に依存しない。
したがって、同一2候補Pairが存在する場合の外部への予約数字は、

```text
reserved = (pair_AB | pair_AC | pair_BC) ? M : 0
```

でよい。これは既存のHidden Single用の重複情報から、Pairの除去マスクを得る方法。
セグメント内の対象外セルへの局所マスクは、各Pairの成立フラグで個別に生成する。
既存のTripleと静的な外部配布回路を共有し、状態729 bit・更新フェーズ・分岐方法は維持する。

## 4. 論理構造の比較（FPGA資源の見積もりではない）

`pair_structure.py` が2入力ANDと反転辺のDAGを直接構築して数える。
対象は81セルの空・Singleton・確定数字、54セグメントのE/M、162個のPair局所マスク、
54個の外部予約マスク。ユニット上位集約、Triple、コア制御等は含まない。

還元ORは平衡木、`x & (x-1)` は `output[i] = x[i] & OR(x[0:i])` の恒等式で展開。
同一式の共有と自明な定数簡約だけを行い、論理書き換え・LUTマッピング・配置・遅延評価は行わない。
この展開方法・対象範囲に依存する構造の指標であり、最小回路の証明でもない。

|構成|対象範囲のANDノード|共通部6,561を引いたPair増分|最大AND段数|
|---|---:|---:|---:|
|モード3：和集合の2候補判定|16,767|10,206|17|
|完全等価なセル特徴共有案（不採用）|20,007|13,446|19|
|同一2候補Pair・通常のマスクOR|13,689|7,128|18|
|モード4：同一2候補Pair＋M再利用|13,311|6,750|18|

モード4はモード3に対して、対象範囲全体で約20.61%、Pair増分で約33.86%少ないANDノード。
一方、最大AND段数は1段増えており、周波数改善を示す結果ではない。
FPGAでは元の表現も最適化・共有されるため、この差がそのままLUT差として残る保証はない。
再現用の集計を `pair_structure.json` に保存した。

## 5. 単一コアRTLシミュレーション

17ヒント49,158問、難問375問、それぞれに付随する4,096件のストレス試験が成功。
独立CPU参照で解と候補制約を確認した。以下の総cycleは通常問題集部分のみで、
先行投入512件／375件、入出力停止なし、RAM待ちと最後の排出を含む。

|方式|17ヒント cycle/問|難問 cycle/問|#9比・同クロック速度向上|
|---|---:|---:|---|
|#9：モード2|23.7426|795.9387|基準|
|完全Pair：モード3|23.0510|785.8400|+3.0005% / +1.2851%|
|軽量化候補：モード4|23.0971|786.8853|**+2.7949% / +1.1505%**|

|モード3 → 4|17ヒント|難問|
|---|---:|---:|
|総cycle|1,133,141 → 1,135,408|294,690 → 295,082|
|総cycle増加|+0.2001%|+0.1330%|
|推論cycle|1,076,455 → 1,078,734|263,345 → 263,743|
|分岐回数|36,071 → 36,071|30,104 → 30,104|
|矛盾イベント|26,276 → 26,276|28,037 → 28,037|

各問題の分岐回数も49,158問／375問すべてで一致した。
ただし探索した候補状態の全履歴の一致を検査したわけではなく、全入力の探索等価性は主張しない。
モード3比で完了間隔が増えた問題は2,061問／126問、減った問題は0問／0問。
モード3の速い直接伝播の一部を省略したことによる、小さいサイクル増加を観測した。
集計元は `pair_benchmark.csv`。

## 6. 検証と判断

- モード0/1/2の基準コミットとの全入力形式等価検証は成功。
- 全5モードで独立モデルとの8,000状態照合、既知正解候補保持3,456件が成功。
- モード4でも、全54位置・全セル対・全数字対の5,832件で#9の固定点から追加除去を確認。
- 4数字の全候補集合組み合わせを横縦で計8,192件検査。
  モード3は1更新、モード4は局所伝播後の候補が、実際の割当列挙のサポートと一致。
- セル特徴、E/M共有、Pairの安全性、同一2候補検出、Mを使う外部予約、2更新の包含関係を形式検証。
- モード3/4の全問・ストレス試験とlintが成功。過去のモード2/3通常問題集の総計も維持。
- 多コアの新規実行試験、技術マッピング合成、配置配線、実機評価は実施していない。

モード4でも#9比の難問の利益は約1.15%で、同じコア数なら周波数を約98.86%以上に保つ必要がある。
6→5コアへの減少も補えない。現時点では「共有する中間情報と規則の選び方に改善案がある」
という結果であり、KV260へ収まる設計が完成したという意味ではない。

次に物理評価を行うなら、最初に共通化後のモード2を基準として、モード3/4の増分を比較する。
今回は合成前で停止し、ユーザーの再開指示を待つ。

## 再現手順（合成・配置配線なし）

```sh
cmake --preset host -B build/segment-sharing/sim \
    -DFADES_BUILD_HOST=OFF -DFADES_CORES=6 -DFADES_SEGMENT_RULES=0
cmake --build build/segment-sharing/sim --target \
    segment_benchmark segment_model_test_0 segment_model_test_1 segment_model_test_2 \
    segment_model_test_3 segment_model_test_4 core_test_segment_3 core_test_segment_4 --parallel 4
ctest --test-dir build/segment-sharing/sim --output-on-failure -j 3 \
    -R '^(segment-model-[01234]|segment-core(-hard)?-[34]|formal-(segment-pairs|pair-factorization|bivalue-pairs|pair-two-rounds|deduction-engine|locked-masks|row-changes)|lint-segment-core-[1234])$'
python3 sim/deduction_engine/compare_revision.py \
    e19edbfc88423d0fccec8a729d8554ffc7712fc0 build/segment-sharing/equivalence
python3 sim/segment_rules/pair_structure.py --output build/segment-sharing/pair-structure.json
build/segment-sharing/sim/sim/segment_rules/segment_benchmark \
    sim/data/17puz49158.txt build/segment-sharing/bivalue-17-model.csv
build/segment-sharing/sim/sim/segment_rules/segment_benchmark \
    sim/data/HardestDatabase110626.txt build/segment-sharing/bivalue-hard-model.csv
python3 sim/segment_rules/compare.py \
    build/segment-sharing/sim/sim/segment_rules/core-2.csv \
    build/segment-sharing/sim/sim/segment_rules/core-3.csv \
    build/segment-sharing/sim/sim/segment_rules/core-4.csv
```
