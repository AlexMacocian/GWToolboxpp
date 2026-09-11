sampler2D shadow_map : register(s0);

float4 shadow_settings : register(c0); // x=bias, y=strength, z=texel size, w=plane compensation

float SampleLight(
    const float2 sample_uv, const float2 receiver_uv,
    const float receiver_depth, const float2 depth_gradient)
{
    const float shadow_depth = tex2D(shadow_map, sample_uv).r;
    const float plane_depth = receiver_depth
        + dot(
            depth_gradient * shadow_settings.w,
            sample_uv - receiver_uv);
    return plane_depth - shadow_settings.x <= shadow_depth ? 1.0 : 0.0;
}

float4 main(float4 light_position : TEXCOORD0) : COLOR0
{
    const float3 projected = light_position.xyz / light_position.w;
    const float2 uv =
        float2(projected.x * 0.5 + 0.5, projected.y * -0.5 + 0.5)
        + shadow_settings.z * 0.5;

    const float2 uv_dx = ddx(uv);
    const float2 uv_dy = ddy(uv);
    const float depth_dx = ddx(projected.z);
    const float depth_dy = ddy(projected.z);
    const float determinant = uv_dx.x * uv_dy.y - uv_dx.y * uv_dy.x;
    float2 depth_gradient = 0.0;
    if (abs(determinant) > 0.00000001) {
        depth_gradient = float2(
            depth_dx * uv_dy.y - depth_dy * uv_dx.y,
            uv_dx.x * depth_dy - uv_dy.x * depth_dx) / determinant;
    }

    if (projected.z <= 0.0 || projected.z >= 1.0
        || uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0) {
        discard;
    }

    const float2 texel_position = uv / shadow_settings.z - 0.5;
    const float2 interpolation = frac(texel_position);
    const float2 base_uv =
        (floor(texel_position) + 0.5) * shadow_settings.z;
    const float lit00 = SampleLight(
        base_uv, uv, projected.z, depth_gradient);
    const float lit10 = SampleLight(
        base_uv + float2(shadow_settings.z, 0.0),
        uv, projected.z, depth_gradient);
    const float lit01 = SampleLight(
        base_uv + float2(0.0, shadow_settings.z),
        uv, projected.z, depth_gradient);
    const float lit11 = SampleLight(
        base_uv + shadow_settings.zz,
        uv, projected.z, depth_gradient);
    const float lit = lerp(
        lerp(lit00, lit10, interpolation.x),
        lerp(lit01, lit11, interpolation.x),
        interpolation.y);
    const float shadow = (1.0 - lit) * shadow_settings.y;
    return float4(0.0, 0.0, 0.0, shadow);
}
