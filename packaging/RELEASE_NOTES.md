オルガンのフルーパイプ（Prinzipal 8'）の物理モデル音源 OkumuLab 1 の v1.1.0 です。今回から macOS 版と Linux 版もあります。

A physical model of a Prinzipal 8' organ flue pipe. Version 1.1.0 sounds like a Prinzipal (the physics of the jet and of the pipe's open ends were revised, with no filter after the physics) and comes for Windows, macOS and Linux. English notes are in the repository's README.md ("What's new in 1.1.0").

## ダウンロード

| ファイル | 対象 | 中身 |
|---|---|---|
| `OkumuLab1-1.1.0-win64.zip` | Windows（x64） | VST3、スタンドアロン |
| `OkumuLab1-1.1.0-macOS.zip` | macOS 10.15 以降（Apple silicon と Intel） | VST3、Audio Unit、スタンドアロン |
| `OkumuLab1-1.1.0-linux-x86_64.tar.gz` | Linux（x86-64、glibc 2.35 以降） | VST3、スタンドアロン |

展開して、中の `README.txt` の手順でインストールしてください。macOS 版は Apple の署名・公証をしていないので、README.txt にあるコマンド（`xattr -dr com.apple.quarantine ...`）を一度実行してから使います。

## v1.0.0 からの変更

- **音色を物理だけで作るようにしました**：v1.0.0 の音色は、ビリビリを抑えるための調整（ジェットの振れの頭打ち、500 Pa での整音など）に頼っていました。ビリビリの本当の原因は v1.0.0 で直したホストのブロック長の不具合だったので、v1.1.0 ではその調整をすべて外し、代わりに発音の物理を文献の式どおりにしました。鳴った音を後から加工する処理（フィルタ・イコライザなど）は使っていません。
  - ジェットが歌口を渡るあいだに、乱流の平面ジェットとして広がる（Thwaites & Fletcher 1983）
  - ジェットの波の成長率が、その場所のジェットの幅で決まる（Yoshikawa 1998 の実測と合う）
  - 管頭と歌口から音が放射されるときの損失（Norris & Sheng 1989 の開口端の反射）

  調整を外しただけのモデルでは上の倍音（第6〜12倍音）が平らに残り、ガンバやハーモニカのように聞こえました。これらの物理を入れると、基音が最も強く、30 dB 以内の倍音が 5〜6 本、第2倍音が低音ほど強いという、実際のプリンツィパルの測定（Pykett）と同じ形になります。v1.0.0 と比べるとオクターブ（第2倍音）は弱めです。強いオクターブが欲しいときは Labium offset を上げてください（+1.0 b で c¹ の第2倍音が −12 dB）。
- **Pitch lock の初期値は 0 %** になりました。風圧・温度・ガスなどで音程が本物のパイプのように動きます。以前のように鍵盤の音程に保つには 100 % にしてください。
- 風を弱めると音程が下がり、さらに弱めると鳴らなくなります（鳴り始める風は標準の約 0.3 倍）。v1.0.0 はごく弱い風でも鳴っていました。
- **Labium offset** の単位 b は「ラビウムに届いたジェットの半幅」になりました。±1.2 b くらいを超えるとパイプは鳴りません。v1.0.0 で保存した設定を開くと、上唇の位置が変わります。
- **音が大きくなりました**：同じ演奏で v1.0.0 より 2〜7 dB 大きく鳴ります（中音域で約 7 dB、最高音域で約 2 dB）。広がるジェットが周りの空気を巻き込み、パイプに入る流れが増えるためです（物理どおりで、出力のゲインは変えていません）。v1.0.0 と同じくらいの音量にするには Output を −6 dB くらいにしてください。大きな和音では出力のリミッター（−1 dBFS）が働きます。
- Nicking はジェットを厚くするだけになり、音が丸くなります。
- macOS 版（VST3、Audio Unit、スタンドアロン）と Linux 版（VST3、スタンドアロン）を加えました。

## 確認したこと

- Windows 版：Windows 11 で、音源の試験（labium_check 13 項目）、DAW と同じ読み込み方での試験（画面を開いたままの演奏を含む）、pluginval（厳しさ 10）に合格しています。
- macOS 版と Linux 版：GitHub のマシンでビルドし、音源の試験（labium_check）、pluginval（厳しさ 10）、Audio Unit の auval に合格しています。実際の Mac や Linux の PC で弾いての確認はまだしていません。気づいたことがあればお知らせください。

## ライセンス

GNU AGPLv3。使っているライブラリとフォントのライセンスは、各パッケージの `THIRD_PARTY_NOTICES.md` と `licenses` フォルダにあります。VST は Steinberg Media Technologies GmbH の商標です。Audio Units と macOS は Apple Inc. の商標です。
