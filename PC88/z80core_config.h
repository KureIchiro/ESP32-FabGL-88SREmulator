/* z80core_config.h - PC-8801: the memory maps and I/O of the two Z80s for z80core
   Main Z80 (z80core.c): 1 KB page tables pc88_rd_map / pc88_wr_map (pc88_core.cpp); a NULL page (GVRAM through
   the V2 ALU and so on) calls RdZ80Slow / WrZ80Slow. The I/O sees the cycles left in the line (z->icount).
   The ED opcodes run from IRAM: games that draw GVRAM through the ALU use LDI a lot.
   After EI and the next instruction the interrupt controller is asked once (pc88IrqTake: vector or -1), because
   loops such as the N88-BASIC key wait only open a short EI .. DI window and the line-end check may never hit it.
   Disk unit Z80 (z80sub.c, Z80CORE_SUB): 4 KB page tables pc88s_rd_map / pc88s_wr_map (pc88_disk.cpp), I/O
   InZ80Sub / OutZ80Sub; it runs from flash (it is stopped most of the time). */
#ifndef Z80CORE_CONFIG_H
#define Z80CORE_CONFIG_H
#include <stdint.h>

#ifdef Z80CORE_SUB
extern uint8_t *pc88s_rd_map[], *pc88s_wr_map[];
uint8_t InZ80Sub(uint16_t port);
void OutZ80Sub(uint16_t port, uint8_t value);
#define Z80CORE_RD(a) (pc88s_rd_map[(a) >> 12][(a) & 0x0FFF])
#define Z80CORE_WR(a, v) (pc88s_wr_map[(a) >> 12][(a) & 0x0FFF] = (v))
#define Z80CORE_IN(p) ({ Z80CORE_SYNC_OUT(); const uint8_t vi_ = InZ80Sub(p); Z80CORE_SYNC_IN(); vi_; })
#define Z80CORE_OUT(p, v) do { Z80CORE_SYNC_OUT(); OutZ80Sub((p), (v)); Z80CORE_SYNC_IN(); } while (0)
#else
extern uint8_t *pc88_rd_map[], *pc88_wr_map[];
uint8_t RdZ80Slow(uint16_t addr);
void WrZ80Slow(uint16_t addr, uint8_t value);
uint8_t InZ80(uint16_t port);
void OutZ80(uint16_t port, uint8_t value);
int pc88IrqTake(void);
#define Z80CORE_RD(a) ({ const uint8_t *pr_ = pc88_rd_map[(a) >> 10]; uint8_t vr_; \
  if (__builtin_expect(pr_ != 0, 1)) vr_ = pr_[(a) & 0x03FF]; else { Z80CORE_SYNC_OUT(); vr_ = RdZ80Slow(a); Z80CORE_SYNC_IN(); } vr_; })
#define Z80CORE_WR(a, v) do { uint8_t *pw_ = pc88_wr_map[(a) >> 10]; \
  if (__builtin_expect(pw_ != 0, 1)) pw_[(a) & 0x03FF] = (v); else { Z80CORE_SYNC_OUT(); WrZ80Slow((a), (v)); Z80CORE_SYNC_IN(); } } while (0)
#define Z80CORE_IN(p) ({ Z80CORE_SYNC_OUT(); const uint8_t vi_ = InZ80(p); Z80CORE_SYNC_IN(); vi_; })
#define Z80CORE_OUT(p, v) do { Z80CORE_SYNC_OUT(); OutZ80((p), (v)); Z80CORE_SYNC_IN(); } while (0)
#define Z80CORE_EI() (z->irq = 1)
#define Z80CORE_IRQ_ACK() ({ Z80CORE_SYNC_OUT(); const int va_ = pc88IrqTake(); Z80CORE_SYNC_IN(); z->irq = 0; va_; })
#if defined(ARDUINO)
#include <esp_attr.h>
#define Z80CORE_ED_ATTR IRAM_ATTR __attribute__((noinline))
#endif
#endif

#endif
