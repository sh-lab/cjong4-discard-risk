# INT8モデル形式 v1

入力スキーマv2（409バイト）に対応します。全ファイルサイズは5,589バイトです。
C構造体のメモリを直接保存しません。多バイト整数はリトルエンディアン、
符号付き整数は2の補数として保存します。

| バイト位置 | サイズ | 内容 |
| ---: | ---: | --- |
| 0 | 8 | ASCII `CJ4DRI8` + NUL |
| 8 | 2 | モデル形式バージョン、uint16 = 1 |
| 10 | 2 | 入力スキーマ、uint16 = 2 |
| 12 | 4 | 全ファイルサイズ、uint32 = 5589 |
| 16 | 36 | pixel_weights[3][4][3], int8 |
| 52 | 48 | pixel_bias[3][4], int32 |
| 100 | 3 | pixel_shift[3], uint8 |
| 103 | 4352 | hidden_weights[8][544], int8 |
| 4455 | 1088 | candidate_weights[136][8], int8 |
| 5543 | 32 | hidden_bias[8], int32 |
| 5575 | 1 | hidden_shift, uint8 |
| 5576 | 8 | output_weights[8], int8 |
| 5584 | 4 | output_bias, int32 |
| 5588 | 1 | output_shift, uint8 |

配列の最後の添字が最も速く変化します。画素処理のbankは0=数牌3色共通、
1=風牌、2=三元牌。チャンネルは0〜3です。
544要素は画像順（萬・筒・索・風・三元）、画像内行優先、画素内チャンネル順。
候補の行番号は画像内位置ではなく、cjong4の物理牌IDそのものです。

全シフトは0〜31、全バイアスは±16,777,216。
ローダーはmagic、形式バージョン、入力スキーマ、長さ、シフト・バイアス範囲を検証します。
余分な末尾バイトも拒否します。チェックサムは含まず、範囲内の重みの破損や
学習品質を検出する形式ではありません。

## 推論の定義

```text
activate(sum, shift, cap) = min(cap, floor(max(0, sum) / 2^shift))

pixel[p,c] =
    activate(pixel_bias[bank,c] +
             Σ rgb[p,k] * pixel_weights[bank,c,k],
             pixel_shift[bank], 127)

hidden[h] =
    activate(hidden_bias[h] +
             candidate_weights[candidate_id,h] +
             Σ pixel_flat[i] * hidden_weights[h,i],
             hidden_shift, 127)

danger =
    activate(output_bias + Σ hidden[h] * output_weights[h],
             output_shift, 255)
```

RGBはuint8のまま使用し、255もその値で処理します。ビット解読や追加の入力正規化はありません。
画素配置・RGBの元データは損失なく保持され、中間表現は学習重みによって変換されます。
NN内部の飽和・量子化まで可逆という意味ではありません。

この形式は整数推論の契約です。floatの学習重みを単純にint8へキャストしても
同じモデルにはなりません。書き出し側でシフトとバイアスのスケール、
ReLU、切捨て、飽和を合わせ、整数推論で精度を検証する必要があります。
