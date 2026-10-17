#pragma once
// ============================================================================
//  pc88_core.cpp / pc88_video.cpp / pc88_disk.cpp が共有する機種の状態
//  (外から使うのは pc88.h。ここは機種の層の中だけ)
// ============================================================================
#include "pc88.h"
#include "ym2203.h"

struct Pc88Crtc {                 // μPD3301
  uint8_t  cmd;                   // 受け付け中のコマンド(パラメータを待つ)
  int      pcnt;                  // 受け取ったパラメータの数
  uint8_t  param[5];
  bool     ve;                    // 表示中(START DISPLAY)
  bool     reverse;               // 画面全体を反転(START DISPLAY bit0)
  uint8_t  intMask;               // SET INTERRUPT MASK
  uint8_t  status;
  int      h, l, r, v, z;         // 1 行の文字数 / 行数 / 1 文字のラスタ数 / 垂直帰線の行数 / 水平帰線の文字数
  uint8_t  attrMode;              // P4 bit7-5
  int      attrNum;               // 1 行の属性の数
  uint8_t  blink;                 // P1 bit7-6
  uint8_t  cursorMode;            // P2 bit6-5(bit6 = ブロック、bit5 = 点滅)
  bool     skipLine;              // P2 bit7
  bool     cursorOn;
  uint8_t  curX, curY;
};

struct Pc88Dmac {                 // μPD8257(テキストは ch2)
  uint16_t addr[4], count[4];
  uint8_t  mode, status;
  bool     ff;                    // 上位/下位のフリップフロップ
};

struct Pc88State {
  Pc88Memory mem;
  Pc88Config cfg;
  bool     highSpeed;             // V1H / V2(DIP SW2 bit6)
  uint8_t  dip30, dip31;
  // ---- ポート ----
  uint8_t  p30, p31, p32, p34, p35, p40, p52, p53, p70, p71;
  int      vramSel;               // 5Ch〜5Fh: 0=B 1=R 2=G 3=メイン RAM
  uint8_t  aluLatch[3];
  // ---- パレット(各 0〜7) ----
  uint8_t  pal[8][3];             // [n][0]=B [1]=R [2]=G
  uint8_t  bg[3];                 // 背景色(白黒グラフィック時)
  bool     palDirty;
  // ---- テキスト ----
  Pc88Crtc crtc;
  Pc88Dmac dmac;
  // ---- 割込み ----
  uint8_t  irqReq;                // bit L = レベル L の要求
  uint8_t  irqMaskE6;
  int      levelLimit;
  bool     canInt;
  bool     opnIrqPrev, opnIrqHeld;
  uint32_t rtcAcc;                // 1/600 秒タイマー(CPU クロック)
  // ---- そのほか ----
  bool     vrtc;
  uint32_t frame;
  uint16_t kanjiAddr;
  uint8_t  opnAddr;
  Ym2203Timer opnTimer;
  uint8_t  ssgShadow[16];
};
extern Pc88State pc88;

// pc88_video.cpp
// GVRAM の書き換えがあった行(0〜199。1 行 = 80 バイト)。描くと 0 に戻す。
// 書き換えの無い行は、テキスト・モードも変わっていなければ描き直さない。
extern uint8_t pc88GvDirty[200];
static inline void pc88GvMark(uint16_t off) { if (off < 16000) pc88GvDirty[off / 80] = 1; }
void pc88VideoReset();
void pc88VideoFrameStart();              // ライン 0(表示の始まり)
void pc88RenderLine(int y);              // 出力の 1 行(0〜199)
void pc88VideoPaletteChanged();
uint8_t pc88CrtcRead(int port);
void pc88CrtcWrite(int port, uint8_t v);
uint8_t pc88DmacRead(int port);
void pc88DmacWrite(int port, uint8_t v);

// pc88_disk.cpp
void    pc88DiskInit();
void    pc88DiskReset();
void    pc88DiskLine();                  // 1 行分ディスクユニットを進める
void    pc88DiskDebug(char *buf, int size);
uint8_t pc88PioRead(int port);           // 本体側 8255(FCh〜FFh)
void    pc88PioWrite(int port, uint8_t v);
