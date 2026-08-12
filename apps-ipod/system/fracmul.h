/***************************************************************************
 * was: apps/fracmul.h
 * GNU General Public License (version 2+)
 *
 * Fixed-point fractional multiply helpers used by the DSP and scalers.
 ****************************************************************************/
#ifndef _FRACMUL_H
#define _FRACMUL_H

#include <stdint.h>
#include "gcc_extensions.h"

/** FRACTIONAL MULTIPLICATION
 *  Multiply two fixed point numbers with 31 fractional bits:
 *      FRACMUL(x, y)
 *
 *  Multiply two fixed point numbers with 31 fractional bits,
 *          then shift left by z bits:
 *      FRACMUL_SHL(x, y, z)
 *          NOTE: z must be in the range 1-8 on Coldfire targets.
 */


/* A bunch of fixed point assembler helper macros */

#if defined(CPU_ARM)

/* Multiply two S.31 fractional integers and return the sign bit and the
 * 31 most significant bits of the result.
 */
static inline int32_t FRACMUL(int32_t x, int32_t y)
{
    int32_t t, t2;
    asm ("smull    %[t], %[t2], %[a], %[b]\n\t"
         "mov      %[t2], %[t2], asl #1\n\t"
         "orr      %[t], %[t2], %[t], lsr #31\n\t"
         : [t] "=&r" (t), [t2] "=&r" (t2)
         : [a] "r" (x), [b] "r" (y));
    return t;
}

/* Multiply two S.31 fractional integers, and return the 32 most significant
 * bits after a shift left by the constant z.
 */
static FORCE_INLINE int32_t FRACMUL_SHL(int32_t x, int32_t y, int z)
{
    int32_t t, t2;
    asm ("smull    %[t], %[t2], %[a], %[b]\n\t"
         "mov      %[t2], %[t2], asl %[c]\n\t"
         "orr      %[t], %[t2], %[t], lsr %[d]\n\t"
         : [t] "=&r" (t), [t2] "=&r" (t2)
         : [a] "r" (x), [b] "r" (y),
           [c] "Mr" ((z) + 1), [d] "Mr" (31 - (z)));
    return t;
}

#else

/* Both hardware targets are ARM. A simulator is not: config.h ties CPU_ARM to
 * ARCH, which a sim build leaves unset. */

static inline int32_t FRACMUL(int32_t x, int32_t y)
{
    return (int32_t) (((int64_t)x * y) >> 31);
}

static inline int32_t FRACMUL_SHL(int32_t x, int32_t y, int z)
{
    return (int32_t) (((int64_t)x * y) >> (31 - z));
}

#endif /* CPU_ARM */

#endif
