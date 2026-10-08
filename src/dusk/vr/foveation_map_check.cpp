#undef NDEBUG
#include "foveation_map.hpp"

#include <cassert>
#include <cstdint>

namespace {

using dusk::vr::FoveationCenter;

std::uint8_t at(const std::vector<std::uint8_t>& rg, std::uint32_t mapWidth, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * mapWidth + x) * 2;
    assert(rg[i] == rg[i + 1]);
    return rg[i];
}

int countFull(const std::vector<std::uint8_t>& rg) {
    int n = 0;
    for (std::size_t i = 0; i < rg.size(); i += 2) {
        n += rg[i] == 255;
    }
    return n;
}

} // namespace

int main() {
    // Quest 2 recommended eye size, double-wide; 3664 isn't a multiple of 16 * 2.
    constexpr std::uint32_t kWidth = 3664, kHeight = 1920;
    const std::uint32_t mapWidth = dusk::vr::foveationMapSize(kWidth);
    const std::uint32_t mapHeight = dusk::vr::foveationMapSize(kHeight);
    assert(mapWidth == 229 && mapHeight == 120);
    assert(dusk::vr::foveationMapSize(3670) == 230); // rounds up

    // Symmetric FOV: centres in the middle of each half.
    const FoveationCenter left = dusk::vr::foveationCenter(0, kWidth, kHeight, -0.8f, 0.8f, 0.8f, -0.8f);
    const FoveationCenter right = dusk::vr::foveationCenter(1, kWidth, kHeight, -0.8f, 0.8f, 0.8f, -0.8f);
    assert(left.x == 916 / 16 && left.y == 960 / 16);
    assert(right.x == (1832 + 916) / 16 && right.y == 960 / 16);

    // Asymmetric FOV (narrower right and upper sides): the centre shifts
    // towards the narrow sides -- right and up.
    const FoveationCenter asym = dusk::vr::foveationCenter(0, kWidth, kHeight, -0.95f, 0.70f, 0.70f, -0.85f);
    assert(asym.x > left.x && asym.y < left.y);

    // Degenerate FOV falls back to the middle instead of dividing by zero.
    assert(dusk::vr::foveationCenter(0, kWidth, kHeight, 0.f, 0.f, 0.f, 0.f) == left);

    const FoveationCenter centers[2] = {left, right};
    for (int level = 1; level <= 3; ++level) {
        const auto rg = dusk::vr::buildFoveationMap(kWidth, kHeight, centers, level);
        assert(rg.size() == static_cast<std::size_t>(mapWidth) * mapHeight * 2);
        // Full density at both centres, quarter in the corners of both eyes.
        assert(at(rg, mapWidth, left.x, left.y) == 255);
        assert(at(rg, mapWidth, right.x, right.y) == 255);
        assert(at(rg, mapWidth, 0, 0) == 64);
        assert(at(rg, mapWidth, mapWidth - 1, mapHeight - 1) == 64);
        assert(at(rg, mapWidth, mapWidth / 2, 0) == 64);
        // Half density just outside the full-density disc.
        const auto fullRadius = static_cast<std::uint32_t>(dusk::vr::kFoveationRadii[level - 1][0] * kHeight /
                                                           dusk::vr::kFoveationTexel);
        assert(at(rg, mapWidth, left.x, left.y + fullRadius + 1) == 128);
        // Only the three densities appear.
        for (std::size_t i = 0; i < rg.size(); ++i) {
            assert(rg[i] == 255 || rg[i] == 128 || rg[i] == 64);
        }
    }

    // Higher levels shrink the full-resolution area.
    const int low = countFull(dusk::vr::buildFoveationMap(kWidth, kHeight, centers, 1));
    const int medium = countFull(dusk::vr::buildFoveationMap(kWidth, kHeight, centers, 2));
    const int high = countFull(dusk::vr::buildFoveationMap(kWidth, kHeight, centers, 3));
    assert(low > medium && medium > high && high > 0);

    // Each half follows its own eye's centre: shifting only the right centre
    // leaves the left half untouched.
    const FoveationCenter shifted[2] = {left, {right.x + 10, right.y}};
    const auto a = dusk::vr::buildFoveationMap(kWidth, kHeight, centers, 2);
    const auto b = dusk::vr::buildFoveationMap(kWidth, kHeight, shifted, 2);
    for (std::uint32_t y = 0; y < mapHeight; ++y) {
        for (std::uint32_t x = 0; x < mapWidth / 2; ++x) {
            assert(at(a, mapWidth, x, y) == at(b, mapWidth, x, y));
        }
    }
    return 0;
}
