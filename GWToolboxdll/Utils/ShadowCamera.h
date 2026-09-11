#pragma once

#include <cstdint>

#include <DirectXMath.h>

namespace ShadowCamera {
    struct DirectionalCameraConfig {
        DirectX::XMFLOAT3 light_direction;
        DirectX::XMFLOAT3 center;
        DirectX::XMFLOAT3 right_hint;
        float half_width;
        float half_height;
        float depth_half_range;
        uint32_t texture_width;
        uint32_t texture_height;
        uint32_t depth_precision;
        bool snap_to_texels;
    };

    struct DirectionalCamera {
        DirectX::XMFLOAT3 eye;
        DirectX::XMFLOAT3 target;
        DirectX::XMFLOAT3 right;
        DirectX::XMFLOAT3 up;
        DirectX::XMFLOAT3 forward;
        DirectX::XMFLOAT3 requested_center;
        DirectX::XMFLOAT3 snapped_center;
        DirectX::XMFLOAT3 bounds;
        DirectX::XMFLOAT3 world_units_per_texel;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 projection;
    };

    bool BuildDirectional(
        const DirectionalCameraConfig& config, DirectionalCamera& camera);
}
