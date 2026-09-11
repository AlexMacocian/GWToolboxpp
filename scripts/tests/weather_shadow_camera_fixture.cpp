#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

#include <Utils/ShadowCamera.h>

constexpr float kDirectionEpsilon = 0.000001f;

REPLAY_FUNCTIONS

int main()
{
    ShadowCamera::DirectionalCameraConfig config{
        {0.4f, 0.7f, -0.6f}, {1234.5f, -785.25f, 95.5f}, {1.f, 0.f, 0.f},
        1994.f, 1994.f, 3500.f, 4096, 4096, 10, false};
    ShadowCamera::DirectionalCamera original{};
    assert(ShadowCamera::BuildDirectional(config, original));
    for (const auto precision : {8u, 23u, 24u}) {
        config.depth_precision = precision;
        ShadowCamera::DirectionalCamera current{};
        assert(ShadowCamera::BuildDirectional(config, current));
        assert(!std::memcmp(&original.eye, &current.eye, sizeof(current.eye)));
        assert(!std::memcmp(&original.target, &current.target, sizeof(current.target)));
        assert(!std::memcmp(&original.forward, &current.forward, sizeof(current.forward)));
        assert(!std::memcmp(&original.bounds, &current.bounds, sizeof(current.bounds)));
        assert(!std::memcmp(&original.view, &current.view, sizeof(current.view)));
        assert(!std::memcmp(&original.projection, &current.projection, sizeof(current.projection)));
    }
}
