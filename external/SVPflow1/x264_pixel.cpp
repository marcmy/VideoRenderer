/*****************************************************************************
 * pixel.c: pixel metrics
 *****************************************************************************
 * Copyright (C) 2003-2014 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
 *          Laurent Aimar <fenrir@via.ecp.fr>
 *          Jason Garrett-Glaser <darkshikari@gmail.com>
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

#include "common.h"
#include "x264_pixel.h"
#include "blockmath.h"

#ifdef Q_PROCESSOR_X86_64
#define ARCH_X86_64 1
#else
#define ARCH_X86 1
#endif

/****************************************************************************
 * x264_pixel_init:
 ****************************************************************************/
void x264_pixel_init(int cpu, x264_pixel_function_t *pixf)
{
    memset(pixf, 0, sizeof(*pixf));

#define INIT2_NAME( name1, name2, cpu ) \
    pixf->name1[PIXEL_16x16] = x264_pixel_##name2##_16x16##cpu;\
    pixf->name1[PIXEL_16x8]  = x264_pixel_##name2##_16x8##cpu;
#define INIT4_NAME( name1, name2, cpu ) \
    INIT2_NAME( name1, name2, cpu ) \
    pixf->name1[PIXEL_8x16]  = x264_pixel_##name2##_8x16##cpu;\
    pixf->name1[PIXEL_8x8]   = x264_pixel_##name2##_8x8##cpu;
#define INIT5_NAME( name1, name2, cpu ) \
    INIT4_NAME( name1, name2, cpu ) \
    pixf->name1[PIXEL_8x4]   = x264_pixel_##name2##_8x4##cpu;
#define INIT6_NAME( name1, name2, cpu ) \
    INIT5_NAME( name1, name2, cpu ) \
    pixf->name1[PIXEL_4x8]   = x264_pixel_##name2##_4x8##cpu;
#define INIT7_NAME( name1, name2, cpu ) \
    INIT6_NAME( name1, name2, cpu ) \
    pixf->name1[PIXEL_4x4]   = x264_pixel_##name2##_4x4##cpu;
#define INIT8_NAME( name1, name2, cpu ) \
    INIT7_NAME( name1, name2, cpu ) \
    pixf->name1[PIXEL_4x16]  = x264_pixel_##name2##_4x16##cpu;
#define INIT2( name, cpu ) INIT2_NAME( name, name, cpu )
#define INIT4( name, cpu ) INIT4_NAME( name, name, cpu )
#define INIT5( name, cpu ) INIT5_NAME( name, name, cpu )
#define INIT6( name, cpu ) INIT6_NAME( name, name, cpu )
#define INIT7( name, cpu ) INIT7_NAME( name, name, cpu )
#define INIT8( name, cpu ) INIT8_NAME( name, name, cpu )

#define INIT_ADS( cpu ) \
    pixf->ads[PIXEL_16x16] = x264_pixel_ads4##cpu;\
    pixf->ads[PIXEL_16x8] = x264_pixel_ads2##cpu;\
    pixf->ads[PIXEL_8x8] = x264_pixel_ads1##cpu;

//	INIT8(sad, );
//    INIT8_NAME(sad_aligned, sad, );
//	INIT7(sad_x3, );
//	INIT7(sad_x4, );
//	INIT8(ssd, );
//	INIT8(satd, );
//	INIT7(satd_x3, );
//	INIT7(satd_x4, );
//	INIT4(hadamard_ac, );
//	INIT_ADS();

//	pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16;
//	pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8;
//	pixf->var[PIXEL_16x16] = x264_pixel_var_16x16;
//	pixf->var[PIXEL_8x16] = x264_pixel_var_8x16;
//	pixf->var[PIXEL_8x8] = x264_pixel_var_8x8;
//	pixf->var2[PIXEL_8x16] = x264_pixel_var2_8x16;
//	pixf->var2[PIXEL_8x8] = x264_pixel_var2_8x8;

//	pixf->ssd_nv12_core = pixel_ssd_nv12_core;
//	pixf->ssim_4x4x2_core = ssim_4x4x2_core;
//	pixf->ssim_end4 = ssim_end4;
//	pixf->vsad = pixel_vsad;
//	pixf->asd8 = pixel_asd8;

//	pixf->intra_sad_x3_4x4 = x264_intra_sad_x3_4x4;
//	pixf->intra_satd_x3_4x4 = x264_intra_satd_x3_4x4;
//	pixf->intra_sad_x3_8x8 = x264_intra_sad_x3_8x8;
//	pixf->intra_sa8d_x3_8x8 = x264_intra_sa8d_x3_8x8;
//	pixf->intra_sad_x3_8x8c = x264_intra_sad_x3_8x8c;
//	pixf->intra_satd_x3_8x8c = x264_intra_satd_x3_8x8c;
//	pixf->intra_sad_x3_8x16c = x264_intra_sad_x3_8x16c;
//	pixf->intra_satd_x3_8x16c = x264_intra_satd_x3_8x16c;
//	pixf->intra_sad_x3_16x16 = x264_intra_sad_x3_16x16;
//	pixf->intra_satd_x3_16x16 = x264_intra_satd_x3_16x16;

#ifndef Q_PROCESSOR_ARM64
    if (cpu&X264_CPU_MMX)
    {
//		INIT8(ssd, _mmx);
    }

    if (cpu&X264_CPU_MMX2)
    {
        INIT8(sad, _mmx2);
//		INIT8_NAME(sad_aligned, sad, _mmx2);
//		INIT7(sad_x3, _mmx2);
//		INIT7(sad_x4, _mmx2);
        INIT8(satd, _mmx2);
//		INIT7(satd_x3, _mmx2);
//		INIT7(satd_x4, _mmx2);
//		INIT4(hadamard_ac, _mmx2);
//		INIT_ADS(_mmx2);
//		pixf->var[PIXEL_16x16] = x264_pixel_var_16x16_mmx2;
//		pixf->var[PIXEL_8x16] = x264_pixel_var_8x16_mmx2;
//		pixf->var[PIXEL_8x8] = x264_pixel_var_8x8_mmx2;
//		pixf->ssd_nv12_core = x264_pixel_ssd_nv12_core_mmx2;
#if ARCH_X86
//		pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_mmx2;
//		pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_mmx2;
//		pixf->intra_sa8d_x3_8x8 = x264_intra_sa8d_x3_8x8_mmx2;
//		pixf->ssim_4x4x2_core = x264_pixel_ssim_4x4x2_core_mmx2;
//		pixf->var2[PIXEL_8x8] = x264_pixel_var2_8x8_mmx2;
//		pixf->var2[PIXEL_8x16] = x264_pixel_var2_8x16_mmx2;
//		pixf->vsad = x264_pixel_vsad_mmx2;

        if (cpu&X264_CPU_CACHELINE_32)
        {
            INIT5(sad, _cache32_mmx2);
//			INIT4(sad_x3, _cache32_mmx2);
//			INIT4(sad_x4, _cache32_mmx2);
        }
        else if (cpu&X264_CPU_CACHELINE_64 && !(cpu&X264_CPU_SLOW_ATOM))
        {
            INIT5(sad, _cache64_mmx2);
//			INIT4(sad_x3, _cache64_mmx2);
//			INIT4(sad_x4, _cache64_mmx2);
        }
#else
        if (cpu&X264_CPU_CACHELINE_64 && !(cpu&X264_CPU_SLOW_ATOM))
        {
            pixf->sad[PIXEL_8x16] = x264_pixel_sad_8x16_cache64_mmx2;
            pixf->sad[PIXEL_8x8] = x264_pixel_sad_8x8_cache64_mmx2;
            pixf->sad[PIXEL_8x4] = x264_pixel_sad_8x4_cache64_mmx2;
//			pixf->sad_x3[PIXEL_8x16] = x264_pixel_sad_x3_8x16_cache64_mmx2;
//			pixf->sad_x3[PIXEL_8x8] = x264_pixel_sad_x3_8x8_cache64_mmx2;
//			pixf->sad_x4[PIXEL_8x16] = x264_pixel_sad_x4_8x16_cache64_mmx2;
//			pixf->sad_x4[PIXEL_8x8] = x264_pixel_sad_x4_8x8_cache64_mmx2;
        }
#endif
//		pixf->intra_satd_x3_16x16 = x264_intra_satd_x3_16x16_mmx2;
//		pixf->intra_sad_x3_16x16 = x264_intra_sad_x3_16x16_mmx2;
//		pixf->intra_satd_x3_8x16c = x264_intra_satd_x3_8x16c_mmx2;
//		pixf->intra_sad_x3_8x16c = x264_intra_sad_x3_8x16c_mmx2;
//		pixf->intra_satd_x3_8x8c = x264_intra_satd_x3_8x8c_mmx2;
//		pixf->intra_sad_x3_8x8c = x264_intra_sad_x3_8x8c_mmx2;
//		pixf->intra_sad_x3_8x8 = x264_intra_sad_x3_8x8_mmx2;
//		pixf->intra_satd_x3_4x4 = x264_intra_satd_x3_4x4_mmx2;
//		pixf->intra_sad_x3_4x4 = x264_intra_sad_x3_4x4_mmx2;
    }

    if (cpu&X264_CPU_SSE2)
    {
//		INIT5(ssd, _sse2slow);
//		INIT2_NAME(sad_aligned, sad, _sse2_aligned);
//		pixf->var[PIXEL_16x16] = x264_pixel_var_16x16_sse2;
//		pixf->ssd_nv12_core = x264_pixel_ssd_nv12_core_sse2;
//		pixf->ssim_4x4x2_core = x264_pixel_ssim_4x4x2_core_sse2;
//		pixf->ssim_end4 = x264_pixel_ssim_end4_sse2;
//		pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_sse2;
//		pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_sse2;
#if ARCH_X86_64
//		pixf->intra_sa8d_x3_8x8 = x264_intra_sa8d_x3_8x8_sse2;
//		pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_sse2;
#endif
//		pixf->var2[PIXEL_8x8] = x264_pixel_var2_8x8_sse2;
//		pixf->var2[PIXEL_8x16] = x264_pixel_var2_8x16_sse2;
//		pixf->vsad = x264_pixel_vsad_sse2;
//		pixf->asd8 = x264_pixel_asd8_sse2;
    }

    if ((cpu&X264_CPU_SSE2) && !(cpu&X264_CPU_SSE2_IS_SLOW))
    {
        INIT2(sad, _sse2);
//		INIT2(sad_x3, _sse2);
//		INIT2(sad_x4, _sse2);
        INIT6(satd, _sse2);
        pixf->satd[PIXEL_4x16] = x264_pixel_satd_4x16_sse2;
//		INIT6(satd_x3, _sse2);
//		INIT6(satd_x4, _sse2);
//		INIT4(hadamard_ac, _sse2);
//		INIT_ADS(_sse2);
//		pixf->var[PIXEL_8x8] = x264_pixel_var_8x8_sse2;
//		pixf->var[PIXEL_8x16] = x264_pixel_var_8x16_sse2;
//		pixf->intra_sad_x3_16x16 = x264_intra_sad_x3_16x16_sse2;
//		pixf->intra_satd_x3_8x16c = x264_intra_satd_x3_8x16c_sse2;
//		pixf->intra_sad_x3_8x16c = x264_intra_sad_x3_8x16c_sse2;
        if (cpu&X264_CPU_CACHELINE_64)
        {
//			INIT2(ssd, _sse2); /* faster for width 16 on p4 */
#if ARCH_X86
            INIT2(sad, _cache64_sse2);
//			INIT2(sad_x3, _cache64_sse2);
//			INIT2(sad_x4, _cache64_sse2);
#endif
            if (cpu&X264_CPU_SSE2_IS_FAST)
            {
//				pixf->sad_x3[PIXEL_8x16] = x264_pixel_sad_x3_8x16_cache64_sse2;
//				pixf->sad_x4[PIXEL_8x16] = x264_pixel_sad_x4_8x16_cache64_sse2;
            }
        }
    }

    if (cpu&X264_CPU_SSE2_IS_FAST && !(cpu&X264_CPU_CACHELINE_64))
    {
//		pixf->sad_aligned[PIXEL_8x16] = x264_pixel_sad_8x16_sse2;
        pixf->sad[PIXEL_8x16] = x264_pixel_sad_8x16_sse2;
//		pixf->sad_x3[PIXEL_8x16] = x264_pixel_sad_x3_8x16_sse2;
//		pixf->sad_x3[PIXEL_8x8] = x264_pixel_sad_x3_8x8_sse2;
//		pixf->sad_x3[PIXEL_8x4] = x264_pixel_sad_x3_8x4_sse2;
//		pixf->sad_x4[PIXEL_8x16] = x264_pixel_sad_x4_8x16_sse2;
//		pixf->sad_x4[PIXEL_8x8] = x264_pixel_sad_x4_8x8_sse2;
//		pixf->sad_x4[PIXEL_8x4] = x264_pixel_sad_x4_8x4_sse2;
    }

    if ((cpu&X264_CPU_SSE3) && (cpu&X264_CPU_CACHELINE_64))
    {
        INIT2(sad, _sse3);
//		INIT2(sad_x3, _sse3);
//		INIT2(sad_x4, _sse3);
    }

    if (cpu&X264_CPU_SSSE3)
    {
//		INIT4(hadamard_ac, _ssse3);
        if (!(cpu&X264_CPU_STACK_MOD4))
        {
//			pixf->intra_sad_x9_4x4 = x264_intra_sad_x9_4x4_ssse3;
//			pixf->intra_satd_x9_4x4 = x264_intra_satd_x9_4x4_ssse3;
//			pixf->intra_sad_x9_8x8 = x264_intra_sad_x9_8x8_ssse3;
#if ARCH_X86_64
//			pixf->intra_sa8d_x9_8x8 = x264_intra_sa8d_x9_8x8_ssse3;
#endif
        }
//		INIT_ADS(_ssse3);
        if (cpu&X264_CPU_SLOW_ATOM)
        {
//			pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_ssse3_atom;
//			pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_ssse3_atom;
            INIT6(satd, _ssse3_atom);
            pixf->satd[PIXEL_4x16] = x264_pixel_satd_4x16_ssse3_atom;
//			INIT6(satd_x3, _ssse3_atom);
//			INIT6(satd_x4, _ssse3_atom);
//			INIT4(hadamard_ac, _ssse3_atom);
#if ARCH_X86_64
//			pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_ssse3_atom;
#endif
        }
        else
        {
//			INIT8(ssd, _ssse3);
//			pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_ssse3;
//			pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_ssse3;
            INIT8(satd, _ssse3);
//			INIT7(satd_x3, _ssse3);
//			INIT7(satd_x4, _ssse3);
#if ARCH_X86_64
//			pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_ssse3;
#endif
        }
//		pixf->intra_satd_x3_16x16 = x264_intra_satd_x3_16x16_ssse3;
//		if (!(cpu&X264_CPU_SLOW_PSHUFB))
//			pixf->intra_sad_x3_16x16 = x264_intra_sad_x3_16x16_ssse3;
//		pixf->intra_satd_x3_8x16c = x264_intra_satd_x3_8x16c_ssse3;
//		pixf->intra_satd_x3_8x8c = x264_intra_satd_x3_8x8c_ssse3;
//		pixf->intra_sad_x3_8x8c = x264_intra_sad_x3_8x8c_ssse3;
//		pixf->var2[PIXEL_8x8] = x264_pixel_var2_8x8_ssse3;
//		pixf->var2[PIXEL_8x16] = x264_pixel_var2_8x16_ssse3;
//		pixf->asd8 = x264_pixel_asd8_ssse3;
        if (cpu&X264_CPU_CACHELINE_64)
        {
            INIT2(sad, _cache64_ssse3);
//			INIT2(sad_x3, _cache64_ssse3);
//			INIT2(sad_x4, _cache64_ssse3);
        }
        else
        {
//			INIT2(sad_x3, _ssse3);
//			INIT5(sad_x4, _ssse3);
        }
        if ((cpu&X264_CPU_SLOW_ATOM) || (cpu&X264_CPU_SLOW_SHUFFLE))
        {
//			INIT5(ssd, _sse2); /* on conroe, sse2 is faster for width8/16 */
        }
    }

    if (cpu&X264_CPU_SSE4)
    {
        INIT8(satd, _sse4);
//		INIT7(satd_x3, _sse4);
//		INIT7(satd_x4, _sse4);
//		INIT4(hadamard_ac, _sse4);
        if (!(cpu&X264_CPU_STACK_MOD4))
        {
//			pixf->intra_sad_x9_4x4 = x264_intra_sad_x9_4x4_sse4;
//			pixf->intra_satd_x9_4x4 = x264_intra_satd_x9_4x4_sse4;
//			pixf->intra_sad_x9_8x8 = x264_intra_sad_x9_8x8_sse4;
#if ARCH_X86_64
//			pixf->intra_sa8d_x9_8x8 = x264_intra_sa8d_x9_8x8_sse4;
#endif
        }
//		pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_sse4;
//		pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_sse4;
//		pixf->intra_satd_x3_8x16c = x264_intra_satd_x3_8x16c_sse4;
#if ARCH_X86_64
//		pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_sse4;
#endif
    }

    if (cpu&X264_CPU_AVX)
    {
//		INIT2_NAME(sad_aligned, sad, _sse2); /* AVX-capable CPUs doesn't benefit from an aligned version */
//		INIT2(sad_x3, _avx);
//		INIT2(sad_x4, _avx);
        INIT8(satd, _avx);
//		INIT7(satd_x3, _avx);
//		INIT7(satd_x4, _avx);
//		INIT_ADS(_avx);
//		INIT4(hadamard_ac, _avx);
        if (!(cpu&X264_CPU_STACK_MOD4))
        {
//			pixf->intra_sad_x9_4x4 = x264_intra_sad_x9_4x4_avx;
//			pixf->intra_satd_x9_4x4 = x264_intra_satd_x9_4x4_avx;
//			pixf->intra_sad_x9_8x8 = x264_intra_sad_x9_8x8_avx;
#if ARCH_X86_64
//			pixf->intra_sa8d_x9_8x8 = x264_intra_sa8d_x9_8x8_avx;
#endif
        }
//		INIT5(ssd, _avx);
//		pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_avx;
//		pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_avx;
//		pixf->intra_satd_x3_8x16c = x264_intra_satd_x3_8x16c_avx;
//		pixf->ssd_nv12_core = x264_pixel_ssd_nv12_core_avx;
//		pixf->var[PIXEL_16x16] = x264_pixel_var_16x16_avx;
//		pixf->var[PIXEL_8x16] = x264_pixel_var_8x16_avx;
//		pixf->var[PIXEL_8x8] = x264_pixel_var_8x8_avx;
//		pixf->ssim_4x4x2_core = x264_pixel_ssim_4x4x2_core_avx;
//		pixf->ssim_end4 = x264_pixel_ssim_end4_avx;
#if ARCH_X86_64
//		pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_avx;
#endif
    }

    if (cpu&X264_CPU_XOP)
    {
        INIT7(satd, _xop);
//		INIT7(satd_x3, _xop);
//		INIT7(satd_x4, _xop);
//		INIT4(hadamard_ac, _xop);
        if (!(cpu&X264_CPU_STACK_MOD4))
        {
//			pixf->intra_satd_x9_4x4 = x264_intra_satd_x9_4x4_xop;
        }
//		INIT5(ssd, _xop);
//		pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_xop;
//		pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_xop;
//		pixf->intra_satd_x3_8x16c = x264_intra_satd_x3_8x16c_xop;
//		pixf->ssd_nv12_core = x264_pixel_ssd_nv12_core_xop;
//		pixf->var[PIXEL_16x16] = x264_pixel_var_16x16_xop;
//		pixf->var[PIXEL_8x16] = x264_pixel_var_8x16_xop;
//		pixf->var[PIXEL_8x8] = x264_pixel_var_8x8_xop;
//		pixf->var2[PIXEL_8x8] = x264_pixel_var2_8x8_xop;
//		pixf->var2[PIXEL_8x16] = x264_pixel_var2_8x16_xop;
#if ARCH_X86_64
//		pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_xop;
#endif
    }

    if (cpu&X264_CPU_AVX2)
    {
//		INIT2(ssd, _avx2);
//		INIT2(sad_x3, _avx2);
//		INIT2(sad_x4, _avx2);
        INIT4(satd, _avx2);
//		INIT2(hadamard_ac, _avx2);
//		INIT_ADS(_avx2);
//		pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_avx2;
//		pixf->var[PIXEL_16x16] = x264_pixel_var_16x16_avx2;
//		pixf->var2[PIXEL_8x16] = x264_pixel_var2_8x16_avx2;
//		pixf->var2[PIXEL_8x8] = x264_pixel_var2_8x8_avx2;
//		pixf->intra_sad_x3_16x16 = x264_intra_sad_x3_16x16_avx2;
//		pixf->intra_sad_x9_8x8 = x264_intra_sad_x9_8x8_avx2;
//		pixf->intra_sad_x3_8x8c = x264_intra_sad_x3_8x8c_avx2;
//		pixf->ssd_nv12_core = x264_pixel_ssd_nv12_core_avx2;
#if ARCH_X86_64
//		pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_avx2;
#endif
    }

	if (cpu&X264_CPU_AVX512)
	{
		INIT8(sad, _avx512);
//		INIT8_NAME(sad_aligned, sad, _avx512);
//		INIT7(sad_x3, _avx512);
//		INIT7(sad_x4, _avx512);
		INIT8(satd, _avx512);
//		pixf->sa8d[PIXEL_8x8] = x264_pixel_sa8d_8x8_avx512;
//		pixf->var[PIXEL_8x8] = x264_pixel_var_8x8_avx512;
//		pixf->var[PIXEL_8x16] = x264_pixel_var_8x16_avx512;
//		pixf->var[PIXEL_16x16] = x264_pixel_var_16x16_avx512;
//		pixf->var2[PIXEL_8x8] = x264_pixel_var2_8x8_avx512;
//		pixf->var2[PIXEL_8x16] = x264_pixel_var2_8x16_avx512;
	}
#else //Q_PROCESSOR_ARM64
    INIT8( sad, _neon );
        // AArch64 has no distinct instructions for aligned load/store
//        INIT8_NAME( sad_aligned, sad, _neon );
//        INIT7( sad_x3, _neon );
//        INIT7( sad_x4, _neon );
//        INIT8( ssd, _neon );
    INIT8( satd, _neon );
//        INIT7( satd_x3, _neon );
//        INIT7( satd_x4, _neon );
//        INIT4( hadamard_ac, _neon );

//        pixf->sa8d[PIXEL_8x8]   = x264_pixel_sa8d_8x8_neon;
//        pixf->sa8d[PIXEL_16x16] = x264_pixel_sa8d_16x16_neon;
//        pixf->sa8d_satd[PIXEL_16x16] = x264_pixel_sa8d_satd_16x16_neon;

//        pixf->var[PIXEL_8x8]    = x264_pixel_var_8x8_neon;
//        pixf->var[PIXEL_8x16]   = x264_pixel_var_8x16_neon;
//        pixf->var[PIXEL_16x16]  = x264_pixel_var_16x16_neon;
//        pixf->var2[PIXEL_8x8]   = x264_pixel_var2_8x8_neon;
//        pixf->var2[PIXEL_8x16]  = x264_pixel_var2_8x16_neon;
//        pixf->vsad = x264_pixel_vsad_neon;
//        pixf->asd8 = x264_pixel_asd8_neon;

//        pixf->intra_sad_x3_4x4    = intra_sad_x3_4x4_neon;
//        pixf->intra_satd_x3_4x4   = intra_satd_x3_4x4_neon;
//        pixf->intra_sad_x3_8x8    = intra_sad_x3_8x8_neon;
//        pixf->intra_sa8d_x3_8x8   = intra_sa8d_x3_8x8_neon;
//        pixf->intra_sad_x3_8x8c   = intra_sad_x3_8x8c_neon;
//        pixf->intra_satd_x3_8x8c  = intra_satd_x3_8x8c_neon;
//        pixf->intra_sad_x3_8x16c  = intra_sad_x3_8x16c_neon;
//        pixf->intra_satd_x3_8x16c = intra_satd_x3_8x16c_neon;
//        pixf->intra_sad_x3_16x16  = intra_sad_x3_16x16_neon;
//        pixf->intra_satd_x3_16x16 = intra_satd_x3_16x16_neon;

//        pixf->ssd_nv12_core     = x264_pixel_ssd_nv12_core_neon;
//        pixf->ssim_4x4x2_core   = x264_pixel_ssim_4x4x2_core_neon;
//        pixf->ssim_end4         = x264_pixel_ssim_end4_neon;
#endif
}
