// ============================================================================
//  PC-8801mkIISR コアの PC 用テストハーネス
//    pc88host fdctest                         μPD765 の単体テスト
//    pc88host boot <romdir> <frames> <outdir> [every]
//        romdir の N88.ROM などで起動し、every フレームごとに PNG を出す
//  環境変数:
//    DISK1/DISK2=<d88>   ドライブ1/2 に入れる
//    MODE=V2|V1S|V1H|N   BASIC のモード(ディップスイッチ)
//    KEYS=<frame>:<port>:<bit>,...   そのフレームから10フレームのあいだキーを押す
//    TRAP=<pc hex> TRAPFRAME=<n> TRAPN=<n>   本体 Z80 の命令トレース(DEBUG ビルド)
//    IOLOG=<from>-<to>   その範囲のフレームの I/O を出す
//    MEMDUMP=lo-hi       最後に本体から見えるメモリを出す
// ============================================================================
#include "../pc88.h"
#include "../pc88_internal.h"
#include "../Z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" {
  extern Z80 pc88_cpu;
  byte InZ80(zword Port);
  void OutZ80(zword Port, byte Value);
  void PatchZ80(Z80 *R);
  byte RdZ80Slow(zword A);
}
int fdcTest();
void pc88DiskDebug(char *buf, int size);
void pc88DiskTrace(int n);
extern void (*pc88PioHook)(int side, int port, uint8_t v, bool write);
static int curFrame, curLine;
static void pioLog(int side, int port, uint8_t v, bool write) {
  static uint8_t lastFE = 0xFF;
  if (!write && port == 0xFE && side == 0) { if (v == lastFE) return; lastFE = v; }
  printf("PIO f%d l%d %s %s %02X=%02X\n", curFrame, curLine, side ? "SUB " : "MAIN", write ? "W" : "R", port, v);
}

Z80 pc88_cpu;
extern "C" { unsigned z80EdCount[256], z80EdCycles, z80LdiDst[16], z80LdiSrc[16], z80PfxCount[3], z80PfxCycles[3]; }   // Z80.c(Z80_OPSTATS)が数える
static unsigned inCount[256], ssgJoyReads[2];     // INLOG: 読んだポートの回数(PNG を出すフレームで表示して 0 に戻す)
extern "C" byte InZ80(zword Port) { inCount[Port & 0xFF]++; return pc88In(Port); }
extern "C" void OutZ80(zword Port, byte Value) { pc88Out(Port, Value); }
extern "C" void PatchZ80(Z80 *R) {}
int pc88LineCycle() {
  int c = PC88_CYCLES_PER_LINE - pc88_cpu.ICount;
  return c < 0 ? 0 : c >= PC88_CYCLES_PER_LINE ? PC88_CYCLES_PER_LINE - 1 : c;
}

// ---- 画面(FabGL と同じ並び: 1語に px2,px3,px0,px1、1画素 = 同期 | B<<4 | G<<2 | R) ----
static uint8_t *rowBuf[PC88_SCREEN_H];
uint8_t **pc88_rows = rowBuf;
uint8_t pc88NativeColor(int r, int g, int b) { return (uint8_t)(0xC0 | ((b >> 1) << 4) | ((g >> 1) << 2) | (r >> 1)); }

// ---- 音(記録だけ) ----
static int opnWrites;
static uint8_t ssgRegs[16];
static FILE *opnLog;                               // OPNLOG=ファイル: OPN への書き込み(フレーム ライン レジスタ 値)
static int curFrameForLog, curLineForLog;
void pc88OpnWrite(uint8_t reg, uint8_t val) {
  opnWrites++;
  if (reg < 16) ssgRegs[reg] = val;
  if (opnLog) fprintf(opnLog, "%d %d %02X %02X\n", curFrameForLog, curLineForLog, reg, val);
}
static uint8_t joyR14 = 0xFF, joyR15 = 0xFF;     // JOY=from-to:R14[:R15],...(ジョイスティック。0 = 押下)
static unsigned ssgReads[16];                     // INLOG: SSG のレジスタを読んだ回数
uint8_t pc88SsgRead(uint8_t reg) {
  ssgReads[reg & 15]++;
  if (reg == 14 || reg == 15) ssgJoyReads[reg - 14]++;
  return reg == 14 ? joyR14 : reg == 15 ? joyR15 : ssgRegs[reg & 15];
}
// OPNLOG にポート 40h の BEEP/SING も FF として出す(5 列目 = ラインの中のクロック)
void pc88Speaker(uint8_t bits) { if (opnLog) fprintf(opnLog, "%d %d FF %02X %d\n", curFrameForLog, curLineForLog, bits & 0xA0, pc88LineCycle()); }

// ---- 命令トレース(Z80.c を DEBUG 付きでビルドしたときだけ呼ばれる) ----
struct TraceEnt { uint16_t pc, sp, af, hl, bc, de; uint8_t op; };
static TraceEnt traceBuf[1 << 20];
static unsigned traceN;
static bool traceDumped;
static uint32_t lineClock;
static void dumpTrace(const char *why, int count) {
  printf("---- trace (%s), last %d ----\n", why, count);
  for (int i = count; i > 0; i--) {
    const TraceEnt &t = traceBuf[(traceN - i) & ((1 << 20) - 1)];
    printf("PC=%04X op=%02X SP=%04X AF=%04X HL=%04X BC=%04X DE=%04X\n", t.pc, t.op, t.sp, t.af, t.hl, t.bc, t.de);
  }
}
extern "C" byte DebugZ80(Z80 *R) {
  TraceEnt &t = traceBuf[traceN++ & ((1 << 20) - 1)];
  t.pc = R->PC.W; t.sp = R->SPtr.W; t.af = R->AF.W; t.hl = R->HL.W; t.bc = R->BC.W; t.de = R->DE.W;
  const uint8_t *p = pc88_rd_map[R->PC.W >> 10];
  t.op = p ? p[R->PC.W & 0x3FF] : RdZ80Slow(R->PC.W);
  static int trapPc = getenv("TRAP") ? (int)strtol(getenv("TRAP"), nullptr, 16) : -1;
  static int trapN = getenv("TRAPN") ? atoi(getenv("TRAPN")) : 40;
  static uint32_t trapLine = getenv("TRAPFRAME") ? (uint32_t)atoi(getenv("TRAPFRAME")) * PC88_LINES_PER_FRAME : 0;
  if (!traceDumped && trapPc >= 0 && t.pc == trapPc && lineClock >= trapLine) {
    traceDumped = true;
    dumpTrace("arrival at TRAP", trapN > (1 << 20) ? (1 << 20) : trapN);
    if (const char *st = getenv("SUBTRACE")) { printf("---- sub trace ----\n"); pc88DiskTrace(atoi(st)); }
  }
  return 1;
}

// ---------------------------------------------------------------------------
//  PNG (無圧縮 deflate)
// ---------------------------------------------------------------------------
static uint32_t crcTable[256];
static void crcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
static void put32(std::vector<uint8_t> &v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> b;
  put32(b, (uint32_t)data.size());
  b.insert(b.end(), type, type + 4);
  b.insert(b.end(), data.begin(), data.end());
  put32(b, crc(b.data() + 4, b.size() - 4) ^ 0xFFFFFFFFu);
  fwrite(b.data(), 1, b.size(), f);
}
static bool writePng(const char *path, int w, int h, const uint8_t *rgb) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, w); put32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3); }
  std::vector<uint8_t> z;
  z.push_back(0x78); z.push_back(0x01);
  size_t pos = 0;
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  while (pos < raw.size()) {
    size_t n = raw.size() - pos; if (n > 65535) n = 65535;
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  put32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", std::vector<uint8_t>());
  fclose(f);
  return true;
}
// 640x200 を縦2倍の 640x400 で保存
static void dumpScreen(const std::string &path) {
  const int W = PC88_SCREEN_W, H = PC88_SCREEN_H;
  std::vector<uint8_t> rgb((size_t)W * H * 2 * 3);
  static const int order[4] = { 2, 3, 0, 1 };
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t v = rowBuf[y][(x & ~3) + order[x & 3]];
      uint8_t r = (v & 3) * 85, g = ((v >> 2) & 3) * 85, bl = ((v >> 4) & 3) * 85;
      for (int k = 0; k < 2; k++) { uint8_t *p = &rgb[(((size_t)(y * 2 + k) * W) + x) * 3]; p[0] = r; p[1] = g; p[2] = bl; }
    }
  if (writePng(path.c_str(), W, H * 2, rgb.data())) printf("wrote %s\n", path.c_str());
}

// ---------------------------------------------------------------------------
//  ファイル
// ---------------------------------------------------------------------------
static uint8_t *loadFile(const std::string &path, uint32_t *size, uint32_t want = 0) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint32_t alloc = want ? want : (uint32_t)n;
  uint8_t *p = (uint8_t *)malloc(alloc > (uint32_t)n ? alloc : (uint32_t)n);
  memset(p, 0xFF, alloc > (uint32_t)n ? alloc : (uint32_t)n);
  if (fread(p, 1, n, f) != (size_t)n) { fclose(f); free(p); return nullptr; }
  fclose(f);
  if (size) *size = (uint32_t)n;
  return p;
}
static uint8_t *loadRom(const std::string &dir, const char *name, uint32_t size) {
  uint32_t n;
  uint8_t *p = loadFile(dir + "/" + name, &n, size);
  if (p) printf("ROM %s: %u bytes\n", name, n);
  return p;
}

static int boot(const std::string &dir, int frames, const std::string &out, int every) {
  static Pc88Drive drv[2];
  static uint8_t d88buf[2][PC88_D88_MAX];
  Pc88Memory mem = {};
  for (int i = 0; i < 4; i++) mem.ram_page[i] = (uint8_t *)calloc(1, 0x4000);
  for (int i = 0; i < 3; i++) mem.gvram[i] = (uint8_t *)calloc(1, 0x4000);
  mem.tvram = (uint8_t *)calloc(1, 0x1000);
  mem.subram = (uint8_t *)calloc(1, 0x4000);
  mem.n88rom = loadRom(dir, "N88.ROM", 0x8000);
  if (!mem.n88rom) { printf("N88.ROM not found in %s\n", dir.c_str()); return 1; }
  static const char *ext[4] = { "N88_0.ROM", "N88_1.ROM", "N88_2.ROM", "N88_3.ROM" };
  for (int i = 0; i < 4; i++) mem.n88ext[i] = loadRom(dir, ext[i], 0x2000);
  mem.n80rom = loadRom(dir, "N80.ROM", 0x8000);
  mem.diskrom = loadRom(dir, "DISK.ROM", 0x2000);
  mem.kanji1 = loadRom(dir, "KANJI1.ROM", 0x20000);
  mem.kanji2 = loadRom(dir, "KANJI2.ROM", 0x20000);
  // DISK1/DISK2 = <d88>[#イメージ番号]。1つのファイルに何枚も入った D88 なら、
  // DISK2 を指定しなければ 2 枚目をドライブ2に入れる
  std::string dsk[2];
  for (int d = 0; d < 2; d++) if (const char *dp = getenv(d ? "DISK2" : "DISK1")) dsk[d] = dp;
  for (int d = 0; d < 2; d++) {
    memset(&drv[d], 0, sizeof(drv[d]));
    drv[d].buffer = d88buf[d];
    if (dsk[d].empty()) continue;
    std::string path = dsk[d];
    int img = 0;
    size_t hash = path.rfind('#');
    if (hash != std::string::npos) { img = atoi(path.c_str() + hash + 1); path = path.substr(0, hash); }
    uint32_t n;
    uint8_t *p = loadFile(path, &n);
    if (!p) { printf("DISK%d %s: not found\n", d + 1, path.c_str()); continue; }
    std::vector<uint32_t> ofs;
    for (uint32_t o = 0; o + 0x2B0 <= n;) {
      uint32_t sz = p[o + 0x1C] | (p[o + 0x1D] << 8) | (p[o + 0x1E] << 16) | ((uint32_t)p[o + 0x1F] << 24);
      if (sz < 0x2B0 || o + sz > n) break;
      ofs.push_back(o);
      o += sz;
    }
    if (d == 0 && dsk[1].empty() && ofs.size() > 1 && hash == std::string::npos) dsk[1] = path + "#1";
    if (img < (int)ofs.size()) {
      uint32_t o = ofs[img], sz = (img + 1 < (int)ofs.size() ? ofs[img + 1] : n) - o;
      printf("DISK%d %s image %d/%d: %s\n", d + 1, path.c_str(), img, (int)ofs.size(), pc88LoadD88(drv[d], p + o, sz) ? "OK" : "NG");
    } else printf("DISK%d %s: no image %d\n", d + 1, path.c_str(), img);
    free(p);
  }
  mem.drives = drv;
  mem.numDrives = 2;
  Pc88Config cfg;
  if (const char *m = getenv("MODE")) {
    std::string s = m;
    cfg.mode = s == "V1S" ? PC88_V1S : s == "V1H" ? PC88_V1H : s == "N" ? PC88_N : PC88_V2;
  }
  pc88Init(mem, cfg);
  memset(&pc88_cpu, 0, sizeof(pc88_cpu));
  ResetZ80(&pc88_cpu);
  pc88_cpu.IPeriod = PC88_CYCLES_PER_LINE;
  pc88_cpu.Trace = 1;

  struct KeyEv { int frame, port, bit, len; };     // KEYS=frame:port(hex):bit[:len],...(len は既定 10 フレーム)
  std::vector<KeyEv> keys;
  if (const char *ks = getenv("KEYS"))
    for (const char *p = ks; *p;) {
      KeyEv e = { 0, 0, 0, 10 };
      if (sscanf(p, "%d:%x:%d:%d", &e.frame, &e.port, &e.bit, &e.len) >= 3) keys.push_back(e);
      const char *c = strchr(p, ',');
      if (!c) break;
      p = c + 1;
    }
  struct JoyEv { int from, to; unsigned r14, r15; };
  std::vector<JoyEv> joys;
  if (const char *js = getenv("JOY"))
    for (const char *p = js; *p;) {
      JoyEv e = { 0, 0, 0xFF, 0xFF };
      if (sscanf(p, "%d-%d:%x:%x", &e.from, &e.to, &e.r14, &e.r15) >= 3) joys.push_back(e);
      const char *c = strchr(p, ',');
      if (!c) break;
      p = c + 1;
    }
  // SWAP=frame:drive:<d88>[#n],...  そのフレームでドライブ(1/2)のディスクを入れ替える(<d88> が - なら取り出す)
  struct SwapEv { int frame, drive; std::string spec; };
  std::vector<SwapEv> swaps;
  if (const char *sw = getenv("SWAP"))
    for (std::string all = sw; !all.empty();) {
      size_t c = all.find(',');
      std::string one = all.substr(0, c);
      all = c == std::string::npos ? "" : all.substr(c + 1);
      SwapEv e = { 0, 0, "" };
      size_t p1 = one.find(':'), p2 = p1 == std::string::npos ? p1 : one.find(':', p1 + 1);
      if (p2 == std::string::npos) continue;
      e.frame = atoi(one.c_str()); e.drive = atoi(one.c_str() + p1 + 1); e.spec = one.substr(p2 + 1);
      swaps.push_back(e);
    }
  if (getenv("PIOLOG")) pc88PioHook = pioLog;
  if (getenv("FDCLOG")) {                          // FDC のコマンドと結果
    upd765CmdHook = [](const uint8_t *c, int n) { printf("FDC f%d cmd", curFrame); for (int i = 0; i < n; i++) printf(" %02X", c[i]); printf("\n"); };
    upd765ResultHook = [](const uint8_t *r, int n) { printf("FDC f%d res", curFrame); for (int i = 0; i < n; i++) printf(" %02X", r[i]); printf("\n"); };
  }
  for (int f = 1; f <= frames; f++) {
    curFrame = f;
    curFrameForLog = f;
    for (const SwapEv &e : swaps) {
      if (e.frame != f || e.drive < 1 || e.drive > 2) continue;
      Pc88Drive &dv = drv[e.drive - 1];
      if (e.spec == "-") { pc88EjectD88(dv); printf("SWAP f%d drive%d: eject\n", f, e.drive); continue; }
      std::string path = e.spec;
      int img = 0;
      size_t hash = path.rfind('#');
      if (hash != std::string::npos) { img = atoi(path.c_str() + hash + 1); path = path.substr(0, hash); }
      uint32_t n;
      uint8_t *p = loadFile(path, &n);
      if (!p) { printf("SWAP %s: not found\n", path.c_str()); continue; }
      uint32_t o = 0;
      for (int i = 0; i < img && o + 0x2B0 <= n; i++) o += p[o + 0x1C] | (p[o + 0x1D] << 8) | (p[o + 0x1E] << 16) | ((uint32_t)p[o + 0x1F] << 24);
      uint32_t sz = o + 0x2B0 <= n ? (p[o + 0x1C] | (p[o + 0x1D] << 8) | (p[o + 0x1E] << 16) | ((uint32_t)p[o + 0x1F] << 24)) : 0;
      if (o + sz > n) sz = n - o;
      printf("SWAP f%d drive%d %s image %d: %s\n", f, e.drive, path.c_str(), img, sz && pc88LoadD88(dv, p + o, sz) ? "OK" : "NG");
      free(p);
    }
    if (f == 1 && getenv("OPNLOG")) opnLog = fopen(getenv("OPNLOG"), "w");
    for (int r = 0; r < 16; r++) pc88_key[r] = 0xFF;
    for (const KeyEv &e : keys)
      if (f >= e.frame && f < e.frame + e.len) pc88_key[e.port & 15] &= (uint8_t)~(1 << e.bit);
    joyR14 = joyR15 = 0xFF;
    for (const JoyEv &e : joys)
      if (f >= e.from && f <= e.to) { joyR14 = (uint8_t)e.r14; joyR15 = (uint8_t)e.r15; }
    for (int l = 0; l < PC88_LINES_PER_FRAME; l++) {
      curLine = l;
      curLineForLog = l;
      lineClock++;
      ExecZ80(&pc88_cpu, PC88_CYCLES_PER_LINE);
      pc88Line(l);
      if (pc88IrqPending() && (pc88_cpu.IFF & IFF_1)) IntZ80(&pc88_cpu, pc88IrqAck());
    }
    if (const char *tw = getenv("TVWATCH")) {       // TVWATCH=lo-hi: 高速 RAM(F000〜FFFF)のその範囲が変わったフレームを出す
      static uint8_t prev[0x1000];
      static bool init;
      int lo = 0, hi = 0;
      if (sscanf(tw, "%x-%x", &lo, &hi) == 2) {
        lo &= 0xFFF; hi &= 0xFFF;
        if (init && memcmp(prev + lo, mem.tvram + lo, hi - lo + 1)) {
          printf("TV %d PC=%04X:", f, pc88_cpu.PC.W);
          for (int a = lo; a <= hi; a++) if (prev[a] != mem.tvram[a]) printf(" %03X=%02X", a, mem.tvram[a]);
          printf("\n");
        }
        memcpy(prev, mem.tvram, 0x1000);
        init = true;
      }
    }
    if (getenv("SCRHASH")) {                        // 毎フレームの画面のチェックサム(描画の変更前後の比較用)
      uint32_t h = 0xFFFFFFFFu;
      for (int y = 0; y < PC88_SCREEN_H; y++) h = crc(rowBuf[y], PC88_SCREEN_W, h);
      printf("H %d %08X %u\n", f, h, (unsigned)pc88LinesDrawn);
      pc88LinesDrawn = 0;
    }
    if (f % every == 0 || f == frames) {
      char name[64];
      snprintf(name, sizeof(name), "/frame%04d.png", f);
      dumpScreen(out + name);
      char dd[160];
      pc88DiskDebug(dd, sizeof(dd));
      printf("  irq: E6=%02X limit=%d canInt=%d req=%02X p32=%02X\n", pc88.irqMaskE6, pc88.levelLimit, pc88.canInt, pc88.irqReq, pc88.p32);
      printf("  frame %d: PC=%04X SP=%04X IFF=%02X opn=%d | %s\n", f, pc88_cpu.PC.W, pc88_cpu.SPtr.W, pc88_cpu.IFF, opnWrites, dd);
      if (getenv("OPSTATS")) {                     // 前に出したときからの本体の ED 命令(回数・サイクルの割合・多い順)
        static int lastF = 0;
        double total = (double)(f - lastF) * PC88_LINES_PER_FRAME * PC88_CYCLES_PER_LINE;
        unsigned n = 0;
        for (int i = 0; i < 256; i++) n += z80EdCount[i];
        printf("  ED: %.0f/frame  cycles %.1f%%  top:", (double)n / (f - lastF), 100.0 * z80EdCycles / total);
        for (int k = 0; k < 5; k++) {
          int best = 0;
          for (int i = 1; i < 256; i++) if (z80EdCount[i] > z80EdCount[best]) best = i;
          if (!z80EdCount[best]) break;
          printf(" %02X=%.0f", best, (double)z80EdCount[best] / (f - lastF));
          z80EdCount[best] = 0;
        }
        static const char *pfx[3] = { "CB", "DD", "FD" };
        for (int i = 0; i < 3; i++) printf(" | %s %.0f/frame %.1f%%", pfx[i], (double)z80PfxCount[i] / (f - lastF), 100.0 * z80PfxCycles[i] / total);
        memset(z80PfxCount, 0, sizeof(z80PfxCount)); memset(z80PfxCycles, 0, sizeof(z80PfxCycles));
        printf("\n  LDI src/dst(4KB): ");
        for (int i = 0; i < 16; i++) if (z80LdiSrc[i] | z80LdiDst[i]) printf(" %X:%u/%u", i, z80LdiSrc[i], z80LdiDst[i]);
        printf("  p32=%02X p35=%02X p5x=%d\n", pc88.p32, pc88.p35, pc88.vramSel);
        memset(z80LdiSrc, 0, sizeof(z80LdiSrc)); memset(z80LdiDst, 0, sizeof(z80LdiDst));
        memset(z80EdCount, 0, sizeof(z80EdCount)); z80EdCycles = 0; lastF = f;
      }
      if (getenv("INLOG")) {                       // 前に出したときからの IN の回数(キーボード 00〜0F と SSG R14/R15)
        printf("  in:");
        for (int p = 0; p < 0x10; p++) if (inCount[p]) printf(" %02X=%u", p, inCount[p]);
        printf(" | R14=%u R15=%u | SSG reads:", ssgJoyReads[0], ssgJoyReads[1]);
        for (int r = 0; r < 14; r++) if (ssgReads[r]) printf(" R%d=%u", r, ssgReads[r]);
        printf(" | crtc p31=%02X p53=%02X text=%d attrMode=%d\n", pc88.p31, pc88.p53, pc88.crtc.ve, pc88.crtc.attrMode);
        memset(inCount, 0, sizeof(inCount)); memset(ssgReads, 0, sizeof(ssgReads)); ssgJoyReads[0] = ssgJoyReads[1] = 0;
      }
      if (getenv("TEXTDUMP")) {                    // テキストの設定と、DMA で読む各文字行(文字 + 属性)
        Pc88Crtc &c = pc88.crtc;
        printf("  crtc ve=%d rev=%d h=%d l=%d r=%d v=%d attrMode=%d attrNum=%d skip=%d | dma mode=%02X addr=%04X count=%04X | p30=%02X p31=%02X p53=%02X hs=%d\n",
               c.ve, c.reverse, c.h, c.l, c.r, c.v, c.attrMode, c.attrNum, c.skipLine, pc88.dmac.mode, pc88.dmac.addr[2], pc88.dmac.count[2],
               pc88.p30, pc88.p31, pc88.p53, pc88.highSpeed);
        uint16_t a = pc88.dmac.addr[2];
        for (int row = 0; row < c.l; row++) {
          printf("  T%02d %04X:", row, a);
          for (int i = 0; i < c.h + c.attrNum * 2; i++, a++) {
            uint8_t v = pc88.highSpeed ? (a >= 0xF000 ? pc88.mem.tvram[a & 0xFFF] : 0xFF) : pc88.mem.ram_page[a >> 14][a & 0x3FFF];
            printf(i == c.h ? " | %02X" : " %02X", v);
          }
          printf("\n");
        }
      }
    }
  }
  if (const char *sv = getenv("SAVED88")) {        // SAVED88=drive:file  最後にそのドライブの D88 を書き出す
    int dn = atoi(sv);
    const char *fn = strchr(sv, ':');
    if (fn && dn >= 1 && dn <= 2 && drv[dn - 1].inserted) {
      FILE *fo = fopen(fn + 1, "wb");
      if (fo) { fwrite(drv[dn - 1].buffer, 1, drv[dn - 1].size, fo); fclose(fo); printf("saved drive%d -> %s (%u bytes)\n", dn, fn + 1, drv[dn - 1].size); }
    }
  }
  if (const char *md = getenv("MEMDUMP")) {
    int lo = 0, hi = 0;
    if (sscanf(md, "%x-%x", &lo, &hi) == 2)
      for (int a = lo & ~15; a <= hi; a += 16) {
        printf("%04X:", a);
        for (int i = 0; i < 16; i++) {
          uint16_t ad = (uint16_t)(a + i);
          const uint8_t *p = pc88_rd_map[ad >> 10];
          printf(" %02X", p ? p[ad & 0x3FF] : RdZ80Slow(ad));
        }
        printf("\n");
      }
  }
  return 0;
}

int main(int argc, char **argv) {
  crcInit();
  for (int y = 0; y < PC88_SCREEN_H; y++) rowBuf[y] = (uint8_t *)calloc(1, PC88_SCREEN_W);
  std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "fdctest") return fdcTest();
  if (mode == "boot" && argc >= 5) return boot(argv[2], atoi(argv[3]), argv[4], argc > 5 ? atoi(argv[5]) : 60);
  printf("usage: pc88host fdctest | boot <romdir> <frames> <outdir> [every]\n");
  return 1;
}
