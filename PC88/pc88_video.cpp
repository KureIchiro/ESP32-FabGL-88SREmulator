// ============================================================================
//  PC-8801mkIISR 表示: μPD3301(CRTC)+ μPD8257(DMAC)のテキストと GVRAM の合成
//  出力は 640×200(1 行 = 2 ラスタ)。テキストは各文字行の先頭で DMA ch2 から 1 行分読む。
// ============================================================================
#ifdef ARDUINO
#pragma GCC optimize("O3")
#endif
#include "pc88_internal.h"
#include <string.h>

static uint8_t palNative[8];
static uint8_t textNative[8];
static uint8_t bgNative;
static uint8_t blackNative;
static PC88_DRAM uint16_t pairW[64];

uint8_t pc88GvDirty[200];
uint32_t pc88LinesDrawn;
static PC88_DRAM uint32_t lineKey[PC88_SCREEN_H];
static volatile uint32_t videoGen = 1;
void pc88VideoInvalidate() { videoGen++; }
static uint32_t colorGen[8], colorGenCnt = 0;
static PC88_DRAM uint8_t  lineColors[PC88_SCREEN_H];
static PC88_DRAM uint32_t lineColorKey[PC88_SCREEN_H];
static PC88_DRAM uint8_t  pairColors[64];
static inline uint32_t colorKey(uint8_t mask) {
  uint32_t k = 0;
  for (int c = 0; mask; c++, mask >>= 1) if (mask & 1) k = k * 0x01000193u + colorGen[c];
  return k;
}
static const uint16_t PALTRACK_FRAMES = 120;
static uint16_t palTrack = 0;
static PC88_DRAM uint32_t lineColorStamp[PC88_SCREEN_H];
static inline bool colorsUnchanged(int y) {
  if (lineColorStamp[y] == colorGenCnt) return true;
  if (colorKey(lineColors[y]) != lineColorKey[y]) return false;
  lineColorStamp[y] = colorGenCnt;
  return true;
}

void pc88VideoPaletteChanged() {
  Pc88State &s = pc88;
  bool changed = false;
  for (int i = 0; i < 8; i++) {
    uint8_t p = pc88NativeColor(s.pal[i][1], s.pal[i][2], s.pal[i][0]);
    if (p != palNative[i]) { colorGen[i] = ++colorGenCnt; palTrack = PALTRACK_FRAMES; }
    palNative[i] = p;
    textNative[i] = pc88NativeColor((i & 2) ? 7 : 0, (i & 4) ? 7 : 0, (i & 1) ? 7 : 0);
  }
  for (int i = 0; i < 64; i++) pairW[i] = (uint16_t)(palNative[i & 7] | (palNative[i >> 3] << 8));
  uint8_t bgn = pc88NativeColor(s.bg[1], s.bg[2], s.bg[0]);
  changed |= (bgn != bgNative);
  bgNative = bgn;
  blackNative = pc88NativeColor(0, 0, 0);
  s.palDirty = false;
  if (changed) videoGen++;
}

enum { CMD_NONE = 0xFF, CMD_RESET = 0, CMD_CURSOR = 4 };
static uint8_t  carryColor = 0xE8, carryDeco = 0;

static void crtcApplyParams() {
  Pc88Crtc &c = pc88.crtc;
  c.h = (c.param[0] & 0x7F) + 2; if (c.h > 80) c.h = 80;
  c.blink = c.param[1] >> 6;
  c.l = (c.param[1] & 0x3F) + 1;
  c.skipLine = (c.param[2] & 0x80) != 0;
  c.cursorMode = (c.param[2] >> 5) & 3;
  c.r = (c.param[2] & 0x1F) + 1;
  c.v = (c.param[3] >> 5) + 1;
  c.z = (c.param[3] & 0x1F) + 2;
  c.attrMode = c.param[4] >> 5;
  c.attrNum = (c.attrMode == 1) ? 0 : (c.param[4] & 0x1F) + 1;
  if (c.attrNum > 20) c.attrNum = 20;
}

uint8_t pc88CrtcRead(int port) {
  Pc88Crtc &c = pc88.crtc;
  if (port == 0x51) {
    uint8_t v = (uint8_t)(c.status | (c.ve ? 0x10 : 0));
    c.status &= 0x10;
    return v;
  }
  return 0x00;
}

void pc88CrtcWrite(int port, uint8_t v) {
  Pc88Crtc &c = pc88.crtc;
  if (port == 0x51) {
    switch (v >> 5) {
      case 0: c.cmd = CMD_RESET; c.pcnt = 0; c.ve = false; carryColor = 0xE8; carryDeco = 0; break;
      case 1: c.ve = true; c.reverse = (v & 1) != 0; c.cmd = CMD_NONE; break;
      case 2: c.intMask = v & 3; if (!(v & 1)) { c.status = 0; c.ve = false; } c.cmd = CMD_NONE; break;
      case 4: c.cursorOn = (v & 1) != 0; c.cmd = CMD_CURSOR; c.pcnt = 0; break;
      case 5: c.status &= (uint8_t)~0x06; c.cmd = CMD_NONE; break;
      default: c.cmd = CMD_NONE; break;
    }
    return;
  }
  if (c.cmd == CMD_RESET) {
    if (c.pcnt < 5) c.param[c.pcnt++] = v;
    if (c.pcnt == 5) { crtcApplyParams(); c.cmd = CMD_NONE; }
  } else if (c.cmd == CMD_CURSOR) {
    if (c.pcnt == 0) c.curX = v; else c.curY = v;
    if (++c.pcnt >= 2) c.cmd = CMD_NONE;
  }
}

uint8_t pc88DmacRead(int port) {
  Pc88Dmac &d = pc88.dmac;
  if (port == 0x68) { uint8_t v = d.status; d.status &= 0xF0; return v; }
  int ch = (port - 0x60) >> 1;
  uint16_t w = (port & 1) ? d.count[ch] : d.addr[ch];
  uint8_t v = d.ff ? (uint8_t)(w >> 8) : (uint8_t)w;
  d.ff = !d.ff;
  return v;
}

void pc88DmacWrite(int port, uint8_t v) {
  Pc88Dmac &d = pc88.dmac;
  if (port == 0x68) { d.mode = v; d.ff = false; return; }
  int ch = (port - 0x60) >> 1;
  uint16_t &w = (port & 1) ? d.count[ch] : d.addr[ch];
  if (!d.ff) w = (uint16_t)((w & 0xFF00) | v); else w = (uint16_t)((w & 0x00FF) | (v << 8));
  d.ff = !d.ff;
  if ((d.mode & 0x80) && ch == 2) { d.addr[3] = d.addr[2]; d.count[3] = d.count[2]; }
}

static uint8_t  rowChar[80];
static uint8_t  rowColor[80];
static uint8_t  rowDeco[80];
static int      curRow;
static int      dispRow;
static bool     textOn;
static uint16_t dmaPtr;
static int      dmaLeft;
static uint32_t rowHash;
static bool     rowBlink;
static uint8_t  textCols[80];
static int      textColN;
static uint8_t  glyphBlank[256];

static void hashRow() {
  uint32_t x = 2166136261u;
  bool bl = false;
  int h = pc88.crtc.h, n = 0;
  for (int i = 0; i < 80; i++) {
    x = (x ^ (uint32_t)(rowChar[i] | (rowColor[i] << 8) | (rowDeco[i] << 16))) * 16777619u;
    bl |= (rowDeco[i] & 0x02) != 0;
    if (i < h) {
      bool blank = !(rowDeco[i] & 0x34) && ((rowColor[i] & 0x10) ? rowChar[i] == 0 : glyphBlank[rowChar[i]] != 0);
      if (!blank) textCols[n++] = (uint8_t)i;
    }
  }
  rowHash = x;
  rowBlink = bl;
  textColN = n;
}

static inline uint8_t dmaByte(uint16_t a) {
  Pc88State &s = pc88;
  if (s.highSpeed) return a >= 0xF000 ? s.mem.tvram[a & 0xFFF] : 0xFF;
  return s.mem.ram_page[a >> 14][a & 0x3FFF];
}

static void fetchRow() {
  Pc88Crtc &c = pc88.crtc;
  int need = c.h + c.attrNum * 2;
  if (dmaLeft < need) {
    textOn = false;
    c.status |= 0x08;
    return;
  }
  uint8_t attr[40];
  for (int i = 0; i < c.h; i++) rowChar[i] = dmaByte(dmaPtr++);
  for (int i = c.h; i < 80; i++) rowChar[i] = 0;
  for (int i = 0; i < c.attrNum * 2; i++) attr[i] = dmaByte(dmaPtr++);
  dmaLeft -= need;
  bool mono = (c.attrMode != 2);
  if (c.attrMode == 1) {
    for (int x = 0; x < 80; x++) { rowColor[x] = 0xE0; rowDeco[x] = 0; }
    hashRow();
    return;
  }
  uint32_t mark[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < c.attrNum; i++) { int col = attr[i * 2] & 0x7F; mark[col >> 5] |= 1u << (col & 31); }
  carryDeco &= (uint8_t)~0x30;
  int vi = 0;
  for (int x = 0; x < 80; x++) {
    if (mark[x >> 5] & (1u << (x & 31))) {
      uint8_t val = attr[vi * 2 + 1];
      vi++;
      if (mono) { carryDeco = val; carryColor = (uint8_t)(0xE0 | ((val & 0x80) ? 0x10 : 0)); }
      else if (val & 0x08) carryColor = val;
      else carryDeco = val;
    }
    rowColor[x] = carryColor; rowDeco[x] = carryDeco;
  }
  hashRow();
}

void pc88VideoFrameStart() {
  Pc88State &s = pc88;
  if (palTrack) palTrack--;
  Pc88Crtc &c = s.crtc;
  textOn = c.ve && (s.dmac.mode & 0x04) && !(s.p53 & 1);
  dmaPtr = s.dmac.addr[2];
  dmaLeft = (s.dmac.count[2] & 0x3FFF) + 1;
  curRow = -1;
  int ras = c.l * c.r;
  s.dispLines = ras >= PC88_DISPLAY_LINES * 2 ? PC88_DISPLAY_LINES : ras < 2 ? 1 : ras / 2;
  if (s.palDirty) pc88VideoPaletteChanged();
}

static void loadFont();

void pc88VideoReset() {
  Pc88State &s = pc88;
  memset(&s.crtc, 0, sizeof(s.crtc));
  memset(&s.dmac, 0, sizeof(s.dmac));
  static const uint8_t def[5] = { 0xCE, 0x98, 0x6F, 0x58, 0x53 };
  memcpy(s.crtc.param, def, 5);
  crtcApplyParams();
  s.crtc.cmd = CMD_NONE;
  curRow = -1;
  textOn = false;
  carryColor = 0xE8; carryDeco = 0;
  loadFont();
  pc88VideoPaletteChanged();
  videoGen++;
}

static PC88_DRAM uint32_t spair[256];
static PC88_DRAM uint32_t mask4[16];
static PC88_DRAM uint8_t  dbl4[16];
static PC88_DRAM uint8_t  fontRam[256 * 8];
static uint8_t allCols[80];
static bool tablesReady;

static void buildTables() {
  static const int ord[4] = { 2, 3, 0, 1 };
  for (int n = 0; n < 16; n++) {
    uint32_t m = 0;
    uint8_t d = 0;
    for (int k = 0; k < 4; k++)
      if ((n >> (3 - k)) & 1) { m |= 0xFFu << (ord[k] * 8); d |= (uint8_t)(0xC0 >> (k * 2)); }
    mask4[n] = m; dbl4[n] = d;
  }
  for (int v = 0; v < 256; v++) {
    uint32_t w = 0;
    for (int j = 0; j < 4; j++) w |= (uint32_t)(((v >> (7 - 2 * j)) & 1) | (((v >> (6 - 2 * j)) & 1) << 3)) << (8 * j);
    spair[v] = w;
    if (v < 64) pairColors[v] = (uint8_t)((1 << (v & 7)) | (1 << (v >> 3)));
  }
  for (int i = 0; i < 80; i++) allCols[i] = (uint8_t)i;
  tablesReady = true;
}

static void loadFont() {
  const uint8_t *f = pc88.mem.kanji1 ? pc88.mem.kanji1 + 0x1000 : nullptr;
  if (f) memcpy(fontRam, f, sizeof(fontRam)); else memset(fontRam, 0, sizeof(fontRam));
  for (int ch = 0; ch < 256; ch++) {
    uint8_t any = 0;
    for (int k = 0; k < 8; k++) any |= fontRam[ch * 8 + k];
    glyphBlank[ch] = any ? 0 : 1;
  }
}

static void PC88_FAST textLine(uint8_t *tb, int lc, bool blinkPhase, bool cursorPhase, const uint8_t *cols, int n) {
  Pc88Crtc &c = pc88.crtc;
  int r = c.r;
  int fl = lc >> 1;
  const uint8_t *fontRow = fl < 8 ? fontRam + fl : nullptr;
  int br = fl >= 8 ? 3 : (fl >> 1);
  uint8_t rev = c.reverse ? 0xFF : 0x00;
  bool over = (lc == 0), under = (lc == r - 1 || lc + 1 == r - 1);
  int curCol = (c.cursorOn && cursorPhase && dispRow == c.curY) ? c.curX : -1;
  uint8_t curXor = (c.cursorMode & 2) ? 0xFF : (lc >= r - 2 ? 0xFF : 0x00);
  for (int i = 0; i < n; i++) {
    int col = cols[i];
    uint8_t ch = rowChar[col], color = rowColor[col], deco = rowDeco[col];
    uint8_t tile;
    if (color & 0x10) tile = (uint8_t)(((ch >> br) & 1 ? 0xF0 : 0) | ((ch >> (br + 4)) & 1 ? 0x0F : 0));
    else tile = fontRow ? fontRow[ch * 8] : 0;
    if (deco & 0x01) tile = 0;
    if (col == curCol) tile ^= curXor;
    else if ((deco & 0x02) && !blinkPhase) tile = 0;
    if (deco & 0x30) {
      if (over && (deco & 0x10)) tile = 0xFF;
      if (under && (deco & 0x20)) tile = 0xFF;
    }
    if (deco & 0x04) tile ^= 0xFF;
    tb[col] = tile ^ rev;
  }
}

static PC88_DRAM uint32_t lineBuf[PC88_SCREEN_W / 4];

void PC88_FAST pc88RenderLine(int y) {
  Pc88State &s = pc88;
  Pc88Crtc &c = s.crtc;
  uint32_t *dst = (uint32_t *)pc88_rows[y];
  if (!dst) return;
  if (!tablesReady) buildTables();
  if (y >= s.dispLines) {
    pc88GvDirty[y] = 0;
    uint32_t bk = (videoGen * 0x9E3779B1u) ^ 0x6B43A9B5u;
    if (bk == lineKey[y]) return;
    lineKey[y] = bk;
    lineColors[y] = 0; lineColorKey[y] = 0; lineColorStamp[y] = colorGenCnt;
    pc88LinesDrawn++;
    const uint32_t blackW = blackNative * 0x01010101u;
    for (int i = 0; i < 160; i++) dst[i] = blackW;
    return;
  }
  int raster = y * 2;
  int r = c.r < 2 ? 2 : c.r;
  int row = raster / r, lc = raster % r;
  bool text = textOn && row < c.l && !(c.skipLine && (row & 1));
  int dmaRow = c.skipLine ? row >> 1 : row;
  dispRow = row;
  if (text) {
    while (curRow < dmaRow && textOn) { curRow++; fetchRow(); }
    text = textOn && curRow == dmaRow;
  }
  uint32_t period = (uint32_t)(c.blink + 1) * 16;
  bool cursorPhase = (c.cursorMode & 1) ? ((s.frame / period) & 1) == 0 : true;
  bool blinkPhase = ((s.frame / (period * 2)) & 1) == 0;
  bool col80 = (s.p30 & 1) != 0;
  bool gOn = (s.p31 & 0x08) != 0, gColor = (s.p31 & 0x10) != 0;
  bool mono400 = gOn && !gColor && !(s.p31 & 0x01);

  uint32_t key = (videoGen * 0x9E3779B1u) ^ ((s.p30 & 1) | ((uint32_t)(s.p31 & 0x19) << 1) | ((uint32_t)s.p53 << 8));
  if (text) {
    key = (key ^ rowHash) * 16777619u;
    key = (key ^ (uint32_t)(lc | (c.r << 6) | (c.reverse << 12) | (c.attrMode << 13) | (c.h << 16) | (c.skipLine << 24))) * 16777619u;
    if (c.cursorOn && dispRow == c.curY)
      key = (key ^ (0x80000000u | ((uint32_t)cursorPhase << 24) | ((uint32_t)c.curX << 8) | c.cursorMode)) * 16777619u;
    if (rowBlink) key = (key ^ (blinkPhase ? 0x55AA55AAu : 0xAA55AA55u)) * 16777619u;
  } else {
    key = (key ^ 0x5BD1E995u) * 16777619u;
  }
  bool gDirty = pc88GvDirty[y] != 0;
  pc88GvDirty[y] = 0;
  if (!mono400 && !(gOn && gDirty) && key == lineKey[y] && colorsUnchanged(y)) return;
  lineKey[y] = mono400 ? key ^ 1 : key;
  pc88LinesDrawn++;
  const uint8_t *cols = textCols;
  int ncol = textColN;
  if (text && (c.reverse || (c.cursorOn && dispRow == c.curY))) { cols = allCols; ncol = c.h; }
  bool overlay = text && ncol > 0;
  uint32_t *out = overlay ? lineBuf : dst;

  uint8_t lb[80], lr[80], lg[80];
  uint8_t cm = 0;
  if (gOn && gColor) {
    memcpy(lb, s.mem.gvram[0] + y * 80, 80);
    memcpy(lr, s.mem.gvram[1] + y * 80, 80);
    memcpy(lg, s.mem.gvram[2] + y * 80, 80);
    if (palTrack) {
      uint32_t orw = 0, andw = 0xFFFFFFFFu;
      for (int cx = 0; cx < 80; cx++) {
        uint32_t i = spair[lb[cx]] | (spair[lr[cx]] << 1) | (spair[lg[cx]] << 2);
        orw |= i; andw &= i;
        out[cx * 2]     = pairW[(i >> 8) & 63] | ((uint32_t)pairW[i & 63] << 16);
        out[cx * 2 + 1] = pairW[i >> 24] | ((uint32_t)pairW[(i >> 16) & 63] << 16);
      }
      uint32_t ob = 0, ab = 7;
      for (int j = 0; j < 4; j++) {
        uint32_t x = (orw >> (8 * j)) & 63, a = (andw >> (8 * j)) & 63;
        ob |= (x & 7) | (x >> 3);
        ab &= (a & 7) & (a >> 3);
      }
      cm = 0;
      for (int col = 0; col < 8; col++) if (!(col & ~ob) && (col & ab) == ab) cm |= (uint8_t)(1 << col);
    } else {
      for (int cx = 0; cx < 80; cx++) {
        uint32_t i = spair[lb[cx]] | (spair[lr[cx]] << 1) | (spair[lg[cx]] << 2);
        out[cx * 2]     = pairW[(i >> 8) & 63] | ((uint32_t)pairW[i & 63] << 16);
        out[cx * 2 + 1] = pairW[i >> 24] | ((uint32_t)pairW[(i >> 16) & 63] << 16);
      }
      cm = 0xFF;
    }
  } else if (gOn) {
    uint8_t mask53 = s.p53;
    memset(lb, 0, 80);
    if (mono400) {
      int ry = y * 2;
      if (ry < 200) { if (!(mask53 & 0x02)) memcpy(lb, s.mem.gvram[0] + ry * 80, 80); }
      else if (!(mask53 & 0x04)) memcpy(lb, s.mem.gvram[1] + (ry - 200) * 80, 80);
    } else {
      if (!(mask53 & 0x02)) memcpy(lb, s.mem.gvram[0] + y * 80, 80);
      if (!(mask53 & 0x04)) { memcpy(lr, s.mem.gvram[1] + y * 80, 80); for (int i = 0; i < 80; i++) lb[i] |= lr[i]; }
      if (!(mask53 & 0x08)) { memcpy(lg, s.mem.gvram[2] + y * 80, 80); for (int i = 0; i < 80; i++) lb[i] |= lg[i]; }
    }
    const uint32_t bgW = bgNative * 0x01010101u;
    bool attrColor = text && c.attrMode == 2;
    uint32_t fgW = textNative[7] * 0x01010101u;
    for (int cx = 0; cx < 80; cx++) {
      if (attrColor) fgW = textNative[(rowColor[col80 ? cx : (cx & ~1)] >> 5) & 7] * 0x01010101u;
      uint8_t dots = lb[cx];
      uint32_t m0 = mask4[dots >> 4], m1 = mask4[dots & 15];
      out[cx * 2] = (fgW & m0) | (bgW & ~m0);
      out[cx * 2 + 1] = (fgW & m1) | (bgW & ~m1);
    }
  } else {
    const uint32_t blackW = blackNative * 0x01010101u;
    for (int i = 0; i < 160; i++) out[i] = blackW;
  }

  lineColors[y] = cm; lineColorKey[y] = colorKey(cm); lineColorStamp[y] = colorGenCnt;

  if (!overlay) return;
  uint8_t tb[80];
  textLine(tb, lc, blinkPhase, cursorPhase, cols, ncol);
  if (col80) {
    for (int i = 0; i < ncol; i++) {
      int col = cols[i];
      uint8_t bits = tb[col];
      if (!bits) continue;
      uint32_t tcW = textNative[(rowColor[col] >> 5) & 7] * 0x01010101u;
      uint32_t m0 = mask4[bits >> 4], m1 = mask4[bits & 15];
      out[col * 2]     = (tcW & m0) | (out[col * 2] & ~m0);
      out[col * 2 + 1] = (tcW & m1) | (out[col * 2 + 1] & ~m1);
    }
  } else {
    for (int i = 0; i < ncol; i++) {
      int col = cols[i];
      if (col & 1) continue;
      uint8_t t = tb[col];
      if (!t) continue;
      uint32_t tcW = textNative[(rowColor[col] >> 5) & 7] * 0x01010101u;
      for (int half = 0; half < 2; half++) {
        uint8_t bits = dbl4[half ? (t & 15) : (t >> 4)];
        int cx = col + half;
        uint32_t m0 = mask4[bits >> 4], m1 = mask4[bits & 15];
        out[cx * 2]     = (tcW & m0) | (out[cx * 2] & ~m0);
        out[cx * 2 + 1] = (tcW & m1) | (out[cx * 2 + 1] & ~m1);
      }
    }
  }
  memcpy(dst, lineBuf, sizeof(lineBuf));
}
