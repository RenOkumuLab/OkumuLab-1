# OkumuLab 1

オルガンのフルーパイプ（Prinzipal 8'）の物理モデル音源です。Windows（x64）用の VST3 インストゥルメントとスタンドアロンアプリで、JUCE 8 で作っています。画面は WebView2 と Three.js で、音源が計算しているジェット、管内の定在波、管壁の振動をそのまま描きます。

OkumuLab 1 is a physical model of a Prinzipal 8' flue organ pipe, built with JUCE 8 as a VST3 instrument and a standalone app for Windows (x64). The documentation is in Japanese. License: GNU AGPLv3.

- バージョン 1.0.0、メーカー名 OkumuLab
- 形式：VST3、スタンドアロン（Windows x64）

## できること

- WIND モードではパイプに風を送って鳴らし、MALLET モードではパイプの胴をマレットで叩いて鳴らします。
- 鍵盤は MIDI 0〜127 の全域で、鍵ごとに寸法の違うパイプが鳴ります。
- 19 の実験レシピ（DAW のプログラム）と、4 スロットの変調マトリクスがあります。
- 残響は、教会の身廊を計算したインパルス応答のたたみ込みです。
- つまみはすべて DAW のパラメータです。右クリックで MIDI ラーン、ダブルクリックで初期値に戻ります。Arturia MiniLab 3 のつまみ・フェーダー・パッドに合わせた割り当てが初期値です。

物理モデルの中身、MIDI の割り当て、検証の結果は [docs/DEVELOPMENT_NOTES_ja.md](docs/DEVELOPMENT_NOTES_ja.md) にあります。

## 動作環境

- Windows（x64）。Windows 11 で確認しています。
- 画面に WebView2 ランタイムを使います（Windows 11 には標準で入っています）。ない場合は簡易なネイティブ画面で開きます。
- AVX2 のある CPU では AVX2 版の計算を使い、ない CPU では SSE2 版を使います。
- VST3 は Bitwig Studio と SAVIHost で確認しています。

## インストール

GitHub の Releases から zip をダウンロードして展開します。

- VST3：`OkumuLab 1.vst3` フォルダを `C:\Program Files\Common Files\VST3\` にコピーし（管理者権限が要ります）、DAW でプラグインを再スキャンします。
- スタンドアロン：`OkumuLab 1.exe` を起動し、左上の Options からオーディオ出力と MIDI 入力を選びます。

バイナリにはコード署名をしていないので、初めて開くときに Windows の SmartScreen の警告が出ることがあります。Windows では MIDI 機器を同時に開けるアプリは 1 つだけなので、スタンドアロンと DAW を同時に起動しないでください。

## ビルド

Visual Studio 2022（Build Tools でも可）の「C++ によるデスクトップ開発」が必要です。CMake と Ninja は Visual Studio に付属のものを使います。

```
build.bat          VST3、スタンドアロン、試験プログラム（Release）
build.bat dsp      音源（DSP）と labium_check だけ（JUCE なし）
```

- できあがるもの：`build\OkumuLab1_artefacts\Release\VST3\OkumuLab 1.vst3` と `build\OkumuLab1_artefacts\Release\Standalone\OkumuLab 1.exe`
- JUCE のファイル名が長いので、`C:\dev\OkumuLab1` のような短いパスに置いてビルドしてください。
- `third_party/JUCE` は JUCE 8.0.12 を変更せずに入れたものです（同梱の DemoRunner.exe と Projucer.exe は除いています）。`third_party/nuget` は WebView2 SDK 1.0.3485.44 の NuGet パッケージを展開したものです。
- C ランタイムは静的リンクなので、配布先に Visual C++ 再頒布パッケージは要りません。

### 試験

- `build-dsp\labium_check.exe [項目]`：音源の検証です。項目は port、derive、midi、pitchlock、wind、tone、blocks、cpu、stress、mallet、room、lab、simd で、省略するとすべて行います。
- `build\okl_hosttest_artefacts\Release\okl_hosttest.exe`：ビルドした VST3 を DAW と同じ方法で読み込んで鳴らす試験です（画面を開いて動かす試験を含みます）。
- pluginval（Tracktion）での検証には、pluginval を別に入手して使います（このリポジトリには含めていません）。

### 試験用の環境変数

ふだん使うときには設定しません。

- `OKL_NATIVE_UI=1`：WebView2 の画面の代わりにネイティブ画面を使う
- `OKL_MUTE=1`：音を出さない（画面の試験用）
- `OKL_PERFTEST=<ファイル>`：画面の試験を自動で行い、結果をそのファイルに書く
- `OKL_RENDERTEST=<ノートの記録>|<WAV>`：プラグインの生成時に、記録したノートを鳴らして WAV に書き出す

## ライセンス

Copyright (C) 2026 Ren Okumura

このプログラムはフリーソフトウェアです。GNU Affero General Public License version 3（AGPLv3）の条件で、再配布や改変ができます。全文は [LICENSE](LICENSE) にあります。保証はありません。

JUCE 8 は AGPLv3 の側で使っています。そのほかに使っているライブラリ（JUCE に同梱の VST3 SDK など）、Three.js、フォントのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) と [licenses/](licenses/) にあります。

VST は Steinberg Media Technologies GmbH の商標です。
