/* ============================================================================
 *  ディスクユニット(PC-80S31)の Z80
 *  本体と同じ z80core.c を、関数名を付け替えてもう1つ作る(メモリと I/O は z80core_config.h の
 *  Z80CORE_SUB の側: pc88_disk.cpp の pc88s_rd_map / pc88s_wr_map と InZ80Sub / OutZ80Sub)。
 *  ディスクユニットは止まっている時間が長いので、フラッシュに置く(Z80CORE_IRAM 0)。
 * ============================================================================ */
#define Z80CORE_SUB 1
#define Z80CORE_IRAM 0
#define z80core_init    z80sub_init
#define z80core_reset   z80sub_reset
#define z80core_run     z80sub_run
#define z80core_nmi     z80sub_nmi
#define z80core_get_r   z80sub_get_r
#define z80core_int     z80sub_int
#define z80core_nmi_now z80sub_nmi_now
#define z80core_cyc_op  z80sub_cyc_op
#define z80core_cyc_cb  z80sub_cyc_cb
#define z80core_cyc_ed  z80sub_cyc_ed

#include "z80core.c"
