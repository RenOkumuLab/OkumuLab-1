OkumuLab 1  v@VERSION@  (Linux x86-64)

A physical model of a Prinzipal 8' flue organ pipe (VST3 instrument and standalone app).
オルガンのフルーパイプ（Prinzipal 8'）の物理モデル音源です。

■ Contents / 入っているもの
  OkumuLab 1.vst3      VST3 plugin (folder) / VST3 プラグイン（フォルダ）
  OkumuLab 1           standalone app / スタンドアロンアプリ
  LICENSE              GNU AGPLv3
  THIRD_PARTY_NOTICES.md, licenses/   licenses of the libraries and fonts / ライブラリとフォントのライセンス

■ Requirements
  x86-64, glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora 36 or later ...), X11 or XWayland,
  ALSA, FreeType and fontconfig (present on most desktops).
  The screen uses WebKitGTK. Without it the plugin opens a simple native screen. To install it:
      Ubuntu / Debian:  sudo apt install libwebkit2gtk-4.1-0
      Fedora:           sudo dnf install webkit2gtk4.1

■ Installation
  VST3:        copy the "OkumuLab 1.vst3" folder to ~/.vst3/ (for all users: /usr/local/lib/vst3/),
               then rescan the plugins in your DAW.
  Standalone:  run "OkumuLab 1" and choose the audio output and the MIDI input under Options.
               ALSA and JACK (also PipeWire's JACK) are supported.

■ インストール
  動作環境：x86-64、glibc 2.35 以降（Ubuntu 22.04、Debian 12、Fedora 36 以降など）、X11 または XWayland、
        ALSA、FreeType、fontconfig（多くのデスクトップには入っています）。
        画面には WebKitGTK を使います。ない場合は簡易なネイティブ画面で開きます。入れ方は上のとおりです。
  VST3：「OkumuLab 1.vst3」フォルダを ~/.vst3/ にコピーし（すべてのユーザーには /usr/local/lib/vst3/）、
        DAW でプラグインを再スキャンしてください。
  スタンドアロン：「OkumuLab 1」を実行し、Options からオーディオ出力と MIDI 入力を選んでください。
        ALSA と JACK（PipeWire の JACK を含む）を使えます。

■ License and source code / ライセンスとソースコード
  OkumuLab 1 is licensed under the GNU Affero General Public License v3 (LICENSE), with no warranty.
  The source code is in the GitHub repository that distributes this archive (the tag of this release).
  OkumuLab 1 は GNU AGPLv3 で配布しています（LICENSE）。保証はありません。ソースコードは、このファイルを
  配布している GitHub リポジトリの、このリリースのタグにあります。

  This software is based in part on the work of the Independent JPEG Group.
  VST is a trademark of Steinberg Media Technologies GmbH.

  Copyright (C) 2026 Ren Okumura
