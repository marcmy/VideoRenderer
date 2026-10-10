#include "../../Source/PausedFrameImageDiagnostics.h"

#include <cassert>
#include <iostream>
#include <algorithm>
#include <vector>
#include <utility>

int main()
{
    // Different alpha and row padding must not look like a changed video frame.
    std::array<uint8_t, 24> first = {1, 2, 3, 0, 4, 5, 6, 0, 90, 90, 90, 90,
        7, 8, 9, 0, 10, 11, 12, 0, 90, 90, 90, 90};
    auto same = first;
    for (size_t row = 0; row < 2; ++row) {
        same[row * 12 + 3] = 255;
        same[row * 12 + 7] = 128;
        for (size_t byte = 8; byte < 12; ++byte) {
            same[row * 12 + byte] = 45;
        }
    }
    const auto original = FingerprintPausedBgraImage(first.data(), 12, 2, 2);
    assert(original.width == 2 && original.height == 2);
    assert(original == FingerprintPausedBgraImage(same.data(), 12, 2, 2));
    same[12] ^= 1;
    const auto changed = FingerprintPausedBgraImage(same.data(), 12, 2, 2);
    assert(original != changed);
    const auto tinyChange = ComparePausedImageSamples(original, changed);
    assert(tinyChange.maximum == 1 && tinyChange.mean > 0.0 && tinyChange.mean < 1.0);
    std::array<uint8_t, 16> black = {}, white = {};
    white.fill(255);
    const auto sceneChange = ComparePausedImageSamples(
        FingerprintPausedBgraImage(black.data(), 8, 2, 2),
        FingerprintPausedBgraImage(white.data(), 8, 2, 2));
    assert(sceneChange.mean == 255.0 && sceneChange.maximum == 255);
    assert(FingerprintPausedBgraImage(nullptr, 12, 2, 2).width == 0);
    assert(FingerprintPausedBgraImage(first.data(), 7, 2, 2).width == 0);
    // Shape matters even if the visible RGB pixel sequence is the same.
    const std::array<uint8_t, 16> packed = {1, 2, 3, 0, 4, 5, 6, 0,
        7, 8, 9, 0, 10, 11, 12, 0};
    const auto flat = FingerprintPausedBgraImage(packed.data(), 16, 4, 1);
    assert(flat.hash == original.hash && flat != original);

    // Sparse CUDA rows must address exactly the same pixel centers as the full
    // D3D readback, including odd/portrait sizes and different channel orders.
    for (const auto& size : std::array<std::pair<uint32_t, uint32_t>, 4>{
            std::pair{1u, 1u}, {3u, 19u}, {320u, 570u}, {1080u, 1924u}}) {
        const auto [width, height] = size;
        const size_t pitch = static_cast<size_t>(width) * 4 + 32;
        std::vector<uint8_t> image(pitch * height);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                auto* pixel = image.data() + y * pitch + x * 4;
                pixel[0] = static_cast<uint8_t>(x * 13 + y * 7);
                pixel[1] = static_cast<uint8_t>(x + y * 3);
                pixel[2] = static_cast<uint8_t>(x * 5 + y + 11);
                pixel[3] = 255;
            }
        }
        // Assemble rows using a separate expression, not the sampling helper.
        std::vector<uint8_t> bgraRows(static_cast<size_t>(width) * 4 * 8);
        for (size_t row = 0; row < 8; ++row) {
            const auto y = (2 * row + 1) * height / 16;
            std::copy_n(image.data() + y * pitch, width * 4, bgraRows.data() + row * width * 4);
        }
        const auto full = FingerprintPausedBgraImage(image.data(), pitch, width, height);
        const auto sparse = FingerprintPausedImageRows(bgraRows.data(), width, height, false);
        assert(ComparePausedImageSamples(full, sparse).maximum == 0);
        auto rgbaRows = bgraRows;
        for (size_t byte = 0; byte < rgbaRows.size(); byte += 4) {
            std::swap(rgbaRows[byte], rgbaRows[byte + 2]);
            rgbaRows[byte + 3] = 0;
        }
        assert(sparse == FingerprintPausedImageRows(rgbaRows.data(), width, height, true));
        assert(sparse != FingerprintPausedImageRows(rgbaRows.data(), width, height, false));
    }
    assert(FingerprintPausedImageRows(nullptr, 8, 8, false).width == 0);

    using Kind = PausedImageCaptureKind;
    using State = CPausedFrameImageDiagnostics::State;
    CPausedFrameImageDiagnostics diagnostics;
    assert(!diagnostics.MarkPending(Kind::Seek));
    diagnostics.BeginSample(10);
    assert(!diagnostics.MarkPending(Kind::None));
    assert(diagnostics.MarkPending(Kind::Seek));
    assert(!diagnostics.MarkPending(Kind::Seek));
    diagnostics.Complete(10, Kind::Seek, original, changed);
    assert(diagnostics.Get(Kind::Seek).state == State::Ready);
    assert(diagnostics.Get(Kind::Seek).input == original);
    // Preserve the first capture, rather than silently replacing it on redraw.
    diagnostics.Complete(10, Kind::Seek, changed, original);
    assert(diagnostics.Get(Kind::Seek).input == original);
    assert(diagnostics.MarkPending(Kind::RedrawWithOsd));
    diagnostics.Complete(10, Kind::RedrawWithOsd, original, original);
    assert(diagnostics.Get(Kind::RedrawWithOsd).output == original);

    // A late GPU readback from a previous seek cannot complete the new seek.
    diagnostics.BeginSample(11);
    assert(diagnostics.MarkPending(Kind::Seek));
    diagnostics.Complete(10, Kind::Seek, original, changed);
    assert(diagnostics.Get(Kind::Seek).state == State::Pending);
    diagnostics.Complete(11, Kind::Seek, changed, original);
    assert(diagnostics.Get(Kind::Seek).input == changed);
    assert(diagnostics.MarkPending(Kind::RedrawWithoutOsd));
    diagnostics.Complete(11, Kind::RedrawWithoutOsd, {}, {}, -1);
    assert(diagnostics.Get(Kind::RedrawWithoutOsd).state == State::Failed);
    assert(diagnostics.Get(Kind::RedrawWithoutOsd).error == -1);
    diagnostics.BeginSample(0);
    assert(diagnostics.Get(Kind::Seek).state == State::Empty);
    assert(!diagnostics.NeedsCapture(Kind::Seek));
    std::cout << "Paused image diagnostics tests passed.\n";
}
