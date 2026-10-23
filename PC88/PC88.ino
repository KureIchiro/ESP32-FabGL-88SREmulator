/* ============================================================================
 *  NEC PC-8801mkIISR Emulator for ESP32 (TTGO VGA32) — 作成中
 *  --- MSX2 / X1 エミュレータと同じ作り(FabGL、Marat Fayzullin 氏の Z80 コア) ---
 *
 *  【必要なファイル(SD カード /PC88/)】
 *   N88.ROM      N88-BASIC 32KB(必須)
 *   N88_0.ROM〜N88_3.ROM  N88 拡張 ROM 各 8KB(V2 / ディスク BASIC に必要)
 *   N80.ROM      N-BASIC 32KB(無くてもよい)
 *   DISK.ROM     ディスクユニットの ROM 2KB / 8KB(無いとディスクは使えない)
 *   KANJI1.ROM   第1水準漢字 ROM 128KB(文字のフォントもここから)。KANJI2.ROM は無くてもよい
 *   *.D88        ディスクイメージ(/PC88 の中のフォルダに置いてもよい。起動メニューで入れる)。
 *                1つのファイルに何枚も入った D88 は、選んだあとに何枚目かを選ぶ。
 *                ドライブ1に2枚以上入ったファイルを選び、ドライブ2が空なら、次の1枚をドライブ2に入れる。
 *
 *  【操作】(MSX / MSX2 版と同じ)
 *   F12 一時停止 / Shift+F12 リセット(ESP32 ごと再起動して起動メニューから)
 *   一時停止中に F10 / F11 でドライブ1 / 2 のディスクを入れ替え、Shift+F10 / F11 で SD へ書き戻し
 *
 *  【ジョイスティック】(いつも有効)1P の方向 = テンキー 8 / 2 / 4 / 6、ボタン1〜3 = 起動メニューの
 *   Joy B1〜B3 で決めたキー(Enter で反転 → 次に押したキーを割り当て。/PC88/JOYKEY.CFG に保存)。
 *   SR のジョイスティック端子(SSG のポート)は再現しない(キーの割り当てだけで使う)
 *
 *  【仕様】PC88_REQUIREMENTS.md
 * ============================================================================
 */
#pragma GCC optimize("O3")

#include <string.h>
#include <stdlib.h>
#include <SPI.h>
#include <SD.h>
#include <fabgl.h>
#include <vector>
#include <algorithm>
#include "soc/rtc_wdt.h"
#include "esp_task_wdt.h"
#include "Z80.h"
#include "pc88.h"
#include "ym2203.h"

#define USE_SERIAL_DEBUG

// TTGO VGA32
#define SD_SCLK 14
#define SD_MISO 2
#define SD_MOSI 12
#define SD_CS 13

#define CPU_CYCLES_PER_LINE PC88_CYCLES_PER_LINE

// D88 ファイルの中のイメージ(何枚も続けて入っていることがある。ヘッダ 1Ch がそのイメージの大きさ)。
// Arduino が関数の宣言を先頭に足すので、型はここで定義しておく
struct D88Image { uint32_t ofs, size; char name[18]; };
static const int D88_MAX_IMAGES = 16;

// PS/2 → PC-8801 のキーマトリクスの表(キーボードと、ジョイスティックのボタンの割り当てで使う)
struct Pc88KeyMap { uint8_t ps2; bool ext; uint8_t row, bit; };
#include "pc88_keymap.h"

// ---------------------------------------------------------------------------
//  Z80 グルー(メモリは Z80.c が pc88_rd_map / pc88_wr_map を直接引く)
// ---------------------------------------------------------------------------
extern "C" {
  extern Z80 cpu;
  byte InZ80(zword Port);
  void OutZ80(zword Port, byte Value);
  void PatchZ80(Z80 *R);
}
Z80 cpu;
byte IRAM_ATTR InZ80(zword Port) { return pc88In(Port); }
void IRAM_ATTR OutZ80(zword Port, byte Value) { pc88Out(Port, Value); }
void PatchZ80(Z80 *R) {}

int pc88LineCycle() {
  int c = CPU_CYCLES_PER_LINE - cpu.ICount;
  return c < 0 ? 0 : c >= CPU_CYCLES_PER_LINE ? CPU_CYCLES_PER_LINE - 1 : c;
}

// ---------------------------------------------------------------------------
//  FabGL
// ---------------------------------------------------------------------------
fabgl::VGAController DisplayController;
fabgl::Canvas *canvas;
fabgl::SoundGenerator soundGenerator;
fabgl::PS2Controller PS2Controller;

volatile bool frameReady = false;
uint8_t **pc88_rows = nullptr;

uint8_t pc88NativeColor(int r, int g, int b) {
  // FabGL の RGB222 は各 2bit。アナログパレットの 3bit(0〜7)を 2bit に落とす
  return DisplayController.createRawPixel(fabgl::RGB222(r >> 1, g >> 1, b >> 1));
}

// ---------------------------------------------------------------------------
//  ジョイスティック(MSX / X1 と同じつなぎ方)
//   別マイコンから Serial1 (RX=GPIO39, 115200bps) で1バイトずつ届く。bit7=0 なら1P、1 なら2P。
//   下位6bit は MSX の並び(bit0 上 / 1 下 / 2 左 / 3 右 / 4 トリガA / 5 トリガB、0=押下)。
//   PC-8801 は SSG の I/O ポート A(R14)= 方向、B(R15)= トリガ(PC88_REQUIREMENTS.md)。
// ---------------------------------------------------------------------------
static volatile uint8_t joyMsx[2] = { 0x7F, 0x7F };
static void updateJoystick() {
  uint8_t j[2] = { 0x7F, 0x7F };
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    j[(b & 0x80) ? 1 : 0] = b & 0x7F;              // bit6 = ボタン3
  }
  joyMsx[0] = j[0];
  joyMsx[1] = j[1];
}

// 起動メニュー・ファイル選択を1P のコントローラで操作する(MSX 版の JoyKeys.hpp と同じ)
//   'U' 'D' 'L' 'R' = 方向(押した瞬間と、0.4 秒押し続けたあと 0.12 秒ごと)、'A' = ボタン1、'B' = ボタン2、0 = なし
//   変換器は約 60Hz で送ってくる。0.1 秒届かなければ離したことにする
static int joyMenuPoll() {
  static uint8_t state = 0x7F, last = 0x7F;
  static uint32_t lastRx = 0, repeatAt = 0;
  uint32_t now = millis();
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    if (!(b & 0x80)) { state = b & 0x7F; lastRx = now; }
  }
  if (now - lastRx > 100) state = 0x7F;
  uint8_t pressed = (uint8_t)(~state & 0x7F), prev = (uint8_t)(~last & 0x7F);
  uint8_t edge = pressed & ~prev;
  last = state;
  if (edge & 0x10) return 'A';
  if (edge & 0x20) return 'B';
  static const char dirs[4] = { 'U', 'D', 'L', 'R' };
  for (int i = 0; i < 4; i++) {
    if (!(pressed & (1 << i))) continue;
    if (edge & (1 << i)) { repeatAt = now + 400; return dirs[i]; }
    if ((int32_t)(now - repeatAt) >= 0) { repeatAt = now + 120; return dirs[i]; }
  }
  return 0;
}
// 操作を PS/2 のスキャンコードに直す: ↑↓ = カーソル、←→ = PageUp/PageDown、ボタン1 = Enter、ボタン2 = ESC
static int joyMenuScancode(bool *e0) {
  *e0 = true;
  switch (joyMenuPoll()) {
    case 'U': return 0x75;
    case 'D': return 0x72;
    case 'L': return 0x7D;
    case 'R': return 0x7A;
    case 'A': *e0 = false; return 0x5A;
    case 'B': *e0 = false; return 0x76;
    default: return 0;
  }
}

// ---------------------------------------------------------------------------
//  ジョイスティック → キーボード(JoyToKey と同じ考え方。いつも有効)
//   1P の方向 = テンキーの 8 / 2 / 4 / 6、ボタン1〜3 = 起動メニューで決めたキー
//   (/PC88/JOYKEY.CFG に保存)。キーボードの押下とは別に持ち、毎フレーム AND して pc88_key にする。
// ---------------------------------------------------------------------------
static volatile uint8_t kbdKeys[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t joyButtonKey[3] = { 0x96, 0x17, 0x97 };   // 0xRB(R = ポート、B = ビット): SPACE / RETURN / ESC
static const char *JOYKEY_FILE = "/PC88/JOYKEY.CFG";

// SR のジョイスティック端子(SSG の I/O ポート)は再現しない(ユーザー指定。何もつないでいない = FFh)。
// 端子とテンキーを両方読むゲームでは上下の意味が逆のことがあり(スペースハリアー: テンキー 8 = 上、
// レバー上 = 下)、両方に出すと打ち消し合っていた。ジョイスティックはキーの割り当てだけで使う
static void joyToKeys() {
  uint8_t joy[16];
  memset(joy, 0xFF, sizeof(joy));
  uint8_t m = joyMsx[0];                           // 0 = 押下
  if (!(m & 0x01)) joy[0x1] &= (uint8_t)~0x01;     // ↑ → テンキー 8
  if (!(m & 0x02)) joy[0x0] &= (uint8_t)~0x04;     // ↓ → テンキー 2
  if (!(m & 0x04)) joy[0x0] &= (uint8_t)~0x10;     // ← → テンキー 4
  if (!(m & 0x08)) joy[0x0] &= (uint8_t)~0x40;     // → → テンキー 6
  for (int b = 0; b < 3; b++)
    if (!(m & (0x10 << b))) joy[joyButtonKey[b] >> 4] &= (uint8_t)~(1 << (joyButtonKey[b] & 7));
  for (int r = 0; r < 16; r++) pc88_key[r] = kbdKeys[r] & joy[r];
}

// PC-8801 のキーの名前(ポート 0〜11、ビット 0〜7)。起動メニューの表示用
static const char *const PC88_KEY_NAMES[12][8] = {
  { "NUM0", "NUM1", "NUM2", "NUM3", "NUM4", "NUM5", "NUM6", "NUM7" },
  { "NUM8", "NUM9", "NUM*", "NUM+", "NUM=", "NUM,", "NUM.", "RETURN" },
  { "@", "A", "B", "C", "D", "E", "F", "G" },
  { "H", "I", "J", "K", "L", "M", "N", "O" },
  { "P", "Q", "R", "S", "T", "U", "V", "W" },
  { "X", "Y", "Z", "[", "\\", "]", "^", "-" },
  { "0", "1", "2", "3", "4", "5", "6", "7" },
  { "8", "9", ":", ";", ",", ".", "/", "_" },
  { "HOME", "UP", "RIGHT", "INS/DEL", "GRPH", "KANA", "SHIFT", "CTRL" },
  { "STOP", "F1", "F2", "F3", "F4", "F5", "SPACE", "ESC" },
  { "TAB", "DOWN", "LEFT", "HELP", "COPY", "NUM-", "NUM/", "CAPS" },
  { "ROLL UP", "ROLL DOWN", "?", "?", "?", "?", "?", "?" },
};
static const char *pc88KeyName(uint8_t rb) { return (rb >> 4) < 12 ? PC88_KEY_NAMES[rb >> 4][rb & 7] : "?"; }

static void loadJoyKeys() {
  File f = SD.open(JOYKEY_FILE, FILE_READ);
  if (!f) return;
  uint8_t b[3];
  if (f.read(b, 3) == 3)
    for (int i = 0; i < 3; i++) if ((b[i] >> 4) < 12) joyButtonKey[i] = b[i];
  f.close();
}
static void saveJoyKeys() {
  File f = SD.open(JOYKEY_FILE, FILE_WRITE);
  if (!f) return;
  f.write(joyButtonKey, 3);
  f.close();
}

// ---------------------------------------------------------------------------
//  音: YM2203(FM 3ch + SSG 3ch)+ ビープ・SING(ポート 40h の bit5 / bit7。スピーカーは 1 本で、
//   BEEP = 2.4kHz の発振を出す / 止める、SING = CPU がスピーカーを直接オン/オフ。SR より前のソフトの
//   音楽・効果音(ハイドライドなど)は SING と BEEP を CPU で切り替えて鳴らす)
//   FM は ym2203.cpp、SSG は MSX2 版と同じ AY-3-8910 の作り(クロック 3.9936MHz/2)。
//   エミュレーション側(コア0)はレジスタの書き込みを「エミュレーションの時刻」付きでキューに積むだけで、
//   サウンド側(コア1 の loop)がキューを読んで、その時刻のサンプルで反映しながら音を作る。
//   (以前は時刻なしで、1ms ごとにまとめて反映していた。CPU は 1 フレームを 5〜15ms で走って残りは
//    待つので、フレームの中の書き込みの間隔が実際の 1/3〜4/5 に縮み、タイマー割込みで鳴らす曲の
//    音の並びが最大 ±6ms ほどがたついていた)
// ---------------------------------------------------------------------------
static const int SND_BUFFER_SIZE = 512;
static const int OPN_QUEUE = 1024;
// bit31-16 = 時刻(CPU 32 クロック単位の下位 16bit)、bit15-8 = レジスタ、bit7-0 = 値。
// 4KB なので setup でメイン RAM のページを内部 RAM に置いたあと(opnGen.begin)に確保する
static volatile uint32_t *opnQueue = nullptr;
static volatile int opnQHead = 0, opnQTail = 0;

// エミュレーションの時刻(CPU クロック)。cpuTask が 1 ラインごとに進める = 終わったラインの終わり。
// これより前の時刻の書き込みは全部キューに入っている
static volatile uint32_t opnEmuClock = 0;

static const uint8_t SPK_REG = 0xFF;             // キューの中でポート 40h(BEEP・SING)を表すレジスタ番号
static void soundQueuePush(uint8_t reg, uint8_t val) {
  int next = (opnQHead + 1) % OPN_QUEUE;
  if (next == opnQTail || !opnQueue) return;       // あふれたら捨てる(ふつうは起きない)
  uint32_t t = opnEmuClock + (uint32_t)pc88LineCycle();
  opnQueue[opnQHead] = ((t >> 5) << 16) | ((uint32_t)reg << 8) | val;
  opnQHead = next;
}
void pc88OpnWrite(uint8_t reg, uint8_t val) { soundQueuePush(reg, val); }
// ポート 40h の bit7(SING)・bit5(BEEP)。SING は数百 Hz〜数 kHz で切り替えられるので、OPN と同じく時刻付きで送る
void pc88Speaker(uint8_t bits) { soundQueuePush(SPK_REG, bits & 0xA0); }

class OpnGenerator : public fabgl::WaveformGenerator {
  Ym2203Fm *fm = nullptr;
  // ---- SSG: MSX2 版の AY38910Generator と同じ作り(ユーザー要望で移植) ----
  //   以前の作りは (1) トーンとノイズを OR で混ぜていた(AY は AND)、(2) 3ch の和が FM と足して
  //   ±127 を大きく超えて割れていた、ので音がおかしかった。
  //   1 刻み = AY の 16 クロック(AY のクロック = 3.9936MHz/2)。トーンは 1 刻みに 2 進めて TP で反転
  //   (周期 16×TP クロック)、ノイズは NP 刻みに 1 回、エンベロープは EP 刻みに 1 段(16 段で 256×EP)。
  const uint32_t psgClock = PC88_CPU_CLOCK / 2 / 16;
  uint32_t tickAccum = 0, tickStep = 0;
  uint8_t  regs[16] = { 0 };
  uint32_t tonePeriod[3] = { 1, 1, 1 }, noisePeriod = 1, envPeriod = 1;
  bool     holding = true, cont = false, alt = false, hold = false, att = false;
  uint32_t envCounter = 0, envStep = 0, envVol = 0;
  uint32_t toneCounter[3] = { 0, 0, 0 };
  uint32_t toneOut = 0, noiseCounter = 0, lfsr = 1, noiseOut = 0;
  // 音量は実機(YM2203 / YM2149)と同じく 1 段 3dB の対数(15 = 127、11 = -12dB、8 = -21dB)。
  // MSX2 版の表は直線に近く、Ys2 の伴奏(音量 8〜11)が実機より 2.5〜4 倍大きく出ていた
  const int16_t volTable[16] = { 0, 1, 1, 2, 3, 4, 6, 8, 11, 16, 22, 32, 45, 64, 90, 127 };
  int32_t  dcPrev = 0, dcOut = 0;                  // 直流を取る1次ハイパス(約10Hz)
  // FM の高音の補正(1次ローパス、y += 0.78 × (x − y)。4kHz −2.3dB、8kHz −3.9dB)。
  // 16384Hz で計算した FM は 8kHz を超える倍音・FB のノイズが折り返して、2kHz より上がチップ
  // (約 55kHz で計算して 7.5kHz で帯域制限した音)より 2〜5dB 強い。Ys2・アルファ・Daiva・LastArmageddon の
  // 比較で、帯域ごとの差が 1/2〜1/3 になる強さ。0 にすると補正なし
  #define FM_TREBLE_FIX 320
  int32_t  fmLp = 0;
  // ---- ビープ(2.4kHz の矩形波)と SING ----
  uint32_t beepPhase = 0, beepStep = 0;
  uint8_t  spk = 0;                                // ポート 40h の bit7(SING)・bit5(BEEP)
  // SING の切り替えがサンプルの途中だったときは、そのサンプルを切り替えの前後の長さで混ぜる
  // (16384Hz に丸めると 1kHz の音でも周期が 1 サンプルずつ揺れてざらつくため)。値は 1/256 単位
  bool     singPend = false;
  int32_t  singArea = 0, singRem = 0;
  static const int SPK_AMP = 60;                   // スピーカーの振れ幅(FM 1ch の 1/2 弱。以前のビープと同じ)

  // w = 次に作るサンプルの中で、切り替えのあとの長さ(0〜256)
  void spkWrite(uint8_t v, int32_t w) {
    bool oldSing = spk & 0x80, newSing = v & 0x80;
    spk = v;
    if (oldSing == newSing) return;
    if (!singPend) { singArea = oldSing ? 256 - w : 0; singRem = w; singPend = true; }
    else {
      if (w > singRem) w = singRem;
      if (oldSing) singArea += singRem - w;
      singRem = w;
    }
  }
  int16_t  buf[SND_BUFFER_SIZE];
  volatile int head = 0, tail = 0;
  int16_t  lastSample = 0;
  int32_t  fmBuf[64];

  void ssgWrite(uint8_t reg, uint8_t val) {
    reg &= 15;
    regs[reg] = val;
    switch (reg) {
      case 0: case 1: { uint32_t p = regs[0] | ((regs[1] & 0x0F) << 8); tonePeriod[0] = p ? p : 1; break; }
      case 2: case 3: { uint32_t p = regs[2] | ((regs[3] & 0x0F) << 8); tonePeriod[1] = p ? p : 1; break; }
      case 4: case 5: { uint32_t p = regs[4] | ((regs[5] & 0x0F) << 8); tonePeriod[2] = p ? p : 1; break; }
      case 6: noisePeriod = (regs[6] & 0x1F) ? (regs[6] & 0x1F) : 1; break;
      case 11: case 12: { uint32_t p = regs[11] | (regs[12] << 8); envPeriod = p ? p : 1; break; }
      case 13:
        envStep = 0; envCounter = 0;
        cont = val & 0x08; att = val & 0x04; alt = val & 0x02; hold = val & 0x01;
        envVol = att ? 0 : 15;
        holding = false;
        break;
    }
  }

  inline void stepEnvelope() {
    if (envStep < 15) {
      envStep++;
      envVol = att ? envStep : (15 - envStep);
    } else if (!cont) {
      holding = true; envVol = 0;
    } else if (hold) {
      holding = true;
      if (alt) att = !att;
      envVol = att ? 15 : 0;
    } else {
      envStep = 0;
      if (alt) att = !att;
      envVol = att ? 0 : 15;
    }
  }

  // 3ch の和(0〜381。直流分は fillBuffer のハイパスで取る)
  int ssgSample() {
    uint32_t r7 = regs[7], r8 = regs[8], r9 = regs[9], r10 = regs[10];
    uint32_t tp0 = tonePeriod[0], tp1 = tonePeriod[1], tp2 = tonePeriod[2], np = noisePeriod;
    tickAccum += tickStep;
    uint32_t ticks = tickAccum >> 16;
    tickAccum &= 0xFFFF;
    while (ticks--) {
      toneCounter[0] += 2; while (toneCounter[0] >= tp0) { toneCounter[0] -= tp0; toneOut ^= 1; }
      toneCounter[1] += 2; while (toneCounter[1] >= tp1) { toneCounter[1] -= tp1; toneOut ^= 2; }
      toneCounter[2] += 2; while (toneCounter[2] >= tp2) { toneCounter[2] -= tp2; toneOut ^= 4; }
      if (++noiseCounter >= np) {
        noiseCounter = 0;
        lfsr = (lfsr >> 1) | (((lfsr & 1) ^ ((lfsr >> 3) & 1)) << 16);
        noiseOut = lfsr & 1;
      }
      if (!holding && ++envCounter >= envPeriod) { envCounter = 0; stepEnvelope(); }
    }
    uint32_t mix = (toneOut | r7) & ((noiseOut ? 0x07 : 0) | (r7 >> 3));   // トーンとノイズは AND(AY と同じ)
    int volA = volTable[(r8 & 0x10) ? envVol : (r8 & 0x0F)];
    int volB = volTable[(r9 & 0x10) ? envVol : (r9 & 0x0F)];
    int volC = volTable[(r10 & 0x10) ? envVol : (r10 & 0x0F)];
    int s = 0;
    if (mix & 1) s += volA;
    if (mix & 2) s += volB;
    if (mix & 4) s += volC;
    return s;
  }

public:
  void begin(int rate) {
    opnQueue = (volatile uint32_t *)heap_caps_malloc(sizeof(uint32_t) * OPN_QUEUE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!opnQueue) opnQueue = (volatile uint32_t *)ps_malloc(sizeof(uint32_t) * OPN_QUEUE);
    fm = ym2203Create(PC88_CPU_CLOCK, rate);
    setSampleRate(rate);
  }
  void setSampleRate(int rate) {
    this->fabgl::WaveformGenerator::setSampleRate(rate);
    tickStep = (uint32_t)(((uint64_t)psgClock << 16) / (rate > 0 ? rate : 16384));
    beepStep = (uint32_t)(((uint64_t)2400 << 32) / (rate > 0 ? rate : 16384));
  }
  uint8_t ssgRead(uint8_t reg) { return regs[reg & 15]; }

  // キューの先頭の書き込みを反映(w = SING の切り替えの位置。applyQueued の説明は spkWrite)
  void applyQueued(int32_t w = 256) {
    uint32_t e = opnQueue[opnQTail];
    opnQTail = (opnQTail + 1) % OPN_QUEUE;
    uint8_t r = (e >> 8) & 0xFF, v = e & 0xFF;
    if (r == SPK_REG) spkWrite(v, w);
    else if (r < 0x10) ssgWrite(r, v); else ym2203WriteReg(fm, r, v);
  }

  // 書き込みの時刻合わせ: 次に作るサンプルのエミュレーションの時刻 sndClock(CPU クロック)。1 サンプル
  // 243.75 クロックずつ進め、時刻がそこまで来た書き込みを反映する。sndClock は CPU が終えた時刻
  // (opnEmuClock)を越えない(越えそうなら止めて待つ = CPU が遅れたときは今までどおり届いたときに反映)。
  // CPU が取り戻しで何フレームも続けて走り 1.25 フレームより先に行ったら、溜まった分をまとめて反映して追いつく
  uint32_t sndClock = 0, sndFrac = 0;
  void advanceClock(int n, uint32_t emu) {
    uint32_t f = sndFrac + (uint32_t)n * 49152u;        // 0.75 クロック = 49152 / 65536
    sndClock += (uint32_t)n * 243u + (f >> 16);
    sndFrac = f & 0xFFFF;
    if ((int32_t)(sndClock - emu) > 0) { sndClock = emu; sndFrac = 0; }
  }

  void fillBuffer() {
    if (!fm) return;
    const uint32_t frameClk = PC88_CYCLES_PER_LINE * PC88_LINES_PER_FRAME;
    uint32_t emu = opnEmuClock;
    if ((int32_t)(emu - sndClock) > (int32_t)(frameClk + frameClk / 4)) {
      while (opnQTail != opnQHead) applyQueued();
      sndClock = emu - frameClk; sndFrac = 0;
    }
    int space = (tail - head - 1 + SND_BUFFER_SIZE) % SND_BUFFER_SIZE;
    while (space > 0) {
      int n = space > 64 ? 64 : space;
      // 時刻が来た書き込みを反映し、次の書き込みの時刻でブロックを切る
      while (opnQTail != opnQHead) {
        uint32_t t16 = opnQueue[opnQTail] >> 16;
        int32_t d = (int16_t)(t16 - (sndClock >> 5));                 // 32 クロック単位
        if (d > 0) {
          if ((int16_t)(t16 - (emu >> 5)) <= 0) {                     // (CPU がまだ終えていないラインの書き込みなら、今回は届かない)
            int m = (int)(((d << 5) + 243) / 244);                    // そこまでのサンプル数(切り上げ)
            if (m < n) n = m;
          }
          break;
        }
        int32_t w = (-d * 32 * 256 + 122) / 244;                        // サンプルの中で書き込みのあとの長さ
        applyQueued(w > 256 ? 256 : w);
      }
      advanceClock(n, emu);
      ym2203Generate(fm, fmBuf, n);
      for (int i = 0; i < n; i++) {
        // FM 1ch 最大 ±64、SSG 1ch 最大 0〜85(振れ幅 FM 1ch の 2/3)。Common Source Project(fmgen、
        // FM・PSG とも 0dB)と同じ比: fmgen の SSG 音量 15 は ±0x4000/3、FM のオペレータは最大 ±8192。
        // (MAME の比 = SSG が FM の約2倍 にしたら Ys2 のオープニング後半の FM のメロディが SSG の伴奏に
        //  埋もれた。その前の 1/3 では前半の SSG のメロディが FM の和音に埋もれていた)
        // 直流をハイパスで取ってから、±96 を超えた分を 1/3 に縮め、±127 で切る
#if FM_TREBLE_FIX
        fmLp += ((fmBuf[i] - fmLp) * FM_TREBLE_FIX) >> 8;
#else
        fmLp = fmBuf[i];
#endif
        int32_t x = ((fmLp >> 7) + ((ssgSample() * 171) >> 8)) << 8;
        if (spk | singPend) {                          // スピーカー: BEEP の発振と SING の OR
          int32_t lv;
          if (singPend) { lv = singArea + ((spk & 0x80) ? singRem : 0); singPend = false; }
          else lv = (spk & 0x80) ? 256 : 0;
          if (spk & 0x20) { beepPhase += beepStep; if (beepPhase & 0x80000000u) lv = 256; }
          x += lv * SPK_AMP;
        }
        dcOut = x - dcPrev + dcOut - (dcOut >> 8);
        dcPrev = x;
        int32_t s = dcOut >> 8;
        if (s > 96) s = 96 + (s - 96) / 3; else if (s < -96) s = -96 + (s + 96) / 3;
        if (s > 127) s = 127; else if (s < -128) s = -128;
        buf[head] = (int16_t)s;
        head = (head + 1) % SND_BUFFER_SIZE;
      }
      space -= n;
    }
  }
  int getSample() override {
    if (head == tail) return lastSample;
    lastSample = buf[tail];
    tail = (tail + 1) % SND_BUFFER_SIZE;
    return lastSample;
  }
  void setFrequency(int value) override {}
};
OpnGenerator opnGen;

// SSG の I/O ポート A/B(R14/R15)の読み出し。ジョイスティック端子は再現しないので何もつないでいない値。
// (R0〜R13 は pc88_core.cpp が CPU 側の写しから返す)
uint8_t pc88SsgRead(uint8_t reg) {
  return reg >= 14 ? 0xFF : opnGen.ssgRead(reg);
}

// ---------------------------------------------------------------------------
//  メモリ
// ---------------------------------------------------------------------------
#define NUM_DRIVES 2
static Pc88Drive drives[NUM_DRIVES];
static String drivePath[NUM_DRIVES];                 // 入っているファイル(SD のフルパス)
static uint32_t driveOfs[NUM_DRIVES];                // そのイメージのファイルの中の位置(書き戻し用)
static Pc88Memory mem;
static Pc88Config config;
static volatile bool isPaused = false;

static void flashStatus(const char *msg) { Serial.println(msg); }

// 一時停止中などに画面のいちばん下に1行出す(ユーザー要望で上から下へ)
static void showStatus(const char *msg) {
  canvas->selectFont(&fabgl::FONT_8x8);
  canvas->setBrushColor(fabgl::Color::Blue);
  canvas->fillRectangle(0, 190, 639, 199);
  canvas->setPenColor(fabgl::Color::BrightWhite);
  canvas->drawText(4, 191, msg);
  canvas->waitCompletion();
}

// ---------------------------------------------------------------------------
//  SD
// ---------------------------------------------------------------------------
static uint8_t *loadRom(const char *path, uint32_t size, bool internal) {
  File f = SD.open(path, FILE_READ);
  if (!f) return nullptr;
  uint8_t *p = internal ? (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) : nullptr;
  if (!p) p = (uint8_t *)ps_malloc(size);
  if (!p) { f.close(); return nullptr; }
  memset(p, 0xFF, size);
  uint32_t done = 0;
  while (done < size) {
    size_t n = f.read(p + done, min((size_t)4096, (size_t)(size - done)));
    if (n == 0) break;
    done += n;
  }
  f.close();
  Serial.printf("ROM %s: %u bytes\n", path, (unsigned)done);
  return p;
}


static int listD88Images(const String &path, D88Image *out, int max) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return 0;
  uint32_t total = f.size(), o = 0;
  int n = 0;
  uint8_t h[0x20];
  while (n < max && o + 0x2B0 <= total) {
    if (!f.seek(o) || f.read(h, 0x20) != 0x20) break;
    uint32_t sz = h[0x1C] | (h[0x1D] << 8) | (h[0x1E] << 16) | ((uint32_t)h[0x1F] << 24);
    if (sz < 0x2B0 || o + sz > total) {
      if (n > 0) break;
      sz = total - o;                              // 大きさが壊れている1枚もの: ファイル全体
    }
    out[n].ofs = o;
    out[n].size = sz;
    for (int i = 0; i < 17; i++) out[n].name[i] = (h[i] >= 0x20 && h[i] < 0x7F) ? (char)h[i] : (h[i] ? '?' : 0);
    out[n].name[17] = 0;
    n++;
    o += sz;
  }
  f.close();
  return n;
}

// path(SD のフルパス)の image 枚目をドライブ d に入れる
static bool loadD88ToDrive(int d, const String &path, int image) {
  Pc88Drive &drv = drives[d];
  D88Image imgs[D88_MAX_IMAGES];
  int n = listD88Images(path, imgs, D88_MAX_IMAGES);
  if (image < 0 || image >= n || imgs[image].size > PC88_D88_MAX) return false;
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;
  uint32_t sz = imgs[image].size, done = 0;
  f.seek(imgs[image].ofs);
  pc88EjectD88(drv);
  while (done < sz) {
    size_t r = f.read(drv.buffer + done, min((size_t)4096, (size_t)(sz - done)));
    if (r == 0) break;
    done += r;
  }
  f.close();
  bool ok = pc88LoadD88(drv, drv.buffer, done);
  if (ok) { drivePath[d] = path; driveOfs[d] = imgs[image].ofs; }
  else drivePath[d] = "";
  Serial.printf("D88 %s [%d/%d]: %u bytes %s\n", path.c_str(), image + 1, n, (unsigned)done, ok ? "OK" : "NG");
  return ok;
}

// ドライブ d のイメージを、ファイルの元の位置へ書き戻す(ほかのイメージはそのまま)
static void writeBackD88(int d) {
  if (drivePath[d] == "" || !drives[d].inserted) { showStatus("NO DISK"); return; }
  File f = SD.open(drivePath[d].c_str(), "r+");
  bool ok = f && f.seek(driveOfs[d]) && f.write(drives[d].buffer, drives[d].size) == drives[d].size;
  if (f) f.close();
  if (ok) drives[d].dirty = false;
  showStatus(ok ? "D88 WRITTEN BACK" : "D88 WRITE BACK FAILED");
  flashStatus(ok ? "D88 WRITTEN BACK" : "D88 WRITE BACK FAILED");
}

// ---------------------------------------------------------------------------
//  ファイル選択と起動メニュー(MSX2 / X1 と同じ形)
// ---------------------------------------------------------------------------
struct D88Item { String name; int kind; };      // kind: 0 = ".." 1 = フォルダ 2 = D88 3 = <NONE>
static std::vector<D88Item> d88Items;
static const char *ITEM_NONE = "<NONE>";

static void listD88Files(const String &path, bool canGoUp) {
  d88Items.clear();
  File dir = SD.open(path.c_str());
  if (!dir) return;
  std::vector<D88Item> dirs, files;
  File e;
  while ((e = dir.openNextFile())) {
    String name = e.name();
    int s = name.lastIndexOf('/');
    if (s >= 0) name = name.substring(s + 1);
    if (name.length() && name[0] != '.' && name != "System Volume Information") {
      if (e.isDirectory()) dirs.push_back({ name, 1 });
      else {
        String upper = name; upper.toUpperCase();
        if (upper.endsWith(".D88")) files.push_back({ name, 2 });
      }
    }
    e.close();
  }
  dir.close();
  auto byName = [](const D88Item &a, const D88Item &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  if (canGoUp) d88Items.push_back({ "..", 0 });
  d88Items.insert(d88Items.end(), dirs.begin(), dirs.end());
  d88Items.insert(d88Items.end(), files.begin(), files.end());
}

// /PC88 の中の D88 を選ぶ(フォルダにも入れる)。返す値: /PC88 からの相対パス、"<NONE>"、ESC なら ""
static String selectD88File(const char *title, const String &current) {
  const String base = "/PC88";
  String sub = "", want = current;
  int slash = current.lastIndexOf('/');
  if (current != ITEM_NONE && slash >= 0) { sub = current.substring(0, slash); want = current.substring(slash + 1); }
  canvas->selectFont(&fabgl::FONT_8x8);
  auto kb = PS2Controller.keyboard();
  const int visible = 20;
  for (;;) {
    String path = base + (sub == "" ? "" : "/" + sub);
    listD88Files(path, sub != "");
    if (sub == "") d88Items.insert(d88Items.begin(), { String(ITEM_NONE), 3 });
    else if (d88Items.empty()) { sub = ""; want = ""; continue; }
    int cursor = 0;
    for (int i = 0; i < (int)d88Items.size(); i++) if (d88Items[i].name == want) { cursor = i; break; }
    bool isBreak = false, isE0 = false, changeDir = false;
    // 画面全体を描くのはフォルダに入ったときだけ。カーソルを動かしたら前と今の2行、
    // スクロールしたら一覧の部分だけ描き直す(全部消して描くとちらつく)
    int shownTop = -1, shownCursor = -1;
    int listTop = 0;                               // 一覧のいちばん上の項目(カーソルが画面の外に出たときだけ動かす)
    auto drawItem = [&](int idx, int top) {
      int y = 36 + (idx - top) * 8;
      canvas->setBrushColor(fabgl::Color::Blue);
      canvas->fillRectangle(8, y, 639, y + 7);
      if (idx < 0 || idx >= (int)d88Items.size()) return;
      const D88Item &it = d88Items[idx];
      String label = (it.kind >= 2) ? it.name : it.name + "/";
      canvas->setPenColor(idx == cursor ? fabgl::Color::BrightYellow
                          : it.kind >= 2 ? fabgl::Color::BrightWhite : fabgl::Color::BrightCyan);
      canvas->drawText(16, y, label.c_str());
    };
    auto draw = [&]() {
      // スクロールはカーソルが画面の上端・下端を越えたときだけ(以前は一番下まで行ってから上へ戻すと、
      // カーソルの位置はそのままで、すぐ一覧のほうが動いた)
      if (cursor < listTop) listTop = cursor;
      if (cursor >= listTop + visible) listTop = cursor - visible + 1;
      int top = listTop;
      if (shownTop < 0) {
        canvas->setBrushColor(fabgl::Color::Blue);
        canvas->clear();
        canvas->setPenColor(fabgl::Color::White);
        canvas->drawText(8, 8, title);
        canvas->drawText(8, 16, "UP/DOWN:SELECT ENTER:OK BS:UP ESC:CANCEL");
        String shown = path + "/";
        if (shown.length() > 76) shown = "..." + shown.substring(shown.length() - 73);
        canvas->setPenColor(fabgl::Color::Cyan);
        canvas->drawText(8, 24, shown.c_str());
      }
      if (shownTop < 0 || top != shownTop) {
        for (int i = 0; i < visible; i++) drawItem(top + i, top);
      } else if (cursor != shownCursor) {
        drawItem(shownCursor, top);
        drawItem(cursor, top);
      }
      shownTop = top;
      shownCursor = cursor;
    };
    draw();
    while (!changeDir) {
      int jsc = 0;                                 // キーボードが無ければ1P のコントローラ
      if (!kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
      if (jsc || kb->scancodeAvailable()) {
        int sc = jsc ? jsc : kb->getNextScancode(0);
        if (sc == 0xF0) { isBreak = true; continue; }
        if (sc == 0xE0) { isE0 = true; continue; }
        if (!isBreak) {
          int n = (int)d88Items.size();
          if (sc == 0x75 && isE0) { if (cursor > 0) cursor--; draw(); }
          else if (sc == 0x72 && isE0) { if (cursor < n - 1) cursor++; draw(); }
          else if (sc == 0x7D && isE0) { cursor = cursor - visible < 0 ? 0 : cursor - visible; draw(); }
          else if (sc == 0x7A && isE0) { cursor = cursor + visible >= n ? n - 1 : cursor + visible; draw(); }
          else if ((sc == 0x66 && sub != "") || (sc == 0x5A && d88Items[cursor].kind == 0)) {
            int p = sub.lastIndexOf('/');
            want = p >= 0 ? sub.substring(p + 1) : sub;
            sub = p >= 0 ? sub.substring(0, p) : "";
            changeDir = true;
          } else if (sc == 0x5A && d88Items[cursor].kind == 1) {
            sub = sub == "" ? d88Items[cursor].name : sub + "/" + d88Items[cursor].name;
            want = "";
            changeDir = true;
          } else if (sc == 0x5A) {
            const D88Item &it = d88Items[cursor];
            return (it.kind == 3 || sub == "") ? it.name : sub + "/" + it.name;
          } else if (sc == 0x76) return "";
        }
        isBreak = false; isE0 = false;
      }
      vTaskDelay(1);
    }
  }
}

// 1つのファイルに何枚も入った D88 から1枚選ぶ。ESC なら -1
static int selectD88Image(const char *title, const D88Image *imgs, int n, int current) {
  auto kb = PS2Controller.keyboard();
  int cursor = current < 0 || current >= n ? 0 : current;
  bool isBreak = false, isE0 = false, redraw = true;
  int shownCursor = -1;                            // -1 = 画面全体を描く。あとは変わった2行だけ
  canvas->selectFont(&fabgl::FONT_8x8);
  auto drawLine = [&](int i) {
    char line[48];
    snprintf(line, sizeof(line), "%2d: %s", i + 1, imgs[i].name[0] ? imgs[i].name : "(NO NAME)");
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->fillRectangle(8, 36 + i * 8, 639, 36 + i * 8 + 7);
    canvas->setPenColor(i == cursor ? fabgl::Color::BrightYellow : fabgl::Color::BrightWhite);
    canvas->drawText(16, 36 + i * 8, line);
  };
  for (;;) {
    if (redraw) {
      if (shownCursor < 0) {
        canvas->setBrushColor(fabgl::Color::Blue);
        canvas->clear();
        canvas->setPenColor(fabgl::Color::White);
        canvas->drawText(8, 8, title);
        canvas->drawText(8, 16, "THIS FILE HAS SEVERAL DISKS. UP/DOWN:SELECT ENTER:OK ESC:CANCEL");
        for (int i = 0; i < n; i++) drawLine(i);
      } else if (shownCursor != cursor) {
        drawLine(shownCursor);
        drawLine(cursor);
      }
      shownCursor = cursor;
      redraw = false;
    }
    int jsc = 0;                                   // キーボードが無ければ1P のコントローラ
    if (!kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
    if (jsc || kb->scancodeAvailable()) {
      int sc = jsc ? jsc : kb->getNextScancode(0);
      if (sc == 0xF0) { isBreak = true; continue; }
      if (sc == 0xE0) { isE0 = true; continue; }
      if (!isBreak) {
        if (sc == 0x75 && isE0) { if (cursor > 0) cursor--; redraw = true; }
        else if (sc == 0x72 && isE0) { if (cursor < n - 1) cursor++; redraw = true; }
        else if (sc == 0x5A) return cursor;
        else if (sc == 0x76) return -1;
      }
      isBreak = false; isE0 = false;
    }
    vTaskDelay(1);
  }
}

// ドライブ d に入れるディスクを選ぶ(ファイル → 何枚目か)。file は /PC88 からの相対パス、"" = なし。
// 選び直さなかった(ESC)ときは false。drive 1 で複数枚のファイルを選び、ドライブ2が空なら次の1枚をそちらへ
// 選択画面は、いちばん最近どちらかのドライブに選んだディスクのフォルダから開く(ユーザー要望:
// 片方のドライブでフォルダ(または何枚も入った D88)を選んだら、もう一方も同じところから)。
// このドライブのディスクがそのフォルダにあればカーソルはそれに、なければ最近選んだファイルに置く。
// もう一方のドライブと同じ「何枚も入った D88」を選んだときは、何枚目かの初期値をその次(ドライブ1 なら前)にする
static String bootDisk[NUM_DRIVES];
static int bootImage[NUM_DRIVES];
static String lastChosen;                          // 最近選んだディスク(/PC88 からの相対パス)
static String dirOf(const String &f) { int s = f.lastIndexOf('/'); return s >= 0 ? f.substring(0, s) : String(""); }

static bool chooseDisk(int d, String &file, int &image, const String &otherFile, int otherImage) {
  String recent = lastChosen != "" ? lastChosen : otherFile;
  String start = file;
  if (recent != "" && (file == "" || dirOf(file) != dirOf(recent))) start = recent;
  String sel = selectD88File(d ? "PC-88  SELECT D88 FOR DRIVE 2" : "PC-88  SELECT D88 FOR DRIVE 1",
                             start == "" ? String(ITEM_NONE) : start);
  if (sel == "") return false;
  if (sel == ITEM_NONE) { file = ""; image = 0; return true; }
  D88Image imgs[D88_MAX_IMAGES];
  int n = listD88Images("/PC88/" + sel, imgs, D88_MAX_IMAGES);
  int img = 0;
  if (n > 1) {
    int def = (sel == file) ? image : (sel == otherFile) ? otherImage + (d == 1 ? 1 : -1) : (d == 1 ? 1 : 0);
    if (def < 0) def = 0;
    if (def >= n) def = n - 1;
    img = selectD88Image(sel.c_str(), imgs, n, def);
    if (img < 0) return false;
  }
  file = sel;
  image = img;
  lastChosen = sel;
  return true;
}

// ドライブ d に今入っているディスク(/PC88 からの相対パスと何枚目か。なければ "")
static void driveDisk(int d, String &file, int &image) {
  file = drivePath[d].startsWith("/PC88/") ? drivePath[d].substring(6) : String("");
  image = 0;
  if (file != "") {
    D88Image imgs[D88_MAX_IMAGES];
    int n = listD88Images(drivePath[d], imgs, D88_MAX_IMAGES);
    for (int i = 0; i < n; i++) if (imgs[i].ofs == driveOfs[d]) image = i;
  }
}

static const char *modeName[4] = { "N88-BASIC V2", "N88-BASIC V1 (S)", "N88-BASIC V1 (H)", "N-BASIC" };

// 起動メニューの各行に出ている内容(選択の状態 + 文字)を覚えておき、変わった行だけ描き直す。
// 毎回画面全体を消して描くとちらつく(ユーザー指摘)。0〜8 = 項目、9 = 案内、10 = メッセージ
static char menuShown[11][100];
static bool menuValid = false;
static void bootMenuInvalidate() { menuValid = false; }   // ファイル選択などで画面を上書きしたあと

// capture = 押されたキーを待っているボタン(4〜6。-1 = なし)
static void bootMenuDraw(int cursor, const char *msg, int capture = -1) {
  const int X0 = 40, X1 = 599, Y0 = 36, DY = 13;
  canvas->selectFont(&fabgl::FONT_8x8);
  if (!menuValid) {
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->clear();
    canvas->setPenColor(fabgl::Color::BrightWhite);
    canvas->drawText(X0, 14, "PC-8801mkIISR BOOT MENU");
    memset(menuShown, 0, sizeof(menuShown));
    menuValid = true;
  }
  for (int i = 0; i < 9; i++) {
    char line[96];
    switch (i) {
      case 0: case 1: {
        D88Image imgs[D88_MAX_IMAGES];
        int n = bootDisk[i] == "" ? 0 : listD88Images("/PC88/" + bootDisk[i], imgs, D88_MAX_IMAGES);
        if (n > 1) snprintf(line, sizeof(line), "Disk%d : %.50s [%d/%d]", i + 1, bootDisk[i].c_str(), bootImage[i] + 1, n);
        else snprintf(line, sizeof(line), "Disk%d : %.60s", i + 1, bootDisk[i].c_str());
        break;
      }
      case 2: snprintf(line, sizeof(line), "Mode  : %s", modeName[config.mode]); break;
      case 4: case 5: case 6:
        if (i == capture) snprintf(line, sizeof(line), "Joy B%d: PRESS A KEY...", i - 3);
        else snprintf(line, sizeof(line), "Joy B%d: %s", i - 3, pc88KeyName(joyButtonKey[i - 4]));
        break;
      case 8: snprintf(line, sizeof(line), "Run"); break;
      default: continue;
    }
    bool sel = (i == cursor);
    char shown[100];                               // 1 文字目 = 状態(N 通常 / S 選択 / C キー待ち)
    snprintf(shown, sizeof(shown), "%c%s", sel ? (i == capture ? 'C' : 'S') : 'N', line);
    if (strcmp(shown, menuShown[i]) == 0) continue;
    strcpy(menuShown[i], shown);
    int y = Y0 + i * DY;
    // 選択中は反転。キー待ちのボタンは黄色の地。行の地を塗ってから文字を書く
    canvas->setBrushColor(!sel ? fabgl::Color::Blue : i == capture ? fabgl::Color::BrightYellow : fabgl::Color::BrightWhite);
    canvas->fillRectangle(X0 - 4, y - 3, X1, y + 9);
    canvas->setPenColor(sel ? fabgl::Color::Blue : fabgl::Color::BrightWhite);
    canvas->drawText(X0, y, line);
  }
  const char *help = capture >= 0 ? "PRESS THE KEY FOR THIS JOYSTICK BUTTON (F6-F12: CANCEL)"
                                  : "UP/DOWN: SELECT   ENTER: CHANGE / RUN   (JOY: 8/2/4/6 = UP/DN/LT/RT)";
  const char *texts[2] = { help, msg ? msg : "" };
  for (int k = 0; k < 2; k++) {
    if (strcmp(texts[k], menuShown[9 + k]) == 0) continue;
    snprintf(menuShown[9 + k], sizeof(menuShown[0]), "%s", texts[k]);
    int y = k ? 200 - 20 : 200 - 34;
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->fillRectangle(X0 - 4, y - 1, 639, y + 8);
    canvas->setPenColor(k ? fabgl::Color::BrightWhite : fabgl::Color::BrightCyan);
    canvas->drawText(X0, y, texts[k]);
  }
}

static void bootMenu() {
  auto kb = PS2Controller.keyboard();
  const int items[7] = { 0, 1, 2, 4, 5, 6, 8 };
  const int NITEMS = 7;
  int pos = 0, capture = -1;
  const char *msg = nullptr;
  bool isBreak = false, isE0 = false, redraw = true, joyChanged = false;
  bootMenuInvalidate();
  for (;;) {
    if (redraw) { bootMenuDraw(items[pos], msg, capture); redraw = false; }
    int jsc = 0;                                   // キーボードが無ければ1P のコントローラ(キーの割り当て中は使わない)
    if (capture < 0 && !kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
    if (jsc || kb->scancodeAvailable()) {
      int sc = jsc ? jsc : kb->getNextScancode(0);
      if (sc == 0xF0) { isBreak = true; continue; }
      if (sc == 0xE0) { isE0 = true; continue; }
      if (!isBreak && capture >= 0) {              // ジョイスティックのボタンに割り当てるキー
        for (int i = 0; i < pc88KeyMapCount; i++)
          if (pc88KeyMapTable[i].ps2 == sc && pc88KeyMapTable[i].ext == isE0) {
            joyButtonKey[capture - 4] = (uint8_t)((pc88KeyMapTable[i].row << 4) | pc88KeyMapTable[i].bit);
            joyChanged = true;
            break;
          }
        capture = -1;                              // PC-8801 に無いキー(F6〜F12 など)なら取り消し
        redraw = true;
      } else if (!isBreak) {
        if (sc == 0x75 && isE0) { pos = (pos + NITEMS - 1) % NITEMS; redraw = true; }
        else if (sc == 0x72 && isE0) { pos = (pos + 1) % NITEMS; redraw = true; }
        else if (sc == 0x5A && items[pos] >= 4 && items[pos] <= 6) { capture = items[pos]; redraw = true; }
        else if (sc == 0x5A) {
          int it = items[pos];
          msg = nullptr;
          if (it == 0 || it == 1) {
            bootMenuInvalidate();                  // ファイル選択の画面で上書きされる
            if (chooseDisk(it, bootDisk[it], bootImage[it], bootDisk[it ^ 1], bootImage[it ^ 1]) && it == 0 && bootDisk[0] != "" && bootDisk[1] == "") {
              D88Image imgs[D88_MAX_IMAGES];
              if (bootImage[0] + 1 < listD88Images("/PC88/" + bootDisk[0], imgs, D88_MAX_IMAGES)) {
                bootDisk[1] = bootDisk[0];
                bootImage[1] = bootImage[0] + 1;
              }
            }
          } else if (it == 2) {
            config.mode = (Pc88BasicMode)((config.mode + 1) % 4);
          } else {
            bootMenuDraw(it, "LOADING...");
            for (int d = 0; d < NUM_DRIVES && !msg; d++)
              if (bootDisk[d] != "" && !loadD88ToDrive(d, "/PC88/" + bootDisk[d], bootImage[d])) msg = d ? "DISK2 LOAD ERROR" : "DISK1 LOAD ERROR";
            if (!msg) break;
          }
          redraw = true;
        }
      }
      isBreak = false; isE0 = false;
    }
    vTaskDelay(1);
  }
  if (joyChanged) saveJoyKeys();
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
}

// 一時停止中の F9 / F10: ドライブ d のディスクを入れ替える
static void swapDisk(int d) {
  String file, other;
  int image, otherImage;
  driveDisk(d, file, image);
  driveDisk(d ^ 1, other, otherImage);
  if (chooseDisk(d, file, image, other, otherImage)) {
    if (file == "") { pc88EjectD88(drives[d]); drivePath[d] = ""; }
    else if (!loadD88ToDrive(d, "/PC88/" + file, image)) { showStatus("D88 LOAD ERROR"); return; }
  }
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
  showStatus("PAUSED  F12:RESUME  F10/F11:DISK  SHIFT+F10/F11:WRITE BACK  SHIFT+F12:RESET");
}
// ---------------------------------------------------------------------------
//  キーボード: PS/2 → PC-8801 のキーマトリクス(ポート 00h〜0Eh)
//  並びは PC88_REQUIREMENTS.md §3 の表。日本語 106 キーボードの位置に合わせる。
// ---------------------------------------------------------------------------

static void updateKeyboard() {
  static bool isBreak = false, isE0 = false, isShift = false, f12Held = false, f10Held = false, f11Held = false;
  static bool resetArmed = false;                    // Shift+F12 で「書き戻していない」と知らせたあと
  auto kb = PS2Controller.keyboard();
  if (!kb) return;
  int count = kb->scancodeAvailable();
  while (count-- > 0) {
    int sc = kb->getNextScancode(0);
    if (sc == -1) break;
    if (sc == 0xF0) { isBreak = true; continue; }
    if (sc == 0xE0) { isE0 = true; continue; }
    if (sc == 0x12 || sc == 0x59) isShift = !isBreak;
    if (sc == 0x07 && !isE0) {                       // F12: 一時停止 / Shift+F12: リセット
      if (!isBreak && !f12Held) {
        f12Held = true;
        if (isShift) {                               // リセットは ESP32 ごと(ユーザー要望)
          // SD に書き戻していないディスクの書き込みがあれば、1回目は止めて知らせる(2回目で再起動)
          bool dirty = false;
          for (int d = 0; d < NUM_DRIVES; d++) dirty |= drives[d].inserted && drives[d].dirty;
          if (dirty && !resetArmed) {
            resetArmed = true;
            isPaused = true;
            vTaskDelay(pdMS_TO_TICKS(40));
            showStatus("DISK CHANGED!  SHIFT+F10/F11:WRITE BACK  SHIFT+F12:RESET ANYWAY  F12:RESUME");
          } else esp_restart();
        } else {
          isPaused = !isPaused;
          resetArmed = false;
          if (isPaused) { vTaskDelay(pdMS_TO_TICKS(40)); showStatus("PAUSED  F12:RESUME  F10/F11:DISK  SHIFT+F10/F11:WRITE BACK  SHIFT+F12:RESET"); }
          else pc88VideoInvalidate();                // 一時停止の表示やディスク選択で上書きした画面を描き直す
        }
      } else if (isBreak) f12Held = false;
      isBreak = isE0 = false;
      continue;
    }
    if ((sc == 0x09 || sc == 0x78) && !isE0) {      // 一時停止中の F10 / F11: ディスクの入れ替え、Shift で SD へ書き戻し(MSX2 と同じ)
      bool &held = (sc == 0x09) ? f10Held : f11Held;
      int d = (sc == 0x09) ? 0 : 1;
      if (!isBreak && !held) {
        held = true;
        if (isPaused) {
          if (isShift) writeBackD88(d);
          else {
            swapDisk(d);
            // 選択画面がキーを離した信号を読んでしまうので、押したままの扱いを消す
            // (残すと次の F10/F11 が1回目だけ無視される)。カナ・CAPS のロックは残す。
            held = isShift = false;
            for (int r = 0; r < 16; r++) {
              uint8_t keep = (r == 0x8) ? 0x20 : (r == 0xA) ? 0x80 : 0x00;
              kbdKeys[r] = (uint8_t)(kbdKeys[r] | ~keep);
            }
          }
        }
      } else if (isBreak) held = false;
      isBreak = isE0 = false;
      continue;
    }
    for (int i = 0; i < pc88KeyMapCount && (!isPaused || isBreak); i++) {   // 一時停止中は押下を伝えない(離したのは伝える)
      if (pc88KeyMapTable[i].ps2 == sc && pc88KeyMapTable[i].ext == isE0) {
        uint8_t row = pc88KeyMapTable[i].row, m = (uint8_t)(1 << pc88KeyMapTable[i].bit);
        // カナ(08h bit5)と CAPS(0Ah bit7)は実機ではロック式のキー: 押すたびに入/切
        bool lock = (row == 0x8 && m == 0x20) || (row == 0xA && m == 0x80);
        if (lock) {
          static bool held[2];                       // キーリピートで何度も切り替わらないように
          bool &h = held[row == 0xA ? 1 : 0];
          if (!isBreak && !h) { kbdKeys[row] ^= m; h = true; }
          else if (isBreak) h = false;
        }
        else if (isBreak) kbdKeys[row] |= m;
        else kbdKeys[row] &= (uint8_t)~m;
      }
    }
    isBreak = isE0 = false;
  }
}

// ---------------------------------------------------------------------------
//  CPU タスク(コア0、262ライン/フレーム)
// ---------------------------------------------------------------------------
void cpuTask(void *) {
  const int64_t frameUs = PC88_FRAME_US;             // 24kHz モニタの 55.4Hz
  int64_t next = esp_timer_get_time();
#ifdef USE_SERIAL_DEBUG
  uint32_t sum = 0, mx = 0, over = 0, n = 0;
#endif
  for (;;) {
    if (isPaused) {
      vTaskDelay(pdMS_TO_TICKS(10));
      next = esp_timer_get_time();
      continue;
    }
    next += frameUs;
    int64_t t0 = esp_timer_get_time();
#ifdef USE_SERIAL_DEBUG
    uint32_t z80Clk = 0;
#endif
    for (int i = 0; i < PC88_LINES_PER_FRAME; i++) {
#ifdef USE_SERIAL_DEBUG
      uint32_t c0 = ESP.getCycleCount();
      ExecZ80(&cpu, CPU_CYCLES_PER_LINE);
      z80Clk += ESP.getCycleCount() - c0;
#else
      ExecZ80(&cpu, CPU_CYCLES_PER_LINE);
#endif
      pc88Line(i);
      opnEmuClock += CPU_CYCLES_PER_LINE;              // 音の書き込みの時刻(このラインまで終わった)
      if (i == PC88_DISPLAY_LINES) frameReady = true;
      if (pc88IrqPending() && (cpu.IFF & IFF_1)) IntZ80(&cpu, pc88IrqAck());
    }
    updateJoystick();
    joyToKeys();                                     // キーボード + ジョイスティック → pc88_key
    int64_t now = esp_timer_get_time();
#ifdef USE_SERIAL_DEBUG
    // 内訳: z80 = 本体の Z80(その中で 8255 を読み書きしたときのディスクユニットの Z80 も含む)、
    //       render = 描画、disk = 行末のディスクユニット(サブの Z80 と FDC)
    static uint64_t zSum, rSum, dSum, lSum;
    zSum += z80Clk; rSum += pc88ProfRender; dSum += pc88ProfDisk; lSum += pc88LinesDrawn;
    pc88ProfRender = pc88ProfDisk = 0;
    pc88LinesDrawn = 0;
    uint32_t us = (uint32_t)(now - t0);
    sum += us; if (us > mx) mx = us; if (us > frameUs) over++;
    if (++n == 300) {
      Serial.printf("FRAME us: avg=%u max=%u over=%u/300 (budget=%d)  z80=%u render=%u disk=%u us/frame  lines=%u/200\n", sum / n, mx, over, (int)frameUs,
                    (unsigned)(zSum / 240 / n), (unsigned)(rSum / 240 / n), (unsigned)(dSum / 240 / n), (unsigned)(lSum / n));
      sum = mx = over = n = 0;
      zSum = rSum = dSum = lSum = 0;
    }
#endif
    int64_t wait = next - now;
    if (wait > 0) {
      if (wait >= 1000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
      while (esp_timer_get_time() < next) asm volatile("nop");
    } else if (wait < -frameUs * 4) {
      next = now;
    }
  }
}

// ---------------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------------
#define HEAPLOG(tag) Serial.printf("HEAP[%-10s] free=%6u largest=%6u\n", (tag), \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL), \
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL))

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  disableCore0WDT();
  disableCore1WDT();
  rtc_wdt_protect_off();
  rtc_wdt_disable();
  esp_task_wdt_delete(NULL);
  delay(100);
  HEAPLOG("boot");

  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1, fabgl::KbdMode::NoVirtualKeys);
  PS2Controller.keyboard()->begin(GPIO_NUM_33, GPIO_NUM_32, false, false);
  Serial1.begin(115200, SERIAL_8N1, 39, -1);

  // 画面: 640x200 を VGA 640x480@60Hz の同期で出す(X1 と同じ。DoubleScan で 400 ライン)。
  // ビューポート(約128KB の内部 DMA RAM)はほかの内部 RAM より先に確保する
  DisplayController.begin();
  DisplayController.setResolution("\"640x200@60Hz\" 25.175 640 656 752 800 200 225 226 262 -HSync -VSync DoubleScan",
                                  -1, -1, false);
  HEAPLOG("after VGA");

  // メイン RAM: 16KB ずつ、入るものだけ内部 RAM に(X1 と同じ)
  const size_t RESERVE = 24 * 1024;
  const int order[4] = { 3, 0, 1, 2 };
  for (int i = 0; i < 4; i++) {
    int pg = order[i];
    uint8_t *p = nullptr;
    // if (heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) >= 0x4000 + RESERVE)
    //   p = (uint8_t *)heap_caps_malloc(0x4000, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!p) p = (uint8_t *)ps_malloc(0x4000);
    mem.ram_page[pg] = p;
  }
  for (int i = 0; i < 3; i++) mem.gvram[i] = (uint8_t *)ps_malloc(0x4000);
  mem.tvram = (uint8_t *)heap_caps_malloc(0x1000, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);   // テキスト(毎行読む)
  if (!mem.tvram) mem.tvram = (uint8_t *)ps_malloc(0x1000);
  mem.subram = (uint8_t *)ps_malloc(0x4000);
  for (int d = 0; d < NUM_DRIVES; d++) {
    memset(&drives[d], 0, sizeof(drives[d]));
    drives[d].buffer = (uint8_t *)ps_malloc(PC88_D88_MAX);
  }
  mem.drives = drives;
  mem.numDrives = NUM_DRIVES;
  HEAPLOG("after RAM");

  canvas = new fabgl::Canvas(&DisplayController);
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();

  opnGen.begin(16384);
  soundGenerator.attach(&opnGen);
  opnGen.enable(true);
  soundGenerator.setVolume(100);
  soundGenerator.play(true);

  // ROM
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  bool sdOK = SD.begin(SD_CS, SPI);
  if (sdOK) {
    mem.n88rom = loadRom("/PC88/N88.ROM", 0x8000, false);
    static const char *ext[4] = { "/PC88/N88_0.ROM", "/PC88/N88_1.ROM", "/PC88/N88_2.ROM", "/PC88/N88_3.ROM" };
    for (int i = 0; i < 4; i++) mem.n88ext[i] = loadRom(ext[i], 0x2000, false);
    mem.n80rom = loadRom("/PC88/N80.ROM", 0x8000, false);
    mem.diskrom = loadRom("/PC88/DISK.ROM", 0x2000, false);   // 2KB の ROM なら残りは FFh
    mem.kanji1 = loadRom("/PC88/KANJI1.ROM", 0x20000, false);
    mem.kanji2 = loadRom("/PC88/KANJI2.ROM", 0x20000, false);
  }
  if (!sdOK || !mem.n88rom) {
    canvas->selectFont(&fabgl::FONT_8x8);
    canvas->setPenColor(fabgl::Color::Red);
    canvas->drawText(8, 8, sdOK ? "/PC88/N88.ROM NOT FOUND" : "SD CARD NOT FOUND");
    while (1) delay(1000);
  }
  loadJoyKeys();
  bootMenu();

  // ビューポートの行(以降は canvas を使わず直接書く)
  DisplayController.processPrimitives();
  pc88_rows = (uint8_t **)heap_caps_malloc(sizeof(uint8_t *) * PC88_SCREEN_H, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  for (int y = 0; y < PC88_SCREEN_H; y++) {
    pc88_rows[y] = DisplayController.getScanline(y);
    if (!pc88_rows[y] || ((uintptr_t)pc88_rows[y] & 3)) {
      Serial.printf("VIEWPORT row %d is NULL or unaligned\n", y);
      while (1) delay(1000);
    }
  }

  pc88Init(mem, config);
  memset(&cpu, 0, sizeof(cpu));
  ResetZ80(&cpu);
  cpu.IPeriod = CPU_CYCLES_PER_LINE;
  Serial.println("PC-8801 Booting...");
  HEAPLOG("end setup");
  xTaskCreatePinnedToCore(cpuTask, "cpuTask", 8192, NULL, 24, NULL, 0);
}

void loop() {
  opnGen.fillBuffer();
  if (frameReady || isPaused) {
    frameReady = false;
    updateKeyboard();
  }
  vTaskDelay(1);
}
