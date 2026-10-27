#pragma once
#include <stdint.h>
#include "pc88_fdc.h"

#define PC88_SCREEN_W        640
#define PC88_SCREEN_H        200
#define PC88_LINES_PER_FRAME 224
#define PC88_DISPLAY_LINES   200
#define PC88_CPU_CLOCK       3993600
#define PC88_CYCLES_PER_LINE 322
#define PC88_FRAME_US        18061

#ifdef ARDUINO
  #include <esp_attr.h>
  #define PC88_FAST IRAM_ATTR
  #define PC88_DRAM DRAM_ATTR
#else
  #define PC88_FAST
  #define PC88_DRAM
#endif

struct Pc88Memory {
  uint8_t   *ram_page[4];
  uint8_t   *gvram[3];
  uint8_t   *tvram;
  uint8_t   *n88rom;
  uint8_t   *n88ext[4];
  uint8_t   *n80rom;
  uint8_t   *kanji1;
  uint8_t   *kanji2;
  uint8_t   *diskrom;
  uint8_t   *subram;
  Pc88Drive *drives;
  int        numDrives;
};

enum Pc88BasicMode { PC88_V2, PC88_V1S, PC88_V1H, PC88_N };
struct Pc88Config {
  Pc88BasicMode mode = PC88_V2;
  bool rows20 = false;
  bool cols40 = false;
  bool diskBoot = true;
};

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

void pc88Line(int line);
bool    pc88IrqPending();
uint8_t pc88IrqAck();

extern volatile uint8_t pc88_key[16];

extern uint8_t **pc88_rows;
uint8_t pc88NativeColor(int r, int g, int b);
void pc88VideoInvalidate();

void    pc88OpnWrite(uint8_t reg, uint8_t val);
uint8_t pc88SsgRead(uint8_t reg);
void    pc88Speaker(uint8_t bits);

int pc88LineCycle();

extern uint32_t pc88ProfRender, pc88ProfDisk;
extern uint32_t pc88LinesDrawn;
