// ============================================================================
//  PC-8801mkIISR 本体: メモリの切り替え、I/O、割込み、カレンダ、OPN のタイマー
//  仕様は PC88_REQUIREMENTS.md(§2 メモリ、§3 I/O、§4 割込み)
// ============================================================================
#ifdef ARDUINO
#pragma GCC optimize("O3")
#endif
#include "pc88_internal.h"
#include <string.h>

Pc88State pc88;
volatile uint8_t pc88_key[16];

extern "C" {
  uint8_t *pc88_rd_map[64];
  uint8_t *pc88_wr_map[64];
  uint8_t RdZ80Slow(uint16_t a);
  void    WrZ80Slow(uint16_t a, uint8_t v);
}
static uint8_t romFF[0x400];                       // 無い ROM の代わり(FFh)

// ---------------------------------------------------------------------------
//  メモリの表(1KB 単位。Z80.c が直接引く)。§2
// ---------------------------------------------------------------------------
static inline uint8_t *ramPtr(uint32_t a) { return pc88.mem.ram_page[(a >> 14) & 3] + (a & 0x3FFF); }

// C000h〜FFFFh のメイン側(F000h〜は高速モードで 32h bit4=0 なら TVRAM)
static inline uint8_t *mainC000(uint32_t a) {
  if (a >= 0xF000 && pc88.highSpeed && !(pc88.p32 & 0x10)) return pc88.mem.tvram + (a & 0xFFF);
  return ramPtr(a);
}

static void updateMap() {
  Pc88State &s = pc88;
  // 0000h〜7FFFh: 書き込みはいつも RAM
  for (int p = 0; p < 32; p++) {
    uint32_t a = (uint32_t)p << 10;
    uint8_t *rd;
    if (s.p31 & 0x02) rd = ramPtr(a);                                   // 64KB RAM
    else if (s.p31 & 0x04) rd = s.mem.n80rom ? s.mem.n80rom + a : romFF; // N-BASIC
    else if (a >= 0x6000 && !(s.p71 & 1)) {                             // N88 拡張 ROM
      uint8_t *e = s.mem.n88ext[s.p32 & 3];
      rd = e ? e + (a & 0x1FFF) : romFF;
    } else rd = s.mem.n88rom + a;
    pc88_rd_map[p] = rd;
    pc88_wr_map[p] = ramPtr(a);
  }
  // 8000h〜83FFh: ROM モードのときはテキストウィンドウ(70h × 100h のメイン RAM)
  if (s.p31 & 0x06) {
    pc88_rd_map[32] = pc88_wr_map[32] = ramPtr(0x8000);
  } else {
    uint32_t wa = (uint32_t)s.p70 << 8;
    uint8_t *w = (wa & 0x3FF) ? nullptr : ramPtr(wa);                   // 1KB に揃っていなければ関数で
    pc88_rd_map[32] = pc88_wr_map[32] = w;
  }
  for (int p = 33; p < 48; p++) pc88_rd_map[p] = pc88_wr_map[p] = ramPtr((uint32_t)p << 10);
  // C000h〜FFFFh
  if (s.p32 & 0x40) s.vramSel = 3;                                      // ALU モード中は 5Ch〜5Eh の選択は無効
  // GVRAM への書き込みは表に載せず WrZ80Slow を通す(書き換えた行を描画に知らせるため)
  for (int p = 48; p < 64; p++) {
    uint32_t a = (uint32_t)p << 10;
    if ((s.p32 & 0x40) && (s.p35 & 0x80)) {                             // ALU を通す
      pc88_rd_map[p] = pc88_wr_map[p] = nullptr;
    } else if (s.vramSel < 3) {
      pc88_rd_map[p] = s.mem.gvram[s.vramSel] + (a & 0x3FFF);
      pc88_wr_map[p] = nullptr;
    } else {
      pc88_rd_map[p] = pc88_wr_map[p] = mainC000(a);
    }
  }
}

// ALU(§6.4)
static uint8_t aluRead(uint16_t off) {
  Pc88State &s = pc88;
  for (int i = 0; i < 3; i++) s.aluLatch[i] = s.mem.gvram[i][off];
  uint8_t b = s.aluLatch[0], r = s.aluLatch[1], g = s.aluLatch[2];
  if (!(s.p35 & 1)) b = ~b;
  if (!(s.p35 & 2)) r = ~r;
  if (!(s.p35 & 4)) g = ~g;
  return b & r & g;
}
static void aluWrite(uint16_t off, uint8_t d) {
  Pc88State &s = pc88;
  switch (s.p35 & 0x30) {
    case 0x00:
      for (int i = 0; i < 3; i++)
        switch ((s.p34 >> i) & 0x11) {
          case 0x00: s.mem.gvram[i][off] &= (uint8_t)~d; break;
          case 0x01: s.mem.gvram[i][off] |= d; break;
          case 0x10: s.mem.gvram[i][off] ^= d; break;
          default: break;
        }
      break;
    case 0x10: for (int i = 0; i < 3; i++) s.mem.gvram[i][off] = s.aluLatch[i]; break;
    case 0x20: s.mem.gvram[0][off] = s.aluLatch[1]; break;
    case 0x30: s.mem.gvram[1][off] = s.aluLatch[0]; break;
  }
}

extern "C" uint8_t PC88_FAST RdZ80Slow(uint16_t a) {
  if (a >= 0xC000) {
    Pc88State &s = pc88;
    if ((s.p32 & 0x40) && (s.p35 & 0x80)) return aluRead(a & 0x3FFF);
    return s.vramSel < 3 ? s.mem.gvram[s.vramSel][a & 0x3FFF] : *mainC000(a);
  }
  uint32_t wa = (((uint32_t)pc88.p70 << 8) + (a & 0x3FF)) & 0xFFFF;   // ずれたウィンドウ
  return *ramPtr(wa);
}
extern "C" void PC88_FAST WrZ80Slow(uint16_t a, uint8_t v) {
  if (a >= 0xC000) {                                                   // GVRAM(ALU / 1 プレーン)
    Pc88State &s = pc88;
    uint16_t off = a & 0x3FFF;
    if ((s.p32 & 0x40) && (s.p35 & 0x80)) aluWrite(off, v);
    else if (s.vramSel < 3) s.mem.gvram[s.vramSel][off] = v;
    else { *mainC000(a) = v; return; }
    pc88GvMark(off);
    return;
  }
  uint32_t wa = (((uint32_t)pc88.p70 << 8) + (a & 0x3FF)) & 0xFFFF;
  *ramPtr(wa) = v;
}

// ---------------------------------------------------------------------------
//  割込み(§4): レベル 0 RXRDY / 1 VRTC / 2 1/600 秒 / 4 OPN。ベクタ = レベル × 2
// ---------------------------------------------------------------------------
static bool levelEnabled(int l) {
  switch (l) {
    case 0: return (pc88.irqMaskE6 & 4) != 0;
    case 1: return (pc88.irqMaskE6 & 2) != 0;
    case 2: return (pc88.irqMaskE6 & 1) != 0;
    case 4: return !(pc88.p32 & 0x80);
    default: return false;
  }
}
static void request(int l) { if (levelEnabled(l)) pc88.irqReq |= (uint8_t)(1 << l); }

bool pc88IrqPending() {
  if (!pc88.canInt || !pc88.irqReq) return false;
  for (int l = 0; l < pc88.levelLimit; l++) if (pc88.irqReq & (1 << l)) return true;
  return false;
}
uint8_t pc88IrqAck() {
  for (int l = 0; l < pc88.levelLimit; l++)
    if (pc88.irqReq & (1 << l)) {
      pc88.irqReq &= (uint8_t)~(1 << l);
      pc88.canInt = false;                           // E4h を書くまで次は出さない
      return (uint8_t)(l * 2);
    }
  return 0;
}
// Z80.c が EI の次の命令のあとで呼ぶ: 受け付けられる割込みがあればベクタ、無ければ -1
extern "C" int PC88_FAST pc88IrqTake(void) { return pc88IrqPending() ? pc88IrqAck() : -1; }

// ---------------------------------------------------------------------------
//  カレンダ μPD1990A(§3.1): 10h でコマンドとデータ、40h bit1 = STB、bit2 = CLK、IN 40h bit4 = データ
// ---------------------------------------------------------------------------
static struct { uint8_t cmd, din, mode; uint64_t sr; bool cdo, stb, clk; } cal;
static struct { int year, mon, day, wday, hour, min, sec; } now = { 2026, 10, 9, 5, 12, 0, 0 };
static uint32_t calFrames;
static inline uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static void calTick() {                             // 1 フレームごと(55.4Hz)
  static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (++calFrames * 1000u < 55416u) return;          // 1 秒 = 55.416 フレーム
  calFrames = 0;
  if (++now.sec < 60) return;
  now.sec = 0;
  if (++now.min < 60) return;
  now.min = 0;
  if (++now.hour < 24) return;
  now.hour = 0;
  now.wday = (now.wday + 1) % 7;
  int md = mdays[now.mon - 1] + (now.mon == 2 && (now.year % 4) == 0);
  if (++now.day <= md) return;
  now.day = 1;
  if (++now.mon <= 12) return;
  now.mon = 1;
  now.year++;
}
static void calStrobe() {                           // STB の立ち上がり: コマンドを実行
  cal.mode = cal.cmd;
  switch (cal.cmd) {
    case 2:                                         // 時刻を合わせる(シフトレジスタから)
      now.sec = unbcd((uint8_t)cal.sr); now.min = unbcd((uint8_t)(cal.sr >> 8)); now.hour = unbcd((uint8_t)(cal.sr >> 16));
      now.day = unbcd((uint8_t)(cal.sr >> 24)); now.wday = (int)((cal.sr >> 32) & 15); now.mon = (int)((cal.sr >> 36) & 15);
      if (now.mon < 1 || now.mon > 12) now.mon = 1;
      break;
    case 3:                                         // 時刻を読む(秒 分 時 日 曜日(4) 月(4)、下位から)
      cal.sr = (uint64_t)bcd(now.sec) | ((uint64_t)bcd(now.min) << 8) | ((uint64_t)bcd(now.hour) << 16) |
               ((uint64_t)bcd(now.day) << 24) | ((uint64_t)(now.wday & 15) << 32) | ((uint64_t)(now.mon & 15) << 36);
      cal.cdo = cal.sr & 1;
      break;
    default: break;
  }
}
static void calClock() {                            // CLK の立ち上がり: シフト(シフトモードのとき)
  if (cal.mode != 1) return;
  cal.sr = (cal.sr >> 1) | ((uint64_t)(cal.din & 1) << 39);
  cal.cdo = cal.sr & 1;
}

// ---------------------------------------------------------------------------
//  I/O(§3)
// ---------------------------------------------------------------------------
extern "C" uint8_t PC88_FAST pc88In(uint16_t port) {
  Pc88State &s = pc88;
  uint8_t p = (uint8_t)port;
  if (p < 0x10) return p < 0x0C ? pc88_key[p] : 0xFF;
  switch (p) {
    case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E: return 0x00;   // USART データ
    case 0x21: case 0x23: case 0x25: case 0x27: case 0x29: case 0x2B: case 0x2D: case 0x2F: return 0x85;   // USART 状態(送信可)
    case 0x30: return s.dip30;
    case 0x31: return s.dip31;
    case 0x32: return s.p32;
    case 0x40: return (uint8_t)(0xC0 | (s.vrtc ? 0x20 : 0) | (cal.cdo ? 0x10 : 0) | (s.cfg.diskBoot && s.mem.diskrom ? 0 : 0x08) | 0x04);
    case 0x44: return (uint8_t)(s.opnTimer.status & 3);                          // OPN の状態(busy=0)
    // SSG のレジスタは CPU 側の写し(ssgShadow)から返す。音を作る側(コア1)のレジスタは書き込みの
    // キューを処理してから変わるので、書いた直後に読み直す(R7 などを読み・変え・書く)ドライバに古い値を返していた
    case 0x45: return s.opnAddr < 14 ? s.ssgShadow[s.opnAddr] : s.opnAddr < 0x10 ? pc88SsgRead(s.opnAddr) : 0x00;   // 14/15 = I/O ポート
    case 0x50: case 0x51: return pc88CrtcRead(p);
    case 0x5C: return (uint8_t)(0xF8 | (s.vramSel < 3 ? 1 << s.vramSel : 0));
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68:
      return pc88DmacRead(p);
    case 0x70: return s.p70;
    case 0x71: return s.p71;
    case 0xE8: return s.mem.kanji1 ? s.mem.kanji1[(uint32_t)s.kanjiAddr * 2 + 1] : 0xFF;
    case 0xE9: return s.mem.kanji1 ? s.mem.kanji1[(uint32_t)s.kanjiAddr * 2] : 0xFF;
    case 0xEC: return s.mem.kanji2 ? s.mem.kanji2[(uint32_t)s.kanjiAddr * 2 + 1] : 0xFF;
    case 0xED: return s.mem.kanji2 ? s.mem.kanji2[(uint32_t)s.kanjiAddr * 2] : 0xFF;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF: return pc88PioRead(p);
    default: return 0xFF;
  }
}

extern "C" void PC88_FAST pc88Out(uint16_t port, uint8_t v) {
  Pc88State &s = pc88;
  uint8_t p = (uint8_t)port;
  switch (p) {
    case 0x10: cal.cmd = v & 7; cal.din = (v >> 3) & 1; break;
    case 0x30: s.p30 = v; break;
    case 0x31: s.p31 = v; updateMap(); break;
    case 0x32: {
      uint8_t old = s.p32;
      s.p32 = v;
      if ((old ^ v) & 0x20) s.palDirty = true;
      if ((old & 0x80) && !(v & 0x80) && s.opnIrqHeld) { s.opnIrqHeld = false; request(4); }   // OPN のマスク解除で保留分
      updateMap();
      break;
    }
    case 0x34: s.p34 = v; break;
    case 0x35: s.p35 = v; if (v & 0x80) s.vramSel = 3; updateMap(); break;
    case 0x40: {
      uint8_t old = s.p40;
      s.p40 = v;
      if ((v & 0x02) && !(old & 0x02)) calStrobe();
      if ((v & 0x04) && !(old & 0x04)) calClock();
      if ((old ^ v) & 0x20) pc88Beep((v & 0x20) != 0);
      break;
    }
    case 0x44: s.opnAddr = v; break;
    case 0x45:
      if (s.opnAddr < 0x10) s.ssgShadow[s.opnAddr] = v;
      ym2203TimerWrite(s.opnTimer, s.opnAddr, v);
      pc88OpnWrite(s.opnAddr, v);
      break;
    case 0x50: case 0x51: pc88CrtcWrite(p, v); break;
    case 0x52: s.p52 = v; s.bg[0] = (v & 0x10) ? 7 : 0; s.bg[1] = (v & 0x40) ? 7 : 0; s.bg[2] = (v & 0x20) ? 7 : 0; s.palDirty = true; break;
    case 0x53: s.p53 = v; break;
    case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B: {
      int n = p - 0x54;
      if (!(s.p32 & 0x20)) {                                  // デジタル
        s.pal[n][0] = (v & 1) ? 7 : 0; s.pal[n][1] = (v & 2) ? 7 : 0; s.pal[n][2] = (v & 4) ? 7 : 0;
      } else if (n == 0 && (v & 0x80)) {                      // アナログの背景色
        if (v & 0x40) s.bg[2] = v & 7; else { s.bg[0] = v & 7; s.bg[1] = (v >> 3) & 7; }
      } else {                                                // アナログ
        if (v & 0x40) s.pal[n][2] = v & 7; else { s.pal[n][0] = v & 7; s.pal[n][1] = (v >> 3) & 7; }
      }
      s.palDirty = true;
      break;
    }
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: s.vramSel = p & 3; updateMap(); break;
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68:
      pc88DmacWrite(p, v);
      break;
    case 0x70: s.p70 = v; updateMap(); break;
    case 0x71: s.p71 = v; updateMap(); break;
    case 0x78: s.p70++; updateMap(); break;
    case 0xE4: s.levelLimit = (v & 8) ? 8 : (v & 7); s.canInt = true; break;
    case 0xE6:
      s.irqMaskE6 = v & 7;
      for (int l = 0; l < 3; l++) if (!levelEnabled(l)) s.irqReq &= (uint8_t)~(1 << l);
      break;
    case 0xE8: case 0xEC: s.kanjiAddr = (uint16_t)((s.kanjiAddr & 0xFF00) | v); break;
    case 0xE9: case 0xED: s.kanjiAddr = (uint16_t)((s.kanjiAddr & 0x00FF) | (v << 8)); break;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF: pc88PioWrite(p, v); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
//  ライン処理
// ---------------------------------------------------------------------------
// 処理時間の内訳(ESP32 の CPU クロック数。PC88.ino が読んで 0 に戻す)
uint32_t pc88ProfRender, pc88ProfDisk;
#ifdef ARDUINO
static inline uint32_t profClock() { uint32_t c; asm volatile("rsr %0, ccount" : "=a"(c)); return c; }
#else
static inline uint32_t profClock() { return 0; }
#endif

void PC88_FAST pc88Line(int line) {
  Pc88State &s = pc88;
  if (line == 0) { s.vrtc = false; pc88VideoFrameStart(); }
  uint32_t t0 = profClock();
  if (line < PC88_DISPLAY_LINES) pc88RenderLine(line);
  uint32_t t1 = profClock();
  pc88ProfRender += t1 - t0;
  if (line == PC88_DISPLAY_LINES) { s.vrtc = true; request(1); s.frame++; calTick(); }
  // 1/600 秒(6656 クロック)
  s.rtcAcc += PC88_CYCLES_PER_LINE;
  while (s.rtcAcc >= 6656) { s.rtcAcc -= 6656; request(2); }
  // OPN のタイマー(1 行 = 322 クロック = 1290/16 us)。割込みは立ち上がりで要求
  ym2203TimerAdvance(s.opnTimer, 1290);
  bool irq = ym2203TimerIrq(s.opnTimer);
  if (irq && !s.opnIrqPrev) {
    if (!(s.p32 & 0x80)) request(4); else s.opnIrqHeld = true;
  }
  if (!irq) s.opnIrqHeld = false;
  s.opnIrqPrev = irq;
  uint32_t t2 = profClock();
  pc88DiskLine();
  pc88ProfDisk += profClock() - t2;
}

// ---------------------------------------------------------------------------
//  初期化
// ---------------------------------------------------------------------------
void pc88Init(const Pc88Memory &mem, const Pc88Config &cfg) {
  pc88.mem = mem;
  pc88.cfg = cfg;
  memset(romFF, 0xFF, sizeof(romFF));
  // 電源投入時の RAM の中身(64 バイトごとに 00h / FFh。FFFFh と FF33h は 00h)§2.6
  for (uint32_t a = 0; a < 0x10000; a++) *ramPtr(a) = (a & 0x40) ? 0xFF : 0x00;
  *ramPtr(0xFFFF) = 0x00;
  *ramPtr(0xFF33) = 0x00;
  for (int i = 0; i < 3; i++) memset(mem.gvram[i], 0, 0x4000);
  memset(mem.tvram, 0, 0x1000);
  pc88DiskInit();
  pc88Reset();
}

void pc88Reset() {
  Pc88State &s = pc88;
  const Pc88Config &c = s.cfg;
  // ディップスイッチ(§3.2)。V2 / 80 桁 / 25 行: 30h = D3h、31h = 79h
  s.dip30 = (uint8_t)(0xD2 | (c.mode == PC88_N ? 0 : 1) | (c.cols40 ? 0x04 : 0) | (c.rows20 ? 0x08 : 0));
  s.dip31 = (uint8_t)(0x39 | (c.mode == PC88_V2 || c.mode == PC88_V1H ? 0x40 : 0) | (c.mode == PC88_V2 ? 0 : 0x80));
  s.highSpeed = (s.dip31 & 0x40) != 0;
  s.p30 = 0; s.p31 = 0x31; s.p32 = 0x80; s.p34 = s.p35 = 0; s.p40 = 0; s.p52 = 0; s.p53 = 0;
  s.p70 = 0x80; s.p71 = 0xFF;
  s.vramSel = 3;
  for (int i = 0; i < 8; i++) { s.pal[i][0] = (i & 1) ? 7 : 0; s.pal[i][1] = (i & 2) ? 7 : 0; s.pal[i][2] = (i & 4) ? 7 : 0; }
  s.bg[0] = s.bg[1] = s.bg[2] = 0;
  s.palDirty = true;
  s.irqReq = 0; s.irqMaskE6 = 0; s.levelLimit = 0; s.canInt = true;
  s.opnIrqPrev = s.opnIrqHeld = false;
  s.rtcAcc = 0;
  s.vrtc = false;
  s.opnAddr = 0;
  memset(s.ssgShadow, 0, sizeof(s.ssgShadow));
  ym2203TimerReset(s.opnTimer, PC88_CPU_CLOCK);
  memset(&cal, 0, sizeof(cal));
  for (int i = 0; i < 16; i++) pc88_key[i] = 0xFF;
  updateMap();
  pc88VideoReset();
  pc88DiskReset();
}
