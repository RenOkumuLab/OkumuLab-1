# Third-party notices

OkumuLab 1 は次のソフトウェアとフォントを使っています。各ライセンスの全文は [licenses/](licenses/) にあります。OkumuLab 1 自体のライセンスは [LICENSE](LICENSE)（GNU AGPLv3）です。

OkumuLab 1 uses the following software and fonts. The full license texts are in [licenses/](licenses/). OkumuLab 1 itself is licensed under the GNU AGPLv3 ([LICENSE](LICENSE)).

| 名前 | 使っているところ | ライセンス | 全文 |
|---|---|---|---|
| JUCE 8.0.12 | プラグインとアプリの土台 | AGPLv3（JUCE は AGPLv3 と JUCE の商用ライセンスの二本立てで、このプロジェクトは AGPLv3 の側で使っています） | [JUCE-LICENSE.md](licenses/JUCE-LICENSE.md)、[LICENSE](LICENSE) |
| VST3 SDK（JUCE 8.0.12 に同梱） | VST3 プラグイン | MIT | [VST3_SDK-LICENSE.txt](licenses/VST3_SDK-LICENSE.txt) |
| AudioUnitSDK（Apple、JUCE 8.0.12 に同梱） | Audio Unit プラグイン（macOS のみ） | Apache License 2.0 | [AudioUnitSDK-LICENSE.txt](licenses/AudioUnitSDK-LICENSE.txt) |
| zlib（JUCE に同梱） | JUCE の内部 | zlib License | [zlib-LICENSE.txt](licenses/zlib-LICENSE.txt) |
| libpng（JUCE に同梱） | JUCE の画像の読み込み | PNG Reference Library License version 2 | [libpng-LICENSE.txt](licenses/libpng-LICENSE.txt) |
| IJG JPEG library（JUCE に同梱） | JUCE の画像の読み込み | Independent JPEG Group のライセンス | [jpeglib-README.txt](licenses/jpeglib-README.txt) |
| HarfBuzz（JUCE に同梱） | JUCE の文字の描画 | Old MIT | [HarfBuzz-COPYING.txt](licenses/HarfBuzz-COPYING.txt) |
| SheenBidi（JUCE に同梱） | JUCE の文字の描画 | Apache License 2.0 | [SheenBidi-LICENSE.txt](licenses/SheenBidi-LICENSE.txt) |
| FLAC（JUCE に同梱） | JUCE の音声ファイルの読み書き | BSD 形式 | [FLAC-LICENSE.txt](licenses/FLAC-LICENSE.txt) |
| Ogg Vorbis（JUCE に同梱） | JUCE の音声ファイルの読み書き | BSD 形式 | [OggVorbis-LICENSE.txt](licenses/OggVorbis-LICENSE.txt) |
| Microsoft WebView2 SDK 1.0.3485.44 | 画面（Windows のみ。ローダーを静的リンク） | BSD 形式（Microsoft） | [WebView2-LICENSE.txt](licenses/WebView2-LICENSE.txt)、[WebView2-NOTICE.txt](licenses/WebView2-NOTICE.txt) |
| three.js r160 | 画面の 3D 表示 | MIT | [three.js-LICENSE.txt](licenses/three.js-LICENSE.txt) |
| Chakra Petch、JetBrains Mono | 画面のフォント | SIL Open Font License 1.1 | [OFL-fonts.txt](licenses/OFL-fonts.txt) |

macOS 版と Linux 版は、OS に入っているライブラリを使い、配布物には含めていません。macOS では Apple のフレームワーク（画面は WKWebView）、Linux では ALSA、X11、FreeType、fontconfig と、ある場合は JACK と WebKitGTK（画面）を、実行するときに OS から読み込みます。

The macOS and Linux builds use libraries of the operating system and do not include them: Apple's frameworks (the screen uses WKWebView) on macOS; ALSA, X11, FreeType, fontconfig and, when installed, JACK and WebKitGTK (the screen) on Linux, loaded from the system at run time.

This software is based in part on the work of the Independent JPEG Group.

VST is a trademark of Steinberg Media Technologies GmbH. VST は Steinberg Media Technologies GmbH の商標です。

Audio Units, macOS and Logic Pro are trademarks of Apple Inc. Audio Units、macOS、Logic Pro は Apple Inc. の商標です。

開発中の検証には pluginval（Tracktion、GPLv3）を使いましたが、プラグインにもこのリポジトリにも含めていません。
