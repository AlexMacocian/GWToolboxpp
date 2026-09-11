// Shared point-light lookup for the world tint.
//
// GW's torches and braziers are not props with a glow texture painted on: they are real point
// lights, loaded from the map's "LITE" chunk and gathered per draw call by GrRenderPrograms.
// The game is therefore ALREADY lighting the world with them. The only thing wrong is that our
// day/night tint is a flat multiply over the finished frame, so it dims a torch-lit wall exactly
// as much as the ground beside it - and a light source that goes dark at night is not a light
// source.
#ifndef REBIRTH_WORLD_LIGHTS_INCLUDED
#define REBIRTH_WORLD_LIGHTS_INCLUDED

// Fixed, and small on purpose - see the unrolled loop below for why this cannot be dynamic.
#define REBIRTH_MAX_WORLD_LIGHTS 8

// (any lights this frame, authority, radius scale, falloff exponent)
float4 world_light_params : register(c6);
// xyz = world position, w = outer radius. Unused slots are zeroed by the CPU, which makes them
// inert here: outer 0 gives a negative reach that saturates away, so they add nothing.
float4 world_light_position[REBIRTH_MAX_WORLD_LIGHTS] : register(c7);
// rgb = colour premultiplied by intensity, a = inner radius
float4 world_light_colour[REBIRTH_MAX_WORLD_LIGHTS] : register(c15);

// c31 is the shared exclusion rect (scene_depth.hlsli). The colour array grows upward from c15,
// so raising the light count far enough would silently overwrite it - fxc does not warn.
#if 15 + REBIRTH_MAX_WORLD_LIGHTS > 31
#error "REBIRTH_MAX_WORLD_LIGHTS overruns c31 (excluded_rect); move one of them."
#endif

// How strongly point lights hold this pixel against the day/night tint, 0..1.
//
// The frame GW hands us is albedo * (sun + lamps), and what the tint should produce is
// albedo * (sun * tint + lamps) - the sun dims into night, the lamps do not. So the correct
// per-pixel factor is lerp(tint, 1, L): a plain multiply can express it exactly, with no light
// added back and nothing re-rendered. All this function has to supply is L.
//
// L is a property of the LAMPS ALONE - distance, radius and brightness - and deliberately not
// of the time of day. It was originally divided by the sun's share, which is defensible physics
// (a torch really is invisible at noon) but made the lamps behave differently by hour, and the
// tint already handles that on its own: at midday the tint is 1, so lerp(1, 1, L) is a no-op no
// matter how large L gets. The time dependence therefore came for free, and computing it twice
// only made the effect impossible to reason about.
float WorldLightFraction(const float3 world_position)
{
    float lamps = 0.0;

    // FULLY UNROLLED over a literal bound, with the slot count a #define rather than a uniform.
    // Both are load-bearing.
    //
    // ps_3_0 has no relative addressing of constant registers - that is a vertex shader feature.
    // Written as a dynamic loop over a `count` uniform, fxc cannot fold world_light_position[i]
    // into a register reference, so it emits an eight-way compare-select cascade to pick the
    // light, wrapped in `rep i0` with i0 = 255 because the trip count is unknown at compile
    // time. That came out as a 224-instruction body that may run 255 times: ~57,000
    // instructions once a driver unrolls it, which is enough to take the shader compiler down.
    // And because wined3d defers that work to the next flush, it fell over inside GW's own
    // render rather than in ours - well away from the actual cause.
    //
    // Unrolled against a literal bound, every index is a compile-time constant, so each
    // iteration reads c7..c14 directly and the whole thing is a few dozen instructions with no
    // flow control at all. Inactive lights are handled by zeroing them, not by branching.
    [unroll] for (int i = 0; i < REBIRTH_MAX_WORLD_LIGHTS; i++) {
        const float3 to_light = world_light_position[i].xyz - world_position;
        const float distance_to_light = length(to_light);
        const float outer = world_light_position[i].w * world_light_params.z;
        const float inner = world_light_colour[i].a * world_light_params.z;
        // Full strength inside the inner radius, gone by the outer one - the same two-radius
        // model the engine's own lights use. Squared so the falloff reads as light rather than
        // as a flat disc with an edge.
        const float reach =
            saturate((outer - distance_to_light) / max(outer - inner, 1e-3));
        // Falloff is authored rather than fixed. A squared curve is a wide, soft pool; raising
        // the exponent pulls the light in tight around the source, which is what a torch
        // actually looks like.
        lamps += pow(reach, world_light_params.w)
            * dot(world_light_colour[i].rgb, float3(0.299, 0.587, 0.114));
    }

    return saturate(lamps * world_light_params.y);
}

#endif
