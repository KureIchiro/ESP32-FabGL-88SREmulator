#pragma once
#include <stdint.h>

#ifndef YM_FB_CLIP_ENABLE
#define YM_FB_CLIP_ENABLE   1
#endif
#ifndef YM_FB_CLIP_MAX
#define YM_FB_CLIP_MAX      192
#endif
#ifndef YM_FB_CLIP_MAX_LO
#define YM_FB_CLIP_MAX_LO   128
#endif
#ifndef YM_FB_CLIP_NOISE
#define YM_FB_CLIP_NOISE    352
#endif
#ifndef YM_FB_CLIP_LO_HZ
#define YM_FB_CLIP_LO_HZ    300
#endif
#ifndef YM_FB_CLIP_HI_HZ
#define YM_FB_CLIP_HI_HZ    1800
#endif
#ifndef YM_FB_CLIP_RATE
#define YM_FB_CLIP_RATE     30000
#endif

struct Ym2203Fm;

Ym2203Fm *ym2203Create(uint32_t clock, uint32_t rate);
void      ym2203Reset(Ym2203Fm *fm);
void      ym2203WriteReg(Ym2203Fm *fm, uint8_t reg, uint8_t val);
void      ym2203Generate(Ym2203Fm *fm, int32_t *out, int n);

struct Ym2203Timer {
  uint32_t clock;
  uint16_t na; uint8_t nb;
  uint8_t  ctrl;
  uint8_t  status;
  uint8_t  prescale;
  int64_t  cntA, cntB;
  uint64_t frac;
};
void ym2203TimerReset(Ym2203Timer &t, uint32_t clock);
void ym2203TimerWrite(Ym2203Timer &t, uint8_t reg, uint8_t val);
void ym2203TimerAdvance(Ym2203Timer &t, uint32_t us16);
inline bool ym2203TimerIrq(const Ym2203Timer &t) { return (t.status & 3) != 0; }
