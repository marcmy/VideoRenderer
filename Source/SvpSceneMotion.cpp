/*
 * (C) 2026 see Authors.txt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "SvpSceneMotion.h"
#include "../external/SVPflow1/groupofplanes.h"
#include "../external/SVPflow1/pyramidlayout.h"
#include <malloc.h>
#include <new>

namespace {
constexpr unsigned Block = 16;
// Independent classifier, not SVPflow2's undocumented SAD/luma units.
constexpr double BlockError = 0.15;
constexpr double BadCoverage = 0.20;
struct AlignedFree { void operator()(uint8_t* value) const { _aligned_free(value); } };
using PlaneBuffer = std::unique_ptr<uint8_t, AlignedFree>;
PlaneBuffer Allocate(size_t size, uint8_t fill) {
    PlaneBuffer buffer(static_cast<uint8_t*>(_aligned_malloc(size, 32)));
    if (!buffer) throw std::bad_alloc();
    memset(buffer.get(), fill, size);
    return buffer;
}
struct Pyramid {
    PlaneBuffer y, u, v;
    std::unique_ptr<MVGroupOfFrames> frames;
    unsigned width = 0, height = 0, pitch = 0;
    Pyramid(unsigned w, unsigned h, unsigned levels) : width(w), height(h), pitch((w + 63) & ~31u) {
        // Constant aligned strides across levels; the extra row/stride guard
        // accommodates the upstream SIMD reduction kernels' boundary loads.
        unsigned rows = 0, uvRows = 0;
        for (unsigned i = 0; i < levels; ++i) {
            const unsigned ih = SvpPyramidLayout::planeSize(h, i);
            rows += ih; uvRows += ih / 2;
        }
        y = Allocate(static_cast<size_t>(pitch) * (rows + 4), 0);
        u = Allocate(static_cast<size_t>(pitch) * (uvRows + 4), 128);
        v = Allocate(static_cast<size_t>(pitch) * (uvRows + 4), 128);
        frames = std::make_unique<MVGroupOfFrames>(levels, w, h, 1);
        frames->Update(y.get(), pitch, u.get(), v.get(), pitch);
    }
    void Fill(const uint8_t* bgra, unsigned sourcePitch, unsigned visibleWidth, unsigned visibleHeight) {
        for (unsigned iy = 0; iy < height; ++iy) {
            const uint8_t* src = bgra + static_cast<size_t>(std::min(iy, visibleHeight - 1)) * sourcePitch;
            uint8_t* dst = y.get() + static_cast<size_t>(iy) * pitch;
            for (unsigned ix = 0; ix < width; ++ix) {
                const uint8_t* pixel = src + std::min(ix, visibleWidth - 1) * 4;
                dst[ix] = static_cast<uint8_t>((722 * pixel[0] + 7152 * pixel[1] + 2126 * pixel[2] + 5000) / 10000);
            }
            // Extend the row for SIMD reads. Only visible pixels contribute
            // to classification weights; model padding never enters the input.
            memset(dst + width, dst[width - 1], pitch - width);
        }
        frames->Reduce(2);
    }
};
}

struct CSvpSceneMotion::Impl {
    unsigned width, height, visibleWidth, visibleHeight;
    std::unique_ptr<Pyramid> first, second;
    std::unique_ptr<GroupOfPlanes> motion;
    std::vector<int> vectors;
    Impl(unsigned w, unsigned h) : width((w + Block - 1) / Block * Block),
        height((h + Block - 1) / Block * Block), visibleWidth(w), visibleHeight(h) {
        unsigned levels = 1;
        while ((width >> levels) >= Block && (height >> levels) >= Block) ++levels;
        first = std::make_unique<Pyramid>(width, height, levels);
        second = std::make_unique<Pyramid>(width, height, levels);
        GroupOfPlanes::InitData config = {};
        auto& ad = config.ad;
        ad.vectorsType = VECTORS_BACKWARD;
        ad.nBlkSizeX = ad.nBlkSizeY = Block;
        ad.nPel = 1; ad.nLvCount = levels;
        ad.nWidth = width; ad.nHeight = height;
        ad.nBlkX = width / Block; ad.nBlkY = height / Block;
        ad.delta = 1;
        config.searchType = HEX2SEARCH; config.nSearchParam = -8;
        config.nLambda = 40000; config.lsad = 32000;
        config.pnew = 50; config.thSAD = 4000;
        config.satd = 2; // coarse SATD, final block error is SAD
        config.fullMathWidth = 530;
        motion = std::make_unique<GroupOfPlanes>(config);
        vectors.resize(motion->GetArraySize());
    }
};

CSvpSceneMotion::CSvpSceneMotion() = default;
CSvpSceneMotion::~CSvpSceneMotion() = default;
void CSvpSceneMotion::Reset() { m_impl.reset(); }

bool CSvpSceneMotion::Analyze(const uint8_t* firstBgra, unsigned firstPitch,
    const uint8_t* secondBgra, unsigned secondPitch, unsigned width, unsigned height, SvpSceneResult& result) {
    result = {};
    // Keep CPU analysis bounded. Unsupported/tiny inputs use Image comparison.
    if (!firstBgra || !secondBgra || width < 32 || height < 32 || width > 4096 || height > 4096
        || static_cast<uint64_t>(width) * height > 1024 * 1024
        || firstPitch < width * 4 || secondPitch < width * 4) return false;
    try {
        if (!m_impl || m_impl->visibleWidth != width || m_impl->visibleHeight != height)
            m_impl = std::make_unique<Impl>(width, height);
        auto& state = *m_impl;
        state.first->Fill(firstBgra, firstPitch, width, height);
        state.second->Fill(secondBgra, secondPitch, width, height);
        int lambda = 0;
        // One direction, once per pair. No frame generation or frame-history
        // policy is taken from SVP; RIFE continues to own interpolation.
        state.motion->SearchMVs(state.second->frames.get(), state.first->frames.get(), HEX2SEARCH, 2,
            1.5, 100, 50, 50, 0, 0, false, false, &lambda, state.vectors.data(), nullptr);
        if (state.vectors[0] != static_cast<int>(state.vectors.size())) return false;
        uint64_t badArea = 0, totalArea = 0;
        double errorSum = 0;
        const unsigned columns = state.width / Block, rows = state.height / Block;
        for (unsigned by = 0; by < rows; ++by) for (unsigned bx = 0; bx < columns; ++bx) {
            const unsigned area = std::min(Block, width - bx * Block) * std::min(Block, height - by * Block);
            // SVPflow1 packs adjusted matching error in the low 24 bits,
            // and mean block brightness in the high byte. Ignore brightness.
            const auto packed = static_cast<uint32_t>(state.vectors[2 + (by * columns + bx) * 2]);
            const double error = static_cast<double>(packed & 0x00ffffff) / (Block * Block * 255.0);
            totalArea += area; errorSum += error * area;
            if (error > BlockError) badArea += area;
        }
        result.blocks = columns * rows;
        result.badFraction = static_cast<double>(badArea) / totalArea;
        result.meanError = errorSum / totalArea;
        result.cut = result.badFraction > BadCoverage;
        return true;
    } catch (const std::bad_alloc&) {
        Reset();
        return false;
    }
}
