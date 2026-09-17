# INT8モデル形式 v2

入力スキーマv3（5枚のRGB画像、408バイト）、出力は34×1のグレースケールです。
全ファイルサイズは4,897バイトです。旧v1の候補ID指定モデルとは互換性がありません。

C構造体のメモリを直接保存しません。多バイト整数はリトルエンディアン、
符号付き整数は2の補数で保存します。

| バイト位置 | サイズ | 内容 |
| ---: | ---: | --- |
| 0 | 8 | ASCII `CJ4DRI8` + NUL |
| 8 | 2 | モデル形式バージョン、uint16 = 2 |
| 10 | 2 | 入力スキーマ、uint16 = 3 |
| 12 | 4 | 全ファイルサイズ、uint32 = 4897 |
| 16 | 36 | pixel_weights[3][4][3], int8 |
| 52 | 48 | pixel_bias[3][4], int32 |
| 100 | 3 | pixel_shift[3], uint8 |
| 103 | 4352 | hidden_weights[8][544], int8 |
| 4455 | 32 | hidden_bias[8], int32 |
| 4487 | 1 | hidden_shift, uint8 |
| 4488 | 272 | output_weights[34][8], int8 |
| 4760 | 136 | output_bias[34], int32 |
| 4896 | 1 | output_shift, uint8 |

配列の最後の添字が最も速く変化します。画素処理のbankは0=数牌3色共通、
1=風牌、2=三元牌。チャンネルは0〜3です。
544要素は画像順（萬・筒・索・風・三元）、画像内行優先、画素内チャンネル順。
出力の行番号はcjong4の牌種ID（0〜33）です。候補ID用の重みはありません。

全シフトは0〜31、全バイアスは±16,777,216。
ローダーはmagic、形式バージョン、入力スキーマ、長さ、シフト・バイアス範囲を検証します。
余分な末尾バイトも拒否します。チェックサムは含まず、
範囲内の重みの破損や学習品質を検出する形式ではありません。

## 推論の定義

```text
activate(sum, shift, cap) = min(cap, floor(max(0, sum) / 2^shift))

pixel[p,c] =
    activate(pixel_bias[bank,c] +
             Σ rgb[p,k] * pixel_weights[bank,c,k],
             pixel_shift[bank], 127)

hidden[h] =
    activate(hidden_bias[h] + Σ pixel_flat[i] * hidden_weights[h,i],
             hidden_shift, 127)

danger[t] =
    activate(output_bias[t] + Σ hidden[h] * output_weights[t,h],
             output_shift, 255)              // t = 0..33
```

画素処理と中間層は1回計算し、全34種の出力で共有します。
出力は独立した値で、合計値による正規化、合法牌マスク、安全牌の強制0化は行いません。
学習時の教師指定マスクはこの推論モデルに含めません。

RGBはuint8のまま使用し、255もその値で処理します。
画素配置とRGBの入力データは損失なく保持され、NN内部の中間表現は学習重みで変換されます。
内部の飽和・量子化まで可逆という意味ではありません。

floatの学習重みを単純にint8へキャストしても同じモデルにはなりません。
書き出し側でシフトとバイアスのスケール、ReLU、切捨て、飽和を合わせ、
整数推論で精度を検証する必要があります。

## 学習側の書き出し

`python -m training.export` が本形式を出力します。
学習時のforwardでもINT8重み・INT32バイアス・固定シフト・切捨て・飽和を再現し、
書き出し時に独立したINT64参照計算との一致を検証します。
`--c-evaluator` を指定すればC推論の全34出力とも照合します。
詳細は [学習手順](../training/README.md) を参照してください。
