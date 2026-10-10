/*
 * (C) 2026 see Authors.txt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
#include <memory>

struct SvpSceneResult {
    bool cut = false;
    double badFraction = 0;
    double meanError = 0;
    unsigned blocks = 0;
};

// Host-free adapter to the GPL SVPflow1 motion search. Each inference worker
// owns its adapter; it keeps allocation capacity, never source-frame history.
class CSvpSceneMotion {
public:
    CSvpSceneMotion();
    ~CSvpSceneMotion();
    CSvpSceneMotion(const CSvpSceneMotion&) = delete;
    CSvpSceneMotion& operator=(const CSvpSceneMotion&) = delete;
    bool Analyze(const uint8_t* firstBgra, unsigned firstPitch,
        const uint8_t* secondBgra, unsigned secondPitch,
        unsigned width, unsigned height, SvpSceneResult& result);
    void Reset();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
