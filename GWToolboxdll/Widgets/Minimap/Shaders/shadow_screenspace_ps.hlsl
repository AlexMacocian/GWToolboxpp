sampler2D camera_depth : register(s0);
sampler2D shadow_depth : register(s1);

// Both matrices are uploaded one row per register and consumed as mul(matrix, column_vector).
row_major float4x4 inverse_view_projection : register(c0);
row_major float4x4 light_world_view_projection : register(c4);
float4 shadow_settings : register(c8); // x=bias (world units), y=strength, z=shadow texel size, w=debug view
float4 depth_params : register(c9);    // x=depth slope, y=receiver-plane bias, z=orthographic
// x = water plane z (world), y = 1 when the water pass is drawing, z = depth over which a
// submerged shadow fades out (world units)
float4 water_surface : register(c16);
// c10, c11 and c19 are the shared cloud field - see clouds.hlsli.
#include "clouds.hlsli"
#include "scene_depth.hlsli"
// (strength, pattern units per world unit, deck altitude above the camera, octaves)
float4 cloud_shadow : register(c12);
float4 cloud_shadow_light : register(c13); // xyz = unit world direction toward the light
// The sky frame's horizontal axes in world space, so a world point can be expressed in the same
// coordinates the sky indexes its clouds by.
float4 cloud_sky_basis : register(c14);  // (sx.x, sx.y, sy.x, sy.y)
float4 cloud_camera : register(c15);     // xyz = camera world position

// How much light the cloud deck takes out of a point on the ground.
//
// Deliberately NOT done by drawing clouds into the shadow map. That map stores the depth of the
// nearest OPAQUE occluder, one value per texel - a cloud is neither opaque nor a surface, so the
// best it could express is a hard-edged hole. It also only covers a couple of thousand units
// around the player, while cloud shadows want the whole visible landscape.
//
// Instead the same density field the sky draws is evaluated here: walk from the receiving point
// toward the light until reaching cloud altitude, and see how much cloud is in the way. That is
// what a cloud shadow IS, it costs one ray-plane crossing plus a few noise samples, and it
// covers as far as the eye can see.
float CloudShadow(const float3 world_position)
{
    const float strength = cloud_shadow.x;
    [branch] if (strength <= 0.001) return 0.0;

    // GW's up is -z, so climbing means travelling against z. A light on the horizon would need
    // an infinite walk, so the divisor is floored - at which point the shadows stretch out and
    // then stop lengthening, rather than shooting off to infinity.
    const float climb = max(-cloud_shadow_light.z, 0.15);
    // The deck sits cloud_shadow.z above the CAMERA, so how far this particular receiver has to
    // climb depends on where it stands relative to the camera. Using the camera's own height for
    // every receiver would slide the shadows up and down hills.
    const float height_to_deck =
        cloud_shadow.z + (world_position.z - cloud_camera.z);
    const float travel = max(height_to_deck, 0.0) / climb;
    const float2 hit = world_position.xy + cloud_shadow_light.xy * travel;

    // Relative to the camera, then into the sky frame - which is exactly how the sky builds its
    // own coordinate (its view-ray term is the offset from the camera at deck height, in pattern
    // units). Adding the same accumulated origin puts both on the same field, so this reads the
    // SAME point of the SAME cloud that is drawn overhead.
    const float2 from_camera = hit - cloud_camera.xy;
    const float2 sky_frame = float2(
        dot(from_camera, cloud_sky_basis.xy), dot(from_camera, cloud_sky_basis.zw));
    float2 plane = sky_frame * cloud_shadow.y + cloud_origin.xy
        + float2(cloud_shape.z, cloud_light.w);
    plane += CloudWarp(plane);
    const float weather = CloudWeather(plane);
    // Fewer octaves than the sky uses: the fine detail of a cloud edge is lost in a shadow
    // thrown a kilometre and a half, so paying for it per screen pixel buys nothing.
    return CloudDensity(plane, (int)cloud_shadow.w, weather) * strength;
}

float SampleShadow(const float2 uv, const float receiver_depth)
{
    return receiver_depth <= tex2D(shadow_depth, uv).r ? 1.0 : 0.0;
}

float4 main(float2 screen_uv : TEXCOORD0) : COLOR0
{
    const int debug_view = (int)shadow_settings.w;
    // 1 = prove the quad reaches the backbuffer at all.
    if (debug_view == 1) {
        return float4(1.0, 0.0, 0.0, 1.0);
    }

    // Never paint over a frame that renders its own 3D - see scene_depth.hlsli.

    if (IsExcludedPixel(screen_uv)) discard;

    const float depth = tex2D(camera_depth, SceneDepthUV(screen_uv)).r;
    if (debug_view == 2) {
        return float4(depth, depth, depth, 1.0);
    }
    if (debug_view == 5) {
        const float shadow_texel = tex2D(shadow_depth, screen_uv).r;
        return float4(shadow_texel, shadow_texel, shadow_texel, 1.0);
    }
    // Amplify: an empty map reads exactly 1.0, a filled one reads slightly below.
    if (debug_view == 6) {
        const float shadow_texel = tex2D(shadow_depth, screen_uv).r;
        return float4(saturate((1.0 - shadow_texel) * 50.0), 0.0, 0.0, 1.0);
    }
    if (IsSkyDepth(depth)) {
        discard;
    }

    const float4 clip_position = float4(
        screen_uv.x * 2.0 - 1.0,
        1.0 - screen_uv.y * 2.0,
        depth,
        1.0);
    float4 world_position = mul(inverse_view_projection, clip_position);
    world_position /= world_position.w;
    if (debug_view == 3) {
        return float4(frac(abs(world_position.xyz) / 1000.0), 1.0);
    }

    // The mirrored-Z convention is already folded into the light matrix.
    const float4 light_position =
        mul(light_world_view_projection, float4(world_position.xyz, 1.0));
    const float3 projected = light_position.xyz / light_position.w;
    const float2 shadow_uv = float2(
        projected.x * 0.5 + 0.5,
        projected.y * -0.5 + 0.5);

    // Bias by moving the receiver toward the light along its own ray, so the
    // offset stays constant in world units. The receiver-plane term grows it
    // where the surface is near edge-on to the light, which is where terrain
    // otherwise shadows itself. Under a perspective light, depth is z/w and
    // distance along the ray is w; under an orthographic one, w is constant and
    // depth advances linearly, so the two need different algebra.
    const float is_ortho = depth_params.z;
    const float light_distance = is_ortho > 0.5
        ? light_position.z / max(depth_params.x, 1e-6)
        : light_position.w;
    const float receiver_slope =
        abs(ddx(light_distance)) + abs(ddy(light_distance));
    const float bias_distance =
        shadow_settings.x + depth_params.y * receiver_slope;
    const float receiver_depth = is_ortho > 0.5
        ? (light_position.z - depth_params.x * bias_distance) / light_position.w
        : (light_position.z - depth_params.x * bias_distance)
            / max(light_position.w - bias_distance, 1e-3);

    if (debug_view == 4) {
        return float4(shadow_uv, projected.z, 1.0);
    }
    // 7 = receiver depth vs stored caster depth, amplified around the crossover.
    // Out-of-coverage stays blue so the shadow map's footprint is visible.
    if (debug_view == 7) {
        if (projected.z <= 0.0 || projected.z >= 1.0
            || shadow_uv.x <= 0.0 || shadow_uv.x >= 1.0
            || shadow_uv.y <= 0.0 || shadow_uv.y >= 1.0) {
            return float4(0.0, 0.0, 1.0, 1.0);
        }
        const float stored = tex2D(shadow_depth, shadow_uv).r;
        return float4(
            saturate((receiver_depth - stored) * 200.0),
            saturate((stored - receiver_depth) * 200.0),
            0.0, 1.0);
    }
    // Cloud shadow first, because it applies to the whole world. The shadow map only covers a
    // couple of thousand units around the player, and everything beyond it used to be discarded
    // outright - so the geometry test may drop out below while this still has to be drawn.
    const float cloud_occlusion = CloudShadow(world_position.xyz);

    const bool inside_shadow_map =
        projected.z > 0.0 && projected.z < 1.0
        && shadow_uv.x > 0.0 && shadow_uv.x < 1.0
        && shadow_uv.y > 0.0 && shadow_uv.y < 1.0;

    float geometry_occlusion = 0.0;
    [branch] if (inside_shadow_map) {
        const float2 texel = shadow_settings.zz;
        const float lit =
            (SampleShadow(shadow_uv + texel * float2(-0.5, -0.5), receiver_depth)
            + SampleShadow(shadow_uv + texel * float2(0.5, -0.5), receiver_depth)
            + SampleShadow(shadow_uv + texel * float2(-0.5, 0.5), receiver_depth)
            + SampleShadow(shadow_uv + texel * float2(0.5, 0.5), receiver_depth))
            * 0.25;
        geometry_occlusion = (1.0 - lit) * shadow_settings.y;

        // Fade out at the edge of the coverage box.
        //
        // The map only reaches a couple of thousand units around the player, and the test above
        // is a hard in-or-out: every pixel inside takes a shadow and every pixel outside takes
        // none, so the boundary itself is drawn - a straight-edged slab lying across whatever
        // happens to be there, water and sand alike, moving with the player. It is the shape of
        // the light camera's box, not of anything in the world.
        //
        // Nothing can be known about casters beyond the box, so the honest edge is a gradual one:
        // shadows thin out over the last stretch of coverage instead of stopping on a line.
        const float2 from_centre = abs(shadow_uv * 2.0 - 1.0);
        const float border =
            1.0 - saturate((max(from_centre.x, from_centre.y) - 0.8) / 0.2);
        // The same at the near and far clip of the light camera, which produce the horizontal
        // edges the vertical fade above does not cover.
        const float depth_edge =
            saturate(projected.z / 0.05) * saturate((1.0 - projected.z) / 0.05);
        geometry_occlusion *= border * depth_edge;
    }
    else if (cloud_occlusion <= 0.001) {
        // Outside the map with no cloud overhead: nothing to draw, as before.
        discard;
    }

    // Fade the geometry shadow out under water.
    //
    // With GW's own water suppressed the depth buffer holds the SEABED, so this pass shades the
    // sea floor - and the water drawn over it afterwards is never fully opaque, so that shading
    // shows through as a large dark region wherever the coverage box reaches the sea. It is also
    // wrong twice over: the caster pass deliberately skips water, so the light camera never saw a
    // surface there, and a hard-edged terrain shadow metres below the waves is not something you
    // could see from above anyway - light that deep is scattered.
    //
    // Faded rather than cut, so a shadow crossing a beach carries on into the shallows and dies
    // out as the bottom drops away, instead of stopping at the waterline.
    [branch] if (water_surface.y > 0.5) {
        // Up is -z: a point below the surface has the LARGER z.
        const float submerged = world_position.z - water_surface.x;
        geometry_occlusion *=
            1.0 - saturate(submerged / max(water_surface.z, 1.0));
    }

    // Combined as two occluders in series - each passes a fraction of the light through and the
    // fractions multiply. Adding them would drive ground already in a building's shadow to black
    // the moment a cloud crossed it.
    const float shadow =
        1.0 - (1.0 - geometry_occlusion) * (1.0 - cloud_occlusion);
    return float4(0.0, 0.0, 0.0, shadow);
}
