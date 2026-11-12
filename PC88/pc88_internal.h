#pragma once
#include "pc88.h"
#include "ym2203.h"

struct Pc88Crtc {
  uint8_t  cmd;
  int      pcnt;
  uint8_t  param[5];
  bool     ve;
  bool     reverse;
  uint8_t  intMask;
  uint8_t  status;
  int      h, l, r, v, z;
  uint8_t  attrMode;
  int      attrNum;
  uint8_t  blink;
  uint8_t  cursorMode;
  bool     skipLine;
  bool     cursorOn;
  uint8_t  curX, curY;
};

struct Pc88Dmac {
  uint16_t addr[4], count[4];
  uint8_t  mode, status;
  bool     ff;
};

struct Pc88State {
  Pc88Memory mem;
  Pc88Config cfg;
  bool     highSpeed;
  uint8_t  dip30, dip31;
  uint8_t  p30, p31, p32, p34, p35, p40, p52, p53, p70, p71;
  int      vramSel;
  uint8_t  aluLatch[3];
  uint8_t  pal[8][3];
  uint8_t  bg[3];
  bool     palDirty;
  Pc88Crtc crtc;
  Pc88Dmac dmac;
  uint8_t  irqReq;
  uint8_t  irqMaskE6;
  int      levelLimit;
  bool     canInt;
  uint32_t rtcAcc;
  bool     vrtc;
  int      dispLines;
  uint32_t frame;
  uint16_t kanjiAddr;
  uint8_t  opnAddr;
  Ym2203Timer opnTimer;
  uint8_t  ssgShadow[16];
  uint8_t  opnAddrHi;
  uint8_t  opnReg29;
  uint8_t  opnStMask;
  uint8_t  opnStatus;
  uint8_t  opnStatusNext;
  struct {
    uint8_t  ctl1, ctl2;
    uint8_t  reg[16];
    uint8_t  gran;
    uint32_t start, stop, limit;
    uint32_t memaddr;
    uint16_t deltaN;
    uint8_t  rdBuf[2];
    bool     play;
    int64_t  remain;
  } adpcm;
};
extern Pc88State pc88;

extern uint8_t pc88GvDirty[200];
static inline void pc88GvMark(uint16_t off) { if (off < 16000) pc88GvDirty[off / 80] = 1; }
void pc88VideoReset();
void pc88VideoFrameStart();
void pc88RenderLine(int y);
void pc88VideoPaletteChanged();
uint8_t pc88CrtcRead(int port);
void pc88CrtcWrite(int port, uint8_t v);
uint8_t pc88DmacRead(int port);
void pc88DmacWrite(int port, uint8_t v);

void    pc88DiskInit();
void    pc88DiskReset();
void    pc88DiskLine();
void    pc88DiskDebug(char *buf, int size);
uint8_t pc88PioRead(int port);
void    pc88PioWrite(int port, uint8_t v);
