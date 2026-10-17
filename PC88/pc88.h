#pragma once
#include <stdint.h>
#include "pc88_fdc.h"

// ============================================================================
//  NEC PC-8801mkIISR エミュレータ: 機種の層(本体 Z80 のメモリ/I/O、表示、ディスクユニット)
//  仕様は PC88_REQUIREMENTS.md(MAME などのソースを突き合わせて決めたもの)。
//  PC88.ino(ESP32)と host/main.cpp(PC のテスト環境)の両方から使う。
// ============================================================================

// ---- 画面とタイミング ----
// 24kHz モニタ・25 行(CRTC の標準設定)では 1 フレーム 448 ラスタ(表示 400)、55.4Hz、
// 1 ラスタ 160.86 クロック(PC88_REQUIREMENTS.md §1)。2 ラスタずつまとめて 1 行として進める
// (出力の 1 行 = 2 ラスタ。200 ラインのグラフィックは実機でも各ラインを 2 回出している)
#define PC88_SCREEN_W        640
#define PC88_SCREEN_H        200
#define PC88_LINES_PER_FRAME 224
#define PC88_DISPLAY_LINES   200
#define PC88_CPU_CLOCK       3993600          // 31.9488MHz / 8
#define PC88_CYCLES_PER_LINE 322              // 160.86 × 2
#define PC88_FRAME_US        18061            // 322 × 224 / 3.9936MHz

#ifdef ARDUINO
  #include <esp_attr.h>
  #define PC88_FAST IRAM_ATTR
  #define PC88_DRAM DRAM_ATTR
#else
  #define PC88_FAST
  #define PC88_DRAM
#endif

// ---- メモリ(PC88.ino / host が確保して渡す) ----
struct Pc88Memory {
  uint8_t   *ram_page[4];     // メイン RAM 64KB(16KB × 4。内部 RAM と PSRAM が混ざってよい)
  uint8_t   *gvram[3];        // グラフィック VRAM(B / R / G 各 16KB)
  uint8_t   *tvram;           // 高速 RAM 4KB(V1H/V2 で F000h〜FFFFh に見えるテキスト VRAM)
  uint8_t   *n88rom;          // N88-BASIC 32KB(必須)
  uint8_t   *n88ext[4];       // N88 拡張 ROM 8KB × 4(N88_0〜3)
  uint8_t   *n80rom;          // N-BASIC 32KB(無ければ nullptr)
  uint8_t   *kanji1;          // 第1水準漢字 ROM 128KB(無ければ nullptr。CG のフォントはここから)
  uint8_t   *kanji2;          // 第2水準(無ければ nullptr)
  uint8_t   *diskrom;         // ディスクユニットの ROM 8KB 分(2KB の ROM なら残りは FFh。無ければディスクなし)
  uint8_t   *subram;          // ディスクユニットの RAM 16KB
  Pc88Drive *drives;          // ドライブ(2台)
  int        numDrives;
};

// ---- 起動時の設定(ディップスイッチ) ----
enum Pc88BasicMode { PC88_V2, PC88_V1S, PC88_V1H, PC88_N };
struct Pc88Config {
  Pc88BasicMode mode = PC88_V2;
  bool rows20 = false;        // テキスト 20 行(false = 25 行)
  bool cols40 = false;        // テキスト 40 桁(false = 80 桁)
  bool diskBoot = true;       // ディスクから起動する(ディスクユニットがあるとき)
};

// Z80 から直接引くメモリの表(Z80.c)。本体は 1KB × 64、ディスクユニットは 4KB × 16
extern "C" {
  extern uint8_t *pc88_rd_map[64];
  extern uint8_t *pc88_wr_map[64];
  extern uint8_t *pc88s_rd_map[16];
  extern uint8_t *pc88s_wr_map[16];
  uint8_t pc88In(uint16_t port);
  void    pc88Out(uint16_t port, uint8_t v);
}

void pc88Init(const Pc88Memory &mem, const Pc88Config &cfg);
void pc88Reset();

// 1ライン分(本体の Z80 を CPU_CYCLES_PER_LINE 動かしたあとに呼ぶ):
// 表示ラインなら描画、VRTC・タイマー・割込みの更新、ディスクユニットの Z80 を進める
void pc88Line(int line);
bool    pc88IrqPending();        // 本体の Z80 の INT
uint8_t pc88IrqAck();            // 受け付けた割込みのベクタ(IM2)

// ---- キーボード(ポート 00h〜0Eh で読むマトリクス。bit=0 で押下) ----
extern volatile uint8_t pc88_key[16];

// ---- 表示(PC88.ino / host が用意する) ----
extern uint8_t **pc88_rows;      // FabGL のビューポートの各行(640 バイト、並びは px2,px3,px0,px1)
uint8_t pc88NativeColor(int r, int g, int b);   // r/g/b は 0〜7(アナログパレット 3bit)
// 変わった行だけ描くので、ビューポートにほかのもの(一時停止中の表示など)を描いたら呼ぶ(次のフレームで全部描き直す)
void pc88VideoInvalidate();

// ---- 音(PC88.ino / host が実装) ----
void    pc88OpnWrite(uint8_t reg, uint8_t val);  // YM2203(FM と SSG。タイマーは pc88_core 側)
uint8_t pc88SsgRead(uint8_t reg);               // SSG のレジスタ読み出し(I/O ポートはジョイスティック)
void    pc88Beep(bool on);                      // ビープ(ポート 40h bit5)

// 今のラインで本体の Z80 が実行したサイクル数(0〜PC88_CYCLES_PER_LINE-1)。PC88.ino / host が実装
int pc88LineCycle();

// 処理時間の内訳(ESP32 の CPU クロック数の積算。描画 / 行末のディスクユニット)。読んだ側が 0 に戻す
extern uint32_t pc88ProfRender, pc88ProfDisk;
extern uint32_t pc88LinesDrawn;            // 描き直した行の数(変わっていない行は描かない)
