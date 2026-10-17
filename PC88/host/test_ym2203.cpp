// YM2203 FM の簡単な確認: 音の高さ・振幅・キーオフ後の減衰・アルゴリズム・タイマー
#include "../ym2203.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>

static int fails;
static void check(const char *what, double got, double lo, double hi) {
  bool ok = got >= lo && got <= hi;
  printf("%s %-46s %.2f (%.2f..%.2f)\n", ok ? "ok " : "NG ", what, got, lo, hi);
  if (!ok) fails++;
}

static const uint32_t CLK = 3993600, RATE = 16000;

// チャンネル0 にサイン波だけの音色(アルゴリズム7、OP1 だけ鳴らす)
static void sineVoice(Ym2203Fm *fm, int alg, int tl1, int fb) {
  for (int k = 0; k < 4; k++) {
    int r = 0x30 + k * 4;
    ym2203WriteReg(fm, (uint8_t)r, 0x01);                       // DT0 MUL1
    ym2203WriteReg(fm, (uint8_t)(r + 0x10), (uint8_t)(k == 0 ? tl1 : 127));
    ym2203WriteReg(fm, (uint8_t)(r + 0x20), 0x1F);              // AR 31
    ym2203WriteReg(fm, (uint8_t)(r + 0x30), 0x00);
    ym2203WriteReg(fm, (uint8_t)(r + 0x40), 0x00);
    ym2203WriteReg(fm, (uint8_t)(r + 0x50), 0x0F);              // SL0 RR15
  }
  ym2203WriteReg(fm, 0xB0, (uint8_t)((fb << 3) | alg));
}

static double measureHz(const std::vector<int32_t> &v) {
  int cross = 0, first = -1, last = -1;
  for (size_t i = 1; i < v.size(); i++)
    if (v[i - 1] < 0 && v[i] >= 0) { cross++; if (first < 0) first = (int)i; last = (int)i; }
  if (cross < 2) return 0;
  return (double)(cross - 1) * RATE / (last - first);
}

int main() {
  Ym2203Fm *fm = ym2203Create(CLK, RATE);
  sineVoice(fm, 7, 0, 0);
  // 440Hz: fnum = 440 * 2^20 * 72 / CLK / 2^(4-1) = 1040 (ブロック4)
  ym2203WriteReg(fm, 0xA4, (4 << 3) | (1040 >> 8));
  ym2203WriteReg(fm, 0xA0, 1040 & 0xFF);
  ym2203WriteReg(fm, 0x28, 0x10);                               // OP1 キーオン
  std::vector<int32_t> buf(RATE);
  ym2203Generate(fm, buf.data(), RATE);
  check("pitch 440Hz (block4 fnum1040)", measureHz(buf), 438, 442);
  int peak = 0;
  for (int i = RATE / 2; i < RATE; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
  check("peak amplitude ~8191", peak, 8000, 8192);
  // TL 6dB 下げる(8 ステップ = 6dB)
  ym2203WriteReg(fm, 0x40, 8);
  ym2203Generate(fm, buf.data(), RATE / 4);
  peak = 0;
  for (int i = 0; i < RATE / 4; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
  check("TL 8 -> half amplitude", peak, 3900, 4200);
  // オクターブ上(ブロック5)
  ym2203WriteReg(fm, 0xA4, (5 << 3) | (1040 >> 8));
  ym2203WriteReg(fm, 0xA0, 1040 & 0xFF);
  ym2203Generate(fm, buf.data(), RATE);
  check("pitch 880Hz (block5)", measureHz(buf), 876, 884);
  // MUL=3
  ym2203WriteReg(fm, 0x30, 0x03);
  ym2203Generate(fm, buf.data(), RATE);
  check("pitch MUL3 = 2640Hz", measureHz(buf), 2620, 2660);
  ym2203WriteReg(fm, 0x30, 0x01);
  // キーオフ: RR15 なのですぐ消える
  ym2203WriteReg(fm, 0x28, 0x00);
  ym2203Generate(fm, buf.data(), RATE / 10);
  peak = 0;
  for (int i = RATE / 20; i < RATE / 10; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
  check("key off (RR15) silent after 50ms", peak, 0, 10);
  // アルゴリズム0 で OP1→OP2→OP3→OP4: 変調で倍音が増える(ゼロクロスが基本より多い)
  for (int k = 0; k < 4; k++) ym2203WriteReg(fm, (uint8_t)(0x40 + k * 4), 0);
  ym2203WriteReg(fm, 0xB0, 0x00);
  ym2203WriteReg(fm, 0xA4, (4 << 3) | (1040 >> 8));
  ym2203WriteReg(fm, 0xA0, 1040 & 0xFF);
  ym2203WriteReg(fm, 0x28, 0xF0);
  ym2203Generate(fm, buf.data(), RATE);
  check("alg0 modulated: more zero crossings", measureHz(buf), 500, 20000);

  // タイマー A: NA=1024-100 → 100 × 72 クロック = 1.803ms ごと
  Ym2203Timer t;
  ym2203TimerReset(t, CLK);
  ym2203TimerWrite(t, 0x24, (uint8_t)((1024 - 100) >> 2));
  ym2203TimerWrite(t, 0x25, (uint8_t)((1024 - 100) & 3));
  ym2203TimerWrite(t, 0x27, 0x05);                              // A を動かしてフラグを立てる
  int irqAt = -1;
  for (int us = 0; us < 5000; us++) { ym2203TimerAdvance(t, 16); if (ym2203TimerIrq(t) && irqAt < 0) irqAt = us; }
  check("timer A first overflow ~1803us", irqAt, 1795, 1810);
  ym2203TimerWrite(t, 0x27, 0x15);                              // フラグを消す
  check("timer A flag cleared", ym2203TimerIrq(t), 0, 0);
  // タイマー B: NB=256-10 → 10 × 1152 クロック = 2.885ms
  ym2203TimerReset(t, CLK);
  ym2203TimerWrite(t, 0x26, 246);
  ym2203TimerWrite(t, 0x27, 0x0A);
  irqAt = -1;
  for (int us = 0; us < 6000; us++) { ym2203TimerAdvance(t, 16); if (ym2203TimerIrq(t) && irqAt < 0) irqAt = us; }
  check("timer B first overflow ~2885us", irqAt, 2875, 2895);

  printf("YM2203 TEST %s (%d failures)\n", fails ? "NG" : "OK", fails);
  return fails ? 1 : 0;
}
