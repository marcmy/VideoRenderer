/*****************************************************************************
 * pixel.c: pixel metrics
 *****************************************************************************
 * Copyright (C) 2004-2014 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
 *          Jason Garrett-Glaser <darkshikari@gmail.com>
            Henrik Gramner <henrik@gramner.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#ifndef X264_PIXEL_H
#define X264_PIXEL_H

#include <inttypes.h>

typedef int (*luma_function_t)(const uint8_t*,int);
typedef void (*copy_function_t)(uint8_t*,const uint8_t*,int);
// SSD assumes all args aligned
// other cmp functions assume first arg aligned
typedef int  (*x264_pixel_cmp_t) ( const uint8_t*, int, const uint8_t*, int );

enum
{
    PIXEL_16x16 = 0,
    PIXEL_16x8  = 1,
    PIXEL_8x16  = 2,
    PIXEL_8x8   = 3,
    PIXEL_8x4   = 4,
    PIXEL_4x8   = 5,
    PIXEL_4x4   = 6,

    /* Subsampled chroma only */
    PIXEL_4x16  = 7,  /* 4:2:2 */
    PIXEL_4x2   = 8,
    PIXEL_2x8   = 9,  /* 4:2:2 */
    PIXEL_2x4   = 10,
    PIXEL_2x2   = 11
};

typedef struct
{
    x264_pixel_cmp_t  sad[8];
//	x264_pixel_cmp_t  ssd[8];
    x264_pixel_cmp_t satd[8];
} x264_pixel_function_t;

void x264_pixel_init( int cpu, x264_pixel_function_t *pixf );
uint32_t x264_cpu_detect(void);

/* CPU flags from x264 */
#define X264_CPU_MMX                (1<<0)
#define X264_CPU_MMX2               (1<<1)  /* MMX2 aka MMXEXT aka ISSE */
#define X264_CPU_MMXEXT             X264_CPU_MMX2
#define X264_CPU_SSE                (1<<2)
#define X264_CPU_SSE2               (1<<3)
#define X264_CPU_LZCNT              (1<<4)
#define X264_CPU_SSE3               (1<<5)
#define X264_CPU_SSSE3              (1<<6)
#define X264_CPU_SSE4               (1<<7)  /* SSE4.1 */
#define X264_CPU_SSE42              (1<<8)  /* SSE4.2 */
#define X264_CPU_AVX                (1<<9)  /* Requires OS support even if YMM registers aren't used */
#define X264_CPU_XOP                (1<<10) /* AMD XOP */
#define X264_CPU_FMA4               (1<<11) /* AMD FMA4 */
#define X264_CPU_FMA3               (1<<12)
#define X264_CPU_BMI1               (1<<13)
#define X264_CPU_BMI2               (1<<14)
#define X264_CPU_AVX2               (1<<15)
#define X264_CPU_AVX512             (1<<16) /* AVX-512 {F, CD, BW, DQ, VL}, requires OS support */
/* x86 modifiers */
#define X264_CPU_CACHELINE_32       (1<<17) /* avoid memory loads that span the border between two cachelines */
#define X264_CPU_CACHELINE_64       (1<<18) /* 32/64 is the size of a cacheline in bytes */
#define X264_CPU_SSE2_IS_SLOW       (1<<19) /* avoid most SSE2 functions on Athlon64 */
#define X264_CPU_SSE2_IS_FAST       (1<<20) /* a few functions are only faster on Core2 and Phenom */
#define X264_CPU_SLOW_SHUFFLE       (1<<21) /* The Conroe has a slow shuffle unit (relative to overall SSE performance) */
#define X264_CPU_STACK_MOD4         (1<<22) /* if stack is only mod4 and not mod16 */
#define X264_CPU_SLOW_ATOM          (1<<23) /* The Atom is terrible: slow SSE unaligned loads, slow
* SIMD multiplies, slow SIMD variable shifts, slow pshufb,
* cacheline split penalties -- gather everything here that
* isn't shared by other CPUs to avoid making half a dozen
* new SLOW flags. */
#define X264_CPU_SLOW_PSHUFB        (1<<24) /* such as on the Intel Atom */
#define X264_CPU_SLOW_PALIGNR       (1<<25) /* such as on the AMD Bobcat */

#define X264_CPU_ARMV6           0x0000001U
#define X264_CPU_NEON            0x0000002U  /* ARM NEON */
#define X264_CPU_FAST_NEON_MRC   0x0000004U  /* Transfer from NEON to ARM register is fast (Cortex-A9) */
#define X264_CPU_ARMV8           0x0000008U

#endif
