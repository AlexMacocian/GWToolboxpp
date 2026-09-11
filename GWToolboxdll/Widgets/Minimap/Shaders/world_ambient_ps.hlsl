// Day/night ambient tint for the world. Drawn as a fullscreen multiply over the
// scene at the post-world seam, so it dims terrain and agents without touching
// the HUD. Sky pixels are skipped: the atmosphere shader does its own night
// darkening, and multiplying it here would darken it twice.
#include "world_lights.hlsli"
#include "scene_depth.hlsli"

sampler2D camera_depth : register(s0);

float4 ambient_tint : register(c0); // rgb = multiply colour
row_major float4x4 inverse_view_projection : register(c2);

float4 main(float2 screen_uv : TEXCOORD0) : COLOR0
{
    // Never paint over a frame that renders its own 3D - see scene_depth.hlsli.
    if (IsExcludedPixel(screen_uv)) discard;
    const float depth = tex2D(camera_depth, SceneDepthUV(screen_uv)).r;
    if (IsSkyDepth(depth)) {
        discard;
    }

    float3 tint = ambient_tint.rgb;

    // Lamps hold their own against the night. Only worth reconstructing a world position when
    // there are lights in range to find.
    [branch] if (world_light_params.x > 0.5) {
        const float4 clip_position = float4(
            screen_uv.x * 2.0 - 1.0, 1.0 - screen_uv.y * 2.0, depth, 1.0);
        float4 world_position = mul(inverse_view_projection, clip_position);
        world_position /= world_position.w;
        tint = lerp(tint, 1.0, WorldLightFraction(world_position.xyz));
    }

    return float4(tint, 1.0);
}
