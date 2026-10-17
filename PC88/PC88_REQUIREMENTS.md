# PC-8801mkIISR エミュレータ 要件定義

MAME / QUASI88 / M88 のソースを突き合わせて決めた仕様（調査: 2026-10-09）。本文の §1〜§10 は調査結果そのまま。
実装で採った方針は次のとおり（本文の「未確認」の表の採用案に従う）。

## 実装方針（このエミュレータで決めたこと）
- 画面は 24kHz モニタ扱い（IN 40h bit1=0）。1 フレーム 448 ラスタを 2 ラスタずつ 224 行として進める（1 行 322 クロック、55.4Hz）。出力は 640×200、表示 400 ラスタの 2 ラスタを 1 行に描く。
- テキストは各文字行の先頭で DMA ch2 から 1 行分（H + 属性数×2 バイト）を読む（M88 方式）。V1H/V2 は TVRAM、V1S はメイン RAM から読む。
- 割込み: E6h で無効の要求は捨てる。E4h bit3=1 は全レベル許可。OPN は立上りエッジで要求（32h bit7 解除時に保留分を出す）。応答後は E4h を書くまで次を出さない。
- 黒の文字は不透明。30h bit1（/COLOR）は無視。52h の背景色は MAME のビット割り当て（白黒グラフィック時のみ影響）。
- ウィンドウ（70h）は常にメイン RAM。TVRAM の切替（32h bit4）は高速モード（V1H/V2）だけ。DMAC 68h 書込みで上位/下位フリップフロップを戻す。
- 拡張 RAM（E2h/E3h）は無し（読出し FFh）。第2水準漢字 ROM は任意。
- ディスクユニット: サブ Z80 は本体がポート C（FEh）を読むたびにサブ側がポート C を読むところまで進める（QUASI88 の「PC 読出しで切替」）。そのほかは 1 行ごとに 322 クロック進める。サブ CPU の割込みは NOP（IM0 で 00h）として扱い、HALT 解除と割込み禁止だけ行う。
- 省略: メモリウェイト、CMT / RS-232C（ステータスだけ返す）、マウス、SING（40h bit7）、15kHz モニタ、640×400 白黒は 200 行に間引いて表示。

---
# PC-8801mkIISR ハードウェア仕様（エミュレータ実装用）

作成: 2026-10-09 / 対象: PC-8801mkIISR（V1S/V1H/V2, N88-BASIC V2/V1, N-BASIC, YM2203, 内蔵 2D FDD = PC-80S31 相当）

## 0. 出典と表記

すべての事実はエミュレータのソースコードから取った（記憶からの補完はしない）。出典は `[略号 ファイル 関数/箇所]` で示す。

| 略号 | 取得元（2026-10-09 取得, master） | 主に使ったファイル |
|---|---|---|
| MAME | github.com/mamedev/mame | `src/mame/nec/pc8801.cpp/.h`, `pc8001.cpp/.h`（pc8801_state の基底 `pc8001_base_state`）, `pc88_alu.cpp`, `pc88_kbd.cpp/.h`, `src/devices/bus/nec_fdd/pc80s31k.cpp/.h`, `src/devices/video/upd3301.cpp/.h`, `src/devices/machine/i8257.cpp`, `i8255.cpp`, `i8214.cpp`, `upd765.cpp/.h`, `upd1990a.cpp/.h`, `src/lib/formats/d88_dsk.cpp` |
| Q88 | github.com/libretro/quasi88-libretro | `src/pc88main.c/.h`, `intr.c`, `crtcdmac.c/.h`, `memory.c/.h`, `pio.c`, `pc88sub.c`, `fdc.c`, `keyboard.c`, `screen.c/.h`, `initval.h`, `z80.c`, `document/MANUAL.TXT`, `FORMAT.TXT` |
| M88 | github.com/rururutan/m88 | `src/pc88/memory.cpp`, `crtc.cpp`, `screen.cpp`, `intc.cpp`, `base.cpp`, `subsys.cpp`, `pio.h/.cpp`, `fdc.cpp`, `pd8257.cpp`, `beep.cpp`, `opnif.cpp/.h`, `joypad.cpp`, `pc88.cpp`, `src/win32/cfgpage.cpp`, `M88.rc` |

- 「（計算）」は出典の数値から私が計算した値、「（推定）」は出典の挙動から推した値。出典同士が食い違う箇所は「**相違**」と明記し、末尾の「未確認」にもまとめた。
- MAME では mkIISR は `pc8801mk2sr_state`（`pc8801_state` + ALU + YM2203）。`pc8801_state` は `pc8001_base_state` を継承し、ポート 10h/30h 書込み、割込み（E4h/E6h）、テキスト描画、属性展開は pc8001.cpp 側にある。

---

## 1. クロック / タイミング

### 1.1 クロック
| 項目 | 値 | 出典 |
|---|---|---|
| メイン Z80 | 31.9488MHz / 8 = **3.9936MHz**（mkIISR は 4MHz 固定。8MHz 切替は FH/MH 以降・ポート 6Eh） | [MAME pc8801.cpp `#define MASTER_CLOCK (PC8801FH_OSC3 / 8)`, 機種表 "PC-8801 mkII SR z80A @ 4", pc8801fh_state::machine_reset][Q88 initval.h CONST_4MHZ_CLOCK 3.9936] |
| 15kHz ドットクロック | 28.636363MHz / 2 = 14.318MHz | [MAME `PIXEL_CLOCK_15KHz`] |
| 24kHz ドットクロック | 42.1052MHz / 2 = 21.0526MHz | [MAME `PIXEL_CLOCK_24KHz`] |
| YM2203 | 3.9936MHz | [MAME pc8801mk2sr(): `YM2203(config, m_opn, MASTER_CLOCK)`][M88 opnif.h baseclock 3993600] |
| BEEP | MASTER_CLOCK/16/13/8 = 2400Hz（CPU クロック設定に依存しない） | [MAME pc8801() BEEP コメント] |
| 1/600 秒タイマ | 600Hz（= 6656 CPU クロック, 計算） | [MAME rtc_timer] |
| サブ Z80 / FDC | 4MHz / 8MHz | §8 |

モニタ周波数はリセット時に IN 40h bit1（MAME の CTRL "Monitor Type"）で決まり、CRTC クロックを切り替える [MAME machine_reset: `set_screen_frequency(); m_crtc->set_unscaled_clock(24k ? PIXEL_CLOCK_24KHz : PIXEL_CLOCK_15KHz)`]。DIP SW ではなくモニタ判別線（/SHG）。

### 1.2 フレーム構成（uPD3301 パラメータから決まる）
- 水平総ドット = (H + Z) × 8、垂直総ライン = (L + V) × R、表示 = H×8 × L×R [MAME upd3301::recompute_parameters]。
- VRTC は表示最終ライン（L×R）で 1、次フレームのライン 0 で 0 [MAME upd3301::update_vrtc_timer]。VRTC 期間 = V×R ライン。
- MAME 記載の実測値: 15kHz 25行 62.422Hz / 20行 61.462Hz、24kHz 25行 55.416Hz / 20行 56.424Hz [MAME pc8801.cpp PIXEL_CLOCK 付近コメント]。

| モード | H/Z | L/R/V | 水平 | 垂直 | ライン周波数 | CPU クロック/ライン | フレーム | CPU クロック/フレーム |
|---|---|---|---|---|---|---|---|---|
| 24kHz 25行 | 80/26 | 25/16/3 | 848 | 448 | 24.826kHz (40.28µs) | 160.86 | 55.416Hz | 約 72,066 |
| 24kHz 20行 | 80/26 | 20/20/2（推定） | 848 | 440 | 24.826kHz | 160.86 | 56.42Hz | 約 70,778 |
| 15kHz 25行 | 80/32 | 25/8/7（推定） | 896 | 256 | 15.980kHz (62.58µs) | 249.9 | 62.42Hz | 約 63,977 |
| 15kHz 20行 | 80/32 | 20/10/6（推定） | 896 | 260 | 15.980kHz | 249.9 | 61.46Hz | 約 64,977 |

（数値は計算。24kHz/25 行の H/Z/L/R/V は Q88 の既定パラメータ `CE 98 6F 58 53` [Q88 crtcdmac.c crtc_init] から、他は MAME 実測値と 896×260（MAME 既定 `set_raw(PIXEL_CLOCK_15KHz, 896, 0, 640, 260, 0, 200)`）、M88 の `vretrace = line200 ? 7 : 3`・コメント「15kHz 256 lines(25), 260 lines(20)」[M88 crtc.cpp HotReset/冒頭コメント] から逆算。）
- M88 の 1 ラスタ時間: 15kHz 62.58µs、24kHz 40.28µs [M88 crtc.cpp HotReset `linetime = line200 ? 6.258*8 : 4.028*16`（10µs 単位, 1 文字行あたり）]。
- Q88 は VSYNC 55.4Hz 固定、フレーム先頭から 47.8/448 を VRTC、400/448 を表示とする近似 [Q88 intr.c VRTC_TOP/VRTC_DISP]。
- HBLANK は Z 文字分（24kHz で 208 ドット）。PC-88 では HRTC は使われない（MAME で HRTC コールバック未接続）。

スキャンライン駆動の推奨（24kHz/25 行）: 1 フレーム 448 ライン、ライン 0–399 表示、400 で VRTC=1 + VRTC 割込み要求、447 の次で VRTC=0。1 ライン 160.86 クロック → 固定小数点で 160/161 を配分（計算）。

### 1.3 200 / 400 ライン
- 24kHz: テキストは R=16（8 ラインフォントを縦 2 倍）、200 ライングラフィックはライン複製、31h bit0=0 & 白黒で 640×400 [MAME draw_text `y_double = 24KHz ? 2 : 1`, draw_bitmap]。
- 15kHz: 200 ラインのみ（400 ラインモードは上半分のみ, MAME TODO）。

---

## 2. メモリマップとバンク切替

### 2.1 物理メモリ
| 名称 | 容量 | 出典 |
|---|---|---|
| メイン RAM | 64KB | [MAME pc8801.cpp machine_start: m_work_ram 0x10000] |
| 高速 RAM（テキスト VRAM, TVRAM） | 4KB | [MAME machine_start: m_hi_work_ram 0x1000][M88 memory.cpp InitMemory: tvram 0x1000] |
| GVRAM | 16KB × 3 プレーン（B, R, G） | [MAME machine_start: m_gvram 0xc000][Q88 memory.c main_vram[0x4000][4] "B/R/G/pad"] |
| N88 ROM | 32KB + 拡張 8KB × 4 | [MAME ROM_START(pc8801mk2sr)] |
| N-BASIC ROM | 32KB | 同上 |
| 拡張 RAM | 標準なし（オプションボード） | [MAME INPUT_PORTS "MEM"][Q88 MANUAL -extram 既定 0] |

プレーン番号: 0 = B, 1 = R, 2 = G。色番号 = B | R<<1 | G<<2 [MAME screen_update: `res |= bit << plane`][MAME palette_reset: b=i&1, r=i&2, g=i&4]。

### 2.2 0000h–7FFFh

```c
// 読出し  [MAME pc8801.cpp pc8801_state::main_map 0x0000-0x7fff の read lambda]
uint8_t rd_0000_7fff(uint16_t a) {
    if (pE2 & 0x01)                 return extram[pE3 * 0x8000 + a];   // 拡張RAM読出し許可
    if (p31 & 0x02)                 return ram[a];                     // MMODE=1: 64KB RAM
    if (p31 & 0x04)                 return n80rom[a];                  // RMODE=1: N-BASIC ROM
    if (a >= 0x6000 && !(p71 & 1))  return n88ext[p32 & 3][a & 0x1fff];// N88 拡張ROM (N88_0..3)
    return n88rom[a];                                                   // N88 ROM
}
// 書込み  [同 write lambda]
void wr_0000_7fff(uint16_t a, uint8_t d) {
    if (pE2 & 0x10) extram[pE3 * 0x8000 + a] = d; else ram[a] = d;   // ROM 選択中も RAM に書ける
}
```
- 補足 [M88 memory.cpp Update60R]: p71 == FFh なら N88 ROM。p71 bit0 = 0 なら N88 拡張 ROM（バンク = p32 & 3）。bit0 = 1 で bit1–7 のどれかが 0 ならオプションボードの外部 EROM（e1.rom–e7.rom）。MAME は bit0 しか見ない。mkIISR 単体では p71 = FFh / FEh だけを扱えばよい。
- Q88 も同じ論理 [Q88 pc88main.c main_memory_mapping_0000_7fff]。
- N-BASIC モード（p31 bit2 = 1）のとき 6000h–7FFFh も N-BASIC ROM [M88 Update60R `(port31 & 6) == 4` → n80+0x6000]。

### 2.3 8000h–83FFh（テキストウィンドウ）

```c
// [MAME pc8801.cpp main_map: m_window_view][MAME port31_w: (m_gfx_ctrl & 6) ? disable : select]
uint8_t *win(uint16_t a) {
    if (p31 & 0x06) return &ram[a];                          // ROM モード以外ではただの RAM
    uint16_t wa = ((uint16_t)p70 << 8) + (a & 0x3ff);         // 16bit で折り返す [M88 RdWindow & 0xffff]
    return &ram[wa];                                          // 実体はメイン RAM
}
```
- **相違**: wa ≥ F000h のとき
  - MAME: p32 bit4 = 1 なら高速 RAM 側（m_hi_work_ram）、0 なら m_work_ram [MAME main_map m_window_view[0]]。
  - Q88: H モード（V1H/V2）では常に「TVRAM の裏のメイン RAM」 [Q88 main_memory_mapping_8000_83ff]。
  - M88: 常にメイン RAM（TVRAM ではない）[M88 Update80/RdWindow]。
  - → Q88/M88 の多数派では「ウィンドウは常にメイン RAM を指し、TVRAM には届かない」。

### 2.4 8400h–BFFFh
常にメイン RAM [MAME main_map 0x8000-0xbfff]。

### 2.5 C000h–FFFFh

```c
// [MAME pc8801mk2sr_state::flush_gvram_access, pc8801_state::gvram_map/wram_c000_r, main_map の m_alu_view]
// [M88 memory.cpp UpdateC0/UpdateF0/GetHiBank][Q88 main_memory_vram_mapping]
int vram_sel;   // 0=B,1=R,2=G,3=メイン  (5Ch-5Fh で設定, リセット=3)

uint8_t rd_c000_ffff(uint16_t a) {
    if (p32 & 0x40) {                 // GVAM=1: 拡張(ALU)アクセスモード
        vram_sel = 3;                 // ALU モード中は 5Ch-5Eh の選択は無効化される
        if (p35 & 0x80) return alu_read(a & 0x3fff);   // GAM=1: ALU 経由で GVRAM
        /* GAM=0 → 下の RAM へ */
    } else if (vram_sel < 3) {
        return gvram[vram_sel][a & 0x3fff];             // C000-FFFF の 16KB 全部がプレーン
    }
    if (a < 0xf000) return ram[a];
    if (high_speed_mode && !(p32 & 0x10)) return tvram[a & 0xfff];  // TMODE=0: 高速RAM(TVRAM)
    return ram[a];                                                   // TMODE=1: メイン RAM
}
// 書込みも同じ分岐（ALU は alu_write）
```
- `high_speed_mode` = DIP SW（IN 31h bit6 = 1, V1H/V2）[M88 UpdateF0: `!(port32 & 0x10) && (sw31 & 0x40)`][Q88 main_memory_mapping_c000_ffff: `high_mode && (misc_ctrl & MISC_CTRL_TEXT_MAIN)` ※Q88 は内部で RAM を入れ替えて表現]。V1S では F000h–FFFFh は普通のメイン RAM で p32 bit4 は無効（M88 コメント「b4 RAM/~TVRAM (V1S 無効)」）。
- **相違**: MAME は H/S に関係なく p32 bit4 で m_work_ram / m_hi_work_ram を切り替える [MAME wram_c000_r]。MAME の命名では「CRTC が DMA で読む方」= m_work_ram[F000h-] = p32 bit4=0 側。3 実装とも「p32 bit4=0 で見える 4KB が CRTC 表示対象」で一致。
- 5Ch–5Fh 選択時は F000h–FFFFh もプレーンになり TVRAM は見えない [M88 SelectGVRAM 0x4000 バイト][Q88 vram_read(addr & 0x3fff)]。
- ALU: §6.4。

### 2.6 関係ポートのビット定義・リセット値・読み返し

| ポート | R/W | ビット | リセット値 | 読み返し |
|---|---|---|---|---|
| 31h | W | bit5 25LINE（高速 CRT 時の行数, エミュレータは無視）, bit4 HCOLOR 1=カラー/0=白黒グラフィック, bit3 GRPH グラフィック表示 1=ON, bit2 RMODE 1=N-BASIC ROM/0=N88 ROM, bit1 MMODE 1=64KB RAM, bit0 200LINE 1=200/0=400ライン（白黒時）[MAME pc8801.cpp port31_w コメント][Q88 screen.h GRPH_CTRL_*] | MAME 31h, Q88 31h, M88 00h（いずれも N88 ROM 選択）[MAME machine_reset m_gfx_ctrl=0x31][Q88 pc88main_init grph_ctrl=0x31][M88 pc88.cpp Reset Out(0x31,0)] | 不可。IN 31h は DIP SW2（§3） |
| 32h | R/W | bit7 SINTM 1=サウンド割込マスク, bit6 GVAM 1=ALU(拡張)アクセス/0=独立アクセス, bit5 PMODE 1=アナログ/0=デジタルパレット, bit4 TMODE 1=F000h-をメインRAM/0=高速RAM(TVRAM), bit3-2 AVC 画面出力モード（00 TV/ビデオ, 01 なし, 10 アナログRGB, 11 オプション）, bit1-0 EROM バンク（N88_0..3）[MAME misc_ctrl_r/w コメント][Q88 pc88main.h MISC_CTRL_*] | MAME 80h, Q88 90h, M88 80h | 書いた値そのまま [MAME misc_ctrl_r][M88 In32][Q88 IN 32h] |
| 34h | W | ALU 演算: bit(i) と bit(i+4) の組（i=0:B,1:R,2:G）。§6.4 | MAME 0 [pc88_alu device_reset], Q88 77h, M88 0 | 不可 |
| 35h | W | bit7 GAM 1=C000h-を ALU 経由 GVRAM, bit5-4 書込みモード, bit2-0 比較色 (bit0=B,bit1=R,bit2=G) [M88 Out35 コメント][MAME alu_ctrl2_w] | 0 | 不可。※書込み時 bit7=1 なら vram_sel=3 に戻す [Q88 OUT 35h][M88 Out35 port5x=3] |
| 5Ch/5Dh/5Eh/5Fh | W | データ無視。5Ch=B, 5Dh=R, 5Eh=G, 5Fh=メイン RAM [MAME vram_select_w: offset&3][Q88 OUT 5C-5F] | 3（メイン） | IN 5Ch = F8h \| (1<<sel)（sel=3 なら F8h）[MAME vram_select_r][M88 In5c {F9,FA,FC,F8}]。5Dh-5Fh は未定義 |
| 70h | R/W | ウィンドウ上位アドレス（×100h） | MAME 80h（=8000h, 恒等）, Q88/M88 00h | 書いた値 [MAME window_bank_r][M88 In70] |
| 78h | W | 70h を +1（8bit で折り返し）[MAME window_bank_inc_w][M88 Out78] | – | – |
| 71h | R/W | bit0 0=N88 拡張 ROM 選択（6000h-7FFFh）, 1=N88 ROM。他ビットは外部 EROM 選択（§2.2 補足） | FFh [MAME machine_reset][Q88][M88] | 書いた値 [MAME ext_rom_bank_r] |
| E2h | R/W | bit4 WREN 拡張 RAM 書込み, bit0 RDEN 拡張 RAM 読出し [MAME extram_mode_w] | 0 | (v ^ 11h) \| EEh（= ~v \| EEh）[MAME extram_mode_r][M88 Ine2][Q88] |
| E3h | R/W | 拡張 RAM バンク（32KB 単位）[MAME extram_bank_w] | 0 | MAME: 書いた値。Q88: 実装なしなら FFh |

- 拡張 RAM は mkIISR 標準では無い。Q88 は拡張 RAM 無しのとき E2h/E3h の書込みを無視し、読出しは FFh [Q88 OUT/IN E2h/E3h `if (use_extram)`]。MAME の既定構成は 8MB 搭載（"MEM" 既定 0x0a）なので注意。実装は「無し（読出し FFh）」で十分。
- MAME のリセット状態 [MAME pc8801_state::machine_reset, pc8801mk2sr_state::machine_reset]: p71=FFh, p31=31h, p70=80h, p32=80h, テキスト表示 ON, グラフィックプレーンマスクなし, vram_sel=3, E2h/E3h=0, パレット=デジタル既定色, ALU GAM=0。
- 電源投入時 RAM 初期パターン: 一部ソフトが依存（「スキーム: FFFFh が 00h」「天使たちの午後2: FFF8h–FFFFh の OR ≠ 0」）[Q88 pc88main.c power_on_ram_init]、[M88 memory.cpp SetRAMPattern, `ram[0xff33] = 0; // PACMAN 対策`]。

---

## 3. I/O マップ

アドレスは下位 8bit デコード（`map.global_mask(0xff)`）、未接続ポートの読出しは FFh [MAME pc8801_state::main_io `unmap_value_high`][Q88 main_io_in 末尾 `return 0xff`]。

### 3.1 一覧（mkIISR）

| ポート | R | W | 出典 |
|---|---|---|---|
| 00h–0Eh | キーマトリクス（§3.3） | –（Q88 は PCG-8100 用に 00h-02h, 0Ch-0Fh を使用。不要） | [MAME main_io map(0x00,0x0f) kbd read_direct] |
| 10h | – | bit2-0 = uPD1990A C2-C0（コマンド）, bit3 = カレンダ DATA IN、8bit 全体 = プリンタ出力データ | [MAME pc8001.cpp port10_w] |
| 20h/21h（～2Fh ミラー） | i8251 データ/ステータス | i8251 データ/モード・コマンド（CMT / RS-232C） | [MAME main_io map(0x20,0x21).mirror(0x0e)] |
| 30h | DIP SW1（§3.2） | システム制御1: bit5-4 BS2/BS1（00=CMT 600bps, 01=CMT 1200bps, 10=RS-232C 非同期, 11=同期）, bit3 MTON（CMT モータ）, bit2 CDS（キャリア 1=mark）, bit1 /COLOR（1=白黒テキスト/0=カラー）, bit0 /40（1=80桁/0=40桁） | [MAME pc8001.cpp port30_w コメント] |
| 31h | DIP SW2（§3.2） | §2.6 | [MAME port31_w] |
| 32h | 32h の値 | §2.6 | [MAME misc_ctrl_r/w] |
| 34h, 35h | – | ALU（§6.4） | [MAME pc8801mk2sr_state::main_io] |
| 40h | ストローブポート入力（下表） | ストローブポート出力（下表） | [MAME port40_r/port40_w] |
| 44h/45h | YM2203 ステータス / データ（レジスタ読出し） | YM2203 アドレス / データ | [MAME pc8801mk2sr main_io map(0x44,0x45) ym2203 read/write] |
| 46h/47h | – | OPNA 拡張（FH 以降）。mkIISR では未使用 | [MAME pc8801fh_state::main_io][Q88 bootup_work_init: SR は 44-45h のみ] |
| 50h | ライトペン（パラメータ読出し） | uPD3301 パラメータ | [MAME upd3301 read/write offset0] |
| 51h | uPD3301 ステータス | uPD3301 コマンド | [MAME upd3301 read/write offset1] |
| 52h | – | 背景色 / ボーダー色（下記 **相違**） | [MAME bgpal_w] |
| 53h | – | 画面重ね合わせ: bit0 1=テキスト非表示, bit1 1=B プレーン非表示, bit2 R, bit3 G | [MAME layer_masking_w][Q88 screen.h GRPH_PILE_*][M88 Out53] |
| 54h–5Bh | – | パレット 0–7（§6.3） | [MAME palram_w] |
| 5Ch | §2.6 | プレーン選択 B | [MAME vram_select_r/w] |
| 5Dh–5Fh | – | プレーン選択 R / G / メイン | 同上 |
| 60h–67h | DMAC ch0–3 アドレス/カウント | 同左（低→高バイトの順。共通フリップフロップ） | [MAME i8257 read/write][Q88 dmac_out_address/counter] |
| 68h | DMAC ステータス（bit0-3 TC, 読むと下位4bit クリア） | DMAC モード: bit0-3 ch 許可, bit4 回転優先, bit5 拡張書込み, bit6 TC ストップ, bit7 オートロード（ch2←ch3） | [MAME i8257.cpp MODE_* マクロ, read REGISTER_STATUS] |
| 6Eh/6Fh | FH 以降のみ（CPU クロック / ボーレート）。mkIISR は FFh | – | [Q88 IN 6Eh/6Fh `ROM_VERSION >= '8'`][MAME pc8801fh_state] |
| 70h, 71h, 78h | §2.6 | §2.6 | |
| E2h, E3h | §2.6 | §2.6 | |
| E4h | – | 割込みレベル（§4） | [MAME irq_level_w] |
| E6h | – | 割込みマスク（§4） | [MAME irq_mask_w] |
| E8h/E9h | 第1水準漢字 ROM データ（E8h = 偶数アドレス側 `rom[addr*2+1]`, E9h = `rom[addr*2]`） | E8h = アドレス下位, E9h = アドレス上位 | [MAME kanji_r/kanji_w][Q88 IN E8h: kanji_rom[0][addr][1]] |
| EAh/EBh | FFh | 読出し開始/終了サイン（エミュレータは無視） | [MAME kanji_w TODO][Q88 OUT EAh/EBh return] |
| ECh–EFh | 第2水準（mkIISR 標準には無い。MAME も "not on stock mkIISR"） | 同上 | [MAME ROM_START(pc8801mk2sr) コメント] |
| FCh–FFh | 8255（ディスクユニットとの通信, §8） | 同左 | [MAME main_io map(0xfc,0xff) pc80s31 host_map] |

- **相違（50h/51h 読出し）**: MAME と Q88 は 51h = ステータス、50h = ライトペン/パラメータ [MAME upd3301::read][Q88 IN 50h crtc_in_parameter, IN 51h crtc_in_status]。M88 は 50h = ステータス [M88 pc88.cpp c_crtc `{0x50, portin, CRTC::getstatus}`]。2対1 で 51h = ステータス。

#### ポート 40h 読出し
| bit | 意味 | 出典 |
|---|---|---|
| 7, 6 | UOP2/UOP1（汎用入力）。常に 1 として返す | [MAME CTRL `PORT_BIT(0xc0, IP_ACTIVE_LOW)`][Q88 `| 0xc0`][M88 Base::Reset port40 = 0xc0+…] |
| 5 | VRTC 1=垂直帰線中 | [MAME port40_r `m_crtc->vrtc_r() << 5`] |
| 4 | CDO（uPD1990A データ出力） | [MAME `m_rtc->data_out_r() << 4`] |
| 3 | 1 = 起動時 FD 自動ブートしない（ROM 起動）/ 0 = FD ブート | [MAME CTRL "Auto-boot floppy at start-up" 0x08=Off][Q88 ctrl_boot = SW_ROMBOOT(0x08)][M88 Base::Reset `+8` if autoboot off] |
| 2 | MAME: CMT CDIN（テープ入力コンパレータ）。Q88 は常に 1 を OR | [MAME port40_r `cmt_cdin_r() << 2`][Q88 IN 40h `| 0x04`] |
| 1 | /SHG モニタ: 1 = 15kHz（標準解像度）, 0 = 24kHz（高解像度） | [MAME CTRL "Monitor Type" 0x02=15KHz][Q88 monitor_15k][M88 `fv15k ? 2 : 0`] |
| 0 | プリンタ BUSY | [MAME `m_centronics_busy`] |

例（24kHz, FD ブート, VRTC=0, CDO=0, プリンタなし）: MAME 相当 C0h、Q88 相当 C4h（計算）。

#### ポート 40h 書込み
| bit | 意味 | 出典 |
|---|---|---|
| 7 | UOP2 / SING（1bit サウンド直接出力） | [MAME port40_w コメント][M88 beep.cpp Mix: port40 & 0x80] |
| 6 | UOP1（マウス/JOP1 ストローブ） | [MAME `m_mouse_port->pin_8_w(BIT(data,6))`][Q88 keyboard_jop1_strobe] |
| 5 | BEEP 1=2400Hz ブザー ON | [MAME port40_w][M88 beep.cpp] |
| 4 | FLASH（M88 は GVRAM ウェイト種別に使用） | [MAME コメント][M88 memory.cpp Out40] |
| 3 | /CLDS CRT 同期制御（エミュレータは無視） | [MAME コメント][Q88 crtcdmac.h set_crtc_sync_bit は空] |
| 2 | CCK カレンダクロック | [MAME `m_rtc->clk_w(BIT(data,2))`] |
| 1 | CSTB カレンダストローブ | [MAME `m_rtc->stb_w(BIT(data,1))`] |
| 0 | /PSTB プリンタストローブ | [MAME `write_strobe(BIT(data,0))`] |

カレンダ（uPD1990A）手順 [MAME upd1990a.cpp stb_w/clk_w][Q88 calendar_stlobe/calendar_shift_clock]:
- OUT 10h で C2-C0（とDATA IN）を置き、40h bit1 をパルスするとコマンド確定。0=レジスタホールド, 1=シフト, 2=時刻セット, 3=時刻読出し, 7=テスト（Q88 は 7 で 4990A 形式のシリアルコマンドも解釈）。
- 3（時刻読出し）→ 1（シフト）の後、40h bit2 のパルス毎に 1bit シフト。データは IN 40h bit4。順序は LSB 先頭で 秒(8) 分(8) 時(8) 日(8) 曜日(4) 月(4) の 40bit（すべて BCD）[MAME rtc_clock_updated: m_time_counter[0..4]]。Q88 は年(8)も持つ 48bit。
- **相違（エッジ）**: MAME は STB/CLK の立上り、Q88 は立下りで動作 [Q88 out_ctrl_signal trg_off]。BIOS はパルスで叩くのでどちらでも動く（推定）。

#### ポート 52h（背景色）**相違**
| 実装 | 背景色 | ボーダー |
|---|---|---|
| MAME [bgpal_w] | bit6=R, bit5=G, bit4=B | bit2=R, bit1=G, bit0=B（後期機種は配線なしとのコメント） |
| Q88 [OUT 52h] | bit0=B, bit1=R, bit2=G | なし |
| M88 [Screen::Out52] | bit3=B, bit4=R, bit5=G（アナログパレット時は無視） | なし |

背景色が効くのは白黒グラフィックモードのみ [MAME screen_update "BG Pal applies to 1bpp mode only"][Q88 screen_get_emu_palette]。アナログパレット時は OUT 54h（bit7=1）で背景色を設定 [M88 Out54][Q88 OUT 54h]。

### 3.2 DIP スイッチ（IN 30h / IN 31h）

IN 30h（DIP SW1）[MAME INPUT_PORTS(pc8801) "DSW1", pc8801mk2sr で bit6 追加][M88 M88.rc DIPSW ラベル, cfgpage.cpp（bit=1 が右側ラベル）]

| bit | 1 | 0 | MAME 既定 |
|---|---|---|---|
| 0 | N88-BASIC | N-BASIC | 1 |
| 1 | BASIC モード（端末モード OFF） | 端末モード | 1 |
| 2 | 40 桁 | 80 桁 | 0 |
| 3 | 20 行 | 25 行 | 0 |
| 4 | S パラメータ無効 | 有効 | 1 |
| 5 | DEL コード無視 | 処理する | 0 |
| 6 | （mkIISR）メモリウェイト OFF | ON | 1 |
| 7 | CMD SING 有効（"Disable CMD SING"=Off） | 無効 | 1 |

IN 31h（DIP SW2）[MAME "DSW2" + pc8801mk2sr の bit6/7]

| bit | 1 | 0 | MAME 既定 |
|---|---|---|---|
| 0 | パリティなし | パリティあり | 1 |
| 1 | 奇数 | 偶数 | 0 |
| 2 | 7 ビット | 8 ビット | 0 |
| 3 | ストップ 1 ビット | 2 ビット | 1 |
| 4 | X パラメータ無効 | 有効 | 1 |
| 5 | 半二重 | 全二重 | 1 |
| 6 | 高速モード（V1H/V2） | 標準（V1S） | 1 |
| 7 | V1 | V2 | 0 |

**N88-BASIC V2・80桁×25行・FD ブートの推奨値**: IN 30h = **D3h**（MAME 既定値そのもの）、IN 31h = **79h**（MAME/Q88/M88 で一致: Q88 `0x39 | SW_H`, M88 `((dipsw>>5)&0x3f) | 0x40`）、IN 40h bit3 = 0、bit1 = 0（24kHz）。
- 参考: Q88 既定は IN 30h = DBh（`dipsw_1 | 0xC0`, 20 行設定）[Q88 initval.h DEFAULT_DIPSW 0x391a, pc88main_init]、M88 既定は CBh（20 行）[M88 base.cpp sw30, cfgpage.cpp dipsw=1829]。色（カラー/白黒）は DIP ではなく BIOS が OUT 30h bit1 で決める。
- モード別 IN 31h（SW2 下位 6bit = 39h のとき, 計算）: V1S = B9h, V1H = F9h, V2 = 79h [M88 Base::Reset `sw31 = ((dipsw>>5)&0x3f) | (H ? 0x40 : 0) | (V2 ? 0 : 0x80)`][Q88 pc88main_init switch(boot_basic)]。N-BASIC は IN 30h bit0 = 0。

### 3.3 キーマトリクス（IN 00h–0Eh, 0 = 押下）

[MAME pc88_kbd.cpp INPUT_PORTS(pc8001_kbd)+(pc8801_kbd), read_direct = `port ^ 0xff`][Q88 keyboard.c keyport[] と一致を確認]

| ポート | bit0 | bit1 | bit2 | bit3 | bit4 | bit5 | bit6 | bit7 |
|---|---|---|---|---|---|---|---|---|
| 00h | テンキー0 | テンキー1 | テンキー2 | テンキー3 | テンキー4 | テンキー5 | テンキー6 | テンキー7 |
| 01h | テンキー8 | テンキー9 | テンキー* | テンキー+ | テンキー= | テンキー, | テンキー. | RETURN |
| 02h | @ | A | B | C | D | E | F | G |
| 03h | H | I | J | K | L | M | N | O |
| 04h | P | Q | R | S | T | U | V | W |
| 05h | X | Y | Z | [ | ¥ | ] | ^ | - |
| 06h | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| 07h | 8 | 9 | : | ; | , | . | / | _ |
| 08h | CLR/HOME | ↑ | → | INS/DEL | GRPH | カナ | SHIFT | CTRL |
| 09h | STOP | f･1 | f･2 | f･3 | f･4 | f･5 | SPACE | ESC |
| 0Ah | TAB | ↓ | ← | HELP | COPY | テンキー- | テンキー/ | CAPS |
| 0Bh | ROLL UP | ROLL DOWN | – | – | – | – | – | – |
| 0Ch–0Eh | FH 以降のみ（F6–F10, BS/INS/DEL, 変換, 決定, PC, 全角, 左右RETURN/SHIFT）。mkIISR は FFh | | | | | | | |

- シフト記号: 1! 2" 3# 4$ 5% 6& 7' 8( 9) :* ;+ ,< .> /? -= [{ ]} ¥| [MAME PORT_CHAR]。
- カナ・CAPS はロック式キー（MAME は PORT_TOGGLE）[MAME KEY8 bit5, KEYA bit7]。
- Q88 には「複数キー同時押しで他行に回り込む」キーボードバグの再現オプションがある [Q88 main_io_in USE_KEYBOARD_BUG]。通常は不要。

---

## 4. 割込み

### 4.1 要因・レベル・ベクタ
Z80 は IM2。割込み応答で i8214（μPD8214）がベクタ下位バイト = **レベル × 2** を出す。分岐先 = mem[(I << 8) | (レベル×2)] [MAME pc8001.cpp int_ack_cb `return (7 - level) * 2`（picu 側レベルを反転）][M88 INTC::IntAck `return i * 2`][Q88 z80.c IM2 `(I << 8) | (level << 1)`]。

| レベル | ベクタ | 要因 | 発生条件 | 出典 |
|---|---|---|---|---|
| 0 | 00h | RXRDY（i8251 受信） | RxRDY 立上り | [MAME rxrdy_irq_w] |
| 1 | 02h | VRTC | 垂直帰線開始（VRTC 0→1）。24kHz/25 行ではラスタ 400 の時点 | [MAME vrtc_irq_w(state=1)][M88 Base::VRTC(en) → pint1][Q88 intr.c vsync] |
| 2 | 04h | CLOCK（1/600 秒） | 600Hz 固定周期 | [MAME TIMER rtc_timer 600Hz][M88 base.cpp AddEvent(167)=1.67ms][Q88 RTC_FREQ_HZ 600] |
| 3 | 06h | INT3（拡張スロット） | – | [MAME enum INT3_IRQ_LEVEL] |
| 4 | 08h | INT4: OPN（YM2203 タイマ） | YM2203 IRQ 出力。32h bit7=1 でマスク | [MAME pc8801mk2sr(): `m_opn->irq_handler().set(int4_irq_w)`] |
| 5 | 0Ah | INT5 | – | |
| 6, 7 | 0Ch, 0Eh | FDCINT1/2（DMA 型 FD I/F 用。PC-80S31 では未使用） | – | [MAME enum] |

- 優先順位: レベル番号が小さいほど高い [MAME i8214 check_interrupt（picu レベル 7 = PC-88 レベル 0 から走査）][M88 IntAck bit0 から][Q88 main_INT_chk if 連鎖]。
- uPD3301 の INT は未接続 [MAME pc8801() コメント]。

### 4.2 E4h（割込みレベル）/ E6h（マスク）
```c
// OUT E4h: 受付可能レベル設定 + 応答後の再許可
//   bit3=1 → 全レベル許可、bit3=0 → レベル L < (data & 7) のみ許可
//   [MAME irq_level_w → i8214::b_sgs_w(~data): sgs=!bit3, current=7-(data&7), level>current で発火]
//   [M88 INTC::SetRegister: mask = ~(-1 << min(8, data))]
//   [Q88 OUT E4h: bit3 ? intr_level=7 : intr_level=data&7、受付は no < intr_level]
// OUT E6h: bit0 = CLOCK(レベル2)許可, bit1 = VRTC(レベル1)許可, bit2 = RXRDY(レベル0)許可（1=許可）
//   [MAME irq_mask_w bitswap<3>(data&7,0,1,2)][M88 SetMask table][Q88 INTERRUPT_MASK_RTC=1,VSYNC=2,SIO=4]
//   レベル4(OPN)は E6h ではなく 32h bit7 でマスク（1=マスク）[MAME misc_ctrl_w][M88 SetIMask(0x32,0x80)][Q88 OUT 32h]
```
推奨実装（3 実装の共通部分）:
```c
uint8_t req;           // bit L = レベルL要求
uint8_t level_limit;   // E4h: bit3 ? 8 : (data & 7)
bool    can_int;       // 応答後 false、E4h 書込みで true

void request(int L) { if (enabled(L)) req |= 1 << L; }   // E6h/32h で無効なら捨てる（M88/Q88）
int  pending() {                                          // CPU が IFF1=1 のとき毎命令チェック
    if (!can_int) return -1;
    for (L = 0; L < level_limit; L++) if (req & (1 << L)) return L;
    return -1;
}
void ack(int L) { req &= ~(1 << L); can_int = false; IntZ80(cpu, L * 2); }  // IM2 ベクタ = L*2
void out_e4(uint8_t d) { level_limit = (d & 8) ? 8 : (d & 7); can_int = true; }
void out_e6(uint8_t d) { /* 許可ビット更新。無効にしたレベルの req はクリア（M88: irq &= mask2） */ }
```
- 応答後は E4h を書き直すまで次の割込みは入らない [M88 IntAck `stat.mask = 0`][Q88 main_INT_chk `intr_level = 0`][MAME i8214 trigger_interrupt `m_int_dis = 1`、b_sgs_w で解除]。BIOS/ゲームの割込みルーチンは最後に E4h を書く前提（推定）。
- **相違（マスク中の要求）**: MAME は E6h で無効中の要求を pending として保持し、許可時に発火 [MAME assert_irq/check_irq]。M88/Q88 は捨てる [M88 Request `bit &= mask2`][Q88 intr.c `if (intr_vsync_enable) VSYNC_flag = TRUE`]。MAME のコメントでは megamit/babylon が「無効にした VRTC を受けると壊れる」。
- **相違（E4h bit3）**: Q88 は bit3=1 をレベル 7（0–6 許可）、M88 は 8（全許可）、MAME は比較なし（全許可）。
- **相違（OPN 割込み）**: MAME はレベル（YM2203 IRQ 線そのまま）、M88/Q88 は立上りエッジで要求 [M88 OPNUnit::Intr][Q88 intr.c SOUND_level 立上り]。
- リセット時: 全マスク 0、レベル要求なし [MAME picu_reset][M88 INTC::Reset, pc88.cpp Reset `Out(0xe6, 0)`][Q88 pc88main_init intr_*_enable=0]。
- N88-BASIC の I レジスタ値（割込みテーブル位置）はソースからは確認できず（未確認）。

---

## 5. テキスト表示

### 5.1 データの流れ（DMAC ch2 → uPD3301）
- uPD3301 の DRQ は i8257 の ch2 に接続、ch2 の I/O 書込み（DACK）で CRTC の FIFO に入る [MAME pc8801() `drq_wr_callback → dreq2_w`, `out_iow_cb<2> → upd3301::dack_w`]。ch2 がベリファイモードでも CRTC にデータを渡す [MAME pc8801_state::dackv]。M88 は書込みモード（count 上位 2bit = 01）だと転送しない [M88 pd8257.cpp RequestRead `!(mode & 0x40)`]。
- プログラム: OUT 64h/64h = 先頭アドレス（下位→上位）、OUT 65h/65h = (バイト数-1) 下位→上位、上位バイトの bit7-6 = モード（10 = メモリ読出し）。OUT 68h bit2 = ch2 許可、bit7 = オートロード（TC で ch3 の値を ch2 に再ロード）[MAME i8257.cpp write/advance]。
- **相違**: 68h 書込みで上位/下位フリップフロップをリセットするのは M88/Q88 [M88 PD8257::SetMode `stat.ff=false`][Q88 dmac_out_mode]。MAME はリセットしない。
- 読み出し元: V1H/V2（高速モード）は TVRAM（F000h–FFFFh の 4KB）、V1S はメイン RAM 全域 [M88 pc88.cpp Reset: `if (isv2) dmac->ConnectRd(tvram, 0xf000, 0x1000) else ConnectRd(ram, 0, 0x10000)`。isv2 = IN 31h bit6]。TVRAM 範囲外を指すと FFh。Q88 の初期 DMA アドレスは F3C8h [Q88 dmac_init]（BIOS が設定し直す）。
- 転送タイミング: MAME は VRTC 終了（表示開始）で DRQ を上げ、全行をまとめて転送（その間 CPU は HALT 扱い）[MAME upd3301 vrtc_update, pc8001 hrq_w]。M88 は 1 文字行ごとに 1 行分を転送 [M88 CRTC::ExpandLine/ExpandLineSub]。スキャンライン方式なら「各文字行の先頭ラスタで 1 行分（H + 2×属性数 バイト）を ch2 から読む」M88 方式が自然。カウント不足は DMA アンダーラン → 表示停止・ステータス bit3 [M88 ExpandLineSub]。
- 1 行の形式（トランスペアレント型）: 文字コード H バイト、続けて属性ペア（桁, 値）× 属性数 [MAME upd3301 dack_w: 先に m_h 文字, 次に m_attr*2 バイト][M88 linesize = width + attrperline*2]。標準は 80 + 20×2 = 120 バイト/行、25 行で 3000 バイト（計算）。
- ノントランスペアレント型（P4 bit7=1）: 文字と属性が 1 バイトおきに並ぶ（1 属性が 2 文字に効く）[Q88 crtc_make_text_attr crtc_attr_non_separate]。

### 5.2 uPD3301 コマンド（OUT 51h）とパラメータ（OUT 50h）
[MAME upd3301.cpp write][M88 crtc.cpp Command][Q88 crtc_out_command/parameter]

| コマンド (bit7-5) | 値 | 内容 |
|---|---|---|
| RESET | 00h | 表示停止（VE=0）、続くパラメータ 5 バイトを受付 |
| START DISPLAY | 20h \| RVV | 表示開始（VE=1）。bit0 = 画面全体反転 |
| SET INTERRUPT MASK | 40h \| MN<<1 \| ME | ME=0 にするとステータス全クリア（VE も落ちて表示消える）[MAME][M88 `if(!(data&1)) status=0`]。BIOS は 43h を使う想定（Q88 は mask==3 でないとテキスト非表示 [Q88 set_text_display]） |
| READ LIGHT PEN | 60h | 50h から 2 バイト読める（未使用） |
| LOAD CURSOR POSITION | 80h \| CM | CM=1 カーソル表示。続く 50h に X（桁）, Y（行） |
| RESET INTERRUPT | A0h | E/N クリア |
| RESET COUNTERS | C0h | |

RESET のパラメータ:

| # | bit | 意味 |
|---|---|---|
| P0 | 7 | DMA モード 1=キャラクタ / 0=バースト（MAME。M88 は「???」） |
| | 6-0 | H-2（1 行の文字数。80 → 4Eh） |
| P1 | 7-6 | B: 点滅速度。MAME: カーソル (B+1)×16 フレーム毎に反転、属性ブリンクはその 2 倍 |
| | 5-0 | L-1（表示行数。25 → 18h） |
| P2 | 7 | S: 1 行おき表示（skip line） |
| | 6-5 | カーソル形状 bit5 = 点滅, bit6 = ブロック（0 = アンダーライン）|
| | 4-0 | R-1（1 文字のラスタ数。24kHz/25行 = 16 → 0Fh） |
| P3 | 7-5 | V-1（垂直帰線の文字行数） |
| | 4-0 | Z-2（水平帰線の文字数） |
| P4 | 7-5 | 属性モード: 000 トランスペアレント白黒(特殊制御文字あり), 001 属性なし, 010 トランスペアレントカラー, 100 ノントランスペアレント白黒, 101 同(特殊制御なし) |
| | 4-0 | 1 行の属性数-1（最大 20）。001 のときは 0 |

- 既定例（Q88 crtc_init）: `RESET; CE 98 6F 58 53` = H80, L25, R16, V3, Z26, カラー, 属性20 + `LOAD CURSOR 0,0`。
- ステータス（IN 51h）: bit4 VE, bit3 U(DMA アンダーラン), bit2 N, bit1 E, bit0 LP。MAME は読むと LP/E/N/U をクリア、RESET/START で bit7 を立てる [MAME read]。
- テキスト表示条件: CRTC が START 済み（VE=1）かつ DMA が転送できていて、53h bit0 = 0 [MAME screen_update `if (!m_text_layer_mask)`, upd3301 screen_update `get_display_status`][Q88 set_text_display `(dmac_mode & 4) && crtc_active && crtc_intr_mask==3`]。N-BASIC は 53h を触らないので 53h の既定はテキスト ON [MAME machine_reset コメント]。
- uPD3301 の INT 出力は PC-88 では未接続 [MAME pc8801() コメント]。VRTC 出力だけ使う（§4）。
- 特殊制御文字（属性モードに SC あり）: 行末の最後の属性ペアの桁バイトが (x & 7Fh) == 60h のとき、値 bit0=1 → 以降の行を空白、bit1=1 → 以降表示抑止 [M88 ExpandLineSub]。MAME は未対応（TODO）。

### 5.3 属性の展開（1 行 → 80 桁分）

```c
// [MAME upd3301.cpp default_attr_fetch] 標準ケース（最初のペアの桁が 0）
// a[] = 行末の属性ペア列 {c0,v0,c1,v1,...}, n = ペア数
uint8_t at[80] = {0};
if (mode == ATTR_NONE) return at;            // 属性なしモード
for (i = 0; i < n; i++) {
    int s = min(c[i], 80), e = (i+1 < n) ? min(c[i+1], 80) : 80;
    if (e == 0) e = 80;                       // 終端桁 0 は行末扱い
    for (x = s; x < e; x++) at[x] = v[i];
    if (e == 80) break;
}
```
- **相違（最初の桁 ≠ 0 のとき）**:
  - MAME: 0..c0-1 を v0 で埋め、以後 v(i+1) を c(i)..c(i+1)-1 に適用（1 つずれる）。N88-BASIC の最下行・jettermi 等のためと注記 [MAME default_attr_fetch]。
  - M88: 桁リストと値リストは独立とみなし、桁リストに印のある桁に来るたびに値を順に 1 つ消費。印より前の桁は前の行から持ち越した属性 [M88 ExpandAttributes コメント「実は桁と属性は独立している模様」]。
  - Q88: 桁 0x80 や重複を特別扱いする独自処理 [Q88 crtc_make_text_attr]。
- 3 実装共通: 属性状態（色・装飾）は**前の行から持ち越し**、フレーム先頭では白・装飾なし [MAME pc8001 attr_fetch `if (y == 0) { m_attr_color = 0xe8; m_attr_decoration = 0; }`][M88 Command RESET `attr = 7<<5`][Q88 global_attr 初期値 G|R|B]。

カラーモード（P4 = 010）での 1 バイト属性の解釈 [MAME pc8001_base_state::attr_fetch / draw_text][M88 ChangeAttr][Q88 COLOR_*/MONO_*]:

```c
// 列ごとに順に処理（色と装飾は別々にラッチ）
if (v & 0x08) color = v;      // 色指定: bit7=G bit6=R bit5=B, bit4=セミグラフィック
else          deco  = v;      // 装飾:   bit5=アンダーライン bit4=アッパーライン bit2=反転 bit1=ブリンク bit0=シークレット
// 文字色番号 = color >> 5  (bit0=B,bit1=R,bit2=G → MAME crtc_palette BRG_3BIT)
```
白黒モード（P4 = 000/100/101）: 色は常に白、装飾は同じビット、bit7 = セミグラフィック [MAME draw_text `semigfx_tile = BIT(attr, 7)`][M88 ChangeAttr mono][Q88 MONO_GRAPH 0x80]。

### 5.4 1 文字の描画（MAME draw_text を要約）

```c
// cc=文字コード, lc=文字内ラスタ(0..R-1), ydbl = 24kHz ? 2 : 1
if (!width80 && (sx & 1)) return;                  // 40桁: 奇数桁は描かない（偶数桁を倍幅）
if (semigfx) tile = cc;
else tile = (lc < 8*ydbl) ? cg[cc*8 + (lc >> (ydbl-1))] : 0;   // 8x8 フォント、余りラスタは空白
if (secret) tile = 0;
if (cursor_here && cursor_phase_on) tile ^= 0xff;   // （MAME はブロックカーソルのみ）
else if (blink && blink_phase_off) tile = 0;
if (lc == 0 && upperline) tile = 0xff;              // 上線・下線はシークレット/ブリンクの影響を受けない
if (lc == R-1 && underline) tile = 0xff;
if (reverse ^ RVV) tile ^= 0xff;
// 画素: 通常 bit7 が左端。セミグラフィックは 2x4 ブロック:
//   行 r = (lc の上位) 0..3, 左半分 = bit r, 右半分 = bit (r+4)
//   [M88 CreateGFont order {01,10,02,20,04,40,08,80}][MAME draw_text mask計算]
```
- 文字高: 24kHz（R=16, 25 行）はフォント 8 ライン ×2 倍、R=20（20 行）は 16 ライン + 空白 4。15kHz は R=8（25 行）/R=10（20 行, 下 2 ライン空白）[MAME draw_text][Q88 crtc_font_height 8/10][M88 linecharlimit = min(R,16)]。
- M88 は下線を R > 14 のときだけ描く [M88 PutLineNormal]。
- 40 桁: OUT 30h bit0 = 0 で偶数桁だけを 16 ドット幅で表示（CRTC は 80 桁のまま）[MAME draw_text][M88 SetTextSize/PutCharW]。
- フォント（CG ROM）: KANJI1.ROM のオフセット 1000h–17FFh（2KB, 256 文字 × 8 バイト, 1 バイト = 1 ラスタ, bit7 = 左）[MAME ROM_START(pc8801mk2sr) `ROM_COPY("kanji", 0x1000, 0x0000, 0x800)`][M88 LoadFontFile `Seek(0x1000)`][Q88 memory.c `kanji_rom[0][(1<<11)]`]。FONT.ROM（2KB）があればそちらを優先（Q88/M88）。Q88 は文字コード 0 を常に空白にしている [Q88 memory.c]。
- 全画面反転: START DISPLAY bit0 [MAME crtc_reverse_w]。Q88 はカラーグラフィック時のみ反映 [Q88 crtc_make_text_attr]。
- 1 行おき表示（P2 bit7）: MAME は次の行位置を R<<1 進める、Q88 は次の行をシークレットで埋める。

### 5.5 グラフィックとの合成
- 順序: グラフィック → テキストを上に重ねる [MAME screen_update: bitmap → copybitmap_trans(text)]。
- テキスト色はカラーグラフィックモードではパレットの影響を受けない固定デジタル 8 色 [Q88 screen_get_emu_palette pal[8..15]][MAME m_crtc_palette BRG_3BIT]。白黒グラフィックモード + アナログパレット時は 54h–5Bh のパレットで色が付く [Q88 同関数][M88 UpdatePalette `tpal = port32 & 0x20 ? xpal : palcolor`]。
- **相違（黒い文字）**: MAME は文字色 0（黒）を透明扱い（copybitmap_trans の透過色 0）。Q88/M88 は黒文字も不透明 [Q88 get_font_gryph color |= 8 → pal[8]=黒][M88 UpdatePalette `*p++ = palcolor[0]`]。
- **相違（OUT 30h bit1 /COLOR）**: M88 は bit1=1 でテキスト色を白に固定 [M88 UpdatePalette `textcolor = port30 & 2 ? 7 : 0`]。Q88 は無視（色は CRTC 設定で決まる）[Q88 OUT 30h コメント]。MAME も描画では使わない。

---

## 6. グラフィック

### 6.1 GVRAM 配置
- 3 プレーン × 16KB（B=0, R=1, G=2）。アドレス = y×80 + x/8、bit7 が左端 [MAME draw_bitmap `bitmap_offset = (y >> y_double)*80 + x_char`, dot_func(…, 7 - xi)]。640×200 で 16000 バイト（0000h–3E7Fh）、3E80h–3FFFh は表示に使われない（MAME コメント）。
- 320×200 モードは PC-8801 には無い（M88 の 320×200 は PC-8001mkIISR(N80V2) 専用 [M88 screen.cpp UpdateScreen `if (!n80mode)` 分岐]、MAME pc8801 にも無い）。

### 6.2 表示モード（OUT 31h）
[MAME pc8801_state::screen_update / draw_bitmap][Q88 screen.c vram2screen][M88 Screen::Out31]

| 31h bit3 GRPH | bit4 HCOLOR | bit0 200LINE | モード | 画素値 |
|---|---|---|---|---|
| 0 | x | x | グラフィック非表示 | 黒（MAME `bitmap.fill(0)`） |
| 1 | 1 | x | 640×200 8 色 | idx = B \| R<<1 \| G<<2 → パレット[idx]。0 も パレット[0]。**53h のプレーンマスクは効かない**（MAME "bugattac relies on this"） |
| 1 | 0 | 1 | 640×200 白黒 | dot = (B & !mask53.b) \| (R & !mask53.r) \| (G & !mask53.g)。dot=1 の色 = その文字セルのテキスト属性色（CRTC がカラー属性モードのとき）、それ以外は白。dot=0 は背景色（52h / アナログ時 54h bit7） |
| 1 | 0 | 0 | 640×400 白黒（24kHz 専用） | ラスタ 0–199 は B プレーン、200–399 は R プレーン（G 不使用）。色は白黒 200 と同じ規則 |

- 24kHz モニタで 200 ライン系モードは各ラインを 2 回表示 [MAME draw_bitmap `y_double = get_screen_frequency(); if ((m_gfx_ctrl & 0x11) == 0) y_double = 0`]。15kHz で 400 ラインモードは不可（MAME TODO コメント, 上半分のみ）。
- 白黒モードで文字属性色をグラフィックに使うため、MAME はラスタ毎の属性色テーブル m_attr_info を保持 [MAME pc8801_state::attr_fetch]。Q88 も `get_pixel_mono(vram, tcol)`（tcol = テキスト色）[Q88 screen-vram-200.h MONO]。
- 53h による非表示: bit1=B, bit2=R, bit3=G（白黒 200 ラインでのみ効果）[MAME layer_masking_w "it's the only place where I/O $53 bits 1-3 have an actual effect"]。M88 も白黒時のみ 53h bit1-3 を参照 [M88 UpdateScreen200b mask]。

### 6.3 パレット（OUT 54h–5Bh = パレット 0–7）
```c
// [MAME pc8801_state::palram_w][Q88 OUT 54h-5Bh][M88 Out54/Out55to5b]
if (!(p32 & 0x20)) {                 // デジタル
    pal[n].b = (d & 1) ? 7 : 0;  pal[n].r = (d & 2) ? 7 : 0;  pal[n].g = (d & 4) ? 7 : 0;
} else if (n == 0 && (d & 0x80)) {   // アナログ & 54h bit7=1 → 背景色 [Q88][M88]（MAME 未実装）
    if (d & 0x40) bg.g = d & 7; else { bg.b = d & 7; bg.r = (d >> 3) & 7; }
} else {                              // アナログ
    if (d & 0x40) pal[n].g = d & 7;              // bit6=1: G (bit2-0)
    else { pal[n].b = d & 7; pal[n].r = (d >> 3) & 7; }   // bit6=0: B (bit2-0), R (bit5-3)
}
```
- 3bit→8bit: MAME pal3bit（×255/7 相当）、Q88 は `x*73/2`。リセット時は pal[i] = (B=i&1, R=i&2, G=i&4 を 0/7) [MAME palette_reset][Q88 pc88main_init][M88 Screen::Init]。
- デジタル⇔アナログ切替時に内容をどう扱うかは不明（MAME TODO）。
- ラスタ単位でパレット変更する技があるため、MAME は書込み時に `update_partial(vpos)` する（アナログ, 24kHz/80桁で確認との注記）[MAME palram_w]。スキャンライン描画なら行単位反映で対応可能。
- 「後期機種で赤が出ない」ソフトは V1 モードを要求するもの（BIOS がアナログパレットにしたまま戻さない）[MAME pc8801.cpp ヘッダ Notes]。mkIISR でも V2 起動時は同じ注意が必要（推定）。

### 6.4 ALU（V2 拡張アクセスモード: 32h bit6=1 かつ 35h bit7=1）
[MAME pc88_alu.cpp alu_r/alu_w][M88 memory.cpp RdALU/WrALUSet/WrALURGB/WrALUB/WrALUR][Q88 ALU_read/ALU_write] — 3 実装で一致

```c
uint8_t latch[3];                       // B,R,G
uint8_t alu_read(uint16_t off) {        // off = addr & 0x3fff
    for (i = 0; i < 3; i++) latch[i] = gvram[i][off];      // 読むたびにラッチ
    uint8_t b = latch[0], r = latch[1], g = latch[2];
    if (!(p35 & 1)) b = ~b;  if (!(p35 & 2)) r = ~r;  if (!(p35 & 4)) g = ~g;
    return b & r & g;                    // 比較色(35h bit2-0)と一致する画素が 1
}
void alu_write(uint16_t off, uint8_t d) {
    switch (p35 & 0x30) {
    case 0x00:                           // 論理演算（プレーン毎に 34h の bit i と bit i+4）
        for (i = 0; i < 3; i++) switch ((p34 >> i) & 0x11) {
            case 0x00: gvram[i][off] &= ~d; break;   // リセット
            case 0x01: gvram[i][off] |=  d; break;   // セット
            case 0x10: gvram[i][off] ^=  d; break;   // 反転
            case 0x11: break;                         // 何もしない
        } break;
    case 0x10: for (i = 0; i < 3; i++) gvram[i][off] = latch[i]; break;  // ラッチを 3 プレーンへ（VRAM 間転送）
    case 0x20: gvram[0][off] = latch[1]; break;   // R ラッチ → B プレーン
    case 0x30: gvram[1][off] = latch[0]; break;   // B ラッチ → R プレーン
    }
}
```
- 書込みデータ d はモード 0 以外では無視。
- ALU モード（32h bit6=1）中は 5Ch–5Fh の独立選択は無効（vram_sel=3）[MAME flush_gvram_access "ALU enabled wins over GVRAM"][Q88 main_memory_vram_mapping][M88 UpdateC0]。35h bit7=0 なら C000h–FFFFh はメイン RAM（TVRAM 規則は §2.5）。
- V1 モードの BASIC/ソフトは 5Ch–5Eh で 1 プレーンずつ書く（独立アクセス）。

---

## 7. サウンド

### 7.1 YM2203（OPN）
| 項目 | 内容 | 出典 |
|---|---|---|
| ポート | 44h W = レジスタ番号 / R = ステータス、45h W = データ / R = レジスタ読出し | [MAME pc8801mk2sr_state::main_io map(0x44,0x45)][M88 c_opn1][Q88 OUT/IN 44h/45h] |
| クロック | 3.9936MHz | §1.1 |
| 割込み | IRQ → レベル 4（ベクタ 08h）。32h bit7=1 でマスク。マスク解除時に保留中なら即要求 | [MAME int4_irq_w, misc_ctrl_w `if (m_sound_irq_enable) int4_irq_w(m_sound_irq_pending)`] |
| 46h/47h | OPNA 拡張部（FH 以降）。mkIISR は無し | [Q88 bootup_work_init] |
| A8h–ADh | サウンドボード（オプション, 割込みマスクは AAh bit7） | [MAME main_io コメント][M88 c_opn2 SetIMask(0xaa,0x80)][Q88 OUT AAh] |

- タイマ周期（YM2203 標準式, Q88 の実装と一致）: Timer A = 72 × (1024 − NA) / fM、Timer B = 1152 × (256 − NB) / fM [Q88 intr.c `TIMER_A_CONST(12) * prescaler * (1024 - TA)`, `TIMER_B_CONST(192) * prescaler * (256 - TB)`, prescaler=6 のとき]。
- M88 は OPNA コアで OPN を模擬するため、OPN モードではレジスタ 10h–1Fh への書込み無視、29h を 3 固定等の補正をしている [M88 OPNIF::WriteData0]。YM2203 専用コアなら不要。
- リセット時プリスケーラ 2Dh [M88 OPNIF::Reset `prescaler = 0x2d`]。

### 7.2 SSG I/O ポート（ジョイスティック / マウス）
- レジスタ 0Eh（ポート A）読出し: bit0 上, bit1 下, bit2 左, bit3 右（0=押下）、bit7-4 = 1 [MAME opn_porta_r `BIT(m_mouse_port->read(), 0, 4) | 0xf0`][M88 JoyPad::Update `~direction | 0xf0`]。
- レジスタ 0Fh（ポート B）読出し: bit0 トリガ A, bit1 トリガ B（0=押下）、bit7-2 = 1 [MAME opn_portb_r][M88 JoyPad `| 0xfc`]。
- ポート B 書込み: bit0/bit1 → コネクタ 6/7 ピン（MAME）。マウスは OUT 40h bit6（JOP1 ストローブ）でニブル読み [MAME port40_w pin_8_w][Q88 keyboard_jop1_strobe]。MSX 互換 9 ピンポート（前面「マウス」）[MAME pc8801.h コメント]。
- M88 は 0Eh/0Fh 読出しを外部ポートに委譲 [M88 ReadData0 `(index0 & 0xfe) == 0x0e`]。

### 7.3 BEEP / SING（OUT 40h）
- bit5 = BEEP: 1 の間 2400Hz 矩形波 [MAME port40_w（立上りで ON, 立下りで OFF）, BEEP 2400Hz][M88 beep.cpp `bperiod = 2400.0/rate`]。
- bit7 = SING: スピーカを直接駆動する 1bit 出力（CMD SING 等）[M88 Beep::Mix で bit7 をレベルとして加算][Q88 xmame_dev_beep_out_data(data) に bit7/bit5 変化を通知]。MAME は bit7=1 で BEEP を止めるだけ（TODO 扱い）。
- DIP SW1 bit7 で CMD SING の可否（§3.2）。

---

## 8. ディスクサブシステム（PC-80S31）

MAME では mkIISR は `PC80S31`（2D, DISK.ROM 2KB）、mkIIMR 以降が `PC80S31K`（8KB ROM, 2HD 対応）[MAME pc8801.cpp pc8801(): `PC80S31(config, m_pc80s31, ...)`, pc8801mk2mr(): `PC80S31K(config.replace(), ...)`]。

### 8.1 サブ CPU
| 項目 | 値 | 出典 |
|---|---|---|
| CPU | Z80, 8MHz/2 = 4MHz（MAME も "TODO: confirm clock arrangement"） | [MAME pc80s31k.cpp device_add_mconfig] |
| FDC | uPD765A, クロック 8MHz, ready ポーリング有効, データレート 250kbps, 300rpm | [同: `UPD765A(config, m_fdc, fdc_xtal, true, true)`, device_reset set_rate(250000), set_rpm(300)] |
| ROM | 0000h–1FFFh 領域に DISK.ROM（2KB, 残りは FFh） | [MAME fdc_map, ROM_REGION 0x2000 ERASEFF] |
| RAM | 4000h–7FFFh（16KB） | [MAME fdc_map] |
| その他 | MAME: 未マップ。Q88: `addr & 0x7fff` でミラー、2KB ROM を 0000h–3FFFh に 8 回ミラー | [Q88 pc88sub.c sub_mem_read, memory.c memcpy ミラー] |

サブ側 I/O [MAME pc80s31_device::fdc_io][Q88 sub_io_in/out][M88 pc88.cpp c_fdc]:

| ポート | R | W |
|---|---|---|
| F0h | – | （PC-80S31K のみ）割込みベクタ。mkIISR では無し |
| F4h | – | （K のみ）ドライブモード 2D/2DD/2HD |
| F7h | – | プリンタ出力（無視可） |
| F8h | **TC パルス**（読むと FDC に TC。MAME は 50µs 後に解除。値は無意味） | モータ制御 bit0 = ドライブ0, bit1 = ドライブ1（1=ON）[MAME motor_control_w] |
| FAh | uPD765 MSR | – |
| FBh | uPD765 データ | uPD765 データ |
| FCh–FFh | サブ側 8255（PA/PB/PC/制御） | 同左 |

### 8.2 メイン ⇔ サブ 8255 のたすき掛け
[MAME pc80s31k.cpp device_add_mconfig（6 個の generic_latch）, latch_r/latch_w][M88 pio.h Read0/Read1/Read2][Q88 pio.c 冒頭の図]

| 書く側 | 読む側 | 備考 |
|---|---|---|
| メイン PB（OUT FDh） | サブ PA（IN FCh） | コマンド・パラメータ・書込みデータ |
| サブ PB（OUT FDh） | メイン PA（IN FCh） | 読出しデータ・ステータス |
| メイン PC 上位 4bit（bit7-4） | サブ PC 下位 4bit（bit3-0） | ニブル入替え: メイン bit4→サブ bit0 … bit7→bit3 |
| サブ PC 上位 4bit | メイン PC 下位 4bit | 同上 |

- 8255 の設定は両側とも「PA 入力, PB 出力, PC 上位出力, PC 下位入力」（モード 0, 制御語 91h 相当）[Q88 pio_init（初期値）, M88 PIO SetCW readmask]。PC 上位の読み返しは自分の出力ラッチ [MAME i8255 read_pc]。ビットセット/リセット（OUT FFh, bit7=0）も PC 出力として相手に見える [MAME i8255 set_pc_bit → output_pc]。
- ハンドシェイク信号（メイン側から見て）[MAME pc80s31k.cpp ヘッダコメント "Port C"]:
  - 出力 bit7 ATN（コマンド送出・中断要求）, bit6 DAC（データ受領）, bit5 RFD（データ要求）, bit4 DAV（PB にデータ有効）
  - 入力 bit2 DAC, bit1 RFD, bit0 DAV（サブ側からの同名信号）
- サブ側コマンド（メイン → サブ, PB 経由）[MAME pc80s31k.cpp ヘッダ "Command Protocol"]:

| コマンド | パラメータ | 内容 |
|---|---|---|
| 00h | – | 初期化 |
| 01h | セクタ数, ドライブ, トラック, セクタ番号（MAME 表記 "sector number +1"） | ディスクへ書込み |
| 02h | 同上 | ディスクから読出し（サブ RAM バッファへ） |
| 03h | – | データをホストへ送信 |
| 04h | セクタ数, 転送元 D/T/S, 転送先 D/T/S | コピー |
| 05h | ドライブ | フォーマット |
| 06h | – | 結果ステータス送信（bit7 I/O 完了, bit6 未読バッファあり, bit0 エラー） |
| 07h | – | ドライブ状態 |
| 0Bh | 先頭アドレス(2), 長さ(2) | サブ側メモリ内容を送信 |
| 11h / 12h | 01h/03h の高速版 | |
| 14h | – | デバイス状態（ST3 相当: bit7 ESIG, bit6 WP, bit5 RDY, bit4 TRK0, bit3 両面, bit2 ヘッド, bit1-0 ドライブ） |
| 17h | モード | モード変更 |

  FDC の ST0–ST2 はサブ RAM 7F0Dh–7F0Fh、CHRN は 7F10h–7F13h に置かれる（同ヘッダ）。
- 実装上の注意（3 実装の工夫）:
  - MAME は perfect quantum（両 CPU を命令単位で同期）を使い、それでも mailbox 化しないと一部ソフトがハングする [MAME pc8801() `set_perfect_quantum("pc80s31:fdc_cpu")`, latch_r コメント]。
  - Q88 は「PC を読んだら相手 CPU に切替え」/「一定ステップ並走」/「常時並走」の 3 方式を持ち、PC を使わず速度同期に依存するソフトがあると明記 [Q88 pio.c 冒頭コメント]。
  - M88 はサブ CPU のモータ回転待ち `CALL 02B4h`（ROM 00FBh, 0105h）を NOP 化して高速化している [M88 subsys.cpp PatchROM]。
  - アイドル時のサブ CPU は PC ポーリングで ATN/DAV 待ち（例: babylon PC=06D8h, prajator PC=7060h, valis2 PC=7009h）[MAME motor_control_w コメント]。

### 8.3 FDC 割込みとサブ CPU
- uPD765 の INT → サブ Z80 の INT [MAME `intrq_wr_callback().set_inputline(m_fdc_cpu, INPUT_LINE_IRQ0)`]。
- 割込み応答時にデータバスへ出る値は **00h**（IM0 なら NOP）[MAME pc80s31_device::irq_cb → m_irq_vector=0 "zero for regular and read only (NOP)"][M88 SubSystem::IntAck `return 0x00`][Q88 sub_INT_chk → 0、z80.c z80_im0_interrupt level 0 = NOP]。
- **実装注意**: 手元の Marat 系 `IntZ80()`（emu\PC88\Z80.c）は IM0 でベクタが INT_RSTxx 以外だと `M_PUSH(PC)` だけして飛ばない（スタックが 1 ワードずれる）。サブ CPU の IM0 割込みは「HALT 解除 + IFF1/IFF2 クリアのみ（NOP 実行相当）」として別処理にする必要がある（Z80.c 636–695 行を確認）。
- ノン DMA モードでは実行フェーズ中 1 バイト毎に INT が立つ（読む/書くと下がる）[MAME check_irq `irq || internal_drq`][M88 FDC::GetData/SetData `Intr(true)`][Q88 fdc.c `fdc_occur_interrupt()` NON_DMA]。結果フェーズ移行時にも INT。TC が来ていない状態で EOT セクタを読み終えると異常終了（ST0=40h, ST1 EN=80h）[MAME read_data_continue][M88 CmdReadData timerphase]。TC（IN F8h）受付後に結果フェーズ。
- MAME の注記: HALT 中に TC を打つのは READ ID 系コピープロテクトの兆候 [MAME tc_zero_tick]。

### 8.4 uPD765A（必要コマンドと結果）
[MAME upd765.cpp check_command/execute_command/read_data_continue/read_id_continue/format_track_continue, upd765.h ST/MSR 定数][M88 fdc.cpp CommandTable]

| コマンド | 第1バイト | バイト数 | 結果フェーズ |
|---|---|---|---|
| SPECIFY | 03h | 3 (03, SRT\|HUT, HLT\|ND) | なし（INT なし） |
| SENSE DRIVE STATUS | 04h | 2 (04, HD\|US) | ST3 |
| RECALIBRATE | 07h | 2 | なし。完了で INT → SENSE INT STATUS 必須 |
| SENSE INTERRUPT STATUS | 08h | 1 | ST0, PCN（要因が無ければ 80h の 1 バイト） |
| SEEK | 0Fh | 3 (0F, HD\|US, NCN) | なし。完了で INT |
| READ DATA | x6h（MT=80h, MF=40h, SK=20h） | 9 (cmd, HD\|US, C, H, R, N, EOT, GPL, DTL) | ST0 ST1 ST2 C H R N |
| READ DELETED DATA | xCh | 9 | 同上 |
| WRITE DATA | x5h | 9 | 同上 |
| WRITE DELETED DATA | x9h | 9 | 同上 |
| READ ID | xAh（MF） | 2 | ST0 ST1 ST2 + 見つけた ID の C H R N |
| FORMAT TRACK | xDh | 6 (cmd, HD\|US, N, SC, GPL, D)、実行中に SC×4 バイト（C,H,R,N） | ST0 ST1 ST2, 0, 0, 0, N（MAME） |
| 不正コマンド | – | 1 | ST0 = 80h |

- ST0: bit7-6 IC（00 正常, 01 異常終了, 10 不正コマンド, 11 Ready 変化）, bit5 SE, bit4 EC, bit3 NR, bit2 HD, bit1-0 US。ST1: bit7 EN, bit5 DE, bit4 OR, bit2 ND, bit1 NW, bit0 MA。ST2: bit6 CM, bit5 DD, bit4 WC, bit3 SH, bit2 SN, bit1 BC, bit0 MD。ST3: bit7 FT, bit6 WP, bit5 RY, bit4 T0, bit3 TS, bit2 HD, bit1-0 US。MSR: bit7 RQM, bit6 DIO, bit5 EXM(ノンDMA実行中), bit4 CB, bit3-0 ドライブ n シーク中 [MAME upd765.h]。
- 読出し結果の R/C 更新: R == EOT で TC 受付 → C+1, R=1（MT 時は H 反転）[MAME read_data_continue][M88 IDIncrement]。
- ノットレディ: ST0 = 48h\|US（NR+異常）[MAME read_data_start]。セクタ無し: ST0=40h, ST1 ND(04h)（ID はあるが一致なし）または MA(01h)。
- Ready ポーリング: 待機中に各ドライブの Ready 変化を検出すると ST0 = C0h\|drv で INT（SENSE INT STATUS で読む）[MAME run_drive_ready_polling]。
- ディスク: 2D = 40 シリンダ × 2 面, MFM, 一般に 16 セクタ × 256 バイト（N=1）。D88 のトラック番号 = C×2 + H [MAME d88_format::load `track_pos[track * head_count + head]`]。

### 8.5 D88 イメージ
[Q88 document/FORMAT.TXT][MAME d88_dsk.cpp]

ヘッダ（2B0h バイト）:
| オフセット | サイズ | 内容 |
|---|---|---|
| 00h | 17 | ディスク名（ASCIIZ） |
| 11h | 9 | 予約 |
| 1Ah | 1 | ライトプロテクト（00h なし, 10h あり） |
| 1Bh | 1 | 種別 00h 2D, 10h 2DD, 20h 2HD（MAME はさらに 30h/40h 片面） |
| 1Ch | 4 | ディスク全体サイズ（LE） |
| 20h | 4×164 | 各トラック先頭オフセット（LE, 0 = 無し） |

セクタ（16 バイトヘッダ + データ）: 00h C, 01h H, 02h R, 03h N, 04h-05h トラック内セクタ数(LE), 06h 密度（00h 倍密 MFM, 40h 単密 FM）, 07h DELETED（00h/10h）, 08h ステータス（00h 正常, 10h 正常(DELETED), A0h ID CRC エラー, B0h データ CRC エラー, E0h アドレスマーク無し, F0h データマーク無し）, 09h-0Dh 予約, 0Eh-0Fh データサイズ(LE)。セクタは任意順・可変長。
- FDC の MF ビットとセクタの密度が一致しないと見つからない扱い [Q88 fdc.c sector_density_mismatch]。
- 複数ディスクの連結あり（Q88 FORMAT.TXT）。

---

## 9. ROM ファイル

### 9.1 MAME（pc8801mk2sr）[MAME pc8801.cpp ROM_START(pc8801mk2sr), pc80s31k.cpp ROM_START(pc80s31)]
| ファイル | サイズ | CRC32 | 配置 |
|---|---|---|---|
| mk2sr_n88.rom | 32KB | a0fc0473 | 0000h–7FFFh（N88, 31h bit2=0, bit1=0）。"2.0 V2 / 1.4 V1" |
| mk2sr_n88_0.rom | 8KB | 710a63ec | 6000h–7FFFh（71h bit0=0, 32h bit1-0=0） |
| n88_1.rom | 8KB | c0bd2aa6 | 同（32h=1） |
| n88_2.rom | 8KB | af2b6efa | 同（32h=2） |
| n88_3.rom | 8KB | 7713c519 | 同（32h=3） |
| mk2sr_n80.rom | 32KB | 27e1857d | 0000h–7FFFh（N-BASIC 1.5, 31h bit2=1） |
| kanji1.rom | 128KB | 6178bd43 | E8h/E9h。1000h–17FFh を CG フォントとして使用 |
| kanji2.rom | 128KB | 154803cc | ECh/EDh（"not on stock mkIISR"） |
| disk.rom | 2KB | 2158d307 | サブ CPU 0000h（PC-80S31。mkIIFR 用は mk2fr_disk.rom 2163b304） |

MAME は 0x7DFh/0x7E0h を 00h にパッチ（PC-8001 用ゲームの両面判定ハック）[MAME ROM_START(pc80s31) ROM_FILL]。PC-8801 用途では不要（推定）。

### 9.2 QUASI88 / M88 のファイル名 [Q88 memory.c rom_list][Q88 MANUAL.TXT「ROMイメージファイル」][M88 memory.cpp LoadROM, crtc.cpp LoadFontFile, subsys.cpp LoadROM, pc88.cpp]
| 内容 | QUASI88 | M88（Q88 でも可） |
|---|---|---|
| N88 BASIC 32KB | N88.ROM | N88.ROM |
| N88 拡張 0–3（8KB 各） | N88EXT0–3.ROM | N88_0–3.ROM |
| N-BASIC 32KB | N88N.ROM | N80.ROM |
| サブ CPU（2KB, M 系 8KB） | N88SUB.ROM | DISK.ROM |
| 漢字 第1/第2水準 128KB | N88KNJ1/2.ROM | KANJI1/2.ROM |
| 辞書 512KB（MA 以降） | N88JISHO.ROM | JISYO.ROM |
| フォント 2KB（任意） | FONT.ROM | FONT.ROM（PC-8001mkIISR 用に FONT80SR.ROM） |

- Q88 は N88.ROM の 79D7h のバイトを ROM バージョン文字として使う（'4' 以上 = SR 以降 → 既定 V2、'8' 以上 = FH/MH 以降）[Q88 memory.h `#define ROM_VERSION main_rom[0x79d7]`, pc88main.c bootup_work_init]。
- Q88 は 2KB のサブ ROM を 8KB 領域にミラー [Q88 memory.c `if (size <= 0x800) memcpy…`]。

### 9.3 結合 ROM（PC88.ROM, P88SR.EXE 互換）[M88 memory.cpp LoadROM / subsys.cpp LoadROM][Q88 memory.c load_compat_rom]
| オフセット | サイズ | 内容 |
|---|---|---|
| 00000h | 8000h | N88 ROM（0000h–7FFFh） |
| 08000h | 2000h | N-BASIC の 6000h–7FFFh 部分 |
| 0A000h | 2000h | （M88/Q88 とも未使用） |
| 0C000h | 8000h | N88 拡張 ROM 0–3（0C000h, 0E000h, 10000h, 12000h） |
| 14000h | 2000h | サブ CPU ROM（DISK.ROM） |
| 16000h | 6000h | N-BASIC の 0000h–5FFFh 部分（無い場合あり） |

---

## 10. 起動に最低限必要なもの

ROM の逆アセンブルはしていないので、「どのポートを何回ポーリングするか」はソースから確認できた範囲のみ。以下は 3 実装が共通して用意している要素を、依存順に並べたもの（順序は推定）。

### 10.1 N88-BASIC V2 の "Ok" まで（ROM 起動）
1. **リセット時マッピング**: 0000h–7FFFh = N88 ROM（31h bit1=bit2=0）, 71h=FFh（6000h–7FFFh も N88 ROM）, 32h=80h, vram_sel=3, 53h=テキスト ON [MAME machine_reset][Q88 pc88main_init][M88 pc88.cpp Reset]。
2. **DIP / ステータス**: IN 30h = D3h（80桁25行）, IN 31h = 79h（V2）, IN 40h = C0h \| VRTC<<5 \| CDO<<4 \| (15kHz ? 2 : 0) \| (ROM起動 ? 8 : 0)。最初は bit3=1（ROM 起動）にすると FD 無しで進められる（Q88 `-romboot` = ctrl_boot 08h）。IN 6Eh/6Fh は FFh（mkIISR は 8MHz 切替なし）[Q88 IN 6Eh]。
3. **VRTC**（IN 40h bit5）がフレーム周期で変化すること（§1.2）。全実装が CRTC のタイミングから生成 [MAME upd3301 vrtc_update][M88 CRTC::StartDisplay/ExpandLineEnd][Q88 intr.c ctrl_vrtc]。
4. **CRTC**（50h/51h）コマンド受理と IN 51h ステータス（VE ビット）、**DMAC**（60h–68h, 読み返し含む）。テキストは DMA ch2 の指す TVRAM（V2 は F000h–FFFFh の高速 RAM）から描画 [§5.1]。
5. **割込み**: IM2, E4h/E6h, VRTC（レベル1）と 1/600 秒（レベル2）。応答ベクタ = レベル×2。応答後は E4h 書込みまで次を出さない [§4]。
6. **カレンダ**（10h, 40h bit1/2 出力, 40h bit4 入力）[§3.1]。応答しないと時刻が不定になるだけか停止するかは未確認。
7. **キーボード**（IN 00h–0Bh, 未押下 FFh。0Ch–0Fh も FFh）。
8. **OPN**（44h/45h）: ステータス読出し（busy=0）とレジスタ書込み。割込みは 32h bit7 でマスクされた状態で始まる（32h リセット値 80h）。
9. **メモリ**: 32h bit4 / 31h bit6（H）による TVRAM 切替（§2.5）、5Ch–5Fh, 35h（ALU）, 70h/78h ウィンドウ。未接続ポートは FFh。
10. 拡張 RAM（E2h/E3h）は「無し」でよい。

### 10.2 ディスクのゲーム起動まで（追加分）
1. IN 40h bit3 = 0（FD 自動起動）。
2. **サブ CPU**（DISK.ROM, 0000h–1FFFh ROM / 4000h–7FFFh RAM）をメイン CPU と細かく交互実行（MAME は命令単位同期、Q88 は PC 読出し時に CPU 切替または並走）[§8.2]。
3. **8255 たすき掛け**（PA⇔PB, PC 上位⇔下位ニブル入替）[§8.2]。
4. **uPD765A**（FAh/FBh, F8h TC/モータ）: SPECIFY, RECALIBRATE/SEEK + SENSE INT STATUS（Ready 変化 INT 含む）, READ DATA（ノン DMA, 1 バイト毎 INT, TC 終了）, READ ID, SENSE DRIVE STATUS。書込み系は後回し可 [§8.4]。
5. **サブ CPU 割込み**: ベクタ 00h（NOP 相当）。Marat IntZ80 の IM0 をそのまま使わない [§8.3]。
6. **D88 読込み**（トラック = C×2+H, 密度と MF の一致判定）[§8.5]。
7. ゲーム側: V1/V2 判定のため IN 31h を正しく返す。V1 専用ソフトは 31h=B9h（V1S）/F9h（V1H）で起動する必要あり（§6.3 の色化け注記）。RAM 初期パターン依存ソフトあり（§2.6）。
8. コピープロテクト対策（READ ID / 欠落セクタ / CRC エラー / DELETED）は D88 のステータス・DELETED フラグを FDC 結果に反映（Q88 fdc.c STATUS_* 処理、MAME tc_zero_tick の注記）。

---

## 未確認

### 出典間で食い違う点（採用案）
| # | 項目 | MAME | QUASI88 | M88 | 採用案 |
|---|---|---|---|---|---|
| 1 | 52h 背景色のビット | bit6-4 = R,G,B（+ bit2-0 ボーダー） | bit0=B, bit1=R, bit2=G | bit3=B, bit4=R, bit5=G | 未決。白黒グラフィック時のみ影響。実機資料で要確認 |
| 2 | 50h/51h の読出し | 51h=ステータス | 51h=ステータス | 50h=ステータス | 51h=ステータス |
| 3 | 黒（色 0）の文字 | 透明 | 不透明 | 不透明 | 不透明（2 対 1）。要実機確認 |
| 4 | E6h で無効中の割込み要求 | 保持して後で発火 | 捨てる | 捨てる | 捨てる |
| 5 | E4h bit3=1 | 全レベル | レベル 0–6 | 全レベル | 全レベル |
| 6 | OPN 割込み | レベル | 立上りエッジ | 立上りエッジ | エッジ（ただし 32h bit7 解除時に保留分を出す MAME 挙動は残す） |
| 7 | ウィンドウ（70h）が F000h 以上 | 32h bit4 で TVRAM/RAM | 常にメイン RAM | 常にメイン RAM | 常にメイン RAM |
| 8 | TVRAM 切替が V1S でも効くか | 効く（H/S 区別なし） | H モードのみ | H モードのみ | H モードのみ |
| 9 | DMAC 68h 書込みで F/F リセット | しない | する | する | する |
| 10 | 32h リセット値 | 80h | 90h | 80h | 80h |
| 11 | 70h リセット値 | 80h | 00h | 00h | どちらでも（BIOS が設定） |
| 12 | 属性ペアの先頭桁 ≠ 0 の解釈 | 値を 1 つずらす | 独自 | 桁と値を独立に消費 | §5.3。実ソフトで要検証 |
| 13 | カレンダ STB/CLK の有効エッジ | 立上り | 立下り | – | どちらでも可（推定） |
| 14 | 30h bit1（/COLOR）の効果 | 未使用 | 無視 | 1 でテキスト白固定 | 無視から開始 |
| 15 | 属性ブリンク/カーソル点滅周期 | カーソル (B+1)×16 フレーム, 属性 2 倍 | blink_cycle = B×8+8 | 32×(B+1) の 1/4 等 | MAME |

### ソースから確認できなかった点
- N88-BASIC が設定する Z80 の I レジスタ（割込みベクタテーブルの位置）。
- BIOS が 15kHz / 20 行で実際に書く CRTC パラメータ（§1.2 の 15kHz 行は周波数からの逆算）。24kHz/25 行も Q88 の既定値であり BIOS ダンプではない。
- サブ CPU（DISK.ROM）が使う割込みモード（IM0 前提の記述は MAME コメント "NOP" と Q88 実装からの推定）。
- PC-80S31 コマンドのパラメータ「sector number +1」の正確な意味（MAME ヘッダの表記そのまま）、および N88-BASIC の IPL 手順（どのセクタをどこに読みどこへ飛ぶか）。
- IN 40h bit2（MAME: CMT CDIN / コメント上は DCD）。Q88 は常に 1 を返す理由。
- IN 40h bit3 の本来の意味（MAME コメント: /EXTON ミニディスク接続信号 SW2-7、入力定義: 自動ブート）。ROM 起動時に BIOS がサブシステムと全く通信しないかどうか。
- 31h bit5（25LINE）の実際の効果（全実装が無視）。
- 30h/40h 書込み bit4（FLASH）の機能（M88 は GVRAM ウェイトに使用）。
- メモリウェイト・DMA による CPU 停止時間（V1S の速度に影響）。M88 に表（memory.cpp waittable）、Q88 に近似（DMA 1 バイト 9 ステート, M1 +1）があるが根拠は不明確。
- パレット: デジタル⇔アナログ切替時の内容、デジタルモードでのラスタ変更可否（MAME TODO）。
- 400 ラインモードを 15kHz モニタで使った場合の挙動。
- uPD3301 の DMA アンダーラン時・特殊制御文字（N 割込み）の正確な挙動（MAME 未実装、M88 は独自解釈）。
- uPD1990A の年データ（MAME は 40bit, Q88 は 4990A 互換で年を含む 48bit）。N88-BASIC がどちらを期待するか。
