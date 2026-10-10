#pragma once

#include <cstdint>
#include <algorithm>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <list>
#include <climits>
#include <string.h>
#include <stdio.h>
#ifndef Q_PROCESSOR_ARM64
#   include <emmintrin.h>
#else
#   include <arm_neon.h>
#endif

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#	include <Windows.h>

#	ifdef AVISYNTH_PLUGIN
#		include <avisynth/avisynth.h>
#	endif

#   undef MSVC
#	define MSVC (_MSC_VER && !__INTEL_COMPILER)

#endif

#ifdef VAPOURSYNTH_PLUGIN
#	include <vapoursynth/VapourSynth.h>
#endif

//std::min, std::max are SLOW in VC++ 2013!
#define _dmin(a,b) ((a)<(b) ? (a):(b))
#define _dmax(a,b) ((a)>(b) ? (a):(b))

template <class T>
inline T _fmin(T a, T b)
{
#if MSVC
    return _dmin(a, b);
#else
    return std::min(a, b);
#endif
}

template <class T>
inline T _fmax(T a, T b)
{
#if MSVC
    return _dmax(a, b);
#else
    return std::max(a, b);
#endif
}

#undef _dmin
#undef _dmax

#define IGNORE_VALUE -1000

#define CHROMA_SHIFT 2

#define VECTORS_FORWARD		1
#define VECTORS_BACKWARD	2

#ifndef Q_PROCESSOR_ARM64
extern "C" void x264_cpu_emms(void);
#endif
inline void _clear_sse()
{
#ifndef Q_PROCESSOR_ARM64
    // MSVC x64 no longer exposes the MMX intrinsic. Keep the upstream cleanup
    // through its existing assembly implementation, including small MMX blocks.
    x264_cpu_emms();
#endif
}

inline void* _aligned_alloc(size_t size, size_t align=16)
{
#ifndef Q_PROCESSOR_ARM64
    return _mm_malloc(size, align);
#else
    return aligned_alloc(align, size%align ? align*(1+size/align) : size);
#endif
}

#ifndef _INC_MALLOC
inline void _aligned_free(void *ptr)
{
#ifndef Q_PROCESSOR_ARM64
    _mm_free(ptr);
#else
    free(ptr);
#endif
}
#endif

