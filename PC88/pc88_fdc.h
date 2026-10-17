#pragma once
#include <stdint.h>

// ============================================================================
//  μPD765A (FDC) と D88 イメージ
//  PC-8801 のディスクユニット(PC-80S31)の中にある FDC。ディスクユニットの Z80 が
//  MSR(状態)とデータレジスタを読み書きして動かす(DMA は使わない)。
//  ディスクイメージは D88 ファイルをそのまま PSRAM に置き、トラックごとのセクタの並び
//  (ID の C/H/R/N・状態・サイズ)を D88 のヘッダから引く(X1 版と同じやり方)。
// ============================================================================

#define PC88_D88_MAX    (1024u * 1024u)   // 1ドライブに置ける D88 の最大バイト数
#define PC88_D88_TRACKS 164               // D88 のトラック表の数(シリンダ×2 + 面)

struct Pc88Drive {
  uint8_t *buffer;                        // D88 イメージそのもの(PC88_D88_MAX バイト確保)
  uint32_t size;                          // イメージのバイト数
  bool     inserted;
  bool     protect;                       // 書き込み禁止(D88 ヘッダ 1Ah)
  bool     dirty;                         // 書き込みがあった(SD へ書き戻す目安)
  uint32_t trk_ofs[PC88_D88_TRACKS];      // 各トラックの先頭セクタの位置(0 = トラック無し)
  uint8_t  trk_nsec[PC88_D88_TRACKS];     // 各トラックのセクタ数
};

bool pc88LoadD88(Pc88Drive &drv, const uint8_t *fileBuf, uint32_t fileSize);
void pc88EjectD88(Pc88Drive &drv);

void    upd765Init(Pc88Drive *drives, int numDrives);
void    upd765Reset();
uint8_t upd765ReadStatus();               // MSR
uint8_t upd765ReadData();
void    upd765WriteData(uint8_t v);
void    upd765TC();                       // ターミナルカウント(転送の終わり)
bool    upd765Irq();                      // INT 端子
void    upd765Tick(int us);               // 時間を進める(シークの完了・回転待ちなど)
void    upd765Debug(char *buf, int size);

// デバッグ用フック(PC のテスト環境): コマンドを受け付けたとき / 結果を返し始めたとき
extern void (*upd765CmdHook)(const uint8_t *cmd, int len);
extern void (*upd765ResultHook)(const uint8_t *res, int len);
