// Textured weather sprite: samples the bound sprite, tints by the vertex colour, and dissolves the
// alpha with distance so particles fade out at the edge of the volume rather than popping.
float4 cur_pos : register(c0);       // xyz: camera focus
float4 max_dist : register(c1);      // x: hard cut distance
float4 fog_starts_at : register(c2); // x: where the fade begins
// rgb = day/night multiply. Particles are drawn as their own compositor callback, AFTER the
// fullscreen tint pass, so they never saw it - snow stayed daylight-white at midnight. Applying
// it here instead of reordering the passes is also the only correct option: these are blended
// sprites, and a screen-space multiply over them would dim whatever they were blended against.
float4 ambient_tint : register(c3);
// x = self-luminous. An ember carries its own light, so the day/night multiply below must not
// reach it - dimming it into the night would put it out exactly when it should be brightest.
float4 particle_flags : register(c4);

#include "world_lights.hlsli"

sampler2D sprite : register(s0);

struct PS_INPUT {
    float4 position : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    float4 world : TEXCOORD1;
};

float4 main(PS_INPUT input) : COLOR
{
    float3 delta = cur_pos.xyz - input.world.xyz;
    float pixel_dist = sqrt(dot(delta, delta));
    if (pixel_dist >= max_dist.x) discard;

    float4 output = tex2D(sprite, input.uv) * input.color;
    if (pixel_dist > fog_starts_at.x) {
        output.a *= 1.0 - saturate(
            (pixel_dist - fog_starts_at.x) / (max_dist.x - fog_starts_at.x));
    }

    // A particle is lit by exactly what lights the ground under it: the day/night tint, lifted
    // back toward full where a lamp reaches. Same lerp(tint, 1, L) the world tint uses, but fed
    // this particle's own world position rather than one reconstructed from depth - so a
    // snowflake drifting past a brazier brightens as it passes and dims again as it leaves,
    // which a screen-space pass could never do.
    float3 tint = ambient_tint.rgb;
    [branch] if (world_light_params.x > 0.5) {
        tint = lerp(tint, 1.0, WorldLightFraction(input.world.xyz));
    }
    output.rgb *= lerp(tint, 1.0, saturate(particle_flags.x));
    return output;
}
