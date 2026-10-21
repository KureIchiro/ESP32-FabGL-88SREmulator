#pragma once
#include <stdint.h>

// ============================================================================
//  YM2203 (OPN) の FM 部分(3チャンネル × 4オペレータ)とタイマー
//  SSG(AY-3-8910 互換)部分は別(X1 / MSX と同じ AY の実装を使う)。
//
//  ・合成は出力のサンプリング周波数で直接行う(チップ本来の FM の周波数 = クロック/72 で
//    作ってから間引くと ESP32 では重いので、位相の増分とエンベロープの進みを比で直す)。
//  ・タイマー A/B とステータスはエミュレーション側(Z80 のタイミング)で進める:
//    ym2203TimerAdvance() を1ラインごとに呼び、割込みは ym2203TimerIrq() で見る。
//  ・合成はサウンド側(別コア)で行う。レジスタの書き込みはエミュレーション側で
//    キューに積み、サウンド側が ym2203WriteReg() で反映する(PC88.ino)。
// ============================================================================

// ---- FB(オペレータ1 の自己フィードバック)のクリップ(ym2203.cpp の ym2203Generate) ----
//  出力 16384Hz ではチップ(約 55kHz)の 3.4 倍の間隔で FB を回すので、効きが FB5 前後のところで
//  実機にない 5〜6kHz の音が出る。そこだけ FB の効きを YM_FB_CLIP_MAX まで弱める。
//  ・実効の強さ e = FB の値 × 64 − オペレータ1 の減衰(env + TL。64 = 6dB = 1 段)。単位は 1/64 段
//    (FB 4・減衰なし = 256、FB 7 を TL 16(12dB)で使う = 7×64 − 128 = 320)
//  ・YM_FB_CLIP_MAX < e < YM_FB_CLIP_NOISE のとき、e を YM_FB_CLIP_MAX にする
//    (オペレータ1 が YM_FB_CLIP_LO_HZ 未満なら YM_FB_CLIP_MAX_LO)。
//    YM_FB_CLIP_NOISE 以上はチップでもノイズになり 16384Hz でも同じようなノイズになるので、そのまま
//  ・オペレータ1 が YM_FB_CLIP_HI_HZ 以上なら制限しない(弱めてもチップに近づかなかった)
//  ・出力が YM_FB_CLIP_RATE Hz 以上なら制限しない。YM_FB_CLIP_ENABLE 0 で制限なし
//  既定値はチップの周波数で計算した音との比較で決めたもの(2026-10-20)
#ifndef YM_FB_CLIP_ENABLE
#define YM_FB_CLIP_ENABLE   1
#endif
#ifndef YM_FB_CLIP_MAX
//#define YM_FB_CLIP_MAX      256     // 4.0 段
#define YM_FB_CLIP_MAX      192     // 3.0 段
#endif
#ifndef YM_FB_CLIP_MAX_LO
//#define YM_FB_CLIP_MAX_LO   272     // 4.25 段(低い音)
#define YM_FB_CLIP_MAX_LO   128     // 2.0 段(低い音)
#endif
#ifndef YM_FB_CLIP_NOISE
//#define YM_FB_CLIP_NOISE    352     // 5.5 段
#define YM_FB_CLIP_NOISE    384      // 6.0 段
#endif
#ifndef YM_FB_CLIP_LO_HZ
#define YM_FB_CLIP_LO_HZ    400
#endif
#ifndef YM_FB_CLIP_HI_HZ
#define YM_FB_CLIP_HI_HZ    1800
#endif
#ifndef YM_FB_CLIP_RATE
#define YM_FB_CLIP_RATE     30000
#endif

struct Ym2203Fm;

// 初期化(clock = 入力クロック Hz、rate = 出力のサンプリング周波数 Hz)
Ym2203Fm *ym2203Create(uint32_t clock, uint32_t rate);
void      ym2203Reset(Ym2203Fm *fm);
void      ym2203WriteReg(Ym2203Fm *fm, uint8_t reg, uint8_t val);   // FM のレジスタ(20h〜B2h)
// n サンプル作って out に書く(1チャンネルの最大はおよそ ±8191、3チャンネルの和)
void      ym2203Generate(Ym2203Fm *fm, int32_t *out, int n);

// ---- タイマーとステータス(エミュレーション側) ----
//  レジスタ 27h: bit0/1 = A/B を動かす、bit2/3 = A/B のフラグを立てる、bit4/5 = A/B のフラグを消す
//  ステータス: bit0 = A、bit1 = B。どちらかが立っていれば割込み(IRQ 端子)
struct Ym2203Timer {
  uint32_t clock;
  uint16_t na; uint8_t nb;               // タイマー A(10bit) / B(8bit) の設定
  uint8_t  ctrl;                          // レジスタ 27h
  uint8_t  status;
  uint8_t  prescale;                      // FM の分周(2Dh〜2Fh。リセット時 6)
  int64_t  cntA, cntB;                    // 残りのクロック数
  uint64_t frac;                          // 時間の端数
};
void ym2203TimerReset(Ym2203Timer &t, uint32_t clock);
void ym2203TimerWrite(Ym2203Timer &t, uint8_t reg, uint8_t val);   // タイマー関係のレジスタだけ見る
void ym2203TimerAdvance(Ym2203Timer &t, uint32_t us16);           // 時間を 1/16 us 単位で進める
inline bool ym2203TimerIrq(const Ym2203Timer &t) { return (t.status & 3) != 0; }
