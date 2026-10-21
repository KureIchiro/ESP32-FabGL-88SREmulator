// ============================================================================
//  YM2203 (OPN) の FM 部分とタイマー  ※説明は ym2203.h
//  オペレータの計算はチップと同じ「対数サイン表 + 減衰量 → 指数表」の形
//  (位相 10bit、エンベロープ 10bit = 0.09375dB 刻み、TL は 0.75dB 刻み)。
//  エンベロープの進み方(レートごとの増分の並び)は YM2612/YM2203 で知られている表のとおり。
//  SSG-EG と CSM モード(タイマー A のあふれでチャンネル3 の全オペレータをキーオン。シルフィードの
//  オープニングの声)は MAME の fm.c(旧版)と同じ考え方。CSM のタイマー A はこちら(音を作る側)で
//  サンプル単位に数える(CPU 側の割込み・フラグは ym2203Timer* が別に扱う)。
//  省略しているもの: モジュレーションの1サンプル遅れ(MEM)、CSM の TL ラッチ。
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

// ---- 表(初回に作る) ----
static uint16_t logSin[256];             // 1/4 波の -log2(sin) × 256
static uint16_t expTab[256];             // 2^(-k/256) × 8192
static bool     tablesReady;

static void buildTables() {
  if (tablesReady) return;
  for (int i = 0; i < 256; i++) {
    double s = sin((i + 0.5) * M_PI / 512.0);
    logSin[i] = (uint16_t)(-log2(s) * 256.0 + 0.5);
    expTab[i] = (uint16_t)(pow(2.0, -i / 256.0) * 8192.0 + 0.5);
  }
  tablesReady = true;
}

// エンベロープ: レート(0〜63)ごとの増分の並び(8段)とシフト
static const uint8_t egInc[19 * 8] = {
  0,1,0,1,0,1,0,1,  0,1,0,1,1,1,0,1,  0,1,1,1,0,1,1,1,  0,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,  1,1,1,2,1,1,1,2,  1,2,1,2,1,2,1,2,  1,2,2,2,1,2,2,2,
  2,2,2,2,2,2,2,2,  2,2,2,4,2,2,2,4,  2,4,2,4,2,4,2,4,  2,4,4,4,2,4,4,4,
  4,4,4,4,4,4,4,4,  4,4,4,8,4,4,4,8,  4,8,4,8,4,8,4,8,  4,8,8,8,4,8,8,8,
  8,8,8,8,8,8,8,8,  16,16,16,16,16,16,16,16,  0,0,0,0,0,0,0,0 };
static inline int egSel(int r) {         // egInc の行
  if (r < 4) return 18 * 0 + (r == 0 ? 18 : r & 3);   // レート 0 は止まる
  if (r < 48) return r & 3;
  if (r < 60) return 4 + (r - 48);
  return 16;
}
static inline int egShift(int r) { return r < 48 ? 11 - (r >> 2) : 0; }

// デチューン(キーコード 0〜31、DT 1〜3。4〜7 は符号を反転)。単位は 20bit 位相の増分
static const uint8_t dtTab[4][32] = {
  { 0 },
  { 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8 },
  { 1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16 },
  { 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22 } };

enum { EG_ATT, EG_DEC, EG_SUS, EG_REL, EG_OFF };

struct FmOp {
  uint8_t  dt, mul, tl, ks, ar, dr, sr, sl, rr;
  uint32_t phase, inc;
  int16_t  env;                          // 0(最大)〜1023(無音)
  uint16_t att;                          // env + TL(0〜1023)。オペレータの計算で使う
  uint8_t  state, ksr;
  uint8_t  ssg;                          // SSG-EG(90h〜 の下位4ビット。bit3 = 有効)
  uint8_t  ssgn;                         // SSG-EG の反転(bit1 = 出力を反転、bit0 = 一度反転した)
  bool     regKey;                       // 28h でキーオンしている(CSM のキーオフで消さない)
};
struct FmCh {
  FmOp     op[4];                        // オペレータ番号 1〜4 の順
  uint16_t fnum; uint8_t block, kc;
  uint8_t  fb, alg;
  int32_t  fb0, fb1;                     // オペレータ1の直前2回の出力(フィードバック用)
};
struct Ym2203Fm {
  uint32_t clock, rate;
  uint64_t scaleQ16;                     // (チップの FM の周波数 / 出力周波数) × 65536
  uint32_t egAcc, egCnt;
  FmCh     ch[3];
  uint8_t  mode27;                       // 27h の上位2ビット(チャンネル3 の効果音モード)
  uint16_t fnum3[3]; uint8_t block3[3], kc3[3];   // チャンネル3 のオペレータ別(OP1, OP2, OP3)
  uint8_t  latchA4, latchAC;             // F ナンバー上位の書き込み待ち
  uint16_t na;                           // タイマー A の値(24h/25h。CSM 用)
  bool     timerA;                       // タイマー A が動いている(27h bit0)
  uint64_t csmAcc;                       // CSM: タイマー A の進み(FM のサンプル数 × 65536)
  uint32_t fbIncLo, fbIncHi;             // FB の制限: オペレータ1 の位相の増分 300Hz / 1800Hz 相当(0 = 制限なし)
};

static inline int keycode(uint16_t fnum, uint8_t block) {
  int f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
  int n3 = (f11 & (f10 | f9 | f8)) | ((!f11) & f10 & f9 & f8);
  return (block << 2) | (f11 << 1) | n3;
}

// オペレータの位相の増分(出力1サンプルあたり、32bit 位相の上位10bit がサイン表の位置)
static void updateInc(Ym2203Fm *fm, FmOp &o, uint16_t fnum, uint8_t block, uint8_t kc) {
  int32_t fc = (int32_t)(((uint32_t)fnum << block) >> 1);
  int d = dtTab[o.dt & 3][kc];
  fc += (o.dt & 4) ? -d : d;
  fc &= 0x1FFFF;
  uint32_t inc20 = o.mul ? (uint32_t)fc * o.mul : (uint32_t)fc >> 1;
  o.inc = (uint32_t)(((uint64_t)inc20 * fm->scaleQ16) >> 4);   // 20bit → 32bit(<<12)、Q16 → >>16
  o.ksr = (uint8_t)(kc >> (3 - o.ks));
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
}

static inline int rateFor(const FmOp &o, int base) {   // base = 2×設定値(RR は 4×+2)
  if (!base) return 0;
  int r = base + o.ksr;
  return r > 63 ? 63 : r;
}

static inline void updateAtt(FmOp &o) {
  int e = o.env;
  if ((o.ssg & 0x08) && (o.ssgn & 2) && o.state < EG_REL) e = (0x200 - e) & 1023;   // SSG-EG の反転
  int a = e + (o.tl << 3);
  o.att = (uint16_t)(a > 1023 ? 1023 : a);
}

static void keyOn(FmOp &o) {
  if (o.state != EG_OFF && o.state != EG_REL) return;
  o.phase = 0;
  o.state = EG_ATT;
  o.ssgn = (o.ssg & 0x04) >> 1;          // SSG-EG: ATTACK のビットなら反転から始める
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
      o.env += ssgEg ? inc * 4 : inc;    // SSG-EG のときは減衰が4倍速い
      int sl = o.sl == 15 ? 31 * 32 : o.sl * 32;
      if (o.env >= sl) { o.env = (int16_t)sl; o.state = EG_SUS; }
      break;
    }
    case EG_SUS:
      if (ssgEg) {
        o.env += inc * 4;
        if (o.env >= 0x200) {            // SSG-EG: -48dB まで下がったら形に合わせて止める / やり直す
          uint8_t swap = 0;
          o.env = 1023;
          if (o.ssg & 0x01) {            // HOLD
            if (!(o.ssgn & 1)) swap = (o.ssg & 0x02) | 1;
          } else {                       // くり返し(キーオンと同じに最初から)
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
  uint32_t x = logSin[j] + ((uint32_t)o.att << 2);
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
  memset(fm, 0, sizeof(*fm));
  fm->clock = clock; fm->rate = rate;
  fm->scaleQ16 = (uint64_t)((double)clock / 72.0 / rate * 65536.0 + 0.5);   // FM の分周はリセット時 1/6(×12)
  if (YM_FB_CLIP_ENABLE && rate < YM_FB_CLIP_RATE) {   // 出力がチップの FM の周波数(約 55kHz)より大幅に低いときだけ FB を制限
    double lo = (double)YM_FB_CLIP_LO_HZ / rate, hi = (double)YM_FB_CLIP_HI_HZ / rate;   // 1 = 出力の周波数
    fm->fbIncLo = lo >= 1.0 ? 0xFFFFFFFFu : (uint32_t)(lo * 4294967296.0);
    fm->fbIncHi = hi >= 1.0 ? 0xFFFFFFFFu : (uint32_t)(hi * 4294967296.0);
  }
  for (int c = 0; c < 3; c++)
    for (int k = 0; k < 4; k++) {
      FmOp &o = fm->ch[c].op[k];
      o.state = EG_OFF; o.env = 1023; o.tl = 127; o.mul = 1;
      updateAtt(o);
    }
  for (int c = 0; c < 3; c++) updateCh(fm, c);
}

void ym2203WriteReg(Ym2203Fm *fm, uint8_t reg, uint8_t v) {
  if (reg == 0x24) { fm->na = (uint16_t)((fm->na & 3) | (v << 2)); return; }      // タイマー A(CSM 用)
  if (reg == 0x25) { fm->na = (uint16_t)((fm->na & 0x3FC) | (v & 3)); return; }
  if (reg == 0x27) {
    uint8_t old = fm->mode27;
    fm->mode27 = v & 0xC0;
    if ((old ^ fm->mode27) & 0xC0) updateCh(fm, 2);
    bool run = (v & 1) != 0;
    if (run && !fm->timerA) fm->csmAcc = 0;             // 動かし始めは最初から数える
    fm->timerA = run;
    return;
  }
  if (reg == 0x28) {                     // キーオン/オフ
    int c = v & 3;
    if (c == 3) return;
    FmCh &ch = fm->ch[c];
    for (int k = 0; k < 4; k++) {
      bool on = (v & (0x10 << k)) != 0;
      ch.op[k].regKey = on;
      if (on) keyOn(ch.op[k]); else keyOff(ch.op[k]);
    }
    return;
  }
  if (reg >= 0x30 && reg < 0xA0) {       // オペレータのパラメータ
    int c = reg & 3;
    if (c == 3) return;
    static const uint8_t slotOfReg[4] = { 0, 2, 1, 3 };   // 並びは OP1, OP3, OP2, OP4
    FmOp &o = fm->ch[c].op[slotOfReg[(reg >> 2) & 3]];
    switch (reg & 0xF0) {
      case 0x30: o.dt = (v >> 4) & 7; o.mul = v & 15; updateCh(fm, c); break;
      case 0x40: o.tl = v & 0x7F; updateAtt(o); break;
      case 0x50: o.ks = v >> 6; o.ar = v & 0x1F; updateCh(fm, c); break;
      case 0x60: o.dr = v & 0x1F; break;
      case 0x70: o.sr = v & 0x1F; break;
      case 0x80: o.sl = v >> 4; o.rr = v & 15; break;
      case 0x90: o.ssg = v & 0x0F; updateAtt(o); break;   // SSG-EG
    }
    return;
  }
  if (reg >= 0xA0 && reg <= 0xA2) {
    FmCh &ch = fm->ch[reg - 0xA0];
    ch.fnum = (uint16_t)(((fm->latchA4 & 7) << 8) | v);
    ch.block = (fm->latchA4 >> 3) & 7;
    updateCh(fm, reg - 0xA0);
    return;
  }
  if (reg >= 0xA4 && reg <= 0xA6) { fm->latchA4 = v; return; }
  if (reg >= 0xA8 && reg <= 0xAA) {      // チャンネル3 のオペレータ別: A9 = OP1、AA = OP2、A8 = OP3
    static const int opOfReg[3] = { 2, 0, 1 };
    int k = opOfReg[reg - 0xA8];
    fm->fnum3[k] = (uint16_t)(((fm->latchAC & 7) << 8) | v);
    fm->block3[k] = (fm->latchAC >> 3) & 7;
    fm->kc3[k] = (uint8_t)keycode(fm->fnum3[k], fm->block3[k]);
    updateCh(fm, 2);
    return;
  }
  if (reg >= 0xAC && reg <= 0xAE) { fm->latchAC = v; return; }
  if (reg >= 0xB0 && reg <= 0xB2) {
    FmCh &ch = fm->ch[reg - 0xB0];
    ch.fb = (v >> 3) & 7;
    ch.alg = v & 7;
    return;
  }
}

void YM_FAST ym2203Generate(Ym2203Fm *fm, int32_t *out, int n) {
  // エンベロープはチップの FM の3サンプルに1回進む
  const uint32_t egStepQ16 = (uint32_t)fm->scaleQ16, egPeriod = 3u << 16;
  for (int s = 0; s < n; s++) {
    // CSM: タイマー A があふれるたびにチャンネル3 の全オペレータをキーオンし、次のサンプルでキーオフ
    // (28h でキーオンしているオペレータはそのまま)。タイマー A の周期 = (1024 - NA) × FM の1サンプル
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
    fm->egAcc += egStepQ16;
    while (fm->egAcc >= egPeriod) {
      fm->egAcc -= egPeriod;
      fm->egCnt++;
      for (int c = 0; c < 3; c++)
        for (int k = 0; k < 4; k++) egStep(fm, fm->ch[c].op[k]);
    }
    int32_t sum = 0;
    for (int c = 0; c < 3; c++) {
      FmCh &ch = fm->ch[c];
      FmOp *op = ch.op;
      bool silent = true;
      for (int k = 0; k < 4; k++) if (op[k].state != EG_OFF) silent = false;
      if (silent) continue;
      int fbm = 0;
      if (ch.fb) {
        fbm = (int)((ch.fb0 + ch.fb1) >> (10 - ch.fb));
        // FB の効きを 16384Hz で成り立つところまでに制限する(値と考え方は ym2203.h の YM_FB_CLIP_*)。
        // FB はオペレータ1 の直前2回の出力を位相に足すので、1 サンプルの間隔がチップ(約 55kHz)の 3.4 倍あると、
        // 効きが FB5 前後のところで実機にない 5〜6kHz の音が出る
        // (既定値でチップの周波数で計算した音との誤差 7〜11dB → 2〜5dB、計算の増加はほぼなし)
        uint32_t inc0 = op[0].inc;
        if (inc0 < fm->fbIncHi) {
          int e = ch.fb * 64 - op[0].att;          // 実効の効き(1/64 段)
          int emax = inc0 < fm->fbIncLo ? YM_FB_CLIP_MAX_LO : YM_FB_CLIP_MAX;
          if (e > emax && e < YM_FB_CLIP_NOISE) {
            int d = (e - emax) * 4;                // 2^(-(e - emax)/64) を掛ける
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
      sum += v;
      for (int k = 0; k < 4; k++) op[k].phase += op[k].inc;
    }
    out[s] = sum;
    if (csmOn)
      for (int k = 0; k < 4; k++) { FmOp &o = fm->ch[2].op[k]; if (!o.regKey) keyOff(o); }
  }
}

// ---------------------------------------------------------------------------
//  タイマー
// ---------------------------------------------------------------------------
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
      if ((v & 1) && !(old & 1)) t.cntA = periodA(t);     // 動かし始め
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
