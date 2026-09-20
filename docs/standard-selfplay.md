# standard 4人の対局から学習する

`cj4dr_generate_standard` は `cj4_opponent_standard(1)` を4人に割り当て、
既定ルールの対局を最後まで進めます。打牌判断前の局面をすべて保存し、
通常の捨て牌で実際にロンが成立した局面には `metadata.actual_ron=true` を付けます。
リーチ宣言牌によるロンも含み、槍槓はこの印の対象にしません。

## 入力・教師・管理情報

入力は**放銃者が牌を切る直前**のマスク済みRGB408バイトです。
牌を切った後や和了後の画像を入力にしません。他家手牌・山順も入力しません。
プレイヤー番号の相対化だけを行い、牌ID・5画像・RGBの構成は維持します。

各局面で、完全状態のコピー上で全合法打牌のロン可否を判定します。
他家が1人でもロン可能ならFF、全員不可なら00、未判定の牌種はマスク0です。
実際にロンされた牌種がFFにならなければ、生成をエラーで中止します。
同じ入力に「選んだ牌はFF、別の合法牌は00」の比較教師を付けられます。

以下の管理情報はNN入力には使いません。

- `actual_ron`: 実際に通常の捨て牌でロンされたか。
- `discarded_tile`: 実際の打牌の物理牌ID。打牌せず和了・槓等を選んだ判断では-1。
- `winner_mask`: 実際の通常ロンの和了者。絶対座席のビットマスク。
- `player`: 打牌判断者の絶対座席。
- `seed`, `game`, `round`, `step`: 再現・追跡用。
- `source=standard-selfplay`, `opponent=standard(1)`, 生成器/入力のバージョン、ルール。

ロン成立を待ってから、保持していた**打牌前**の入力・教師と結果の管理情報を保存します。
ロンしなかった判断も保存するため、通常分布の検証・最終評価に使えます。
ロンが発生した局の過去の打牌を、まとめてFFにすることはありません。

## Linuxでビルド

`cjong4-workspace` 内の `cjong4`・`cjong4-opponent`・`cjong4-discard-risk` を使います。
ソースから組み込む場合は、親のcjong4ターゲットを再利用できる `cjong4-opponent` 1.0.5以降が必要です。
workspaceに登録された1.0.5は対応済みで、opponent側の追加変更は不要です。

```sh
cd ~/Project/cjong4-workspace/cjong4-discard-risk
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

まず100対局を作る例です。以前のデータを上書きしない新しい保存先を使います。
生成時間が長くなる場合はtmux等の中で実行してください。

```sh
mkdir -p datasets/standard-v1
./build/cj4dr_generate_standard \
  --games 100 --seed 82000 \
  --output datasets/standard-v1/all.jsonl

python -m training.data pack \
  datasets/standard-v1/all.jsonl datasets/standard-v1/all.npz
python -m training.data split \
  datasets/standard-v1/all.npz datasets/standard-v1/split --seed 1
```

`--games` は局数ではなく対局数です。連荘・得点精算・次局への遷移もcjong4に従います。
`--max-steps` は1対局の上限（既定10,000）です。
上限・不正遷移・書き込み失敗等は明示的なエラーにし、不完全な完成ファイルを残しません。
既存ファイルを上書きせず、`.partial` で作成して成功時だけ完成名で公開します。

`--start-game` で対局番号を指定できます。同じseedで番号の重ならない範囲へ分ければ、
並列生成したファイルを結合できます。各対局の乱数系列は独立しています。

```sh
./build/cj4dr_generate_standard --games 50 --start-game 0 --seed 82000 \
  --output datasets/standard-v1/part0.jsonl
./build/cj4dr_generate_standard --games 50 --start-game 50 --seed 82000 \
  --output datasets/standard-v1/part1.jsonl
```

groupは `standard-v1/seed-…/game-…` です。同じ対局の全局・全座席・全局面をまとめます。
**ロン局面の抽出・再サンプリングより先に、対局単位で学習・検証・評価へ分割します。**
生成器・cjong4・opponentのリビジョンも実験時に記録し、異なる実装で再生成した同じ対局IDを混ぜないでください。

## 危険例を増やして学習

```sh
python -m training.train \
  --train datasets/standard-v1/split/train.npz \
  --validation datasets/standard-v1/split/validation.npz \
  --output runs/risk-standard-v2 \
  --epochs 100 --batch-size 256 --lr 0.003 --seed 1 \
  --ron-fraction 0.5 --selection-metric auc --device cpu
```

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
- `nonzero_auc`: 非ゼロ教師を高く予測する順位性能。同点のみなら0.5、完全な順位なら1。
- `balanced_mse`: 安全側・非ゼロ側MSEの平均。片側だけの場合はnull。

非ゼロ教師のない評価では危険側指標やAUCはnullです。
00/FF以外の教師を使うデータでも、AUCの正例は「非ゼロ」と定義します。
全00でなくなったことやAUCの改善だけで、危険度の尺度・実戦精度が保証されるわけではありません。

学習チェックポイントはv2です。v1チェックポイントは推論・INT8書き出しできますが、
勾配と学習設定が変わるためv1からの途中再開は拒否します。新しい出力先で学習してください。
v2内の再開は同じ `--ron-fraction`・`--selection-metric` を含む設定で `--resume …/last.pt` を指定します。

## 書き出し・最終評価

```sh
python -m training.export \
  runs/risk-standard-v2/best.pt runs/risk-standard-v2/model.i8 \
  --validation datasets/standard-v1/split/validation.npz \
  --c-evaluator build/cj4dr_evaluate
python -m training.evaluate \
  runs/risk-standard-v2/model.i8 datasets/standard-v1/split/test.npz \
  --exclude datasets/standard-v1/split/train.npz \
  --exclude datasets/standard-v1/split/validation.npz \
  --c-evaluator build/cj4dr_evaluate > runs/risk-standard-v2/test-metrics.json
```

NNは同じ4,897バイト形式、入力408バイト・出力34バイトです。
C照合は出力一致の検証で、危険牌を識別できることの証明とは分けて評価してください。
