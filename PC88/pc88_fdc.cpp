// ============================================================================
//  μPD765A (FDC) と D88 イメージ  ※説明は pc88_fdc.h
//  データシートの動き(コマンド・実行・結果の3段階、MSR、ST0〜ST3)をそのまま作る。
//  転送はプログラム I/O(ディスクユニットの Z80 が MSR の RQM を見て1バイトずつ読み書き)。
// ============================================================================
#include "pc88_fdc.h"
#include <string.h>
#include <stdio.h>

void (*upd765CmdHook)(const uint8_t *cmd, int len);
void (*upd765ResultHook)(const uint8_t *res, int len);

static inline uint32_t rd32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

enum { SH_C = 0, SH_H = 1, SH_R = 2, SH_N = 3, SH_NSEC = 4, SH_DENS = 6, SH_DEL = 7, SH_STAT = 8, SH_SIZE = 14 };
#define ST_ID_CRC   0xA0
#define ST_DATA_CRC 0xB0
#define ST_NO_AM    0xE0
#define ST_NO_DAM   0xF0

bool pc88LoadD88(Pc88Drive &drv, const uint8_t *fileBuf, uint32_t fileSize) {
  if (fileSize < 0x2B0 || fileSize > PC88_D88_MAX || !drv.buffer) return false;
  if (fileBuf != drv.buffer) memcpy(drv.buffer, fileBuf, fileSize);
  const uint8_t *b = drv.buffer;
  drv.size = fileSize;
  drv.protect = (b[0x1A] & 0x10) != 0;
  drv.dirty = false;
  uint32_t end = rd32le(&b[0x1C]);
  if (end == 0 || end > fileSize) end = fileSize;
  for (int t = 0; t < PC88_D88_TRACKS; t++) {
    uint32_t ofs = rd32le(&b[0x20 + t * 4]);
    drv.trk_ofs[t] = 0;
    drv.trk_nsec[t] = 0;
    if (ofs < 0x20 + 4 || ofs + 16 > end) continue;
    uint32_t next = end;
    for (int u = 0; u < PC88_D88_TRACKS; u++) {
      uint32_t o = rd32le(&b[0x20 + u * 4]);
      if (o > ofs && o < next) next = o;
    }
    int want = rd16le(&b[ofs + SH_NSEC]);
    if (want <= 0 || want > 255) want = 255;
    uint32_t pos = ofs;
    int n = 0;
    while (n < want && pos + 16 <= next) {
      uint32_t sz = rd16le(&b[pos + SH_SIZE]);
      if (pos + 16 + sz > next) break;
      pos += 16 + sz;
      n++;
    }
    if (n > 0) { drv.trk_ofs[t] = ofs; drv.trk_nsec[t] = (uint8_t)n; }
  }
  drv.inserted = true;
  return true;
}

void pc88EjectD88(Pc88Drive &drv) {
  drv.inserted = false;
  drv.size = 0;
  for (int t = 0; t < PC88_D88_TRACKS; t++) { drv.trk_ofs[t] = 0; drv.trk_nsec[t] = 0; }
}

enum Phase { PH_CMD, PH_READ, PH_WRITE, PH_FORMAT, PH_SECTOR_END, PH_RESULT };

static Pc88Drive *drives;
static int numDrives;
static Phase    phase;
static uint8_t  cmd[9];
static int      cmdLen, cmdNeed;
static uint8_t  res[7];
static int      resLen, resPos;
static bool     nonDma = true;
static uint8_t  pcn[4];
static uint8_t  rot[4];
static bool     seekEnd[4];
static uint8_t  seekSt0[4];
static bool     resultIrq;
static bool     tc;

static int      drv, head;
static uint8_t  C, H, R, N, EOT, DTL;
static bool     mt, mfm, sk, deleted;
static uint8_t  st0, st1, st2;
static uint8_t *dataPtr;
static uint32_t dataLen, dataAvail, dataPos;
static uint32_t curHdr;
static int      waitUs;
static bool     endAfterSector;
static bool     xferWrite;
static uint8_t  fmtBuf[4 * 64];
static int      fmtCount, fmtNeed;
static uint8_t  fmtN, fmtSC, fmtD;

static inline Pc88Drive *curDrive() {
  return (drv < numDrives && drives[drv].inserted) ? &drives[drv] : nullptr;
}
static inline int trackSlot(int d, int h) { return pcn[d] * 2 + h; }

static uint32_t sectorHeader(const Pc88Drive &d, int slot, int n) {
  if (slot < 0 || slot >= PC88_D88_TRACKS || n >= d.trk_nsec[slot]) return 0;
  uint32_t pos = d.trk_ofs[slot];
  for (int i = 0; i < n; i++) pos += 16 + rd16le(&d.buffer[pos + SH_SIZE]);
  return pos;
}

static int cmdLength(uint8_t c) {
  switch (c & 0x1F) {
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0C: case 0x11: case 0x19: case 0x1D: return 9;
    case 0x03: return 3;
    case 0x04: case 0x07: case 0x0A: return 2;
    case 0x08: return 1;
    case 0x0D: return 6;
    case 0x0F: return 3;
    default: return 1;
  }
}

static void result(const uint8_t *r, int n, bool irq) {
  memcpy(res, r, n);
  resLen = n;
  resPos = 0;
  phase = PH_RESULT;
  resultIrq = irq;
  if (upd765ResultHook) upd765ResultHook(res, n);
}

static void finishRW() {
  uint8_t c = C, h = H, r = R;
  if (R == EOT && !(st0 & 0xC0)) {
    if (mt && !(head & 1)) { h = H ^ 1; r = 1; }
    else { c = C + 1; r = 1; if (mt) h = H ^ 1; }
  } else if (!(st0 & 0xC0) || (st1 & 0x80)) {
    r = R + 1;
  }
  uint8_t out[7] = { (uint8_t)(st0 | (head << 2) | drv), st1, st2, c, h, r, N };
  result(out, 7, true);
}

static void abnormal(uint8_t s1, uint8_t s2) {
  st0 = 0x40; st1 |= s1; st2 |= s2;
  uint8_t out[7] = { (uint8_t)(st0 | (head << 2) | drv), st1, st2, C, H, R, N };
  result(out, 7, true);
}

static uint32_t findSector(bool *missingAm, bool *badCyl) {
  Pc88Drive *d = curDrive();
  *missingAm = true;
  *badCyl = false;
  if (!d) return 0;
  int slot = trackSlot(drv, head);
  int n = (slot < PC88_D88_TRACKS) ? d->trk_nsec[slot] : 0;
  for (int i = 0; i < n; i++) {
    int k = (rot[drv] + i) % n;
    uint32_t h = sectorHeader(*d, slot, k);
    const uint8_t *sh = &d->buffer[h];
    bool fm = (sh[SH_DENS] & 0x40) != 0;
    if (fm == mfm) continue;
    if (sh[SH_STAT] == ST_NO_AM) continue;
    *missingAm = false;
    if (sh[SH_R] != R || sh[SH_N] != N || sh[SH_H] != H) continue;
    if (sh[SH_C] != C) { *badCyl = true; continue; }
    if (sh[SH_STAT] == ST_ID_CRC) continue;
    rot[drv] = (uint8_t)((k + 1) % n);
    return h;
  }
  return 0;
}

static void startSector() {
  Pc88Drive *d = curDrive();
  if (!d) { st0 = 0x48; uint8_t out[7] = { (uint8_t)(0x48 | (head << 2) | drv), 0, 0, C, H, R, N }; result(out, 7, true); return; }
  bool write = (phase == PH_WRITE);
  if (write && d->protect) { abnormal(0x02, 0); return; }
  bool noAm, badCyl;
  uint32_t h = findSector(&noAm, &badCyl);
  if (!h) { abnormal(noAm ? 0x01 : 0x04, badCyl ? (C == 0xFF ? 0x02 : 0x10) : 0); return; }
  const uint8_t *sh = &d->buffer[h];
  endAfterSector = false;
  if (!write) {
    if (sh[SH_STAT] == ST_NO_DAM) { abnormal(0x01, 0x01); return; }
    bool del = (sh[SH_DEL] & 0x10) != 0;
    if (del != deleted) {
      if (sk) {
        if (R == EOT) { abnormal(0x80, 0); return; }
        R++;
        startSector();
        return;
      }
      st2 |= 0x40;
      endAfterSector = true;
    }
    if (sh[SH_STAT] == ST_DATA_CRC) { st1 |= 0x20; st2 |= 0x20; endAfterSector = true; }
  }
  curHdr = h;
  dataPtr = &d->buffer[h + 16];
  dataAvail = rd16le(&sh[SH_SIZE]);
  dataLen = N ? (128u << (N > 7 ? 7 : N)) : (DTL ? DTL : 128);
  dataPos = 0;
}

static void sectorDone() {
  if (phase == PH_WRITE) {
    Pc88Drive *d = curDrive();
    if (d) d->dirty = true;
  }
  if (tc || endAfterSector) {
    if (endAfterSector) st0 = 0x40;
    finishRW();
    return;
  }
  phase = PH_SECTOR_END;
  waitUs = 200;
}

static void nextSector(bool write) {
  if (R == EOT) {
    if (mt && !(head & 1)) { head = 1; H ^= 1; R = 1; }
    else { abnormal(0x80, 0); return; }
  } else {
    R++;
  }
  phase = write ? PH_WRITE : PH_READ;
  startSector();
}

static void execCommand() {
  uint8_t c = cmd[0];
  if (upd765CmdHook) upd765CmdHook(cmd, cmdLen);
  int op = c & 0x1F;
  mt = (c & 0x80) != 0; mfm = (c & 0x40) != 0; sk = (c & 0x20) != 0;
  if (cmdLen > 1) { drv = cmd[1] & 3; head = (cmd[1] >> 2) & 1; }
  tc = false;
  switch (op) {
    case 0x03:
      nonDma = (cmd[2] & 1) != 0;
      phase = PH_CMD;
      return;
    case 0x04: {
      Pc88Drive *d = curDrive();
      uint8_t st3 = (uint8_t)((head << 2) | drv | 0x08);
      if (d) st3 |= 0x20;
      if (pcn[drv] == 0) st3 |= 0x10;
      if (d && d->protect) st3 |= 0x40;
      result(&st3, 1, false);
      return;
    }
    case 0x07:
      pcn[drv] = 0;
      seekEnd[drv] = true;
      seekSt0[drv] = (uint8_t)(0x20 | drv);
      phase = PH_CMD;
      return;
    case 0x0F:
      pcn[drv] = cmd[2];
      seekEnd[drv] = true;
      seekSt0[drv] = (uint8_t)(0x20 | drv);
      phase = PH_CMD;
      return;
    case 0x08: {
      for (int d = 0; d < 4; d++)
        if (seekEnd[d]) {
          seekEnd[d] = false;
          uint8_t out[2] = { seekSt0[d], pcn[d] };
          result(out, 2, false);
          return;
        }
      uint8_t inv = 0x80;
      result(&inv, 1, false);
      return;
    }
    case 0x0A: {
      Pc88Drive *d = curDrive();
      st0 = st1 = st2 = 0;
      if (!d) { uint8_t out[7] = { (uint8_t)(0x48 | (head << 2) | drv), 0, 0, 0, 0, 0, 0 }; result(out, 7, true); return; }
      int slot = trackSlot(drv, head);
      int n = (slot < PC88_D88_TRACKS) ? d->trk_nsec[slot] : 0;
      for (int i = 0; i < n; i++) {
        int k = (rot[drv] + i) % n;
        const uint8_t *sh = &d->buffer[sectorHeader(*d, slot, k)];
        if (((sh[SH_DENS] & 0x40) != 0) == mfm || sh[SH_STAT] == ST_NO_AM || sh[SH_STAT] == ST_ID_CRC) continue;
        rot[drv] = (uint8_t)((k + 1) % n);
        uint8_t out[7] = { (uint8_t)((head << 2) | drv), 0, 0, sh[SH_C], sh[SH_H], sh[SH_R], sh[SH_N] };
        result(out, 7, true);
        return;
      }
      uint8_t out[7] = { (uint8_t)(0x40 | (head << 2) | drv), 0x01, 0, 0, 0, 0, 0 };
      result(out, 7, true);
      return;
    }
    case 0x06: case 0x0C: case 0x05: case 0x09:
    case 0x02:
      C = cmd[2]; H = cmd[3]; R = cmd[4]; N = cmd[5]; EOT = cmd[6]; DTL = cmd[8];
      st0 = st1 = st2 = 0;
      deleted = (op == 0x0C || op == 0x09);
      xferWrite = (op == 0x05 || op == 0x09);
      phase = (op == 0x05 || op == 0x09) ? PH_WRITE : PH_READ;
      startSector();
      return;
    case 0x0D: {
      Pc88Drive *d = curDrive();
      fmtN = cmd[2]; fmtSC = cmd[3]; fmtD = cmd[5];
      st0 = st1 = st2 = 0;
      C = H = R = 0; N = fmtN;
      if (!d) { uint8_t out[7] = { (uint8_t)(0x48 | (head << 2) | drv), 0, 0, 0, 0, 0, fmtN }; result(out, 7, true); return; }
      if (d->protect) { abnormal(0x02, 0); return; }
      fmtCount = 0;
      fmtNeed = (fmtSC > 64 ? 64 : fmtSC) * 4;
      phase = PH_FORMAT;
      return;
    }
    default: {
      uint8_t inv = 0x80;
      result(&inv, 1, false);
      return;
    }
  }
}

static void finishFormat() {
  Pc88Drive *d = curDrive();
  int slot = trackSlot(drv, head);
  int n = fmtNeed / 4;
  bool ok = d && slot < PC88_D88_TRACKS && d->trk_nsec[slot] == n;
  uint32_t size = 128u << (fmtN > 7 ? 7 : fmtN);
  if (ok)
    for (int i = 0; i < n; i++)
      if (rd16le(&d->buffer[sectorHeader(*d, slot, i) + SH_SIZE]) != size) { ok = false; break; }
  if (!ok) { abnormal(0x02, 0); return; }
  for (int i = 0; i < n; i++) {
    uint8_t *sh = &d->buffer[sectorHeader(*d, slot, i)];
    sh[SH_C] = fmtBuf[i * 4]; sh[SH_H] = fmtBuf[i * 4 + 1]; sh[SH_R] = fmtBuf[i * 4 + 2]; sh[SH_N] = fmtBuf[i * 4 + 3];
    sh[SH_DENS] = mfm ? 0x00 : 0x40;
    sh[SH_DEL] = 0; sh[SH_STAT] = 0;
    memset(sh + 16, fmtD, size);
  }
  d->dirty = true;
  uint8_t out[7] = { (uint8_t)((head << 2) | drv), 0, 0, fmtBuf[(n - 1) * 4], fmtBuf[(n - 1) * 4 + 1], fmtBuf[(n - 1) * 4 + 2], fmtN };
  result(out, 7, true);
}

void upd765Init(Pc88Drive *d, int n) { drives = d; numDrives = n; upd765Reset(); }

void upd765Reset() {
  phase = PH_CMD;
  cmdLen = cmdNeed = 0;
  resLen = resPos = 0;
  nonDma = true;
  memset(pcn, 0, sizeof(pcn));
  memset(rot, 0, sizeof(rot));
  memset(seekEnd, 0, sizeof(seekEnd));
  resultIrq = tc = false;
}

uint8_t upd765ReadStatus() {
  uint8_t busy = 0;
  for (int d = 0; d < 4; d++) if (seekEnd[d]) busy |= (uint8_t)(1 << d);
  switch (phase) {
    case PH_CMD:        return (uint8_t)(0x80 | (cmdLen ? 0x10 : 0) | busy);
    case PH_READ:       return (uint8_t)(0x80 | 0x40 | 0x20 | 0x10 | busy);
    case PH_WRITE:
    case PH_FORMAT:     return (uint8_t)(0x80 | 0x20 | 0x10 | busy);
    case PH_SECTOR_END: return (uint8_t)(0x20 | 0x10 | busy);
    case PH_RESULT:     return (uint8_t)(0x80 | 0x40 | 0x10 | busy);
  }
  return 0x80;
}

uint8_t upd765ReadData() {
  if (phase == PH_RESULT) {
    resultIrq = false;
    uint8_t v = res[resPos++];
    if (resPos >= resLen) phase = PH_CMD;
    return v;
  }
  if (phase == PH_READ) {
    uint8_t v = (dataPos < dataAvail) ? dataPtr[dataPos] : 0x4E;
    if (++dataPos >= dataLen) sectorDone();
    return v;
  }
  return 0xFF;
}

void upd765WriteData(uint8_t v) {
  switch (phase) {
    case PH_CMD:
      if (cmdLen == 0) cmdNeed = cmdLength(v);
      cmd[cmdLen++] = v;
      if (cmdLen >= cmdNeed) { execCommand(); cmdLen = 0; }
      return;
    case PH_WRITE:
      if (dataPos < dataAvail) dataPtr[dataPos] = v;
      if (++dataPos >= dataLen) sectorDone();
      return;
    case PH_FORMAT:
      if (fmtCount < (int)sizeof(fmtBuf)) fmtBuf[fmtCount] = v;
      if (++fmtCount >= fmtNeed) finishFormat();
      return;
    default:
      return;
  }
}

void upd765TC() {
  tc = true;
  if (phase == PH_SECTOR_END) { finishRW(); return; }
  if (phase == PH_READ || phase == PH_WRITE) {
    if (phase == PH_WRITE) { Pc88Drive *d = curDrive(); if (d) d->dirty = true; }
    finishRW();
  }
}

bool upd765Irq() {
  if (resultIrq) return true;
  for (int d = 0; d < 4; d++) if (seekEnd[d]) return true;
  if (nonDma && (phase == PH_READ || phase == PH_WRITE || phase == PH_FORMAT)) return true;
  return false;
}

void upd765Tick(int us) {
  if (phase == PH_SECTOR_END) {
    waitUs -= us;
    if (waitUs <= 0) nextSector(xferWrite);
  }
}

void upd765Debug(char *buf, int size) {
  snprintf(buf, size, "FDC ph=%d cmd=%02X pcn=%d,%d C%d H%d R%d N%d EOT%d pos=%u/%u res=%d/%d",
           (int)phase, cmd[0], pcn[0], pcn[1], C, H, R, N, EOT, (unsigned)dataPos, (unsigned)dataLen, resPos, resLen);
}
