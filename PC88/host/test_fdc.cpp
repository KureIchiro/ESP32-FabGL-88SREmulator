// ============================================================================
//  μPD765 + D88 の単体テスト(合成した D88 でコマンドを1つずつ確かめる)
// ============================================================================
#include "../pc88_fdc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

static int fails;
static void expect(const char *what, int got, int want) {
  if (got != want) { printf("NG  %-44s got %02X want %02X\n", what, got, want); fails++; }
  else printf("ok  %-44s %02X\n", what, got);
}

// 2D (40 シリンダ × 2 面 × 16 セクタ × 256 バイト)。データは (C*2+H)*16+R の値で埋める。
// トラック 1 面 0 の R=5 は削除データ、R=6 はデータ CRC エラー
static std::vector<uint8_t> makeD88(bool protect) {
  std::vector<uint8_t> d(0x2B0, 0);
  strcpy((char *)&d[0], "TEST");
  d[0x1A] = protect ? 0x10 : 0;
  d[0x1B] = 0x00;
  for (int t = 0; t < 80; t++) {
    uint32_t ofs = (uint32_t)d.size();
    d[0x20 + t * 4] = ofs & 0xFF; d[0x21 + t * 4] = (ofs >> 8) & 0xFF;
    d[0x22 + t * 4] = (ofs >> 16) & 0xFF; d[0x23 + t * 4] = (ofs >> 24) & 0xFF;
    for (int r = 1; r <= 16; r++) {
      uint8_t h[16] = { 0 };
      h[0] = (uint8_t)(t / 2); h[1] = (uint8_t)(t & 1); h[2] = (uint8_t)r; h[3] = 1;
      h[4] = 16; h[5] = 0;
      if (t == 2 && r == 5) h[7] = 0x10;
      if (t == 2 && r == 6) h[8] = 0xB0;
      h[14] = 0; h[15] = 1;               // 256
      d.insert(d.end(), h, h + 16);
      for (int i = 0; i < 256; i++) d.push_back((uint8_t)((t * 16 + r + i) & 0xFF));
    }
  }
  uint32_t sz = (uint32_t)d.size();
  d[0x1C] = sz & 0xFF; d[0x1D] = (sz >> 8) & 0xFF; d[0x1E] = (sz >> 16) & 0xFF; d[0x1F] = (sz >> 24) & 0xFF;
  return d;
}

static void out(std::initializer_list<int> bytes) { for (int b : bytes) upd765WriteData((uint8_t)b); }
static std::vector<uint8_t> resultBytes() {
  std::vector<uint8_t> r;
  while ((upd765ReadStatus() & 0xC0) == 0xC0 && !(upd765ReadStatus() & 0x20)) r.push_back(upd765ReadData());
  return r;
}

int fdcTest() {
  static Pc88Drive drv[2];
  static uint8_t buf0[PC88_D88_MAX], buf1[PC88_D88_MAX];
  memset(drv, 0, sizeof(drv));
  drv[0].buffer = buf0; drv[1].buffer = buf1;
  std::vector<uint8_t> img = makeD88(false), imgP = makeD88(true);
  expect("load D88 drive 0", pc88LoadD88(drv[0], img.data(), (uint32_t)img.size()), 1);
  expect("track 0 has 16 sectors", drv[0].trk_nsec[0], 16);
  upd765Init(drv, 2);

  expect("idle MSR", upd765ReadStatus(), 0x80);
  out({ 0x03, 0xDF, 0x03 });                               // SPECIFY (ND=1)
  expect("specify: no result", upd765ReadStatus(), 0x80);
  out({ 0x07, 0x00 });                                     // RECALIBRATE drive 0
  expect("recal: INT", upd765Irq(), 1);
  expect("recal: drive busy bit", upd765ReadStatus() & 0x01, 0x01);
  out({ 0x08 });
  std::vector<uint8_t> r = resultBytes();
  expect("sense int: 2 bytes", (int)r.size(), 2);
  expect("sense int: ST0 = SE", r[0], 0x20);
  expect("sense int: PCN", r[1], 0);
  expect("INT cleared", upd765Irq(), 0);
  out({ 0x08 });
  r = resultBytes();
  expect("sense int again: invalid", r.size() == 1 ? r[0] : 0xEE, 0x80);

  out({ 0x0F, 0x00, 0x01 });                               // SEEK cylinder 1
  out({ 0x08 });
  r = resultBytes();
  expect("seek: PCN 1", r.size() == 2 ? r[1] : 0xEE, 1);

  out({ 0x04, 0x04 });                                     // SENSE DRIVE STATUS head 1
  r = resultBytes();
  expect("ST3 ready/two-side/head1", r.size() == 1 ? r[0] : 0xEE, 0x2C);

  out({ 0x4A, 0x00 });                                     // READ ID (MFM)
  expect("read id: INT", upd765Irq(), 1);
  r = resultBytes();
  expect("read id: 7 bytes", (int)r.size(), 7);
  expect("read id: C", r[3], 1);
  expect("read id: N", r[6], 1);

  // READ DATA C1 H0 R1..R3 (EOT=3)、3セクタ目の最後で TC
  out({ 0x46, 0x00, 1, 0, 1, 1, 3, 0x1B, 0xFF });
  expect("read: exec MSR", upd765ReadStatus(), 0xF0);
  int n = 0, bad = 0;
  for (int s = 1; s <= 3; s++) {
    for (int i = 0; i < 256; i++) {
      for (int k = 0; k < 400 && !(upd765ReadStatus() & 0x80); k++) upd765Tick(10);
      uint8_t v = upd765ReadData();
      if (v != (uint8_t)((2 * 16 + s + i) & 0xFF)) bad++;
      n++;
    }
  }
  upd765TC();
  expect("read: 768 bytes", n, 768);
  expect("read: data correct", bad, 0);
  r = resultBytes();
  expect("read: ST0 normal", r.size() == 7 ? r[0] : 0xEE, 0x00);
  expect("read: result R = EOT+1 rule (C+1)", r.size() == 7 ? r[3] : 0xEE, 2);
  expect("read: result R", r.size() == 7 ? r[5] : 0xEE, 1);

  // TC を出さずに EOT を越える: 少し待つと EN で終わる
  out({ 0x46, 0x00, 1, 0, 2, 1, 2, 0x1B, 0xFF });
  for (int i = 0; i < 256; i++) upd765ReadData();
  expect("no TC: waiting (no RQM)", upd765ReadStatus() & 0x80, 0x00);
  for (int k = 0; k < 50; k++) upd765Tick(10);
  r = resultBytes();
  expect("no TC: ST0 abnormal", r.size() == 7 ? r[0] : 0xEE, 0x40);
  expect("no TC: ST1 EN", r.size() == 7 ? r[1] : 0xEE, 0x80);

  // TC を最初のセクタの途中で: 正常終了、R+1
  out({ 0x46, 0x00, 1, 0, 4, 1, 16, 0x1B, 0xFF });
  for (int i = 0; i < 10; i++) upd765ReadData();
  upd765TC();
  r = resultBytes();
  expect("TC mid sector: ST0 normal", r.size() == 7 ? r[0] : 0xEE, 0x00);
  expect("TC mid sector: R+1", r.size() == 7 ? r[5] : 0xEE, 5);

  // 削除データ(SK=0): CM を立てて1セクタで終わる
  out({ 0x46, 0x00, 1, 0, 5, 1, 16, 0x1B, 0xFF });
  for (int i = 0; i < 256; i++) upd765ReadData();
  r = resultBytes();
  expect("deleted: ST0 abnormal", r.size() == 7 ? r[0] : 0xEE, 0x40);
  expect("deleted: ST2 CM", r.size() == 7 ? r[2] : 0xEE, 0x40);
  // データ CRC エラー
  out({ 0x46, 0x00, 1, 0, 6, 1, 16, 0x1B, 0xFF });
  for (int i = 0; i < 256; i++) upd765ReadData();
  r = resultBytes();
  expect("data CRC: ST1 DE", r.size() == 7 ? r[1] : 0xEE, 0x20);
  expect("data CRC: ST2 DD", r.size() == 7 ? r[2] : 0xEE, 0x20);
  // 無いセクタ
  out({ 0x46, 0x00, 1, 0, 40, 1, 40, 0x1B, 0xFF });
  r = resultBytes();
  expect("no sector: ST1 ND", r.size() == 7 ? r[1] : 0xEE, 0x04);
  // 違うシリンダ番号を指定
  out({ 0x46, 0x00, 7, 0, 1, 1, 1, 0x1B, 0xFF });
  r = resultBytes();
  expect("wrong C: ST2 WC", r.size() == 7 ? r[2] : 0xEE, 0x10);

  // WRITE DATA C1 H0 R2、読み直して確認
  out({ 0x45, 0x00, 1, 0, 2, 1, 2, 0x1B, 0xFF });
  for (int i = 0; i < 256; i++) upd765WriteData((uint8_t)(0xA5 ^ i));
  upd765TC();
  r = resultBytes();
  expect("write: ST0 normal", r.size() == 7 ? r[0] : 0xEE, 0x00);
  expect("write: dirty", drv[0].dirty, 1);
  out({ 0x46, 0x00, 1, 0, 2, 1, 2, 0x1B, 0xFF });
  bad = 0;
  for (int i = 0; i < 256; i++) if (upd765ReadData() != (uint8_t)(0xA5 ^ i)) bad++;
  upd765TC();
  resultBytes();
  expect("write: read back", bad, 0);

  // 書き込み禁止
  expect("load protected D88 drive 1", pc88LoadD88(drv[1], imgP.data(), (uint32_t)imgP.size()), 1);
  out({ 0x45, 0x01, 0, 0, 1, 1, 1, 0x1B, 0xFF });
  r = resultBytes();
  expect("protect: ST1 NW", r.size() == 7 ? r[1] : 0xEE, 0x02);
  // ディスクなし
  pc88EjectD88(drv[1]);
  out({ 0x46, 0x01, 0, 0, 1, 1, 1, 0x1B, 0xFF });
  r = resultBytes();
  expect("no disk: ST0 NR", r.size() == 7 ? r[0] : 0xEE, 0x49);

  // 片面 FM の READ は MFM のセクタを見つけない
  out({ 0x06, 0x00, 1, 0, 1, 1, 1, 0x1B, 0xFF });
  r = resultBytes();
  expect("FM read on MFM track: MA", r.size() == 7 ? r[1] : 0xEE, 0x01);

  // MT=1 で面0の最後(EOT)から面1へ続ける
  out({ 0xC6, 0x00, 1, 0, 16, 1, 16, 0x1B, 0xFF });
  bad = 0;
  for (int i = 0; i < 256; i++) if (upd765ReadData() != (uint8_t)((2 * 16 + 16 + i) & 0xFF)) bad++;
  for (int k = 0; k < 50; k++) upd765Tick(10);
  for (int i = 0; i < 256; i++) if (upd765ReadData() != (uint8_t)((3 * 16 + 1 + i) & 0xFF)) bad++;
  upd765TC();
  r = resultBytes();
  expect("MT: continues on head 1", bad, 0);
  expect("MT: result H=1 R=2", r.size() == 7 ? (r[4] << 4 | r[5]) : 0xEE, 0x12);

  // FORMAT (同じ並び): ID を書き直してデータを埋める
  out({ 0x4D, 0x00, 1, 16, 0x32, 0xE5 });
  for (int s = 1; s <= 16; s++) out({ 1, 0, s, 1 });
  r = resultBytes();
  expect("format: ST0 normal", r.size() == 7 ? r[0] : 0xEE, 0x00);
  out({ 0x46, 0x00, 1, 0, 3, 1, 3, 0x1B, 0xFF });
  bad = 0;
  for (int i = 0; i < 256; i++) if (upd765ReadData() != 0xE5) bad++;
  upd765TC();
  resultBytes();
  expect("format: filled with E5", bad, 0);

  printf("FDC TEST %s (%d failures)\n", fails ? "NG" : "OK", fails);
  return fails ? 1 : 0;
}

#ifdef FDC_TEST_MAIN
int main() { return fdcTest(); }
#endif
