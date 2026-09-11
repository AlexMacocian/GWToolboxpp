//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// Derived from MiniEngine ShadowCamera by James Stanard.
//

#include "stdafx.h"

#include <Utils/ShadowCamera.h>

namespace {
    constexpr float kDirectionEpsilon = 0.000001f;

    bool IsFinite(const DirectX::XMFLOAT3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y)
            && std::isfinite(value.z);
    }
}

bool ShadowCamera::BuildDirectional(
    const DirectionalCameraConfig& config, DirectionalCamera& camera)
{
    camera = {};
    if (!IsFinite(config.light_direction) || !IsFinite(config.center)
        || !IsFinite(config.right_hint)
        || !std::isfinite(config.half_width)
        || !std::isfinite(config.half_height)
        || !std::isfinite(config.depth_half_range)
        || config.half_width <= 0.0f || config.half_height <= 0.0f
        || config.depth_half_range <= 0.0f
        || config.texture_width == 0 || config.texture_height == 0) {
        return false;
    }

    auto forward = DirectX::XMLoadFloat3(&config.light_direction);
    const auto forward_length_sq =
        DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(forward));
    if (forward_length_sq <= kDirectionEpsilon) return false;
    forward = DirectX::XMVector3Normalize(forward);

    auto right = DirectX::XMLoadFloat3(&config.right_hint);
    right = DirectX::XMVectorSubtract(
        right,
        DirectX::XMVectorScale(
            forward, DirectX::XMVectorGetX(DirectX::XMVector3Dot(right, forward))));
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(right))
        <= kDirectionEpsilon) {
        DirectX::XMFLOAT3 forward_value;
        DirectX::XMStoreFloat3(&forward_value, forward);
        const auto abs_x = std::fabs(forward_value.x);
        const auto abs_y = std::fabs(forward_value.y);
        const auto abs_z = std::fabs(forward_value.z);
        const auto reference = abs_x <= abs_y && abs_x <= abs_z
            ? DirectX::g_XMIdentityR0
            : abs_y <= abs_z
                ? DirectX::g_XMIdentityR1
                : DirectX::g_XMIdentityR2;
        right = DirectX::XMVector3Cross(reference, forward);
    }
    right = DirectX::XMVector3Normalize(right);
    auto up = DirectX::XMVector3Normalize(
        DirectX::XMVector3Cross(forward, right));
    right = DirectX::XMVector3Normalize(
        DirectX::XMVector3Cross(up, forward));

    DirectX::XMStoreFloat3(&camera.right, right);
    DirectX::XMStoreFloat3(&camera.up, up);
    DirectX::XMStoreFloat3(&camera.forward, forward);
    camera.requested_center = config.center;

    const auto rotation = DirectX::XMMatrixSet(
        camera.right.x, camera.up.x, camera.forward.x, 0.0f,
        camera.right.y, camera.up.y, camera.forward.y, 0.0f,
        camera.right.z, camera.up.z, camera.forward.z, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    const auto full_width = config.half_width * 2.0f;
    const auto full_height = config.half_height * 2.0f;
    const auto full_depth = config.depth_half_range * 2.0f;
    const auto depth_precision = std::clamp(config.depth_precision, 1u, 24u);
    const auto depth_levels =
        static_cast<float>((1u << depth_precision) - 1u);
    camera.bounds = {full_width, full_height, full_depth};
    camera.world_units_per_texel = {
        full_width / static_cast<float>(config.texture_width),
        full_height / static_cast<float>(config.texture_height),
        full_depth / depth_levels};

    auto center_light = DirectX::XMVector3TransformCoord(
        DirectX::XMLoadFloat3(&config.center), rotation);
    if (config.snap_to_texels) {
        DirectX::XMFLOAT3 value;
        DirectX::XMStoreFloat3(&value, center_light);
        value.x = std::round(
            value.x / camera.world_units_per_texel.x)
            * camera.world_units_per_texel.x;
        value.y = std::round(
            value.y / camera.world_units_per_texel.y)
            * camera.world_units_per_texel.y;
        value.z = std::round(
            value.z / camera.world_units_per_texel.z)
            * camera.world_units_per_texel.z;
        center_light = DirectX::XMLoadFloat3(&value);
    }

    const auto snapped_center = DirectX::XMVector3TransformCoord(
        center_light, DirectX::XMMatrixTranspose(rotation));
    DirectX::XMStoreFloat3(&camera.snapped_center, snapped_center);
    camera.target = camera.snapped_center;

    const auto eye = DirectX::XMVectorSubtract(
        snapped_center,
        DirectX::XMVectorScale(forward, config.depth_half_range));
    DirectX::XMStoreFloat3(&camera.eye, eye);

    auto view = rotation;
    view.r[3] = DirectX::XMVectorSet(
        -DirectX::XMVectorGetX(DirectX::XMVector3Dot(eye, right)),
        -DirectX::XMVectorGetX(DirectX::XMVector3Dot(eye, up)),
        -DirectX::XMVectorGetX(DirectX::XMVector3Dot(eye, forward)),
        1.0f);
    const auto projection = DirectX::XMMatrixOrthographicLH(
        full_width, full_height, 0.0f, full_depth);
    DirectX::XMStoreFloat4x4(&camera.view, view);
    DirectX::XMStoreFloat4x4(&camera.projection, projection);

    const auto handedness = DirectX::XMVectorGetX(
        DirectX::XMVector3Dot(
            DirectX::XMVector3Cross(up, forward), right));
    return IsFinite(camera.eye) && IsFinite(camera.target)
        && IsFinite(camera.right) && IsFinite(camera.up)
        && IsFinite(camera.forward) && IsFinite(camera.snapped_center)
        && handedness > 0.999f;
}
