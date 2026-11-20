// ============================================================================
//  PC-8801mkIISR のディスクユニット(PC-80S31 相当): サブ Z80 + 8255 + μPD765A
//   ・サブ Z80: 4MHz。0000h〜1FFFh に DISK.ROM(8KB。2KB の ROM なら残りは FFh)、2000h〜3FFFh は FFh、
//     4000h〜7FFFh に RAM 16KB
//     (8000h〜FFFFh は 0000h〜7FFFh と同じ)。I/O は F8h(読むと TC / 書くとモーター)、
//     FAh = FDC の状態、FBh = FDC のデータ、FCh〜FFh = サブ側の 8255。
//   ・8255 のたすき掛け: 本体 PB → サブ PA、サブ PB → 本体 PA、
//     本体 PC 上位4bit → サブ PC 下位4bit、サブ PC 上位4bit → 本体 PC 下位4bit。
//   ・同期: 本体が 8255(FCh〜FFh)を読み書きするときは、その前にサブを本体の今の時刻
//     (行の中のクロック位置)まで進める。行の終わりでもサブを行の終わりまで進める。
//     サブが本体より先に進むことはない(最後の1バイトを出してすぐ 8255 を初期化する
//     Thexder のローダーなどで、本体が合図を取りこぼさない)。
//     サブがポート C を何度読んでも何も変わらない(本体からの指示待ち)ときは、
//     本体が 8255 に書くまで止めておく(処理の節約)。ただし本体も 8255 を読み続けている
//     (お互いに相手を待っている)ときは止めない: サブの待ちには時間切れで抜けるものがある。
//   ・FDC の割込みは NOP(データバス 00h)として受ける: HALT を抜けて割込み禁止にするだけ。
// ============================================================================
#ifdef ARDUINO
#pragma GCC optimize("O3")
#endif
#include "pc88_internal.h"
#include "z80core.h"
#include <stdio.h>
#include <string.h>

extern "C" {
  uint8_t *pc88s_rd_map[16];
  uint8_t *pc88s_wr_map[16];
  void z80sub_init(z80_t *z);
  int  z80sub_run(z80_t *z, int cycles);
  int  z80sub_int(z80_t *z, uint8_t vector);
  uint8_t InZ80Sub(uint16_t Port);
  void OutZ80Sub(uint16_t Port, uint8_t Value);
}

static z80_t   sub;
static bool    present;
static uint8_t romImg[0x2000];
static uint8_t romFF[0x1000];
static uint8_t sink[0x1000];

struct Pio { uint8_t a, b, c; uint8_t inA, inB, inC; };
static Pio     pm, ps;
static int     subCycles;
static bool    subSleep;
static int     samePolls;
static uint8_t lastSubPCIn;
static int     mainPolls;
#define MAIN_POLL_WAKE 4

void (*pc88PioHook)(int side, int port, uint8_t v, bool write);

static void subMap() {
  for (int p = 0; p < 16; p++) {
    int q = p & 7;
    if (q < 4) { pc88s_rd_map[p] = q < 2 ? romImg + q * 0x1000 : romFF; pc88s_wr_map[p] = sink; }
    else pc88s_rd_map[p] = pc88s_wr_map[p] = pc88.mem.subram + (q - 4) * 0x1000;
  }
}

static inline uint8_t rot4(uint8_t v) { return (uint8_t)((v << 4) | (v >> 4)); }
static inline uint8_t pioReadA(const Pio &me, const Pio &o) { return (uint8_t)((o.b & me.inA) | (me.a & ~me.inA)); }
static inline uint8_t pioReadB(const Pio &me, const Pio &o) { return (uint8_t)((o.a & me.inB) | (me.b & ~me.inB)); }
static inline uint8_t pioReadC(const Pio &me, const Pio &o) { return (uint8_t)((rot4(o.c) & me.inC) | (me.c & ~me.inC)); }
static void pioControl(Pio &p, uint8_t v) {
  if (v & 0x80) {
    p.inA = (v & 0x10) ? 0xFF : 0x00;
    p.inB = (v & 0x02) ? 0xFF : 0x00;
    p.inC = (uint8_t)(((v & 0x08) ? 0xF0 : 0x00) | ((v & 0x01) ? 0x0F : 0x00));
    p.a = p.b = p.c = 0;
  } else {
    int bit = (v >> 1) & 7;
    if (v & 1) p.c |= (uint8_t)(1 << bit); else p.c &= (uint8_t)~(1 << bit);
  }
}

extern "C" uint8_t InZ80Sub(uint16_t port) {
  switch (port & 0xFF) {
    case 0xF8: upd765TC(); return 0xFF;
    case 0xFA: return upd765ReadStatus();
    case 0xFB: return upd765ReadData();
    case 0xFC: { uint8_t v = pioReadA(ps, pm); if (pc88PioHook) pc88PioHook(1, 0xFC, v, false); return v; }
    case 0xFD: return pioReadB(ps, pm);
    case 0xFE: {
      uint8_t v = pioReadC(ps, pm);
      if (v == lastSubPCIn) samePolls++; else samePolls = 0;
      lastSubPCIn = v;
      return v;
    }
    default: return 0xFF;
  }
}

extern "C" void OutZ80Sub(uint16_t port, uint8_t v) {
  if (pc88PioHook && (port & 0xFF) >= 0xFC) pc88PioHook(1, port & 0xFF, v, true);
  switch (port & 0xFF) {
    case 0xF8: break;
    case 0xFB: upd765WriteData(v); break;
    case 0xFC: ps.a = v; break;
    case 0xFD: ps.b = v; break;
    case 0xFE: ps.c = v; break;
    case 0xFF: pioControl(ps, v); break;
    default: break;
  }
}
#ifdef PC88_SUB_TRACE
void pc88DiskTrace(int n) { (void)n; }
#endif

static void subIrq() {
  if (!sub.iff1 || !upd765Irq()) return;
  if (sub.im == 1) { z80sub_int(&sub, 0xFF); return; }
  if (sub.im == 2) { z80sub_int(&sub, 0x00); return; }
  sub.halted = 0;
  sub.ei_delay = 0;
  sub.iff1 = sub.iff2 = 0;
}

static void syncSub(int target) {
  while (subCycles < target) {
    int slice = target - subCycles < 64 ? target - subCycles : 64;
    int left = z80sub_run(&sub, slice);
    int ran = slice - left;
    subCycles += ran > 0 ? ran : slice;
    subIrq();
  }
}
static inline int mainNow() { return pc88LineCycle(); }

uint8_t pc88PioRead(int port) {
  if (!present) return 0xFF;
  switch (port) {
    case 0xFC: case 0xFD: case 0xFE: {
      if (subSleep) { if (++mainPolls > MAIN_POLL_WAKE) { subSleep = false; subCycles = mainNow(); } }
      else { mainPolls++; syncSub(mainNow()); }
      uint8_t v = port == 0xFC ? pioReadA(pm, ps) : port == 0xFD ? pioReadB(pm, ps) : pioReadC(pm, ps);
      if (pc88PioHook) pc88PioHook(0, port, v, false);
      return v;
    }
    default: return 0xFF;
  }
}

void pc88PioWrite(int port, uint8_t v) {
  if (!present) return;
  if (pc88PioHook) pc88PioHook(0, port, v, true);
  if (subSleep) { subSleep = false; subCycles = mainNow(); }
  else syncSub(mainNow());
  switch (port) {
    case 0xFC: pm.a = v; break;
    case 0xFD: pm.b = v; break;
    case 0xFE: pm.c = v; break;
    case 0xFF: pioControl(pm, v); break;
    default: return;
  }
  samePolls = 0;
  mainPolls = 0;
}

void pc88DiskLine() {
  if (!present) return;
  upd765Tick(80);
  if (subSleep) { subCycles = 0; mainPolls = 0; return; }
  syncSub(PC88_CYCLES_PER_LINE);
  subCycles -= PC88_CYCLES_PER_LINE;
  if (samePolls > 64 && mainPolls <= MAIN_POLL_WAKE && (upd765ReadStatus() & 0xF0) == 0x80 && !upd765Irq()) subSleep = true;
  mainPolls = 0;
}

void pc88DiskDebug(char *buf, int size) {
  snprintf(buf, size, "sub PC=%04X SP=%04X IFF=%02X sleep=%d polls=%d mPB=%02X mPC=%02X sPB=%02X sPC=%02X MSR=%02X irq=%d",
           sub.pc.w, sub.sp.w, (sub.iff1 ? 1 : 0) | (sub.iff2 ? 8 : 0) | (sub.halted ? 0x80 : 0), subSleep, samePolls, pm.b, pm.c, ps.b, ps.c, upd765ReadStatus(), upd765Irq());
}

void pc88DiskInit() {
  present = pc88.mem.diskrom != nullptr;
  if (present) {
    memcpy(romImg, pc88.mem.diskrom, 0x2000);
    memset(romFF, 0xFF, sizeof(romFF));
  }
  upd765Init(pc88.mem.drives, pc88.mem.numDrives);
}

void pc88DiskReset() {
  if (!present) return;
  subMap();
  z80sub_init(&sub);
  pioControl(pm, 0x9B);
  pioControl(ps, 0x9B);
  subCycles = 0; subSleep = false; samePolls = 0; lastSubPCIn = 0; mainPolls = 0;
  upd765Reset();
}
