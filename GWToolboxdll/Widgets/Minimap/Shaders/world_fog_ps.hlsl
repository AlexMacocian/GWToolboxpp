// Distance haze that follows the sky.
//
// GW fogs distant geometry to a fixed colour taken from the map's environment data, which is
// chosen for daylight and never changes. Our day/night tint is a multiply over the finished
// frame, and a multiply cannot fix it: scaling a pale blue only makes it a darker pale blue, so
// the far mountains keep reading as daylit haze in the middle of the night.
//
// This replaces the far colour instead of scaling it. Distant pixels are already almost entirely
// GW's fog colour, so blending them toward ours effectively substitutes it - no need to find or
// hook whatever sets the original.
sampler2D camera_depth : register(s0);
#include "scene_depth.hlsli"

row_major float4x4 inverse_view_projection : register(c0);
float4 fog_colour : register(c4);  // rgb = far colour, already tinted for time of day
float4 fog_range : register(c5);   // (start, end, strength, unused)
float4 fog_camera : register(c6);  // xyz = camera world position
float4 fog_edge : register(c7);    // (texel width, texel height, edge strength, search radius)
float4 fog_debug : register(c8);    // x = debug view

// How close this pixel is to the sky, 1 right against it and falling to 0 at the search radius.
//
// Searched over a RADIUS rather than the four immediate neighbours. Four taps at one texel can
// only ever mark pixels directly touching the sky, so the repaired band was exactly one pixel
// wide no matter how hard it was applied - while the artifact itself is several pixels across.
// Detection was the limit, not strength, which is why raising the gain changed so little.
//
// The band being wider than one pixel also says the contamination is not plain anti-aliasing:
// that touches a single pixel. Whatever produces it - a resolve, an upscale, a filtered
// composite - it spreads over a few pixels, so the repair has to cover the same width.
//
// Eight directions, marched outward. The nearest hit wins, so a pixel is weighted by how close
// the sky actually is rather than by how many taps happened to land on it.
float SkyProximity(const float2 uv)
{
    const float2 step = float2(fog_edge.x, fog_edge.y);
    const float radius = max(fog_edge.w, 1.0);
    float nearest = 0.0;

    // Literal bound so it unrolls and every tap is a constant offset - see world_lights.hlsli
    // for what a dynamic bound costs in ps_3_0.
    [unroll] for (int r = 1; r <= 4; r++) {
        [branch] if ((float)r > radius) break;
        const float2 d = step * (float)r;
        float hit = 0.0;
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2( d.x, 0.0)).r) ? 1.0 : 0.0);
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2(-d.x, 0.0)).r) ? 1.0 : 0.0);
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2( 0.0, d.y)).r) ? 1.0 : 0.0);
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2( 0.0,-d.y)).r) ? 1.0 : 0.0);
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2( d.x, d.y)).r) ? 1.0 : 0.0);
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2(-d.x, d.y)).r) ? 1.0 : 0.0);
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2( d.x,-d.y)).r) ? 1.0 : 0.0);
        hit = max(hit, IsSkyDepth(tex2D(camera_depth, uv + float2(-d.x,-d.y)).r) ? 1.0 : 0.0);
        // Closer hits are worth more, and the first one found is the closest.
        nearest = max(nearest, hit * (1.0 - ((float)r - 1.0) / radius));
    }
    return nearest;
}

float4 main(float2 screen_uv : TEXCOORD0) : COLOR0
{
    // Never paint over a frame that renders its own 3D - see scene_depth.hlsli.
    if (IsExcludedPixel(screen_uv)) discard;
    const float depth = tex2D(camera_depth, SceneDepthUV(screen_uv)).r;

    // Debug view 1: what this pass believes about every pixel, drawn opaquely.
    //   blue  = classified as SKY (discarded by every world pass)
    //   grey  = geometry, shaded by raw depth so the far distance is visible
    //   red   = geometry with a sky neighbour, i.e. a silhouette edge
    // If the pale outline shows up BLUE here, the depth copy is calling geometry "sky" and the
    // fault is in the depth, not the colour. If it shows up grey, the depth is right and the
    // contamination is in GW's own blend.
    [branch] if (fog_debug.x > 0.5) {
        if (IsSkyDepth(depth)) return float4(0.0, 0.2, 1.0, 1.0);
        const float near_sky = SkyProximity(screen_uv);
        // Red where the repair acts, brightest closest to the sky, so the detected band can be
        // compared directly against the width of the artifact it is meant to cover.
        if (near_sky > 0.0) return float4(near_sky, 0.0, 0.0, 1.0);
        const float shade = saturate((depth - 0.99) * 100.0);
        return float4(shade, shade, shade, 1.0);
    }

    // The sky is drawn by our own composite pass and already carries the right colour; hazing it
    // a second time would double the effect at the horizon, which is exactly where they meet.
    if (IsSkyDepth(depth)) {
        discard;
    }

    const float4 clip_position = float4(
        screen_uv.x * 2.0 - 1.0, 1.0 - screen_uv.y * 2.0, depth, 1.0);
    float4 world_position = mul(inverse_view_projection, clip_position);
    world_position /= world_position.w;

    const float distance_to_pixel = length(world_position.xyz - fog_camera.xyz);
    const float fade = smoothstep(fog_range.x, fog_range.y, distance_to_pixel);

    // === silhouette repair ===
    //
    // Why an object outlined against the sky picks up a pale rim, while the same object against
    // other geometry does not.
    //
    // GW draws its sky first, then geometry over it with anti-aliased edges, so a silhouette
    // pixel ends up holding a BLEND of the object and whatever sky was behind it - and that sky
    // is GW's, chosen for daylight. Our own sky only replaces pixels that are entirely sky
    // (depth exactly at the far value); an edge pixel belongs to the geometry, keeps its
    // geometry depth, and is never touched. So GW's daylight blue survives inside the blend, on
    // exactly the pixels that border the sky and nowhere else. Against other geometry the edge
    // blends with that geometry instead, which is why those silhouettes look right.
    //
    // The lost quantity is the edge's sky coverage, and it can be recovered: how much of a
    // pixel's neighbourhood is sky is a good estimate of how much sky went into its blend. Four
    // taps give that, and pulling the pixel toward the sky colour by that fraction puts back
    // what the anti-aliasing mixed in - as it should have looked with our sky behind it.
    //
    // Costs nothing away from a silhouette: with no sky neighbour the term is zero.
    const float edge = saturate(SkyProximity(screen_uv) * fog_edge.z);

    // Premultiplied, and the coverage goes in alpha: with ONE / INVSRCALPHA this composites as
    // lerp(scene, fog_colour, fade), which is what a fog is - unlike the ambient tint's multiply,
    // it can change the HUE of what is behind it rather than only its brightness.
    // The repair is not scaled by the haze strength - it is correcting a blend that is wrong
    // regardless of how far away the pixel is, so a near silhouette needs it just as much.
    const float coverage = saturate(max(fade * fog_range.z, edge));
    return float4(fog_colour.rgb * coverage, coverage);
}
