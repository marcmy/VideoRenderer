#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct PausedImageFingerprint {
    uint64_t hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::array<uint32_t, 64> sampledRgb = {};

    bool operator==(const PausedImageFingerprint&) const = default;
};

inline uint32_t PausedImageSampleCoordinate(const uint32_t extent, const uint32_t index)
{
    return static_cast<uint32_t>((2ull * index + 1) * extent / 16);
}

// CUDA readback contains only the eight sampled rows, packed without GPU pitch.
// Normalize RGBA to the same BGR ordering used by D3D's BGRA8 fingerprints.
inline PausedImageFingerprint FingerprintPausedImageRows(const void* data,
    const uint32_t width, const uint32_t height, const bool rgba)
{
    if (!data || !width || !height) {
        return {};
    }
    PausedImageFingerprint fingerprint = {14695981039346656037ull, width, height};
    const auto* pixels = static_cast<const uint8_t*>(data);
    for (uint32_t y = 0; y < 8; ++y) {
        const auto* row = pixels + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < 8; ++x) {
            const auto* pixel = row + static_cast<size_t>(PausedImageSampleCoordinate(width, x)) * 4;
            const uint32_t rgb = pixel[rgba ? 2 : 0] | (static_cast<uint32_t>(pixel[1]) << 8)
                | (static_cast<uint32_t>(pixel[rgba ? 0 : 2]) << 16);
            fingerprint.sampledRgb[y * 8 + x] = rgb;
            fingerprint.hash = (fingerprint.hash ^ rgb) * 1099511628211ull;
        }
    }
    return fingerprint;
}

// Fingerprint visible BGRA8 pixels, excluding undefined alpha and row padding.
// Readback callers supply a completed, nonblocking staging-texture mapping.
inline PausedImageFingerprint FingerprintPausedBgraImage(const void* data,
    const size_t rowPitch, const uint32_t width, const uint32_t height)
{
    if (!data || !width || !height || width > rowPitch / 4) {
        return {};
    }
    uint64_t hash = 14695981039346656037ull;
    const auto* row = static_cast<const uint8_t*>(data);
    for (uint32_t y = 0; y < height; ++y, row += rowPitch) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto* pixel = row + static_cast<size_t>(x) * 4;
            const uint32_t rgb = pixel[0] | (static_cast<uint32_t>(pixel[1]) << 8)
                | (static_cast<uint32_t>(pixel[2]) << 16);
            hash = (hash ^ rgb) * 1099511628211ull;
        }
    }
    PausedImageFingerprint fingerprint = {hash, width, height};
    // A small grid quantifies large image changes separately from a hash
    // changing due to tiny numerical differences. It needs no extra readback.
    const auto* pixels = static_cast<const uint8_t*>(data);
    for (uint32_t y = 0; y < 8; ++y) {
        const auto* sampleRow = pixels + static_cast<size_t>(PausedImageSampleCoordinate(height, y)) * rowPitch;
        for (uint32_t x = 0; x < 8; ++x) {
            const auto* pixel = sampleRow + static_cast<size_t>(PausedImageSampleCoordinate(width, x)) * 4;
            fingerprint.sampledRgb[y * 8 + x] = pixel[0]
                | (static_cast<uint32_t>(pixel[1]) << 8) | (static_cast<uint32_t>(pixel[2]) << 16);
        }
    }
    return fingerprint;
}

struct PausedImageDifference {
    double mean = 0.0;
    uint32_t maximum = 0;
};

inline PausedImageDifference ComparePausedImageSamples(const PausedImageFingerprint& first,
    const PausedImageFingerprint& second)
{
    PausedImageDifference difference;
    for (size_t pixel = 0; pixel < first.sampledRgb.size(); ++pixel) {
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const uint32_t a = (first.sampledRgb[pixel] >> shift) & 255;
            const uint32_t b = (second.sampledRgb[pixel] >> shift) & 255;
            const uint32_t delta = a > b ? a - b : b - a;
            difference.mean += delta;
            if (delta > difference.maximum) {
                difference.maximum = delta;
            }
        }
    }
    difference.mean /= first.sampledRgb.size() * 3;
    return difference;
}

enum class PausedImageCaptureKind : size_t {
    None,
    Seek,
    RedrawWithOsd,
    RedrawWithoutOsd,
    Count
};

class CPausedFrameImageDiagnostics
{
public:
    enum class State { Empty, Pending, Ready, Failed };
    struct Snapshot {
        State state = State::Empty;
        int32_t error = 0;
        PausedImageFingerprint input;
        PausedImageFingerprint output;
    };

    void BeginSample(const uint64_t serial)
    {
        m_serial = serial;
        m_snapshots = {};
    }

    uint64_t GetSerial() const { return m_serial; }

    const Snapshot& Get(const PausedImageCaptureKind kind) const
    {
        return m_snapshots[static_cast<size_t>(kind)];
    }

    bool NeedsCapture(const PausedImageCaptureKind kind) const
    {
        return m_serial && kind != PausedImageCaptureKind::None
            && kind < PausedImageCaptureKind::Count && Get(kind).state == State::Empty;
    }

    bool MarkPending(const PausedImageCaptureKind kind)
    {
        if (!NeedsCapture(kind)) {
            return false;
        }
        m_snapshots[static_cast<size_t>(kind)].state = State::Pending;
        return true;
    }

    void Complete(const uint64_t serial, const PausedImageCaptureKind kind,
        const PausedImageFingerprint input, const PausedImageFingerprint output,
        const int32_t error = 0)
    {
        // A readback from an earlier seek must never describe the new sample.
        if (serial != m_serial || kind == PausedImageCaptureKind::None
                || kind >= PausedImageCaptureKind::Count) {
            return;
        }
        auto& snapshot = m_snapshots[static_cast<size_t>(kind)];
        if (snapshot.state != State::Pending) {
            return;
        }
        snapshot = {error ? State::Failed : State::Ready, error, input, output};
    }

private:
    uint64_t m_serial = 0;
    std::array<Snapshot, static_cast<size_t>(PausedImageCaptureKind::Count)> m_snapshots = {};
};
