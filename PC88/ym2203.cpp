// ============================================================================
//  YM2203 (OPN) の FM 部分とタイマー  ※説明は ym2203.h
//  オペレータの計算はチップと同じ「対数サイン表 + 減衰量 → 指数表」の形
//  (位相 10bit、エンベロープ 10bit = 0.09375dB 刻み、TL は 0.75dB 刻み)。
//  エンベロープの進み方(レートごとの増分の並び)は YM2612/YM2203 で知られている表のとおり。
//  SSG-EG と CSM モード(タイマー A のあふれでチャンネル3 の全オペレータをキーオン。シルフィードの
//  オープニングの声)は MAME の fm.c(旧版)と同じ考え方。CSM のタイマー A はこちら(音を作る側)で
//  サンプル単位に数える(CPU 側の割込み・フラグは ym2203Timer* が別に扱う)。
//  省略しているもの: モジュレーションの1サンプル遅れ(MEM)、CSM の TL ラッチ。
//  2026-11-10 PC-88 版は YM2608(OPNA。FA/MA と同じく 44h〜47h)の FM 部分も扱う: チャンネル 4〜6(29h bit7)、
//  LFO(22h、B4h〜 の AMS/PMS、60h〜 の AM)、左右(B4h〜。出力はモノラルなので両方 0 のときだけ消す)。
//  LFO は 128 段の段が変わるたびに位相の増分(PM)と減衰(AM)を作り直す。
//  リズム音源(10h・11h・18h〜1Dh)は fmgen と同じく WAV の音を鳴らす(ym2203RhythmFromWav / ym2203SetRhythm)。
//  ADPCM(100h〜10Dh)は fmgen と同じ展開・補間・音量で、CPU 側の RAM(ym2203SetAdpcmRam)から読んで鳴らす。
// ============================================================================
#include "ym2203.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <esp_attr.h>
#define YM_FAST IRAM_ATTR
#else
#define YM_FAST
#endif

static uint16_t logSin[256];
static uint16_t expTab[256];
static bool     tablesReady;

static uint16_t rhyVolTab[128];

static void buildTables() {
  if (tablesReady) return;
  for (int i = 0; i < 256; i++) {
    double s = sin((i + 0.5) * M_PI / 512.0);
    logSin[i] = (uint16_t)(-log2(s) * 256.0 + 0.5);
    expTab[i] = (uint16_t)(pow(2.0, -i / 256.0) * 8192.0 + 0.5);
  }
  for (int i = 0; i < 128; i++) rhyVolTab[i] = (uint16_t)(((uint32_t)(65536.0 * pow(2.0, -i / 8.0)) - 1) >> 4);
  tablesReady = true;
}

static const uint8_t egInc[19 * 8] = {
  0,1,0,1,0,1,0,1,  0,1,0,1,1,1,0,1,  0,1,1,1,0,1,1,1,  0,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,  1,1,1,2,1,1,1,2,  1,2,1,2,1,2,1,2,  1,2,2,2,1,2,2,2,
  2,2,2,2,2,2,2,2,  2,2,2,4,2,2,2,4,  2,4,2,4,2,4,2,4,  2,4,4,4,2,4,4,4,
  4,4,4,4,4,4,4,4,  4,4,4,8,4,4,4,8,  4,8,4,8,4,8,4,8,  4,8,8,8,4,8,8,8,
  8,8,8,8,8,8,8,8,  16,16,16,16,16,16,16,16,  0,0,0,0,0,0,0,0 };
static inline int egSel(int r) {
  if (r < 4) return 18 * 0 + (r == 0 ? 18 : r & 3);
  if (r < 48) return r & 3;
  if (r < 60) return 4 + (r - 48);
  return 16;
}
static inline int egShift(int r) { return r < 48 ? 11 - (r >> 2) : 0; }

static const uint8_t dtTab[4][32] = {
  { 0 },
  { 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8 },
  { 1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16 },
  { 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22 } };

enum { EG_ATT, EG_DEC, EG_SUS, EG_REL, EG_OFF };

struct FmOp {
  uint8_t  dt, mul, tl, ks, ar, dr, sr, sl, rr;
  uint32_t phase, inc;
  int16_t  env;
  uint16_t att;
  uint8_t  state, ksr;
  uint8_t  ssg;
  uint8_t  ssgn;
  bool     regKey;
  bool     am;
  uint16_t amOfs;
  uint32_t incEff;
};
struct FmCh {
  FmOp     op[4];
  uint16_t fnum; uint8_t block, kc;
  uint8_t  fb, alg;
  int32_t  fb0, fb1;
  uint8_t  pan;
  uint8_t  ams, pms;
};
struct Ym2203Fm {
  uint32_t clock, rate;
  uint8_t  prescale;
  uint64_t scaleQ16;
  uint32_t egAcc, egCnt;
  FmCh     ch[6];
  uint8_t  nch;
  uint8_t  mode27;
  uint16_t fnum3[3]; uint8_t block3[3], kc3[3];
  uint8_t  latchA4[2], latchAC;
  struct {
    const int16_t *data; int len, pos;
    uint8_t level, pan;
    int vol;
  } rhy[6];
  uint8_t  rhyKey;
  uint8_t  rhyTl;
  struct {
    const uint8_t *ram;
    uint8_t  ctl1, ctl2, reg[16], gran;
    uint32_t start, stop, limit, memaddr;
    int32_t  x, d;
    int32_t  out0, out1, prev;
    int32_t  lc, ld, base;
    int32_t  vol;
    bool     play;
  } ab;
  uint8_t  reg22;
  uint32_t lfoPhase, lfoInc;
  int      lfoStep;
  uint16_t na;
  bool     timerA;
  uint64_t csmAcc;
  uint32_t fbIncLo, fbIncHi;
};

static inline int keycode(uint16_t fnum, uint8_t block) {
  int f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
  int n3 = (f11 & (f10 | f9 | f8)) | ((!f11) & f10 & f9 & f8);
  return (block << 2) | (f11 << 1) | n3;
}

static void updateInc(Ym2203Fm *fm, FmOp &o, uint16_t fnum, uint8_t block, uint8_t kc) {
  int32_t fc = (int32_t)(((uint32_t)fnum << block) >> 1);
  int d = dtTab[o.dt & 3][kc];
  fc += (o.dt & 4) ? -d : d;
  fc &= 0x1FFFF;
  uint32_t inc20 = o.mul ? (uint32_t)fc * o.mul : (uint32_t)fc >> 1;
  o.inc = (uint32_t)(((uint64_t)inc20 * fm->scaleQ16) >> 4);
  o.ksr = (uint8_t)(kc >> (3 - o.ks));
}

static const float lfoHz[8] = { 3.98f, 5.56f, 6.02f, 6.37f, 6.88f, 9.63f, 48.1f, 72.2f };
static const uint8_t  amShift[4] = { 8, 3, 1, 0 };
static const uint16_t pmDepthQ16[8] = { 0, 129, 254, 379, 532, 760, 1525, 3069 };

static void updateLfoInc(Ym2203Fm *fm) {
  double hz = lfoHz[fm->reg22 & 7] * 6.0 / fm->prescale;
  fm->lfoInc = (uint32_t)(hz / fm->rate * 4294967296.0);
}

static void rhyUpdateVol(Ym2203Fm *fm, int i) {
  int db = fm->rhyTl + fm->rhy[i].level;
  fm->rhy[i].vol = rhyVolTab[db > 127 ? 127 : db];
}

static inline uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int ym2203RhythmFromWav(const uint8_t *w, uint32_t len, uint32_t rate, int16_t *out, int outMax) {
  if (!w || len < 12 || memcmp(w, "RIFF", 4) || memcmp(w + 8, "WAVE", 4) || !rate) return 0;
  uint32_t p = 12, srcRate = 0, dlen = 0;
  int fmt = 0, ch = 0, bits = 0;
  const uint8_t *data = nullptr;
  while (p + 8 <= len) {
    uint32_t sz = rd32le(w + p + 4);
    if (!memcmp(w + p, "fmt ", 4) && sz >= 16 && p + 24 <= len) {
      fmt = rd16le(w + p + 8); ch = rd16le(w + p + 10); srcRate = rd32le(w + p + 12); bits = rd16le(w + p + 22);
    } else if (!memcmp(w + p, "data", 4)) {
      data = w + p + 8;
      dlen = sz;
      if (p + 8 + (uint64_t)dlen > len) dlen = len - p - 8;
    }
    p += 8 + sz + (sz & 1);
  }
  if (fmt != 1 || ch != 1 || bits != 16 || !data || !srcRate) return 0;
  const int nIn = (int)(dlen / 2);
  const float ratio = (float)srcRate / rate;
  const int nOut = (int)(nIn / ratio);
  if (!out) return nOut;
  const int n = nOut < outMax ? nOut : outMax;
  const float fc = ratio > 1.0f ? 0.45f / ratio : 0.45f;
  const float R = 4.0f * (ratio > 1.0f ? ratio : 1.0f);
  const int PH = 64, nk = (int)(2.0f * R * PH) + 2;
  float *kt = (float *)malloc(sizeof(float) * nk);
  if (!kt) return 0;
  for (int k = 0; k < nk; k++) {
    float t = -R + (float)k / PH;
    float x = 2.0f * fc * t;
    float s = fabsf(x) < 1e-6f ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x);
    float win = fabsf(t) >= R ? 0.0f : 0.42f + 0.5f * cosf((float)M_PI * t / R) + 0.08f * cosf(2.0f * (float)M_PI * t / R);
    kt[k] = s * win;
  }
  for (int i = 0; i < n; i++) {
    const float c = i * ratio;
    const int j0 = (int)ceilf(c - R), j1 = (int)floorf(c + R);
    float acc = 0.0f, ws = 0.0f;
    for (int j = j0; j <= j1; j++) {
      int k = (int)((j - c + R) * PH + 0.5f);
      if (k < 0 || k >= nk) continue;
      const float h = kt[k];
      ws += h;
      if (j >= 0 && j < nIn) acc += h * (int16_t)rd16le(data + 2 * j);
    }
    float v = ws != 0.0f ? acc / ws : 0.0f;
    out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)lrintf(v));
  }
  free(kt);
  return n;
}

void ym2203SetAdpcmRam(Ym2203Fm *fm, const uint8_t *ram) { fm->ab.ram = ram; }

static inline int32_t abDecodeNibble(Ym2203Fm *fm, uint32_t n) {
  static const int8_t  t1[16] = { 1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15 };
  static const uint8_t t2[16] = { 57, 57, 57, 57, 77, 102, 128, 153, 57, 57, 57, 57, 77, 102, 128, 153 };
  auto &a = fm->ab;
  int32_t x = a.x + t1[n] * a.d / 8;
  a.x = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
  int32_t d = a.d * t2[n] / 64;
  a.d = d > 24576 ? 24576 : d < 127 ? 127 : d;
  return a.x;
}
static int32_t abReadNibble(Ym2203Fm *fm) {
  auto &a = fm->ab;
  const uint32_t half = 1u << (a.gran - 1);
  const uint8_t v = a.ram ? a.ram[(a.memaddr >> a.gran) & 0x3FFFF] : 0x80;
  a.memaddr += half;
  if (a.memaddr & half) return abDecodeNibble(fm, v >> 4);
  abDecodeNibble(fm, v & 15);
  if (a.memaddr == a.stop) {
    if (a.ctl1 & 0x10) {
      a.memaddr = a.start;
      int32_t r = a.x;
      a.x = 0; a.d = 127;
      return r;
    }
    a.memaddr &= 0x3FFFFF;
    a.play = false;
  }
  if (a.memaddr == a.limit) a.memaddr = 0;
  return a.x;
}
static inline void abDecode(Ym2203Fm *fm) {
  auto &a = fm->ab;
  a.out0 = a.out1;
  int32_t s = (abReadNibble(fm) * a.vol) >> 13;
  a.out1 = a.prev + s;
  a.prev = s;
}
static int32_t abSample(Ym2203Fm *fm) {
  auto &a = fm->ab;
  int32_t s;
  if (a.ld <= 8192) {
    if (a.lc < 0) {
      a.lc += 8192;
      abDecode(fm);
      if (!a.play) goto stop;
    }
    s = (a.lc * a.out0 + (8192 - a.lc) * a.out1) >> 13;
    a.lc -= a.ld;
  } else {
    const int32_t t = (-8192 * 8192) / a.ld;
    int64_t acc = (int64_t)a.out0 * (8192 + a.lc);
    while (a.lc < 0) {
      abDecode(fm);
      if (!a.play) goto stop;
      acc -= (int64_t)a.out0 * (a.lc > t ? a.lc : t);
      a.lc -= t;
    }
    a.lc -= 8192;
    s = (int32_t)(acc >> 13);
  }
  return (a.ctl2 & 0xC0) ? s : 0;
stop:
  a.out0 = a.out1 = a.prev = 0;
  a.lc = 0;
  return 0;
}
static void abWrite(Ym2203Fm *fm, uint8_t r, uint8_t v) {
  auto &a = fm->ab;
  if (r > 0x0D) return;
  a.reg[r] = v;
  switch (r) {
    case 0x00:
      if ((v & 0x80) && !a.play) { a.play = true; a.memaddr = a.start; a.x = 0; a.d = 127; a.lc = 0; }
      if (v & 1) { a.play = false; a.out0 = a.out1 = a.prev = 0; a.lc = 0; }
      a.ctl1 = v;
      break;
    case 0x01: a.ctl2 = v; a.gran = (v & 2) ? 1 : 4; break;
    case 0x02: case 0x03: a.start = (uint32_t)(a.reg[3] * 256 + a.reg[2]) << 6; a.memaddr = a.start; break;
    case 0x04: case 0x05: a.stop = (uint32_t)(a.reg[5] * 256 + a.reg[4] + 1) << 6; break;
    case 0x09: case 0x0A: {
      int32_t dn = a.reg[10] * 256 + a.reg[9];
      if (dn < 256) dn = 256;
      a.ld = (dn * a.base) >> 16;
      break;
    }
    case 0x0B: a.vol = 16 * v; break;
    case 0x0C: case 0x0D: a.limit = (uint32_t)(a.reg[13] * 256 + a.reg[12] + 1) << 6; break;
  }
}

void ym2203SetRhythm(Ym2203Fm *fm, int i, const int16_t *data, int len) {
  if (i < 0 || i >= 6) return;
  fm->rhy[i].data = data;
  fm->rhy[i].len = data ? len : 0;
  fm->rhy[i].pos = fm->rhy[i].len;
}

static void applyLfoCh(Ym2203Fm *fm, int c) {
  FmCh &ch = fm->ch[c];
  const bool on = (fm->reg22 & 0x08) != 0;
  const int step = on ? (int)(fm->lfoPhase >> 25) : 0;
  const int amv = step < 64 ? step * 2 : (127 - step) * 2;
  const int pmv = step < 32 ? step : step < 96 ? 64 - step : step - 128;
  const int32_t pmf = on && ch.pms ? (int32_t)pmDepthQ16[ch.pms] * pmv : 0;
  for (int k = 0; k < 4; k++) {
    FmOp &o = ch.op[k];
    o.incEff = pmf ? (uint32_t)((int64_t)o.inc + (((int64_t)o.inc * pmf) >> 21)) : o.inc;
    o.amOfs = (on && o.am) ? (uint16_t)(amv >> amShift[ch.ams]) : 0;
  }
}

static void updateCh(Ym2203Fm *fm, int c) {
  FmCh &ch = fm->ch[c];
  ch.kc = (uint8_t)keycode(ch.fnum, ch.block);
  if (c == 2 && (fm->mode27 & 0xC0)) {
    for (int k = 0; k < 3; k++) updateInc(fm, ch.op[k], fm->fnum3[k], fm->block3[k], fm->kc3[k]);
    updateInc(fm, ch.op[3], ch.fnum, ch.block, ch.kc);
  } else {
    for (int k = 0; k < 4; k++) updateInc(fm, ch.op[k], ch.fnum, ch.block, ch.kc);
  }
  applyLfoCh(fm, c);
}

static inline int rateFor(const FmOp &o, int base) {
  if (!base) return 0;
  int r = base + o.ksr;
  return r > 63 ? 63 : r;
}

static inline void updateAtt(FmOp &o) {
  int e = o.env;
  if ((o.ssg & 0x08) && (o.ssgn & 2) && o.state < EG_REL) e = (0x200 - e) & 1023;
  int a = e + (o.tl << 3);
  o.att = (uint16_t)(a > 1023 ? 1023 : a);
}

static void keyOn(FmOp &o) {
  if (o.state != EG_OFF && o.state != EG_REL) return;
  o.phase = 0;
  o.state = EG_ATT;
  o.ssgn = (o.ssg & 0x04) >> 1;
  if (rateFor(o, o.ar * 2) >= 62) { o.env = 0; o.state = EG_DEC; }
  updateAtt(o);
}
static void keyOff(FmOp &o) { if (o.state != EG_OFF) o.state = EG_REL; }

static void YM_FAST egStep(Ym2203Fm *fm, FmOp &o) {
  int r;
  switch (o.state) {
    case EG_ATT: r = rateFor(o, o.ar * 2); break;
    case EG_DEC: r = rateFor(o, o.dr * 2); break;
    case EG_SUS: r = rateFor(o, o.sr * 2); break;
    case EG_REL: r = rateFor(o, o.rr * 4 + 2); break;
    default: return;
  }
  int sh = egShift(r);
  if (fm->egCnt & ((1u << sh) - 1)) return;
  int inc = egInc[egSel(r) * 8 + ((fm->egCnt >> sh) & 7)];
  if (!inc) return;
  bool ssgEg = (o.ssg & 0x08) != 0;
  switch (o.state) {
    case EG_ATT:
      o.env += (~o.env * inc) >> 4;
      if (o.env <= 0) { o.env = 0; o.state = EG_DEC; }
      break;
    case EG_DEC: {
      o.env += ssgEg ? inc * 4 : inc;
      int sl = o.sl == 15 ? 31 * 32 : o.sl * 32;
      if (o.env >= sl) { o.env = (int16_t)sl; o.state = EG_SUS; }
      break;
    }
    case EG_SUS:
      if (ssgEg) {
        o.env += inc * 4;
        if (o.env >= 0x200) {
          uint8_t swap = 0;
          o.env = 1023;
          if (o.ssg & 0x01) {
            if (!(o.ssgn & 1)) swap = (o.ssg & 0x02) | 1;
          } else {
            o.phase = 0;
            o.env = 511;
            o.state = EG_ATT;
            swap = o.ssg & 0x02;
          }
          o.ssgn ^= swap;
        }
        break;
      }
      o.env += inc;
      if (o.env >= 1023) o.env = 1023;
      break;
    case EG_REL:
      o.env += inc;
      if (o.env >= 1023) { o.env = 1023; o.state = EG_OFF; }
      break;
  }
  updateAtt(o);
}

static inline int YM_FAST opCalc(FmOp &o, int mod) {
  uint32_t idx = ((o.phase >> 22) + (uint32_t)mod) & 1023;
  uint32_t j = idx & 255;
  if (idx & 256) j = 255 - j;
  uint32_t x = logSin[j] + ((uint32_t)(o.att + o.amOfs) << 2);
  int v = (x >> 8) >= 14 ? 0 : (expTab[x & 255] >> (x >> 8));
  return (idx & 512) ? -v : v;
}

Ym2203Fm *ym2203Create(uint32_t clock, uint32_t rate) {
  buildTables();
  Ym2203Fm *fm = (Ym2203Fm *)calloc(1, sizeof(Ym2203Fm));
  if (!fm) return nullptr;
  fm->clock = clock;
  fm->rate = rate;
  ym2203Reset(fm);
  return fm;
}

void ym2203Reset(Ym2203Fm *fm) {
  uint32_t clock = fm->clock, rate = fm->rate;
  const int16_t *rd[6]; int rl[6];
  for (int i = 0; i < 6; i++) { rd[i] = fm->rhy[i].data; rl[i] = fm->rhy[i].len; }
  const uint8_t *abRam = fm->ab.ram;
  memset(fm, 0, sizeof(*fm));
  fm->clock = clock; fm->rate = rate;
  fm->ab.ram = abRam;
  fm->ab.base = (int32_t)(8192.0 * (clock / 72.0) / rate);
  for (int r = 0; r <= 0x0D; r++) abWrite(fm, (uint8_t)r, 0);
  fm->rhyTl = 63;
  for (int i = 0; i < 6; i++) { fm->rhy[i].level = 31; ym2203SetRhythm(fm, i, rd[i], rl[i]); rhyUpdateVol(fm, i); }
  fm->prescale = 6;
  fm->scaleQ16 = (uint64_t)((double)clock / (12.0 * fm->prescale) / rate * 65536.0 + 0.5);
  if (YM_FB_CLIP_ENABLE && rate < YM_FB_CLIP_RATE) {
    double lo = (double)YM_FB_CLIP_LO_HZ / rate, hi = (double)YM_FB_CLIP_HI_HZ / rate;
    fm->fbIncLo = lo >= 1.0 ? 0xFFFFFFFFu : (uint32_t)(lo * 4294967296.0);
    fm->fbIncHi = hi >= 1.0 ? 0xFFFFFFFFu : (uint32_t)(hi * 4294967296.0);
  }
  for (int c = 0; c < 6; c++) {
    for (int k = 0; k < 4; k++) {
      FmOp &o = fm->ch[c].op[k];
      o.state = EG_OFF; o.env = 1023; o.tl = 127; o.mul = 1;
      updateAtt(o);
    }
    fm->ch[c].pan = 0xC0;
  }
  fm->nch = 3;
  fm->lfoStep = -1;
  updateLfoInc(fm);
  for (int c = 0; c < 6; c++) updateCh(fm, c);
}

void ym2203WriteReg(Ym2203Fm *fm, uint16_t addr, uint8_t v) {
  const uint8_t reg = (uint8_t)addr;
  const int bank = (addr >> 8) & 1;
  if (bank) {
    if (reg <= 0x10) { abWrite(fm, reg, v); return; }
    if (reg < 0x30) return;
  } else {
  if (reg >= 0x2D && reg <= 0x2F) {
    static const uint8_t pres[3] = { 6, 3, 2 };
    if (fm->prescale != pres[reg - 0x2D]) {
      fm->prescale = pres[reg - 0x2D];
      fm->scaleQ16 = (uint64_t)((double)fm->clock / (12.0 * fm->prescale) / fm->rate * 65536.0 + 0.5);
      updateLfoInc(fm);
      for (int c = 0; c < 6; c++) updateCh(fm, c);
    }
    return;
  }
  if (reg == 0x22) {
    if ((fm->reg22 ^ v) & 0x08) fm->lfoPhase = 0;
    fm->reg22 = v;
    updateLfoInc(fm);
    fm->lfoStep = -1;
    return;
  }
  if (reg == 0x29) { fm->nch = (v & 0x80) ? 6 : 3; return; }
  if (reg == 0x10) {
    if (!(v & 0x80)) {
      for (int i = 0; i < 6; i++) if ((v & (1 << i)) && fm->rhy[i].len) { fm->rhy[i].pos = 0; fm->rhyKey |= (uint8_t)(1 << i); }
    } else {
      fm->rhyKey &= (uint8_t)~(v & 0x3F);
    }
    return;
  }
  if (reg == 0x11) { fm->rhyTl = (uint8_t)(~v & 63); for (int i = 0; i < 6; i++) rhyUpdateVol(fm, i); return; }
  if (reg >= 0x18 && reg <= 0x1D) {
    const int i = reg - 0x18;
    fm->rhy[i].pan = (v >> 6) & 3;
    fm->rhy[i].level = (uint8_t)(~v & 31);
    rhyUpdateVol(fm, i);
    return;
  }
  if (reg == 0x24) { fm->na = (uint16_t)((fm->na & 3) | (v << 2)); return; }
  if (reg == 0x25) { fm->na = (uint16_t)((fm->na & 0x3FC) | (v & 3)); return; }
  if (reg == 0x27) {
    uint8_t old = fm->mode27;
    fm->mode27 = v & 0xC0;
    if ((old ^ fm->mode27) & 0xC0) updateCh(fm, 2);
    bool run = (v & 1) != 0;
    if (run && !fm->timerA) fm->csmAcc = 0;
    fm->timerA = run;
    return;
  }
  if (reg == 0x28) {
    int c = v & 3;
    if (c == 3) return;
    if (v & 4) c += 3;
    FmCh &ch = fm->ch[c];
    for (int k = 0; k < 4; k++) {
      bool on = (v & (0x10 << k)) != 0;
      ch.op[k].regKey = on;
      if (on) keyOn(ch.op[k]); else keyOff(ch.op[k]);
    }
    return;
  }
  }
  const int cb = bank * 3;
  if (reg >= 0x30 && reg < 0xA0) {
    int c = reg & 3;
    if (c == 3) return;
    c += cb;
    static const uint8_t slotOfReg[4] = { 0, 2, 1, 3 };
    FmOp &o = fm->ch[c].op[slotOfReg[(reg >> 2) & 3]];
    switch (reg & 0xF0) {
      case 0x30: o.dt = (v >> 4) & 7; o.mul = v & 15; updateCh(fm, c); break;
      case 0x40: o.tl = v & 0x7F; updateAtt(o); break;
      case 0x50: o.ks = v >> 6; o.ar = v & 0x1F; updateCh(fm, c); break;
      case 0x60: o.dr = v & 0x1F; if (o.am != ((v & 0x80) != 0)) { o.am = (v & 0x80) != 0; applyLfoCh(fm, c); } break;
      case 0x70: o.sr = v & 0x1F; break;
      case 0x80: o.sl = v >> 4; o.rr = v & 15; break;
      case 0x90: o.ssg = v & 0x0F; updateAtt(o); break;
    }
    return;
  }
  if (reg >= 0xA0 && reg <= 0xA2) {
    const int c = reg - 0xA0 + cb;
    FmCh &ch = fm->ch[c];
    ch.fnum = (uint16_t)(((fm->latchA4[bank] & 7) << 8) | v);
    ch.block = (fm->latchA4[bank] >> 3) & 7;
    updateCh(fm, c);
    return;
  }
  if (reg >= 0xA4 && reg <= 0xA6) { fm->latchA4[bank] = v; return; }
  if (!bank && reg >= 0xA8 && reg <= 0xAA) {
    static const int opOfReg[3] = { 2, 0, 1 };
    int k = opOfReg[reg - 0xA8];
    fm->fnum3[k] = (uint16_t)(((fm->latchAC & 7) << 8) | v);
    fm->block3[k] = (fm->latchAC >> 3) & 7;
    fm->kc3[k] = (uint8_t)keycode(fm->fnum3[k], fm->block3[k]);
    updateCh(fm, 2);
    return;
  }
  if (!bank && reg >= 0xAC && reg <= 0xAE) { fm->latchAC = v; return; }
  if (reg >= 0xB0 && reg <= 0xB2) {
    FmCh &ch = fm->ch[reg - 0xB0 + cb];
    ch.fb = (v >> 3) & 7;
    ch.alg = v & 7;
    return;
  }
  if (reg >= 0xB4 && reg <= 0xB6) {
    const int c = reg - 0xB4 + cb;
    FmCh &ch = fm->ch[c];
    ch.pan = v & 0xC0;
    ch.ams = (v >> 4) & 3;
    ch.pms = v & 7;
    applyLfoCh(fm, c);
    return;
  }
}

void YM_FAST ym2203Generate(Ym2203Fm *fm, int32_t *out, int n, int32_t *rhythm) {
  const uint32_t egStepQ16 = (uint32_t)fm->scaleQ16, egPeriod = 3u << 16;
  for (int s = 0; s < n; s++) {
    bool csmOn = false;
    if ((fm->mode27 & 0xC0) == 0x80 && fm->timerA) {
      fm->csmAcc += fm->scaleQ16;
      uint64_t period = (uint64_t)(1024 - fm->na) << 16;
      if (fm->csmAcc >= period) {
        fm->csmAcc %= period;
        for (int k = 0; k < 4; k++) keyOn(fm->ch[2].op[k]);
        csmOn = true;
      }
    }
    const int nch = fm->nch;
    fm->egAcc += egStepQ16;
    while (fm->egAcc >= egPeriod) {
      fm->egAcc -= egPeriod;
      fm->egCnt++;
      for (int c = 0; c < nch; c++)
        for (int k = 0; k < 4; k++) egStep(fm, fm->ch[c].op[k]);
    }
    if (fm->reg22 & 0x08) {
      fm->lfoPhase += fm->lfoInc;
      const int step = (int)(fm->lfoPhase >> 25);
      if (step != fm->lfoStep) { fm->lfoStep = step; for (int c = 0; c < nch; c++) applyLfoCh(fm, c); }
    } else if (fm->lfoStep != 0) {
      fm->lfoStep = 0;
      for (int c = 0; c < 6; c++) applyLfoCh(fm, c);
    }
    int32_t sum = 0;
    for (int c = 0; c < nch; c++) {
      FmCh &ch = fm->ch[c];
      FmOp *op = ch.op;
      bool silent = true;
      for (int k = 0; k < 4; k++) if (op[k].state != EG_OFF) silent = false;
      if (silent) continue;
      int fbm = 0;
      if (ch.fb) {
        fbm = (int)((ch.fb0 + ch.fb1) >> (10 - ch.fb));
        uint32_t inc0 = op[0].inc;
        if (inc0 < fm->fbIncHi) {
          int e = ch.fb * 64 - op[0].att;
          int emax = inc0 < fm->fbIncLo ? YM_FB_CLIP_MAX_LO : YM_FB_CLIP_MAX;
          if (e > emax && e < YM_FB_CLIP_NOISE) {
            int d = (e - emax) * 4;
            fbm = (d >> 8) >= 14 ? 0 : (fbm * (int)(expTab[d & 255] >> (d >> 8))) >> 13;
          }
        }
      }
      int o1 = opCalc(op[0], fbm), o2, o3, o4, v;
      ch.fb0 = ch.fb1; ch.fb1 = o1;
      switch (ch.alg) {
        case 0: o2 = opCalc(op[1], o1 >> 1); o3 = opCalc(op[2], o2 >> 1); v = opCalc(op[3], o3 >> 1); break;
        case 1: o2 = opCalc(op[1], 0); o3 = opCalc(op[2], (o1 + o2) >> 1); v = opCalc(op[3], o3 >> 1); break;
        case 2: o2 = opCalc(op[1], 0); o3 = opCalc(op[2], o2 >> 1); v = opCalc(op[3], (o1 + o3) >> 1); break;
        case 3: o2 = opCalc(op[1], o1 >> 1); o3 = opCalc(op[2], 0); v = opCalc(op[3], (o2 + o3) >> 1); break;
        case 4: o2 = opCalc(op[1], o1 >> 1); o3 = opCalc(op[2], 0); o4 = opCalc(op[3], o3 >> 1); v = o2 + o4; break;
        case 5: o2 = opCalc(op[1], o1 >> 1); o3 = opCalc(op[2], o1 >> 1); o4 = opCalc(op[3], o1 >> 1); v = o2 + o3 + o4; break;
        case 6: o2 = opCalc(op[1], o1 >> 1); o3 = opCalc(op[2], 0); o4 = opCalc(op[3], 0); v = o2 + o3 + o4; break;
        default: o2 = opCalc(op[1], 0); o3 = opCalc(op[2], 0); o4 = opCalc(op[3], 0); v = o1 + o2 + o3 + o4; break;
      }
      if (v > 8191) v = 8191; else if (v < -8192) v = -8192;
      if (ch.pan) sum += v;
      for (int k = 0; k < 4; k++) op[k].phase += op[k].incEff;
    }
    int32_t rs = 0;
    if (fm->rhyKey) {
      for (int i = 0; i < 6; i++) {
        if (!(fm->rhyKey & (1 << i))) continue;
        auto &h = fm->rhy[i];
        if (h.pos >= h.len) { fm->rhyKey &= (uint8_t)~(1 << i); continue; }
        if (h.pan) rs += (h.data[h.pos] * h.vol) >> 12;
        h.pos++;
      }
    }
    if (fm->ab.play) rs += abSample(fm);
    if (rhythm) rhythm[s] = rs; else sum += rs;
    out[s] = sum;
    if (csmOn)
      for (int k = 0; k < 4; k++) { FmOp &o = fm->ch[2].op[k]; if (!o.regKey) keyOff(o); }
  }
}

static inline int64_t periodA(const Ym2203Timer &t) { return (int64_t)(1024 - t.na) * 12 * t.prescale; }
static inline int64_t periodB(const Ym2203Timer &t) { return (int64_t)(256 - t.nb) * 16 * 12 * t.prescale; }

void ym2203TimerReset(Ym2203Timer &t, uint32_t clock) {
  memset(&t, 0, sizeof(t));
  t.clock = clock;
  t.prescale = 6;
}

void ym2203TimerWrite(Ym2203Timer &t, uint8_t reg, uint8_t v) {
  switch (reg) {
    case 0x24: t.na = (uint16_t)((t.na & 3) | (v << 2)); break;
    case 0x25: t.na = (uint16_t)((t.na & 0x3FC) | (v & 3)); break;
    case 0x26: t.nb = v; break;
    case 0x27: {
      uint8_t old = t.ctrl;
      if ((v & 1) && !(old & 1)) t.cntA = periodA(t);
      if ((v & 2) && !(old & 2)) t.cntB = periodB(t);
      if (v & 0x10) t.status &= ~1;
      if (v & 0x20) t.status &= ~2;
      t.ctrl = v;
      break;
    }
    case 0x2D: t.prescale = 6; break;
    case 0x2E: t.prescale = 3; break;
    case 0x2F: t.prescale = 2; break;
    default: break;
  }
}

void ym2203TimerAdvance(Ym2203Timer &t, uint32_t us16) {
  if (!(t.ctrl & 3)) return;
  uint64_t ticks = (uint64_t)us16 * t.clock + t.frac;
  int64_t clocks = (int64_t)(ticks / 16000000u);
  t.frac = ticks % 16000000u;
  if (t.ctrl & 1) {
    t.cntA -= clocks;
    while (t.cntA <= 0) { t.cntA += periodA(t); if (t.ctrl & 4) t.status |= 1; }
  }
  if (t.ctrl & 2) {
    t.cntB -= clocks;
    while (t.cntB <= 0) { t.cntB += periodB(t); if (t.ctrl & 8) t.status |= 2; }
  }
}
