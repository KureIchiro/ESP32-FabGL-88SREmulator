# NEC PC-8801mkIISR Emulator for ESP32

[ [日本語](#日本語) | [English](#english) ]

---

<a id="日本語"></a>
# 日本語

ESP32 と **FabGL** ライブラリを使った NEC PC-8801mkIISR のエミュレータです（開発中）。VGA 出力、PS/2 キーボード、D88 ディスクイメージ、FDD のサブ CPU、FM 音源（YM2203）、シリアル接続の外部ジョイスティックに対応しています。

## 📝 概要

- **ターゲットフレームレート**: **55.4Hz**（55.416Hz）
  - これは **24kHz（高解像度）モニタをつないだ実機の垂直走査周波数**（25 行モード、1 フレーム 448 ラスタ・表示 400 ラスタ）です。性能の都合で下げた値ではありません。
  - PC-8801 が「約 60Hz」と言われるのは 15kHz（標準解像度）モニタのときの値（約 61〜62Hz）です。このエミュレータは 24kHz モニタのみを再現しています。
  - VGA の出力自体は 640×480・60Hz です（PC-8801 の 1 ラインを 2 回表示）。
- **再現しているもの**: 本体 Z80（4MHz）＋ FDD のサブ CPU（Z80）、N88-BASIC V2 / V1(S) / V1(H) / N-BASIC、テキスト（μPD3301 CRTC ＋ μPD8257 DMA、属性、セミグラフィック、スキップライン）、640×200 8 色グラフィックとアナログパレット、ALU・独立アクセス、YM2203 FM 3ch（SSG-EG・CSM 音声合成を含む）＋ SSG 3ch、ビープ、μPD765 FDC と D88 イメージ（1 ファイルに複数枚入った D88 も可）。
- **再現していないもの**: 15kHz モニタ、640×400 白黒（200 ラインに間引いて表示）、サウンドボード II（OPNA）、ジョイスティック端子、CMT・RS-232C・マウス、メモリウェイト。
- **サウンド**: 16384Hz（FabGL の標準）。FM と SSG を混ぜて 8bit DAC に出力します。

> ⚠️ **開発・ライセンスに関するご注意**
>
> メインソースコードの大部分は AI 生成コードをベースに構築・改変しているため、開発者もすべてのロジックを完全に把握しているわけではありません。そのため、ソースコードの詳細な仕様や内部ロジックに関する質問にはお答えできかねます。あらかじめご了承ください。
> ライセンスの詳細な条件はソースコード冒頭のヘッダコメントを参照し、同意の上でご利用ください。

---

## 🤝 クレジットと謝辞

- **グラフィック/オーディオエンジン**: Fabrizio Di Vittorio 氏による [FabGL](https://github.com/fdivitto/FabGL) を使用。
- **Z80 CPU コア**: Marat Fayzullin 氏のコードをベースに改変（本体とサブ CPU の両方に使用）。
- **FM 音源（YM2203）、CRTC・DMA、FDC など**: 本プロジェクトで新規に作成。実機の動作は **MAME**・**QUASI88**・**M88** のソースコードと資料を突き合わせて調べました（`PC88_REQUIREMENTS.md`）。

### 📜 各コアのライセンスについて
Z80 コアは Marat Fayzullin 氏の **fMSX License**、FabGL はそのライセンスに従います。

---

## 🛠️ 開発環境とビルド設定

- **ボードマネージャ**: ESP32 by Espressif Systems（**v2.0.5 推奨**）
- **ライブラリ**: FabGL v1.0.9
- **ボード**: `ESP32 Dev Module`（TTGO VGA32）
- **PSRAM**: `"Enabled"`（**必須**）

---

## 📂 SD カードの構成

```text
/ (SD ルート)
└── /PC88
    ├── N88.ROM          : N88-BASIC 32KB（必須）
    ├── N88_0.ROM〜N88_3.ROM : N88 拡張 ROM 各 8KB（V2・ディスク BASIC に必要）
    ├── N80.ROM          : N-BASIC 32KB（無くてもよい）
    ├── DISK.ROM         : ディスクユニットの ROM 2KB / 8KB（無いとディスクは使えない）
    ├── KANJI1.ROM       : 第 1 水準漢字 ROM 128KB（文字のフォントもここから）
    ├── KANJI2.ROM       : 第 2 水準漢字 ROM（無くてもよい）
    ├── JOYKEY.CFG       : ジョイスティックのボタンのキー割り当て（自動で作られる）
    └── <フォルダ>/*.D88 : ディスクイメージ（フォルダ分けしてよい）
```

---

## 🚀 起動メニュー

| 項目 | 内容 |
| :--- | :--- |
| Disk1 / Disk2 | 各ドライブの D88 を選ぶ。複数枚入ったファイルは何枚目かを選ぶ。Disk1 に複数枚入ったファイルを選び Disk2 が空なら、次の 1 枚を自動で Disk2 に入れる。選択画面は、最後に選んだディスクのフォルダから開く。 |
| Mode | N88-BASIC V2 / V1(S) / V1(H) / N-BASIC |
| Joy B1〜B3 | ジョイスティックのボタン 1〜3 で押すキー（Enter のあと割り当てるキーを押す。F6〜F12 で取り消し）。既定は SPACE / RETURN / ESC。 |
| Run | 起動 |

起動メニューとディスクの選択画面は、1P のコントローラでも操作できます：↑↓ = 選択（押し続けると連続）、←→ = 1画面送り、ボタン 1 = 決定（Enter）、ボタン 2 = 取り消し（ESC）。

---

## 🕹️ 操作方法

- **`[F12]`** : 一時停止 / 再開
- **`[Shift] + [F12]`** : リセット（ESP32 ごと再起動して起動メニューへ）。SD に書き戻していないディスクの書き込みがあるときは、1 回目は警告を出して一時停止するだけです。もう一度押すとリセットします。
- **一時停止中に `[F10]` / `[F11]`** : ドライブ 1 / 2 のディスクを入れ替え
- **一時停止中に `[Shift] + [F10]` / `[Shift] + [F11]`** : ドライブ 1 / 2 の内容を SD へ書き戻し
- 特殊キー（日本語 106 キーボード）: STOP = End、HELP = ScrollLock、GRPH = 左 Alt、カナ = カタカナ/ひらがな または右 Alt、ROLL UP/DOWN = PageUp/PageDown、INS/DEL = BackSpace / Delete。カナと CAPS はロック式です。

---

## 🔌 ジョイスティック（外部コントローラ）

MSX・マーク III 版と同じ変換器・同じ通信仕様です：シリアル `IO39`、115200bps、ポートごとに 1 バイト（b7 = ポート、b6〜b4 = ボタン 3〜1、b3〜b0 = 右/左/下/上、0 = 押下）。

- ジョイスティックは JoyToKey と同じ使い方です。方向はテンキーの 8 / 2 / 4 / 6、ボタン 1〜3 は起動メニューで決めたキーを押します。
- SR のジョイスティック端子（SSG の I/O ポート）は**再現しません**（何もつないでいない扱い）。端子とテンキーを両方読み、上下の意味が逆のゲームがあるためです（例：スペースハリアーはテンキー 8 = 上、レバー上 = 下）。

---

## 免責事項

本ソフトウェアの使用によって生じたいかなる損害についても、FABRIZIO DI VITTORIO 氏、MARAT FAYZULLIN 氏、および開発者は一切の責任を負いません。自己責任でご利用ください。

---

<a id="english"></a>
# English

An emulator for the NEC PC-8801mkIISR using ESP32 and the **FabGL** library (Work in Progress). It supports VGA output, PS/2 keyboards, D88 disk images, the FDD sub-CPU, FM sound (YM2203) and a serial-connected external joystick.

## 📝 Overview

- **Target Frame Rate**: **55.4 Hz** (55.416 Hz)
  - This is the **vertical scanning frequency of the real PC-8801 with a 24 kHz (high-resolution) monitor** in 25-line mode (448 rasters per frame, 400 displayed). It is not a performance compromise.
  - The "about 60 Hz" often quoted for the PC-8801 is the 15 kHz (standard resolution) monitor (approx. 61–62 Hz). This emulator emulates the 24 kHz monitor only.
  - VGA output itself is 640x480 @ 60 Hz (each PC-8801 line is shown twice).
- **Emulated**: main Z80 (4 MHz) + FDD sub-CPU Z80, N88-BASIC V2 / V1(S) / V1(H) / N-BASIC, text (μPD3301 CRTC + μPD8257 DMA, attributes, semigraphics, skip line), 640x200 8-colour graphics with analog palette, ALU / independent plane access, YM2203 FM 3ch (with SSG-EG and CSM voice) + SSG 3ch, beeper, μPD765 FDC with D88 images (multi-image files supported).
- **Not emulated**: 15 kHz monitor mode, 640x400 monochrome (shown thinned to 200 lines), Sound Board II (OPNA), joystick port, CMT / RS-232C / mouse, memory wait.
- **Audio**: 16384 Hz (FabGL default). FM and SSG are mixed into the 8-bit DAC.

> ⚠️ **Notice Regarding Development & Licenses**
>
> Most of the main source code is built and modified based on AI-generated code. Therefore, the developer does not completely grasp all internal logic and cannot answer detailed questions regarding source code specifications. Thank you for your understanding.
> Please refer to the header comments at the beginning of the source code for license details and agree to them before use.

---

## 🤝 Credits & Acknowledgments

- **Graphics/Audio Engine**: Uses [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio.
- **Z80 CPU Core**: Modified based on the work by Marat Fayzullin (used for both the main CPU and the FDD sub-CPU).
- **FM sound (YM2203), CRTC / DMA, FDC and the rest**: newly written for this project. The hardware behaviour was studied from the source code and documents of **MAME**, **QUASI88** and **M88** (see `PC88_REQUIREMENTS.md`).

### 📜 Core Licenses
The Z80 core follows Marat Fayzullin's **fMSX License**; FabGL follows its own license.

---

## 🛠️ Environment & Build Settings

- **Board Manager**: ESP32 by Espressif Systems (**v2.0.5 Recommended**)
- **Library**: FabGL v1.0.9
- **Board**: `ESP32 Dev Module` (TTGO VGA32)
- **PSRAM**: `"Enabled"` (**Required**)

---

## 📂 SD Card Structure

```text
/ (SD Root)
└── /PC88
    ├── N88.ROM          : N88-BASIC 32KB (required)
    ├── N88_0.ROM〜N88_3.ROM : N88 extension ROMs 8KB each (needed for V2 / disk BASIC)
    ├── N80.ROM          : N-BASIC 32KB (optional)
    ├── DISK.ROM         : FDD unit ROM 2KB / 8KB (without it, disks cannot be used)
    ├── KANJI1.ROM       : Kanji ROM level 1, 128KB (the character font is also taken from here)
    ├── KANJI2.ROM       : Kanji ROM level 2 (optional)
    ├── JOYKEY.CFG       : Joystick button key assignment (created automatically)
    └── <folders>/*.D88  : Disk images (sub-folders are allowed)
```

---

## 🚀 Boot Menu

| Item | Description |
| :--- | :--- |
| Disk1 / Disk2 | Choose a D88 for each drive. For a file containing several disks, choose which one. If Disk1 is such a file and Disk2 is empty, the next disk goes to Disk2 automatically. The selector opens in the folder of the most recently chosen disk. |
| Mode | N88-BASIC V2 / V1(S) / V1(H) / N-BASIC |
| Joy B1〜B3 | Key pressed by joystick buttons 1–3 (Enter → press the key; F6–F12 cancel). Default SPACE / RETURN / ESC. |
| Run | Start |

The boot menu and the disk selectors can also be operated with the 1P controller: Up/Down = select (repeats while held), Left/Right = page up/down, button 1 = Enter, button 2 = cancel (ESC).

---

## 🕹️ Operations Guide

- **`[F12]`** : Pause / Resume
- **`[Shift] + [F12]`** : Reset (reboots the ESP32 and returns to the boot menu). If a disk has been written and not saved to SD, the first press only pauses with a warning; press again to reset.
- **While paused `[F10]` / `[F11]`** : Change the disk in drive 1 / 2
- **While paused `[Shift] + [F10]` / `[Shift] + [F11]`** : Write drive 1 / 2 back to the SD card
- Special keys (Japanese 106 keyboard): STOP = End, HELP = ScrollLock, GRPH = left Alt, KANA = Katakana/Hiragana or right Alt, ROLL UP/DOWN = PageUp/PageDown, INS/DEL = BackSpace / Delete. KANA and CAPS are lock keys.

---

## 🔌 Joystick (External Controller)

Same converter and protocol as the MSX / MarkIII emulators: serial `IO39`, 115200 bps, 1 byte per port (b7 = port, b6–b4 = buttons 3–1, b3–b0 = right/left/down/up, active low).

- The joystick works like JoyToKey: directions press the numeric keypad 8 / 2 / 4 / 6, buttons 1–3 press the keys chosen in the boot menu.
- The SR joystick port (SSG I/O ports) is **not** emulated (reads as nothing connected). Some games read both the port and the keypad with opposite meanings (e.g. Space Harrier: keypad 8 = up, lever up = down), so only the key mapping is used.

---

## Disclaimer

IN NO EVENT SHALL FABRIZIO DI VITTORIO, MARAT FAYZULLIN, OR THE DEVELOPER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE. USE AT YOUR OWN RISK.
