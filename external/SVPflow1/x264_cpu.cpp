/*****************************************************************************
* cpu.c: cpu detection
*****************************************************************************
* Copyright (C) 2003-2014 x264 project
*
* Authors: Loren Merritt <lorenm@u.washington.edu>
*          Laurent Aimar <fenrir@via.ecp.fr>
*          Fiona Glaser <fiona@x264.com>
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

#include <string.h>

#include "common.h"
#include "x264_pixel.h"

#ifndef Q_PROCESSOR_ARM64
extern "C" int x264_cpu_cpuid_test( void );
extern "C" void x264_cpu_cpuid( uint32_t op, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx );
extern "C" uint64_t x264_cpu_xgetbv(int xcr);
extern "C" void x264_intel_cpu_indicator_init( void )
{}

uint32_t x264_cpu_detect(void)
{
	uint32_t cpu = 0;
	uint32_t eax, ebx, ecx, edx;
	uint32_t vendor[4] = { 0 };
	uint32_t max_extended_cap, max_basic_cap;
	uint64_t xcr0 = 0;

#ifndef Q_PROCESSOR_X86_64
	if (!x264_cpu_cpuid_test())
		return 0;
#endif

	x264_cpu_cpuid(0, &max_basic_cap, vendor + 0, vendor + 2, vendor + 1);
	if (max_basic_cap == 0)
		return 0;

	x264_cpu_cpuid(1, &eax, &ebx, &ecx, &edx);
	if (edx & 0x00800000)
		cpu |= X264_CPU_MMX;
	else
		return cpu;
	if (edx & 0x02000000)
		cpu |= X264_CPU_MMX2 | X264_CPU_SSE;
	if (edx & 0x04000000)
		cpu |= X264_CPU_SSE2;
	if (ecx & 0x00000001)
		cpu |= X264_CPU_SSE3;
	if (ecx & 0x00000200)
		cpu |= X264_CPU_SSSE3 | X264_CPU_SSE2_IS_FAST;
	if (ecx & 0x00080000)
		cpu |= X264_CPU_SSE4;
	if (ecx & 0x00100000)
		cpu |= X264_CPU_SSE42;

	if (ecx & 0x08000000) /* XGETBV supported and XSAVE enabled by OS */
	{
		xcr0 = x264_cpu_xgetbv(0);
		if ((xcr0 & 0x6) == 0x6) /* XMM/YMM state */
		{
			if (ecx & 0x10000000)
				cpu |= X264_CPU_AVX;
			if (ecx & 0x00001000)
				cpu |= X264_CPU_FMA3;
		}
	}

	if (max_basic_cap >= 7)
	{
		x264_cpu_cpuid(7, &eax, &ebx, &ecx, &edx);

		if (ebx & 0x00000008)
			cpu |= X264_CPU_BMI1;
		if (ebx & 0x00000100)
			cpu |= X264_CPU_BMI2;

		if ((xcr0 & 0x6) == 0x6) /* XMM/YMM state */
		{
			if (ebx & 0x00000020)
				cpu |= X264_CPU_AVX2;

			if ((xcr0 & 0xE0) == 0xE0) /* OPMASK/ZMM state */
			{
				if ((ebx & 0xD0030000) == 0xD0030000)
					cpu |= X264_CPU_AVX512;
			}
		}
	}

	x264_cpu_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
	max_extended_cap = eax;

	if (max_extended_cap >= 0x80000001)
	{
		x264_cpu_cpuid(0x80000001, &eax, &ebx, &ecx, &edx);

		if (ecx & 0x00000020)
			cpu |= X264_CPU_LZCNT;             /* Supported by Intel chips starting with Haswell */
		if (ecx & 0x00000040) /* SSE4a, AMD only */
		{
			int family = ((eax >> 8) & 0xf) + ((eax >> 20) & 0xff);
			cpu |= X264_CPU_SSE2_IS_FAST;      /* Phenom and later CPUs have fast SSE units */
			if (family == 0x14)
			{
				cpu &= ~X264_CPU_SSE2_IS_FAST; /* SSSE3 doesn't imply fast SSE anymore... */
				cpu |= X264_CPU_SSE2_IS_SLOW;  /* Bobcat has 64-bit SIMD units */
				cpu |= X264_CPU_SLOW_PALIGNR;  /* palignr is insanely slow on Bobcat */
			}
			if (family == 0x16)
			{
				cpu |= X264_CPU_SLOW_PSHUFB;   /* Jaguar's pshufb isn't that slow, but it's slow enough
											   * compared to alternate instruction sequences that this
											   * is equal or faster on almost all such functions. */
			}
		}

		if (cpu & X264_CPU_AVX)
		{
			if (ecx & 0x00000800) /* XOP */
				cpu |= X264_CPU_XOP;
			if (ecx & 0x00010000) /* FMA4 */
				cpu |= X264_CPU_FMA4;
		}

		if (!strcmp((char*)vendor, "AuthenticAMD"))
		{
			if (edx & 0x00400000)
				cpu |= X264_CPU_MMX2;
			if ((cpu&X264_CPU_SSE2) && !(cpu&X264_CPU_SSE2_IS_FAST))
				cpu |= X264_CPU_SSE2_IS_SLOW; /* AMD CPUs come in two types: terrible at SSE and great at it */
		}
	}

	if (!strcmp((char*)vendor, "GenuineIntel"))
	{
		x264_cpu_cpuid(1, &eax, &ebx, &ecx, &edx);
		int family = ((eax >> 8) & 0xf) + ((eax >> 20) & 0xff);
		int model = ((eax >> 4) & 0xf) + ((eax >> 12) & 0xf0);
		if (family == 6)
		{
			/* 6/9 (pentium-m "banias"), 6/13 (pentium-m "dothan"), and 6/14 (core1 "yonah")
			* theoretically support sse2, but it's significantly slower than mmx for
			* almost all of x264's functions, so let's just pretend they don't. */
			if (model == 9 || model == 13 || model == 14)
			{
				cpu &= ~(X264_CPU_SSE2 | X264_CPU_SSE3);
				//assert(!(cpu&(X264_CPU_SSSE3 | X264_CPU_SSE4)));
			}
			/* Detect Atom CPU */
			else if (model == 28)
			{
				cpu |= X264_CPU_SLOW_ATOM;
				cpu |= X264_CPU_SLOW_PSHUFB;
			}
			/* Conroe has a slow shuffle unit. Check the model number to make sure not
			* to include crippled low-end Penryns and Nehalems that don't have SSE4. */
			else if ((cpu&X264_CPU_SSSE3) && !(cpu&X264_CPU_SSE4) && model < 23)
				cpu |= X264_CPU_SLOW_SHUFFLE;
		}
	}

	if ((!strcmp((char*)vendor, "GenuineIntel") || !strcmp((char*)vendor, "CyrixInstead")) && !(cpu&X264_CPU_SSE42))
	{
		/* cacheline size is specified in 3 places, any of which may be missing */
		x264_cpu_cpuid(1, &eax, &ebx, &ecx, &edx);
		int cache = (ebx & 0xff00) >> 5; // cflush size
		if (!cache && max_extended_cap >= 0x80000006)
		{
			x264_cpu_cpuid(0x80000006, &eax, &ebx, &ecx, &edx);
			cache = ecx & 0xff; // cacheline size
		}
		if (!cache && max_basic_cap >= 2)
		{
			// Cache and TLB Information
            static const char cache32_ids[] = { 0x0a, 0x0c, 0x41, 0x42, 0x43, 0x44, 0x45, (char)0x82, (char)0x83, (char)0x84, (char)0x85, 0 };
            static const char cache64_ids[] = { 0x22, 0x23, 0x25, 0x29, 0x2c, 0x46, 0x47, 0x49, 0x60, 0x66, 0x67,
                0x68, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7c, 0x7f, (char)0x86, (char)0x87, 0 };
			uint32_t buf[4];
			int max, i = 0;
			do {
				x264_cpu_cpuid(2, buf + 0, buf + 1, buf + 2, buf + 3);
				max = buf[0] & 0xff;
				buf[0] &= ~0xff;
				for (int j = 0; j < 4; j++)
					if (!(buf[j] >> 31))
						while (buf[j])
						{
							if (strchr(cache32_ids, buf[j] & 0xff))
								cache = 32;
							if (strchr(cache64_ids, buf[j] & 0xff))
								cache = 64;
							buf[j] >>= 8;
						}
			} while (++i < max);
		}

		if (cache == 32)
			cpu |= X264_CPU_CACHELINE_32;
		else if (cache == 64)
			cpu |= X264_CPU_CACHELINE_64;
		//else
		//	x264_log(NULL, X264_LOG_WARNING, "unable to determine cacheline size\n");
	}

#if STACK_ALIGNMENT < 16
	cpu |= X264_CPU_STACK_MOD4;
#endif

	return cpu;
}
#else //ARM64
uint32_t x264_cpu_detect(void)
{
    return X264_CPU_ARMV8 | X264_CPU_NEON;
}
#endif
