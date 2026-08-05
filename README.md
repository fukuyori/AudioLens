# AudioLens 0.1.6

PC で再生される音声を、用途に合わせて聞き取りやすく調整するプリセット型オーディオ補助アプリ(Windows)。

- リポジトリ: <https://github.com/fukuyori/AudioLens>
- バージョンは `CMakeLists.txt` の `project(... VERSION ...)` が唯一の出所で、そこから
  `AUDIOLENS_VERSION` としてコードに渡る。トレイメニューと `AudioLens.exe --version` で確認できる。

「会話」「講義」「映画」「深夜」「ゲーム」に加え、「ロック」「ジャズ」「クラシック」「アンビエント」のプリセットを選ぶだけで、低音の濁りを抑え、人の声に必要な周波数帯を強調し、音量差を自動的に整える。専門知識は不要で、「低音」「声の明瞭さ」「音量差」の 3 項目で効果の強さを調整でき、出力音量と左右バランスも同じ場所で動かせる。

## ステータス

**M3(GUI)実装済み。** Qt 6 の GUI からプリセットを選び、システム音声を補正して聞ける。取り込み元には VB-Cable などの仮想オーディオデバイスを別途用意する。

| マイルストーン | 状態 |
|---|---|
| M1 エンジン検証 | 実装済み・実測済み |
| M2 DSP コア | 実装済み・実測済み |
| M3 GUI・プリセット保存 | 実装済み・実機確認済み |
| M3.5 堅牢化 (N-03) | 実装済み・実測確認済み(デバイス消失の分岐のみ未検証) |
| N-04 既定デバイスの復旧 | 実装済み・実測確認済み |
| M4 仮想デバイスドライバ | **中止。**ビルド成功までの記録([driver/README.md](driver/README.md)) |
| M5 仕上げ | 未着手 |

**個人利用のみとし、自作カーネルドライバは使いません**(2026-08-04 決定)。カーネルモードのバグはブルースクリーンや起動不能を招くのに対し、仮想オーディオデバイスは音の入口にすぎません。広く配布され Microsoft の署名を受けた既存ドライバのほうが安全なので、取り込み元には VB-Cable 等を使います。経緯は [docs/requirements.md](docs/requirements.md) §4.1。

映画プリセットで、音量差(EBU R128 の LRA)が 17.99 → 13.13 LU に縮小することを実測済み。音楽プリセットは逆に音量差を変えない(17.99 → 17.99 LU)ことを実測済みで、これは意図した動作 — 録音のダイナミクスは編曲そのものなので触らない。リアルタイム時の CPU 使用率は 1 コアあたり 0.55%、追加遅延は 2 ms。

## ビルド

Visual Studio(C++ ワークロード)があれば、CMake と Ninja は同梱のものを使うため追加インストールは不要。GUI には Qt 6 (msvc2022_64 キット) が要る。`C:\Qt` などから自動検出され、見つからなければ GUI だけをスキップして残りをビルドする。

```powershell
.\build.ps1                  # RelWithDebInfo -> build\release\bin
.\build.ps1 -Preset debug    # Debug         -> build\debug\bin
.\build.ps1 -Clean           # 構成からやり直す
.\build.ps1 -QtDir C:\Qt\6.11.1\msvc2022_64   # Qt を明示指定
.\build.ps1 -NoGui           # GUI を作らない
```

## 動かす

```powershell
$bin = ".\build\release\bin"

# GUI(通常はこちら)
& $bin\AudioLens.exe

# --- 以下は検証・測定用の CLI ---

# デバイス一覧とプリセット一覧
& $bin\audiolens_passthrough.exe --list
& $bin\audiolens_process.exe --list-presets

# システム音声をプリセットで補正して別デバイスへ流す
#   --capture には「再生デバイス」を指定する(ループバックで取り込む)
#   --ab 8 を付けると 8 秒ごとに補正のオン/オフが切り替わり、効果を比較できる
& $bin\audiolens_passthrough.exe --capture "CABLE Input" --render "ヘッドホン" --preset movie --ab 8

# --takeover を付けると、実行中だけ取り込み元をシステム既定の出力にし、終了時に戻す
& $bin\audiolens_passthrough.exe --capture "CABLE Input" --render "ヘッドホン" --takeover

# 既定の出力が仮想ケーブルのまま取り残されて音が出なくなったときの復旧
& $bin\audiolens_passthrough.exe --set-default "ヘッドホン"

# デバイス変更への追従(N-03)を試す。サンプルレートを一時的に変えて元に戻す
& $bin\audiolens_passthrough.exe --invalidate "CABLE Input"

# WAV に対してオフラインで処理し、効果を数値で確認する
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie

# 段ごとの寄与を切り分ける(ミッド/サイド・遅いレベリング・ディエッサーを個別に外す)
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie --no-midside
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie --no-autogain
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie --no-deesser

# 単体テスト(ハードウェア不要、111 件)
& $bin\audiolens_tests.exe

# プロセスループバック検証(案 E の判定に使った。結論は不成立)
& $bin\audiolens_procloop.exe --list
& $bin\audiolens_procloop.exe --pid <PID> --auto-mute 6 --duration 14
```

VB-Cable などの仮想オーディオデバイスを手動でインストールし、それをシステム既定の出力にしたうえで `--capture` に指定する。自作ドライバ(M4)は凍結したため、この構成が最終形になる。

## ライセンス

**Apache License 2.0**([LICENSE](LICENSE))。ただし `driver/` は例外で、**Microsoft Public License (MS-PL)** である。

`driver/` は Microsoft の [Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) の `audio/simpleaudiosample` に由来する。MS-PL 3(D) はソース形式での配布を MS-PL 下に置くことを求めるため、この部分だけ Apache 2.0 の許諾から外れる。各ファイルの Microsoft 著作権表示は保持すること。詳細は [NOTICE](NOTICE) と [driver/LICENSE-MS-PL.txt](driver/LICENSE-MS-PL.txt)。

## ドキュメント

| ドキュメント | 内容 |
|---|---|
| [docs/requirements.md](docs/requirements.md) | 要件定義書(機能要件・非機能要件・成功基準) |
| [docs/architecture.md](docs/architecture.md) | アーキテクチャ設計書(方式選定・DSP 設計・ロードマップ) |
| [docs/m1-engine-notes.md](docs/m1-engine-notes.md) | M1 実装メモ(ドリフト補正・遅延実測・既知の制限) |
| [docs/m2-dsp-notes.md](docs/m2-dsp-notes.md) | M2 実装メモ(DSP 設計・プリセット効果の実測・検出した不具合) |
| [docs/m3-gui-notes.md](docs/m3-gui-notes.md) | M3 実装メモ(Qt 採用理由・画面構成・設定の保存・検出した不具合) |
| [docs/m3.5-robustness-notes.md](docs/m3.5-robustness-notes.md) | M3.5 実装メモ(リサンプラー・ドリフト制御・デバイス変更への追従) |
| [driver/README.md](driver/README.md) | M4 仮想デバイスドライバ(ライセンス・ビルド前提・テスト署名・公式配布までの道筋) |

## リポジトリ構成

```
src/app/               Qt 6 の GUI(メイン画面、トレイ常駐、設定の保存)
src/common/            ログ、COM/ハンドルの RAII ラッパー、denormal 対策
src/engine/            WASAPI キャプチャ/再生、リングバッファ、形式変換
src/dsp/               フィルタ、コンプレッサー、リミッター、ミッド/サイド、
                       ディエッサー、遅いレベリング、DSP チェーン
src/core/              プリセットとスライダーのマッピング
src/analysis/          ラウドネス測定 (ITU-R BS.1770-4 / EBU R128)
src/audiofile/         WAV 入出力
src/tools/passthrough/ 取り込み → 補正 → 再生の CLI
src/tools/process/     WAV のオフライン処理と効果測定 CLI
src/tools/tone/        信号発生 CLI(デバイス出力 / WAV 書き出し)
src/tools/procloop/    プロセスループバック検証 CLI(案 E の判定用)
tests/                 オフライン単体テスト
scripts/               連続稼働テスト
docs/                  設計ドキュメント
```
