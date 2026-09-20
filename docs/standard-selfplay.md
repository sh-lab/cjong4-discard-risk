# standard 4人の対局から学習する

`cj4dr_generate_standard` は `cj4_opponent_standard(1)` を4人に割り当て、
既定ルールの対局を最後まで進めます。打牌判断前の局面をすべて保存し、
通常の捨て牌で実際にロンが成立した局面には `metadata.actual_ron=true` を付けます。
リーチ宣言牌によるロンも含み、槍槓はこの印の対象にしません。

## 入力・教師・管理情報

入力は**放銃者が牌を切る直前**のマスク済みRGB408バイトです。
牌を切った後や和了後の画像を入力にしません。他家手牌・山順も入力しません。
プレイヤー番号の相対化だけを行い、牌ID・5画像・RGBの構成は維持します。

教師方式は **staged-v1** です。各局面の合法な普通打牌だけをmask=1にし、
持っていない牌種・合法に切れない牌種はmask=0にします。
**実際に通常ロンされた牌種だけ255（FF）**とし、仮に切ればロン可能という理由でFFにはしません。
ロンしなかった打牌を自動的に00にすることもありません。

優先順位は「実ロン255 → 他家3人全員に安全と確定なら0 → その他は該当条件の最大値」です。
未確認人数は、その牌種が現在安全と確定できない相手の人数です。

| 条件 | 教師値（10進） |
| --- | ---: |
| リーチ者に安全未確認 | 192 |
| 3〜4副露の相手に安全未確認 | 192 |
| 1〜2副露の相手に安全未確認 | 128 |

| 残り通常ツモ枚数 | 未確認3人 | 未確認2人 | 未確認1人 |
| --- | ---: | ---: | ---: |
| 46枚以上 | 64 | 32 | 16 |
| 45〜22枚 | 128 | 96 | 64 |
| 21〜0枚 | 192 | 160 | 128 |

副露はチー・ポン・明槓・加槓を数え、暗槓は含めません。加槓も1副露です。
嶺上牌は残り通常ツモ枚数に含めません。これらは教師を作る条件でありNN入力を増やしません。
値は初期学習用の危険度スコアで、放銃確率ではありません。

安全の根拠は次を使います。他家の隠れた手牌やフリテンフラグから安全を付与しません。

- 相手の現物。鳴かれて河から移動した牌も捨て牌履歴に残るため含めます。
- 全員の応答がパスで完了した牌。非リーチ者には次のツモまで、リーチ者にはツモ後も保持します。
- 鳴き・槓で手牌構成が変わった相手の通過履歴は消去し、次局では全員分を初期化します。
- 他家の鳴きが成立した牌は、全員がロンを見送ったとは扱わず、通過履歴に追加しません。
- 残り0枚では河底の役が新たに成立しうるため、非リーチ者の通過履歴を消去します。

安全を証明できないケースは未確認に残す保守的な判定です。筋などの確率的な安全は00にしません。

以下の管理情報はNN入力には使いません。

- `actual_ron`: 実際に通常の捨て牌でロンされたか。
- `discarded_tile`: 実際の打牌の物理牌ID。打牌せず和了・槓等を選んだ判断では-1。
- `winner_mask`: 実際の通常ロンの和了者。絶対座席のビットマスク。
- `player`: 打牌判断者の絶対座席。
- `seed`, `game`, `round`, `step`: 再現・追跡用。
- `teacher_policy=staged-v1`, `generator_version=2`。
- `source=standard-selfplay`, `opponent=standard(1)`, 生成器/入力のバージョン、ルール。

ロン成立を待ってから、保持していた**打牌前**の入力・教師と結果の管理情報を保存します。
ロンしなかった判断も保存するため、通常分布の検証・最終評価に使えます。
ロンが発生した局の過去の打牌を、まとめてFFにすることはありません。

## Linuxでビルド

`cjong4-workspace` 内の `cjong4`・`cjong4-opponent`・`cjong4-discard-risk` を使います。
ソースから組み込む場合は、親のcjong4ターゲットを再利用できる `cjong4-opponent` 1.0.5以降が必要です。
workspaceに登録された1.0.5は対応済みで、opponent側の追加変更は不要です。

`cjong4-workspace` のルートディレクトリから実行します。
以降の生成・学習・評価コマンドは、移動先の `cjong4-discard-risk` 内で実行してください。

```sh
cd cjong4-discard-risk
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r training/requirements.txt

cmake -S . -B build \
  -DCJONG4_SOURCE_DIR=../cjong4 \
  -DCJ4DR_OPPONENT_SOURCE_DIR=../cjong4-opponent \
  -DCJ4DR_BUILD_STANDARD_GENERATOR=ON \
  -DCJ4DR_BUILD_TOOLS=ON \
  -DCJ4DR_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

既にインストールした `cjong4-opponent` 1.0.4以降を使う場合は、
`CJ4DR_OPPONENT_SOURCE_DIR` を省略して `CMAKE_PREFIX_PATH` で指定できます。
この生成器は任意機能であり、既存の推論ライブラリ・生成器だけをビルドする場合は不要です。
ソース指定時はcjong4を二重にビルドせず、同じライブラリを共有します。

## 生成と対局単位の分割

まず100対局を再生成します。以前の00/FFデータと混ぜず、新しい保存先を使います。
旧JSONLには全状態遷移がないため、この教師へ単純変換せず同じseedで対局を再実行します。
既存チェックポイントから再開せず、新しいrunで学習してください。
生成時間が長くなる場合はtmux等の中で実行してください。

```sh
mkdir -p datasets/standard-staged-v1
./build/cj4dr_generate_standard \
  --games 100 --seed 82000 \
  --output datasets/standard-staged-v1/all.jsonl

python -m training.data pack \
  datasets/standard-staged-v1/all.jsonl datasets/standard-staged-v1/all.npz
python -m training.data split \
  datasets/standard-staged-v1/all.npz datasets/standard-staged-v1/split --seed 1
```

`--games` は局数ではなく対局数です。連荘・得点精算・次局への遷移もcjong4に従います。
`--max-steps` は1対局の上限（既定10,000）です。
上限・不正遷移・書き込み失敗等は明示的なエラーにし、不完全な完成ファイルを残しません。
既存ファイルを上書きせず、`.partial` で作成して成功時だけ完成名で公開します。

`--start-game` で対局番号を指定できます。同じseedで番号の重ならない範囲へ分ければ、
並列生成したファイルを結合できます。各対局の乱数系列は独立しています。

```sh
./build/cj4dr_generate_standard --games 50 --start-game 0 --seed 82000 \
  --output datasets/standard-staged-v1/part0.jsonl
./build/cj4dr_generate_standard --games 50 --start-game 50 --seed 82000 \
  --output datasets/standard-staged-v1/part1.jsonl
```

groupは引き続き `standard-v1/seed-…/game-…` です。教師方式を変えても同じ対局は
同じIDにすることで、旧教師と新教師をまたいだ対局の混入を検出できます。同じ対局の全局・全座席・全局面をまとめます。
**ロン局面の抽出・再サンプリングより先に、対局単位で学習・検証・評価へ分割します。**
生成器・cjong4・opponentのリビジョンも実験時に記録し、異なる実装で再生成した同じ対局IDを混ぜないでください。

## 段階教師で初期学習

```sh
python -m training.train \
  --train datasets/standard-staged-v1/split/train.npz \
  --validation datasets/standard-staged-v1/split/validation.npz \
  --output runs/risk-staged-v1 \
  --epochs 100 --batch-size 256 --lr 0.003 --seed 1 \
  --ron-fraction 0 --selection-metric mse --device cpu
```

初期学習では **`--ron-fraction 0 --selection-metric mse`** を使い、
中間値を含む教師全体の再現を確認します。実ロンの増量は後の比較実験用です。

`--ron-fraction 0.5` は、学習epochの行数を保ちながら、約半分を実際のロン直前局面、
残りをその他の局面から復元抽出します。各行に含まれる他の合法打牌の教師も使います。
0なら従来通り全行を1回ずつシャッフルします。正の値を指定したのに学習データに
ロン局面または通常局面がない場合はエラーです。未指定マスクだけの行は抽出しません。
管理情報を元に抽出しますが、モデルへはRGBしか渡しません。

**検証・最終評価には再サンプリングを適用しません。**
個別ケースを併用する場合も、元ケース単位の分離を守り、通常対局の最終評価セットを保持してください。
再サンプリングした学習分布からのスコアを、そのまま実戦の放銃確率とは扱いません。

`--selection-metric auc` は通常分布の検証データで非ゼロ教師を高く順位付けできるモデルを保存します。
AUCが同じ場合は通常MSEが低いモデルを優先します。
`mse`（既定）は従来通りの全指定要素MSE、`balanced` は安全/非ゼロ教師のMSEの平均です。
AUC・balancedには、検証データに両方の教師が必要です。
balanced MSEは一定の中間値を返すだけでも改善しうるため、識別性能はAUC等で別途確認します。

## 全00化の検出と勾配

学習v2では、NNのforwardは従来通りの整数量子化・切捨て・飽和です。
backwardでは、活性化の範囲内は勾配をそのまま、範囲外は0.01倍で通します。
誤った00/FF側に飽和しても復帰できるようにする学習専用の近似勾配であり、
C推論のReLUや出力を変更する処理ではありません。既定学習率は0.003へ下げました。

- `zero_baseline_mse`: 全指定要素を00と予測した場合のMSE。
- `baseline_improvement`: その基準に対する改善率（比率。0.1は10%、負なら悪化）。
- `safe_mse`, `danger_mse`: 教師値0／非ゼロそれぞれのMSE。
- `danger_mean_prediction`, `danger_zero_predictions`: 危険教師への平均予測と00の件数。
- `all_zero_predictions`: 教師指定された全要素への予測が00か。
- `per_target`: 各教師値の件数・平均予測・MAE。段階値を再現できているか確認します。
- `ff_labels`, `ff_mean_prediction`, `ff_mse`: FF教師だけの件数・平均予測・MSE。
- `ff_auc`: FF対それ以外の順位性能。staged-v1ではFFが実ロンですが、
  その他の候補牌は実際に切っていない場合もあるため、全牌の真のロン可否AUCではありません。
- `nonzero_auc`: 非ゼロ教師を高く予測する順位性能。同点のみなら0.5、完全な順位なら1。
- `balanced_mse`: 安全側・非ゼロ側MSEの平均。片側だけの場合はnull。

非ゼロ教師のない評価では危険側指標やAUCはnullです。
`nonzero_auc`の正例は「非ゼロ」で、中間値も含みます。実ロン識別とは区別してください。
FFまたはそれ以外の指定教師がない場合、`ff_auc`もnullです。
全00でなくなったことやAUCの改善だけで、危険度の尺度・実戦精度が保証されるわけではありません。

学習チェックポイントはv2です。v1チェックポイントは推論・INT8書き出しできますが、
勾配と学習設定が変わるためv1からの途中再開は拒否します。新しい出力先で学習してください。
v2内の再開は同じ `--ron-fraction`・`--selection-metric` を含む設定で `--resume …/last.pt` を指定します。

## 書き出し・最終評価

```sh
python -m training.export \
  runs/risk-staged-v1/best.pt runs/risk-staged-v1/model.i8 \
  --validation datasets/standard-staged-v1/split/validation.npz \
  --c-evaluator build/cj4dr_evaluate
python -m training.evaluate \
  runs/risk-staged-v1/model.i8 datasets/standard-staged-v1/split/test.npz \
  --exclude datasets/standard-staged-v1/split/train.npz \
  --exclude datasets/standard-staged-v1/split/validation.npz \
  --c-evaluator build/cj4dr_evaluate > runs/risk-staged-v1/test-metrics.json
```

NNは同じ4,897バイト形式、入力408バイト・出力34バイトです。
C照合は出力一致の検証で、危険牌を識別できることの証明とは分けて評価してください。
