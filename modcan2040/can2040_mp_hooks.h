// Force-included (-include) into every MicroPython rp2 port source file so the
// port's soft-reset hook releases the CAN hardware before the heap is torn down.
#ifndef CAN2040_MP_HOOKS_H
#define CAN2040_MP_HOOKS_H

#ifndef __ASSEMBLER__
void can2040_mp_soft_reset(void);

#ifndef MICROPY_BOARD_START_SOFT_RESET
#define MICROPY_BOARD_START_SOFT_RESET() can2040_mp_soft_reset()
#endif
#endif // __ASSEMBLER__

#endif // CAN2040_MP_HOOKS_H
