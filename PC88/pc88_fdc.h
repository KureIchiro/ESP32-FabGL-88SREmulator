#pragma once
#include <stdint.h>

#define PC88_D88_MAX    (1024u * 1024u)
#define PC88_D88_TRACKS 164

struct Pc88Drive {
  uint8_t *buffer;
  uint32_t size;
  bool     inserted;
  bool     protect;
  bool     dirty;
  uint32_t trk_ofs[PC88_D88_TRACKS];
  uint8_t  trk_nsec[PC88_D88_TRACKS];
};

bool pc88LoadD88(Pc88Drive &drv, const uint8_t *fileBuf, uint32_t fileSize);
void pc88EjectD88(Pc88Drive &drv);

void    upd765Init(Pc88Drive *drives, int numDrives);
void    upd765Reset();
uint8_t upd765ReadStatus();
uint8_t upd765ReadData();
void    upd765WriteData(uint8_t v);
void    upd765TC();
bool    upd765Irq();
void    upd765Tick(int us);
void    upd765Debug(char *buf, int size);

extern void (*upd765CmdHook)(const uint8_t *cmd, int len);
extern void (*upd765ResultHook)(const uint8_t *res, int len);
