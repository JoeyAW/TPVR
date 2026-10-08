#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dusk::vr {

// Fixed foveated rendering: the VK_EXT_fragment_density_map texels (RG8 =
// horizontal/vertical density, 255 full, 128 half, 64 quarter) for the
// single-pass stereo target -- both eyes side by side, each targetWidth / 2
// wide. One map texel per kFoveationTexel x kFoveationTexel target pixels: a
// map of exactly ceil(size / 16) makes the spec's texel-size formula
// (2^ceil(log2(floor(fb / map)))) pick 16, Quest's minimum.
inline constexpr std::uint32_t kFoveationTexel = 16;

// {full-density radius, half-density radius} for levels 1..3 (Low, Medium,
// High) as fractions of the eye image height; quarter density beyond.
// ponytail: hand-picked, not yet judged in-headset -- the tuning knob.
inline constexpr float kFoveationRadii[3][2] = {{0.40f, 0.60f}, {0.30f, 0.45f}, {0.22f, 0.34f}};

inline std::uint32_t foveationMapSize(std::uint32_t targetSize) {
    return (targetSize + kFoveationTexel - 1) / kFoveationTexel;
}

// Map texel holding an eye's projection centre, i.e. where its view axis
// (tangent 0) lands: -tan(left) / (tan(right) - tan(left)) across the eye's
// half from its left edge, tan(up) / (tan(up) - tan(down)) down from the top.
// Angles in radians, OpenXR's XrFovf convention (left/down negative).
struct FoveationCenter {
    int x = 0;
    int y = 0;
    bool operator==(const FoveationCenter&) const = default;
};

inline FoveationCenter foveationCenter(int eye, std::uint32_t targetWidth, std::uint32_t targetHeight, float angleLeft,
                                       float angleRight, float angleUp, float angleDown) {
    const float l = std::tan(angleLeft), r = std::tan(angleRight);
    const float u = std::tan(angleUp), d = std::tan(angleDown);
    const float cu = r > l ? -l / (r - l) : 0.5f;
    const float cv = u > d ? u / (u - d) : 0.5f;
    const float eyeWidth = static_cast<float>(targetWidth / 2);
    return {static_cast<int>((static_cast<float>(eye) + cu) * eyeWidth / kFoveationTexel),
            static_cast<int>(cv * static_cast<float>(targetHeight) / kFoveationTexel)};
}

// foveationMapSize(targetWidth) x foveationMapSize(targetHeight) RG8 texels,
// row-major. A texel belongs to the eye its centre pixel falls in.
inline std::vector<std::uint8_t> buildFoveationMap(std::uint32_t targetWidth, std::uint32_t targetHeight,
                                                   const FoveationCenter (&centers)[2], int level) {
    const std::uint32_t mapWidth = foveationMapSize(targetWidth);
    const std::uint32_t mapHeight = foveationMapSize(targetHeight);
    const float texelsPerEyeHeight = static_cast<float>(targetHeight) / kFoveationTexel;
    const float full = kFoveationRadii[level - 1][0] * texelsPerEyeHeight;
    const float half = kFoveationRadii[level - 1][1] * texelsPerEyeHeight;
    std::vector<std::uint8_t> rg(static_cast<std::size_t>(mapWidth) * mapHeight * 2);
    for (std::uint32_t y = 0; y < mapHeight; ++y) {
        for (std::uint32_t x = 0; x < mapWidth; ++x) {
            const FoveationCenter& c = centers[x * kFoveationTexel + kFoveationTexel / 2 < targetWidth / 2 ? 0 : 1];
            const float dx = static_cast<float>(static_cast<int>(x) - c.x);
            const float dy = static_cast<float>(static_cast<int>(y) - c.y);
            const float dist2 = dx * dx + dy * dy;
            const std::uint8_t density = dist2 < full * full ? 255 : dist2 < half * half ? 128 : 64;
            const std::size_t i = (static_cast<std::size_t>(y) * mapWidth + x) * 2;
            rg[i] = density;
            rg[i + 1] = density;
        }
    }
    return rg;
}

} // namespace dusk::vr
