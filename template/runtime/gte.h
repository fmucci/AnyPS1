/* GTE (COP2) register file shared by the runtime and the oracle (queue P4-1 will add the commands). Behaviour of register reads/writes follows
 * psx-spx "GTE Register Summary" / "GTE Data Registers" / "GTE Control Registers": 16-bit registers read sign- or zero-extended, SXYP pushes the
 * screen-XY FIFO, IRGB writes IR1..IR3, LZCS write computes LZCR, FLAG bit 31 is the OR of the error bits. Commands abort until P4-1. */
#ifndef PS1_GTE_H
#define PS1_GTE_H
#include <stdint.h>
uint32_t gte_core_mfc2(uint32_t *d, uint32_t *c, unsigned n);
uint32_t gte_core_cfc2(uint32_t *d, uint32_t *c, unsigned n);
void gte_core_mtc2(uint32_t *d, uint32_t *c, unsigned n, uint32_t v);
void gte_core_ctc2(uint32_t *d, uint32_t *c, unsigned n, uint32_t v);
int gte_core_cmd(uint32_t *d, uint32_t *c, uint32_t imm25);   /* returns 0 = not implemented */
#endif
