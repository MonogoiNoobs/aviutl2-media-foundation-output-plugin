# MFOutput

Windows標準のMicrosoft Media Foundationに出力させるAviUtl ExEdit2用プラグインです。開発中です。

劣化版[かんたんMP4出力](https://aoytsk.blog.jp/aviutl/34586383.html)です。

## 既知の問題

* 再生時間が長いファイルを出力する際、メモリ不足で落ちる
* ハードウェアアクセラレーションありで出力する際、クソミドリ
* そもそもいつからかハードウェアアクセラレーション使用時に`nvEncMFTHx264x.dll`が例外を吐くようになった
* そもそも例外を吐くとAviUtl側が途中で止めてしまうため後始末がなされない

## ライセンス

MIT License
