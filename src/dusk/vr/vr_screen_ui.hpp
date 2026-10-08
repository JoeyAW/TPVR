#pragma once

#include <algorithm>
#include <cstdint>

namespace dusk::vr {

// ponytail: two horizontal rectangles reuse Session's existing X-offset copy
// transport; no second Vulkan/D3D import implementation or immersive extent change.
struct ScreenUiLayout {
    uint32_t hudWidth;
    uint32_t hudHeight;
    uint32_t menuWidth;
    uint32_t menuHeight;

    constexpr uint32_t width() const { return hudWidth + menuWidth; }
    constexpr uint32_t height() const { return std::max(hudHeight, menuHeight); }
};

constexpr ScreenUiLayout screenUiLayout(uint32_t maxWidth, uint32_t maxHeight) {
    const float scale = std::min({1.0f, static_cast<float>(maxWidth) / 2880.0f,
                                static_cast<float>(maxHeight) / 900.0f});
    return {static_cast<uint32_t>(1280.0f * scale), static_cast<uint32_t>(720.0f * scale),
            static_cast<uint32_t>(1600.0f * scale), static_cast<uint32_t>(900.0f * scale)};
}

struct ScreenUiExtent {
    uint32_t width;
    uint32_t height;
};

// Fit arbitrary desktop/menu aspect ratios without stretching or cropping.
constexpr ScreenUiExtent fitScreenUi(uint32_t width, uint32_t height,
                                     uint32_t maxWidth, uint32_t maxHeight) {
    if (width == 0 || height == 0 || maxWidth == 0 || maxHeight == 0) {
        return {};
    }
    const float scale = std::min({1.0f, static_cast<float>(maxWidth) / static_cast<float>(width),
                                static_cast<float>(maxHeight) / static_cast<float>(height)});
    // Round once; scale <= 1 keeps a rounded edge within its max + 0.5 texel.
    return {std::min(maxWidth, std::max(1u, static_cast<uint32_t>(static_cast<float>(width) * scale + 0.5f))),
            std::min(maxHeight, std::max(1u, static_cast<uint32_t>(static_cast<float>(height) * scale + 0.5f)))};
}

static_assert(screenUiLayout(4096, 4096).width() == 2880);
static_assert(screenUiLayout(2048, 1024).width() <= 2048);
static_assert(screenUiLayout(4096, 512).height() <= 512);
static_assert(fitScreenUi(1920, 1080, 1600, 900).width == 1600);
static_assert(fitScreenUi(1000, 1000, 1600, 900).width == 900);
static_assert(fitScreenUi(0, 1080, 1600, 900).width == 0);

} // namespace dusk::vr
