OkumuLab 1  v@VERSION@  (Windows x64)

A physical model of a Prinzipal 8' flue organ pipe (VST3 instrument and standalone app).
オルガンのフルーパイプ（Prinzipal 8'）の物理モデル音源です。

■ Contents / 入っているもの
  OkumuLab 1.vst3      VST3 plugin (folder) / VST3 プラグイン（フォルダ）
  OkumuLab 1.exe       standalone app / スタンドアロンアプリ
  LICENSE              GNU AGPLv3
  THIRD_PARTY_NOTICES.md, licenses\   licenses of the libraries and fonts / ライブラリとフォントのライセンス

■ Installation
  VST3:        copy the "OkumuLab 1.vst3" folder to C:\Program Files\Common Files\VST3\ (this needs
               administrator rights), then rescan the plugins in your DAW.
  Standalone:  start "OkumuLab 1.exe" and choose the audio output and the MIDI input under Options
               at the top left.

  The screen uses the WebView2 runtime (part of Windows 11). Without it the plugin opens a simple
  native screen. The files are not code-signed, so SmartScreen may warn the first time you open them.
  On Windows only one application at a time can open a MIDI device: do not run the standalone app and
  a DAW at the same time.

■ インストール
  VST3：「OkumuLab 1.vst3」フォルダを C:\Program Files\Common Files\VST3\ にコピーし
        （管理者権限が要ります）、DAW でプラグインを再スキャンしてください。
  スタンドアロン：「OkumuLab 1.exe」を起動し、左上の Options からオーディオ出力と
        MIDI 入力を選んでください。

  画面には WebView2 ランタイムを使います（Windows 11 には標準で入っています）。ない場合は
  簡易なネイティブ画面で開きます。コード署名をしていないので、初めて開くときに SmartScreen の
  警告が出ることがあります。Windows では MIDI 機器を同時に開けるアプリは 1 つだけです。
  スタンドアロンと DAW を同時に起動しないでください。

■ License and source code / ライセンスとソースコード
  OkumuLab 1 is licensed under the GNU Affero General Public License v3 (LICENSE), with no warranty.
  The source code is in the GitHub repository that distributes this zip (the tag v@VERSION@).
  OkumuLab 1 は GNU AGPLv3 で配布しています（LICENSE）。保証はありません。ソースコードは、この zip を
  配布している GitHub リポジトリの、タグ v@VERSION@ にあります。

  This software is based in part on the work of the Independent JPEG Group.
  VST is a trademark of Steinberg Media Technologies GmbH.

  Copyright (C) 2026 Ren Okumura
