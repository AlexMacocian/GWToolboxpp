// Surface gradients and foam, derived once per frame from the transformed surface so the water
// shader does not pay for them per screen pixel.
//
// Gradients rather than a packed normal: they stay correct under bilinear filtering (a filtered
// normal is not a normalised one), and the water pass has to rebuild the vector in Guild Wars'
// -z-up convention anyway.
//
// Foam comes from the Jacobian of the horizontal displacement. Where the surface folds in on
// itself the determinant drops below one, and that is exactly where a real wave is breaking - so
// whitecaps land on the faces that are actually pitching over rather than on whatever happens to
// be steep.
#include "ocean_common.hlsli"

sampler2D displacement : register(s0);

// x = resolution, y = patch size, z = foam sharpness, w unused
float4 ocean_params : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    const float resolution = ocean_params.x;
    const float2 texel = vpos - 0.5;
    const float2 step = float2(1.0, 0.0) / resolution;
    const float2 uv = (texel + 0.5) / resolution;

    // R = displacement x, G = height, B = displacement y (see ocean_spectrum_ps.hlsl).
    const float4 left = tex2D(displacement, uv - step.xy);
    const float4 right = tex2D(displacement, uv + step.xy);
    const float4 down = tex2D(displacement, uv - step.yx);
    const float4 up = tex2D(displacement, uv + step.yx);

    // World distance between the two samples of each central difference.
    const float spacing = 2.0 * ocean_params.y / resolution;
    const float2 height_gradient = float2(
        (right.g - left.g) / spacing, (up.g - down.g) / spacing);

    const float jacobian_xx = 1.0 + (right.r - left.r) / spacing;
    const float jacobian_yy = 1.0 + (up.b - down.b) / spacing;
    const float jacobian_xy = (up.r - down.r) / spacing;
    const float jacobian_yx = (right.b - left.b) / spacing;
    const float jacobian =
        jacobian_xx * jacobian_yy - jacobian_xy * jacobian_yx;
    const float foam = saturate((1.0 - jacobian) * ocean_params.z);

    return float4(height_gradient, foam, 0.0);
}
