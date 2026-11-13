オルガンのフルーパイプの物理モデル音源 OkumuLab 1 の v2.0.0 です。2 つ目のストップ **Gedackt 8'**（木製の停管）を加えました。Prinzipal 8' の WIND の音は v1.1.x と同じです（MALLET は高い鍵で変わりました。下の「叩いた管壁のモード」）。

A physical model of organ flue pipes. Version 2.0.0 adds a second stop, the Gedackt 8' (a stopped wooden pipe); the Prinzipal 8' sounds as in 1.1.x when blown (struck, its upper keys changed: see below). English notes are in the repository's README.md ("What's new in 2.0.0").

## ダウンロード

| ファイル | 対象 | 中身 |
|---|---|---|
| `OkumuLab1-2.0.0-win64.zip` | Windows（x64） | VST3、スタンドアロン |
| `OkumuLab1-2.0.0-macOS.zip` | macOS 10.15 以降（Apple silicon と Intel） | VST3、Audio Unit、スタンドアロン |
| `OkumuLab1-2.0.0-linux-x86_64.tar.gz` | Linux（x86-64、glibc 2.35 以降） | VST3、スタンドアロン |

展開して、中の `README.txt` の手順でインストールしてください。macOS 版は Apple の署名・公証をしていないので、README.txt にあるコマンド（`xattr -dr com.apple.quarantine ...`）を一度実行してから使います。

## v1.1.1 からの変更

- **Gedackt 8'**：木製の停管です。寸法は英国の Stopped Diapason（Dickson 1882：C の内寸 82.5 × 101.6 mm、テナー C 54.0 × 66.7 mm、19.7 半音で半分）、歌口は管の内幅いっぱい、切り上げは Janke の規則（ハウスオルガンの低い風圧で、正規の切り上げより 5 半音高い）に、g から上は耳で決めた整音の鍵ごとの倍率（g〜f#¹ 約 ×1.46、g¹〜f#² 約 ×1.58〜1.61、g²〜g³ 約 ×1.53〜1.61、g#³〜c⁴ 約 ×1.41〜1.46）を掛けたもの、風道は実際の木管の幅（内幅 47 mm で 1 mm、9 mm で 0.6 mm。BDO の木管と Fischer らの Schuke の停管。C で 1.19 mm）、歌口の端補正は Fletcher & Rossing の式に、前板を貫く歌口の首（板厚）の空気を加えたもの、壁の損失は長方形の管の式に木管の実測（Rucz ら 2023）の超過分です。栓をした管頭は鳴りません。栓は、製作者と同じく、各鍵が鍵盤の音程で鳴る位置に置いてあります。上唇はジェットの中心に置いてあり（Labium offset 0 b）、奇数倍音の音です。偶数倍音は、ジェットの駆動の Bernoulli の項（Fletcher 1976）によるごく弱いもの（基音より約 70 dB 下）だけです。第3倍音は c¹ で基音より約 14 dB、c² で約 27 dB 下です（Arched cut-up を上げると丸くなり、Labium offset を動かすと偶数倍音が出ます）。画面の木管は、Bund Deutscher Orgelbaumeister の木製の停管の比率で描いています。
- **Open/Stopped は栓の抜き差し**：Gedackt では初期値が Stopped 100 % です。Open へ回すと、画面でも革を巻いた栓が管頭から抜けていき、音はオクターブ近くまで上がります（開いた端の端補正のため、オクターブより少し低く）。
- **新しいつまみ**：側面の髭（Ears）と半円形の切り上げ（Arched cut-up、VOICING）。初期値は両方のストップとも 0 %（髭なし、まっすぐな切り上げ）です。
- **叩いた木管**：MALLET モードの Gedackt は、前板を木の直交異方性の板として計算します。木材は Pipe wood で選べます（ホワイトオーク〈初期値〉、レッドパイン、シトカスプルース、ウェスタンレッドシダー。Wood Handbook の値）。
- **毛糸のマレット**：Mallet head に Yarn（ゴムの芯に毛糸を巻いた頭）を加えました（両方のストップ）。羊毛の F = K x^p（p = 2.54、フェルトのマレットの文献値）で接触するので、ほかの頭より長く柔らかく当たり、高い倍音の少ない柔らかい音になります。
- **叩いた管壁のモード**（MALLET、両方のストップ）：計算を軽くするために、接触時間から決めた上限（12 ÷ 接触時間）より上のモードを入れていませんでした。この上限をやめ、聴こえる範囲（20 kHz まで）のモードをすべて候補にしました。どのモードがどれだけ鳴るかは、接触の力が決めます。柔らかい頭（毛糸、真鍮、硬いゴム）で、ある鍵から上の管壁の音が急に消えていた崖（Gedackt の毛糸で c¹ → d¹ に −23 dB など）がなくなりました。あわせて、マレットが押しているあいだのモードの放射を、加速度で決まる正しい式にしました。そのため毛糸の Gedackt は、低い鍵で約 12 dB、硬いゴムの Gedackt は高い鍵で 6〜16 dB 小さくなります。Prinzipal の c¹ より下は変わりません。
- **調律**：上のバー（ストップの隣）の TUNING ボタンで開くパネルに、Temperament（平均律、ピタゴラス、1/4 コンマ・ミーントーン、ヴェルクマイスター III、キルンベルガー III、ヴァロッティ、ヤング、Custom）と Pitch a¹（380〜480 Hz）を加えました。歴史的な音律は五度圏の定義から計算しています。同じパネルで Scala 形式の音階（.scl）と鍵盤の割り当て（.kbm）を読み込めます（Surge Synth Team の tuning-library、MIT。ファイルの中身はプロジェクトに保存されます）。調律した鍵は、最も近い鍵の管を調律した長さに切って鳴らします。平均律・440 Hz では何も変わらず、ストップを替えても調律は残ります。
- **ストップの選択**：上のバーの実験レシピのメニューを、同じ場所・同じ見た目のストップのメニューにしました。DAW のプログラムもストップです。選ぶと全部のつまみがそのストップの値になり、RESET（全体と行ごと）は使っているストップの初期値に戻します。鍵を押したまま替えると、その鍵が新しいストップで鳴ります。実験レシピはなくなりました。
- **音域は MIDI 24〜96**（C, 〜c⁴）：両方のストップとも、それ以外の鍵は鳴りません。

## 注意

- Gedackt の最低音 C,（32.7 Hz）は発音の限界のすぐ上で、基準の風ではゆっくり弱まります。Cut-up を ×0.90 にするか、風を強めると安定します。
- Arched cut-up を上げると、Gedackt は 1 本あたり Prinzipal の約 2 倍の CPU を使います（半円形の切り上げの計算）。開発に使ったノート PC（Ryzen 5 3500U）では、そのとき Gedackt を 16 本同時に鳴らすと 1 コアの約 6 割、32 本では 1 コアを超えます。初期値の 0 % では Prinzipal と同じくらいです。
- Mallet head の Yarn は選択肢の最後に加えたので、保存した設定の頭の種類は変わりません。DAW のオートメーションを 0〜1 の値で書いている場合は、位置がずれます。
- 1.x のプロジェクトを開くと、つまみの値はそのまま戻り、ストップは Prinzipal 8' です（新しいつまみ Ears と Arched cut-up は 0 %）。レシピの番号は使いません。MIDI 24〜96 の外の音符は鳴らなくなります。

## 確認したこと

- Windows 版：Windows 11 で、音源の試験（labium_check 14 項目）、DAW と同じ読み込み方での試験（ストップと音域、画面を開いたままの演奏、RESET ボタンとストップのメニューの操作を含む）、pluginval（厳しさ 10）に合格しています。Prinzipal の WIND の音は v1.1.1 と全サンプル一致しました。MALLET は、上の「叩いた管壁のモード」の変更で高い鍵が変わります（和音を叩くと、打鍵を計算時間で振り分ける仕組みのため実行ごとに数ミリ秒の差が出ます。v1.1.1 も同じです）。
- macOS 版と Linux 版：GitHub のマシンでビルドし、音源の試験（labium_check）、pluginval（厳しさ 10）、Audio Unit の auval に合格したものを配布します。実際の Mac や Linux の PC で弾いての確認はまだしていません。気づいたことがあればお知らせください。

## ライセンス

GNU AGPLv3。使っているライブラリとフォントのライセンスは、各パッケージの `THIRD_PARTY_NOTICES.md` と `licenses` フォルダにあります。VST は Steinberg Media Technologies GmbH の商標です。Audio Units と macOS は Apple Inc. の商標です。
