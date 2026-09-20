# Linuxでの学習

5枚のRGB画像408バイトから34牌種のグレースケールを学習し、
既存C推論器で読める4,897バイトのINT8モデルを作成します。
このディレクトリはPython側だけで使い、Cライブラリの依存関係には加えません。

136枚の物理牌とRGBのバイト配置を維持します。入力はすでに
`cj4dr_encode_input()` でプレイヤー番号を相対化したものを渡します。
追加の入力正規化、牌種集約、色変換、画像補間、候補牌・ドラの追加は行いません。

ランダムな合法局面と個別ケースの生成、cjong4による00/FF教師判定、データ分割、
量子化学習、再開、整数モデル書き出し、評価を実装しています。
実戦牌譜と、実用精度を確認した学習済み重みはまだ含みません。
プログラム検証用の人工データと、生成器が出す合法局面の教師データは用途が異なります。

## 環境

Python 3.10以上、NumPy 1.26以上、PyTorch 2.6以上（いずれも3未満）。
リポジトリのルートから実行します。

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
# CPU用。torchvision/torchaudioは不要です。
python -m pip install 'torch>=2.6,<3' --index-url https://download.pytorch.org/whl/cpu
python -m pip install -r training/requirements.txt
```

CUDAを使う場合は、CPU用torchの代わりにLinux機のGPU・ドライバーに合った
[PyTorch公式インストール手順](https://pytorch.org/get-started/locally/)でtorchを導入してから
requirementsを適用してください。学習引数は `--device cuda` です。

CPU/CUDAともに内部計算はfloat64です。Cの有効なINT32積和を正確に再現するためで、
RGBは0〜255の生の値をそのまま渡します。AMP、TF32、MPS、分散学習は使用しません。
float64の速度はGPUによって異なり、この小型NNではCPUとの実測比較を推奨します。

## standard対局のロン局面を使う

`standard(1)` 4人の実対局から通常ロン直前の危険例を集める場合は、
[standard対局による生成・学習手順](../docs/standard-selfplay.md) を使用してください。
通常局面も保存して対局単位で分割し、ロン局面の再サンプリングは学習側だけに適用します。
学習v2では飽和した出力へも近似勾配を通し、全00基準・危険側誤差・AUCを確認できます。
以前のv1チェックポイントは書き出し可能ですが、途中再開せず新しいrunで学習します。

## 本学習用の教師データを生成する

ルートREADMEの手順で `CJ4DR_BUILD_TOOLS=ON` にして再ビルドします。
まずランダム100局と、34牌種×3条件×10バリエーションの個別ケースを作る例です。

```sh
mkdir -p datasets
./build/cj4dr_generate --mode random --rounds 100 --seed 60000 \
    --output datasets/random-60000.jsonl
./build/cj4dr_generate --mode cases --variants 10 --seed 70000 \
    --output datasets/manual-70000.jsonl
cat datasets/random-60000.jsonl datasets/manual-70000.jsonl > datasets/all.jsonl
```

ランダム局面では合法な捨て牌をコピー上で切り、他家が1人でもロン可能ならFF、全員不可なら00。
判定しない牌種はマスク0です。NNにはマスク済みRGBだけを渡します。
個別ケースは全他家の現物、リーチ後の通過牌、リーチ者へ放銃する比較ケースを含みます。
条件の意味・局数・分割生成・00/FFの比率については
[教師データ生成の仕様](../docs/teacher-generation.md) を参照してください。

## 独自の個別教師データを追加する

JSONLの1行を1サンプルとして記述します。

- `rgb_file`: JSONLの場所を基準にした408バイト入力ファイルのパス。
  代わりに `rgb_hex`（同じ408バイトの16進数文字列）も使えます。両方の指定は不可。
- `target`: 牌種ID順の整数34個、範囲0〜255。
- `mask`: 0または1の整数34個。1の出力だけを学習・評価に使用。
- `group`: 必須の元対局・元ケースID。派生局面は同じIDにします。
- `metadata`: 任意のJSONオブジェクト。生成元、教師の根拠など。NNには渡しません。

例えば、cjong4側で安全条件を確認した五萬の個別ケースを登録する例です。
`cases/five-man.rgb` はマスク済みビューから生成した実際の入力を事前に用意します。
この例だけで画像の内容や安全条件を生成・判定するわけではありません。

```sh
mkdir -p datasets
python - <<'PYDATA'
import json
from pathlib import Path

target, mask = [0] * 34, [0] * 34
mask[4] = 1  # 五萬だけ教師値00。残り33種の0は未指定なので無視される。
sample = {
    "rgb_file": "../cases/five-man.rgb",
    "target": target,
    "mask": mask,
    "group": "manual/all-opponents-discarded/base-001",
    "metadata": {"source": "manual", "condition": "全他家が五萬を捨てている"},
}
Path("datasets/manual.jsonl").write_text(json.dumps(sample, ensure_ascii=False) + "\n")
PYDATA
python -m training.data pack datasets/manual.jsonl datasets/manual.npz
```

ランダム局面由来のデータも同じJSONL形式で追加できます。
局面の合法性と教師値の正しさは生成側で確認します。packはCの入力スキーマと同じ
バイト範囲・他家手牌のマスクを検証しますが、局面全体の合法性や教師の意味は判定しません。
PNG/JPEG等の画像ファイルは受け付けません。

複数ソースのJSONLを結合する場合は、`rgb_file` の相対パスを結合先に合わせてください。
全件が未指定のサンプルは保存可能ですが、学習時には除外します。
学習用・検証用にはそれぞれ少なくとも1つの指定教師値が必要です。

### 保存形式v1（NPZ）

NumPy NPZアーカイブに次の配列を保存します。オブジェクト配列やpickleは使用しません。
形式・型・形状が異なるファイルは拒否します。

| キー | 型・形状 | 内容 |
| --- | --- | --- |
| `version` | uint16 スカラー | 1 |
| `input_schema` | uint16 スカラー | 3 |
| `rgb` | uint8 `[N,408]` | 5画像の元バイト列 |
| `target` | uint8 `[N,34]` | 00〜FFの教師値 |
| `mask` | uint8 `[N,34]` | 0=未指定、1=指定 |
| `group` | Unicode `[N]` | 元対局・元ケースID |
| `metadata` | Unicode `[N]` | JSONオブジェクトを文字列化した管理情報 |

入力・教師値・マスクの論理サイズは1件476バイト＋管理情報です。
最初の実装はNPZを全件RAMに読み込みます。分割時のコピーやUnicode管理情報も必要なので、
大規模化してRAMが不足する段階では分割ファイルの順次読込へ拡張します。
GPUには各バッチだけを転送します。

## 対局・元ケース単位の分割

学習・検証・最終評価の3つへ分けます。少なくとも3つの独立したgroupが必要です。
上記の1ケースだけでは分割できないので、独立した実データを追加してください。

```sh
python -m training.data pack datasets/all.jsonl datasets/all.npz
python -m training.data split datasets/all.npz datasets/split \
    --validation-fraction 0.1 --test-fraction 0.1 --seed 1
```

比率はサンプル数ではなくgroup数に適用します。
seedとgroup IDのSHA-256で順序を決めるため、元ファイル内の行順には依存しません。
各評価側に最低1groupを割り当て、残りを学習用にします。
同一groupは跨がせず、別groupに同じRGBが紛れた場合も分割間の重複を拒否します。
近似局面の関係までは自動検出できないため、派生元groupの指定は生成側の責任です。

## 学習と再開

```sh
python -m training.train \
    --train datasets/split/train.npz \
    --validation datasets/split/validation.npz \
    --output runs/risk-001 --epochs 100 --batch-size 256 --lr 0.003 --seed 1 \
    --device cpu
```

損失は指定された要素の `mean(((prediction - target) / 255)^2)` です。
255で割るのは損失の尺度だけであり、入力RGBの加工ではありません。
教師値は確率と断定せず、独立した34出力のグレースケール回帰として扱います。
マスク外の要素は損失・評価の分母にも含めません。
00の教師値を与えても、推論時の強制00化は行いません。

量子化方式:

- Cと同じ画素処理3→4、544→8→34の構成を使用。
- 学習中の実数パラメーターから、INT8重みとINT32バイアスを毎回作る。
- 量子化は最近接丸め（ちょうど中間なら偶数）、範囲内への制限を行う。
- forwardは量子化済み重みで計算し、Cと同じReLU・2の累乗除算・切捨て・飽和を行う。
- backwardは丸め・切捨ての勾配をそのまま通すSTEを使用。活性化の範囲外でも0.01倍の近似勾配を通し、誤った飽和からの復帰を可能にする。
  重み・バイアス自体は引き続き有効範囲へ制限する。
- シフトは固定。既定は数牌3、風3、三元3、中間6、出力4。
  `--shifts 3 3 3 6 4` で指定でき、各値は0〜31。

保存物:

- `best.pt`: 検証の選択指標が最良だったモデル（既定は指定要素MSE）。
- `last.pt`: 最終完了epochの重み・optimizer・設定を含む再開用チェックポイント。
- `metrics.jsonl`: 各epochの学習MSE、検証MSE/MAE、過小推定量、安全教師値での平均予測、
  牌種別の指定数・MAE、全00基準、危険側MSEと平均予測、AUC。

チェックポイントには入力/モデルのバージョン、データファイルのSHA-256、
設定、PyTorch/NumPyバージョンも保存します。
再開時は同じ出力ディレクトリの `last.pt` と、同じデータ・設定を使います。
`--epochs` は追加回数ではなく、最初からの合計epoch数です。

```sh
python -m training.train \
    --train datasets/split/train.npz \
    --validation datasets/split/validation.npz \
    --output runs/risk-001 --epochs 200 --batch-size 256 --lr 0.003 --seed 1 \
    --device cpu --resume runs/risk-001/last.pt
```

同じ実行環境・設定での再開を再現できるよう、epoch単位でシャッフルseedを固定します。
CPUとCUDA、ライブラリバージョン間で学習経路の一致までは保証しません。
学習率やシフトを選ぶのは検証データで行い、最終評価データを選択に使いません。
指定00だけのデータで学習すると全出力を低くする解に偏るため、
通常・危険例も含めたデータ構成と実局面での評価が別途必要です。

## INT8書き出しとC照合

ルートREADMEの手順で `build/cj4dr_evaluate` をビルドしてから実行します。

```sh
python -m training.export runs/risk-001/best.pt runs/risk-001/model.i8 \
    --validation datasets/split/validation.npz \
    --c-evaluator build/cj4dr_evaluate
```

保存された検証データのSHA-256を確認し、全検証入力について
PyTorch forwardと独立したINT64参照計算の34出力を比較します。
`--c-evaluator` を指定した場合は全検証入力をC CLIにも渡して完全一致を確認し、
不一致ならモデルを書き出しません。CLIは1件ずつ起動するため、大規模データでは時間がかかります。
省略時はPython側の一致のみを確認し、レポートのC照合件数は0になります。

`model.i8` と `model.i8.json`（検証値・照合件数・設定）を保存します。
既存ファイルの上書きは拒否します。モデルサイズは4,897バイト、
Cの作業領域は552バイトのままです。

最終評価用データは、書き出した整数モデルで評価します。

```sh
python -m training.evaluate runs/risk-001/model.i8 datasets/split/test.npz \
    --exclude datasets/split/train.npz --exclude datasets/split/validation.npz \
    --c-evaluator build/cj4dr_evaluate > runs/risk-001/test-metrics.json
```

`--exclude` は指定データとのgroup/同一RGBの重複を拒否します。
学習器・exportは最終評価データを読みません。評価は指定教師値のある要素に限られ、
安全の保証やスコアの確率校正を示すものではありません。

## テスト

```sh
CJ4DR_EVALUATOR="$PWD/build/cj4dr_evaluate" \
    python -m unittest discover -s tests -p test_training.py -v
```

部分教師の勾配、損失低下、データ保存・分割・重複拒否、学習再開の一致、
量子化・直列化、シフト0〜31と極端な重み、出力順、書き出し・最終評価までを確認します。
`CJ4DR_EVALUATOR` 指定時はCとの推論一致とRGB全チャンネルの全256値の検証一致も確認します。
Cの既存テストは従来通り `ctest` で別途実行します。

参考: チェックポイントはPyTorchの
[state_dictとweights_onlyによる保存・読込](https://docs.pytorch.org/tutorials/beginner/saving_loading_models.html)
を使用します。
