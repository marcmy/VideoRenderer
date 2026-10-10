
#pragma once

#include "x264_pixel.h"
#include "common.h"

const x264_pixel_function_t* x264functions();

static inline void BLIT_null(uint8_t*, const uint8_t*, int) {}
static inline int SAD_null(const uint8_t*, int, const uint8_t*, int) { return 0; }
static inline int AVERAGE_null(const uint8_t*, int) { return 0; }

void Copy32x32(uint8_t*, const uint8_t *, int);
void Copy32x16(uint8_t*, const uint8_t *, int);
void Copy16x16(uint8_t*, const uint8_t *, int);
void Copy16x8(uint8_t*, const uint8_t *, int);
void Copy8x8(uint8_t*, const uint8_t *, int);
void Copy8x4(uint8_t*, const uint8_t *, int);
void Copy4x4(uint8_t*, const uint8_t *, int);
void Copy4x2(uint8_t*, const uint8_t *, int);
void Copy2x2(uint8_t*, const uint8_t *, int);

int Sad32x32(const uint8_t*, int, const uint8_t*, int);
int Sad32x16(const uint8_t*, int, const uint8_t*, int);
int Sad4x2(const uint8_t*, int, const uint8_t*, int);
int Sad2x2(const uint8_t*, int, const uint8_t*, int);

int Satd32x32(const uint8_t*, int, const uint8_t*, int);
int Satd32x16(const uint8_t*, int, const uint8_t*, int);

int Luma32x32(const uint8_t*, int);
int Luma32x16(const uint8_t*, int);
int Luma16x16(const uint8_t*, int);
int Luma16x8(const uint8_t*, int);
int Luma8x8(const uint8_t*, int);
int Luma8x4(const uint8_t*, int);
int Luma4x4(const uint8_t*, int);
#define Luma4x2 AVERAGE_null
#define Luma2x2 AVERAGE_null

// x264
#define DECL_PIX( name, suffix, size ) \
    extern "C" int x264_pixel_##name##_##size##_##suffix ( const uint8_t *, int, const uint8_t *, int );

#define DECL_X1( name, suffix ) \
    DECL_PIX( name, suffix, 16x16 ) \
    DECL_PIX( name, suffix, 16x8 ) \
    DECL_PIX( name, suffix, 8x16 ) \
    DECL_PIX( name, suffix, 8x8 ) \
    DECL_PIX( name, suffix, 8x4 ) \
    DECL_PIX( name, suffix, 4x8 ) \
    DECL_PIX( name, suffix, 4x4 ) \
    DECL_PIX( name, suffix, 4x16 )

#ifndef Q_PROCESSOR_ARM64
DECL_X1( sad, mmx2 )
DECL_X1( sad, sse2 )
DECL_X1( sad, sse3 )
DECL_X1( sad, sse2_aligned )
DECL_X1( sad, ssse3 )
DECL_X1( sad, ssse3_aligned )
DECL_X1( sad, avx2 )
DECL_X1( ssd, mmx )
DECL_X1( ssd, mmx2 )
DECL_X1( ssd, sse2slow )
DECL_X1( ssd, sse2 )
DECL_X1( ssd, ssse3 )
DECL_X1( ssd, avx )
DECL_X1( ssd, xop )
DECL_X1( ssd, avx2 )
DECL_X1( satd, mmx2 )
DECL_X1( satd, sse2 )
DECL_X1( satd, ssse3 )
DECL_X1( satd, ssse3_atom )
DECL_X1( satd, sse4 )
DECL_X1( satd, avx )
DECL_X1( satd, xop )
DECL_X1( satd, avx2 )
DECL_X1( sad, cache32_mmx2 )
DECL_X1( sad, cache64_mmx2 )
DECL_X1( sad, cache64_sse2 )
DECL_X1( sad, cache64_ssse3 )
DECL_X1(sad, avx512)
DECL_X1(satd, avx512)
#else //ARM64
DECL_X1(sad, neon)
DECL_X1(satd, neon)
#endif

#undef DECL_PIX
#undef DECL_X1

