OkumuLab 1  v@VERSION@  (macOS 10.15 or later, Apple silicon and Intel)

A physical model of a Prinzipal 8' flue organ pipe (VST3 / Audio Unit instrument and standalone app).
オルガンのフルーパイプ（Prinzipal 8'）の物理モデル音源です。

■ Contents / 入っているもの
  OkumuLab 1.vst3        VST3 plugin / VST3 プラグイン
  OkumuLab 1.component   Audio Unit plugin (Logic Pro, GarageBand ...) / Audio Unit プラグイン
  OkumuLab 1.app         standalone app / スタンドアロンアプリ
  LICENSE                GNU AGPLv3
  THIRD_PARTY_NOTICES.md, licenses/   licenses of the libraries and fonts / ライブラリとフォントのライセンス

■ Installation
  VST3:        copy "OkumuLab 1.vst3" to ~/Library/Audio/Plug-Ins/VST3/
  Audio Unit:  copy "OkumuLab 1.component" to ~/Library/Audio/Plug-Ins/Components/
               (for all users: /Library/Audio/Plug-Ins/VST3/ and /Library/Audio/Plug-Ins/Components/)
  Standalone:  copy "OkumuLab 1.app" to Applications, open it and choose the audio output and the MIDI
               input under Options.
  Then restart your DAW (or rescan the plugins).

  The files are not signed with an Apple Developer ID and not notarized, so macOS refuses to open them at
  first. Run this once in Terminal, in the folder where you put them:

      xattr -dr com.apple.quarantine "OkumuLab 1.vst3" "OkumuLab 1.component" "OkumuLab 1.app"

  (or open the app with right-click > Open).

■ インストール
  VST3：「OkumuLab 1.vst3」を ~/Library/Audio/Plug-Ins/VST3/ にコピーします。
  Audio Unit：「OkumuLab 1.component」を ~/Library/Audio/Plug-Ins/Components/ にコピーします。
        （すべてのユーザーに入れるときは /Library/Audio/Plug-Ins/ の下の同じ名前のフォルダ）
  スタンドアロン：「OkumuLab 1.app」をアプリケーションフォルダにコピーして開き、Options から
        オーディオ出力と MIDI 入力を選びます。
  そのあと DAW を再起動する（またはプラグインを再スキャンする）と使えます。

  Apple の Developer ID による署名と公証をしていないので、そのままでは macOS が開かせません。
  コピーした先のフォルダで「ターミナル」から次を一度だけ実行してください。

      xattr -dr com.apple.quarantine "OkumuLab 1.vst3" "OkumuLab 1.component" "OkumuLab 1.app"

  （アプリは Finder で右クリック →「開く」でも開けます。）

■ License and source code / ライセンスとソースコード
  OkumuLab 1 is licensed under the GNU Affero General Public License v3 (LICENSE), with no warranty.
  The source code is in the GitHub repository that distributes this zip (the tag of this release).
  OkumuLab 1 は GNU AGPLv3 で配布しています（LICENSE）。保証はありません。ソースコードは、この zip を
  配布している GitHub リポジトリの、このリリースのタグにあります。

  This software is based in part on the work of the Independent JPEG Group.
  VST is a trademark of Steinberg Media Technologies GmbH.
  Audio Units, macOS and Logic Pro are trademarks of Apple Inc.

  Copyright (C) 2026 Ren Okumura
