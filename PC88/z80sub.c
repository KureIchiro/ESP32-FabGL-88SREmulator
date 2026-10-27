/* ============================================================================
 *  ディスクユニット(PC-80S31)の Z80
 *  本体と同じ Z80.c を、関数名を付け替えてもう1つ作る(Marat の Z80 は RdZ80 などが
 *  大域関数なので、同じものを2つ動かすにはこうするのが簡単)。
 *  メモリは pc88_disk.cpp の表(pc88s_rd_map / pc88s_wr_map)、I/O は InZ80Sub / OutZ80Sub。
 *  ディスクユニットは止まっている時間が長いので、ExecZ80 と表はフラッシュに置く。
 * ============================================================================ */
#define PC88_SUB
#define Z80_EXEC_ATTR
#define Z80_TABLE_ATTR

#define ResetZ80  ResetZ80Sub
#define ExecZ80   ExecZ80Sub
#define IntZ80    IntZ80Sub
#define RunZ80    RunZ80Sub
#define StepZ80   StepZ80Sub
#define InZ80     InZ80Sub
#define OutZ80    OutZ80Sub
#define PatchZ80  PatchZ80Sub
#define DebugZ80  DebugZ80Sub
#define LoopZ80   LoopZ80Sub

#if defined(DEBUG) && !defined(PC88_SUB_TRACE)
#undef DEBUG
#endif

#include "Z80.c"
