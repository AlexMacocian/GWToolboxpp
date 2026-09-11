// Screen-space water. Guild Wars' own water surface is suppressed and this replaces it.
//
// Two things make it behave like water rather than like a tinted sheet:
//
//   1. The surface is found by INTERSECTING the view ray with the map's water plane. The water is
//      an infinite plane at a known height - GW models it that way too - so the intersection is
//      exact. The pass used to look for depth-buffer pixels lying near the plane, which painted
//      any beach that strayed into the tolerance and could never measure a depth.
//
//   2. With GW's water gone, the depth buffer holds the SEABED, so the distance from the surface
//      down to it is a real water depth. Absorption, transparency and the surf line all follow
//      it, and water is drawn exactly where the land dips below the waterline.
//
// The surface itself comes from the FFT ocean simulation (ocean_*.hlsl): a spectrum of thousands
// of wave components, inverse-transformed each frame into a tiling height field, horizontal
// displacement and a foam mask. Sampling it here costs two texture reads.
#include "scene_depth.hlsli"

sampler2D camera_depth : register(s0);
sampler2D ocean_displacement : register(s1); // r = displacement x, g = height, b = displacement y
sampler2D ocean_gradient : register(s2);     // rg = height gradient, b = foam (Jacobian fold)

row_major float4x4 inverse_view_projection : register(c0);
float4 water_plane : register(c4);      // x = plane z (world), w = overall strength
float4 camera_position : register(c5);
float4 sun_direction : register(c6);    // xyz world, w = daylight blend
float4 water_deep : register(c7);
float4 water_shallow : register(c8);
float4 water_foam_colour : register(c9);
float4 sky_horizon : register(c10);
float4 sky_zenith : register(c11);
float4 sun_colour : register(c12);      // rgb, w = specular power
// x = depth over which the water hides the bottom, y = surf depth, z = foam amount, w = scatter
float4 water_depth_params : register(c13);
// x = simulation patch size (world units), y = height scale (world units per simulation unit),
// z = characteristic wave height (world units), w = detail overlay scale
float4 ocean_sample : register(c14);
// x = detail overlay weight, y = march steps, z = wave envelope (the most the surface can stand
// above the plane, world units), w > 0.5 paints which pass OWNS each pixel:
//   blue = no water here (the ray never meets the plane), green = ours, magenta = the scene's
float4 ocean_detail : register(c15);
// How far the wave surface is searched along the ray. Past this the waves are small against the
// distance and the flat plane is indistinguishable from them.
static const float kMaxMarchDistance = 9000.0;
// x = swell amplitude (world units), y = base wavenumber (rad per world unit), z = steepness,
// w = time (seconds)
float4 swell_params : register(c16);
// xy = unit wind direction in world XY, z = fan half-angle (radians), w = gravity in world units
float4 swell_wind : register(c17);

// === swell ===
// Large waves marching downwind, on top of the simulated sea.
//
// The spectrum alone does not give this. It is a statistical description - thousands of small
// components in a broad band - and the eye reads that as texture rather than as waves. A real sea
// has both: a readable swell with detail riding on it. This is the readable half.
//
// Analytic, not sampled from a texture, so it has no tile at all: the FFT's patch repeats, this
// never does, and the longest waves - the ones a repeat shows up in first - come from here.
//
// SIX waves, and their wavelengths are deliberately INCOMMENSURATE. A sum of sinusoids repeats
// over the least common multiple of its wavelengths, so simple ratios (2:1, 3:2) come back into
// alignment quickly and beat into an obvious pattern.
//
// Two things then keep it from ever settling. The ratios below share no near-simple factor, so
// the spatial pattern does not close within sight; and each wave travels at the speed its own
// length implies, so whatever alignment does occur slides apart again on its own. A sum of sines
// looks mechanical when the sines are related and when they move together - these are neither.
static const int kSwellWaves = 4;
// Wavelength multipliers, chosen so no pair sits near a simple ratio (the closest is 0.8% off
// 5/3), so no two waves realign within sight.
static const float kSwellLength[4] = {0.605, 1.000, 1.374, 2.355};
// Amplitudes: a narrow band peaking at the chosen length, summing to 1. Equal amplitudes would
// read as stacked sines; this reads as a sea with a dominant swell and smaller sets between.
static const float kSwellWeight[4] = {0.235, 0.357, 0.302, 0.106};
// Long waves hold their line and short ones scatter, as a real sea does.
static const float kSwellFan[4] = {1.0, 0.35, -0.5, -0.15};
// Offsets so the set does not align at the world origin, which would leave one spot on every map
// where all four crests coincide.
static const float kSwellPhase[4] = {0.0, 4.1, 1.2, 3.4};
// Four waves only reach their combined amplitude where every crest lines up, which essentially
// never happens: the usual crest-to-trough is 2.8 * the rms of the weights, or about 1.5
// amplitudes, not 2. Dividing it back out is what makes the swell height slider mean the height
// you see - asking for 186 units and getting 112 is the sort of thing that gets tuned around
// forever.
static const float kSwellPeakToTrough = 1.496;

// Height only, and WITHOUT the horizontal displacement below - this is what the surface march
// calls several times per pixel, and the displacement would double its cost to move the answer by
// less than the march's own tolerance.
float SwellHeight(const float2 world_xy)
{
    if (swell_params.x <= 0.001) return 0.0;
    float height = 0.0;
    [unroll] for (int i = 0; i < kSwellWaves; i++) {
        const float angle = kSwellFan[i] * swell_wind.z;
        const float2 direction = float2(
            swell_wind.x * cos(angle) - swell_wind.y * sin(angle),
            swell_wind.x * sin(angle) + swell_wind.y * cos(angle));
        const float wavenumber = swell_params.y / kSwellLength[i];
        const float amplitude =
            swell_params.x * kSwellWeight[i] / kSwellPeakToTrough;
        // Deep-water dispersion, so each wave travels at the speed its own length implies. Long
        // waves outrunning short ones is why the set never settles into a fixed pattern.
        const float phase = wavenumber * dot(direction, world_xy)
            - sqrt(swell_wind.w * wavenumber) * swell_params.w + kSwellPhase[i];
        height += amplitude * cos(phase);
    }
    return height;
}

// Height and slope at the shading point, with Gerstner horizontal displacement: crests draw water
// in from either side, so they narrow and the troughs broaden. Without it a swell is a row of
// round humps.
void SwellSurface(const float2 world_xy, out float height, out float2 slope)
{
    height = 0.0;
    slope = 0.0;
    if (swell_params.x <= 0.001) return;

    float2 displacement = 0.0;
    [unroll] for (int i = 0; i < kSwellWaves; i++) {
        const float angle = kSwellFan[i] * swell_wind.z;
        const float2 direction = float2(
            swell_wind.x * cos(angle) - swell_wind.y * sin(angle),
            swell_wind.x * sin(angle) + swell_wind.y * cos(angle));
        const float wavenumber = swell_params.y / kSwellLength[i];
        const float amplitude =
            swell_params.x * kSwellWeight[i] / kSwellPeakToTrough;
        const float phase = wavenumber * dot(direction, world_xy)
            - sqrt(swell_wind.w * wavenumber) * swell_params.w + kSwellPhase[i];
        displacement -= direction * (swell_params.z * amplitude * sin(phase));
    }

    const float2 p = world_xy + displacement;
    [unroll] for (int j = 0; j < kSwellWaves; j++) {
        const float angle = kSwellFan[j] * swell_wind.z;
        const float2 direction = float2(
            swell_wind.x * cos(angle) - swell_wind.y * sin(angle),
            swell_wind.x * sin(angle) + swell_wind.y * cos(angle));
        const float wavenumber = swell_params.y / kSwellLength[j];
        const float amplitude =
            swell_params.x * kSwellWeight[j] / kSwellPeakToTrough;
        const float phase = wavenumber * dot(direction, p)
            - sqrt(swell_wind.w * wavenumber) * swell_params.w + kSwellPhase[j];
        float wave_sin, wave_cos;
        sincos(phase, wave_sin, wave_cos);
        height += amplitude * wave_cos;
        slope -= direction * (amplitude * wavenumber * wave_sin);
    }
}

// The simulated surface, sampled by world position. One tile covers ocean_sample.x units; the
// second sample at an unrelated scale is what stops that tile reading as a repeat - the two
// periods have no common multiple worth seeing, so no tile edge lines up with another.
float3 SampleOcean(const float2 world_xy)
{
    const float2 uv = world_xy / ocean_sample.x;
    const float3 coarse = tex2D(ocean_displacement, uv).rgb;
    const float3 fine = tex2D(ocean_displacement, uv * ocean_sample.w).rgb;
    return coarse + fine * ocean_detail.x;
}

// rg = height gradient, b = foam.
float3 SampleOceanGradient(const float2 world_xy)
{
    const float2 uv = world_xy / ocean_sample.x;
    const float3 coarse = tex2D(ocean_gradient, uv).rgb;
    const float3 fine = tex2D(ocean_gradient, uv * ocean_sample.w).rgb;
    return coarse + fine * ocean_detail.x;
}

// Depth of the water column at a neighbouring pixel, for the silhouette guard in main(). Sky
// counts as open water, since there is no seabed behind it to be shallow over.
float WaterDepthAt(const float2 screen_uv, const float plane_z)
{
    const float d = tex2D(camera_depth, SceneDepthUV(screen_uv)).r;
    if (IsSkyDepth(d)) return 1e6;
    const float2 clip = float2(screen_uv.x * 2.0 - 1.0, 1.0 - screen_uv.y * 2.0);
    float4 p = mul(inverse_view_projection, float4(clip, d, 1.0));
    p /= p.w;
    return p.z - plane_z;
}

// Surface height above the still water plane, in world units.
float SurfaceHeight(const float2 world_xy)
{
    return SampleOcean(world_xy).g * ocean_sample.y + SwellHeight(world_xy);
}

// Cheap stand-in for the atmosphere: the real one is far too heavy to evaluate per reflected ray,
// but it is driven by the same colours so the water tracks the sky through the day.
float3 SampleSky(const float3 direction)
{
    const float height = saturate(direction.z * 0.5 + 0.5);
    float3 sky = lerp(sky_horizon.rgb, sky_zenith.rgb, pow(height, 0.65));
    sky += sun_colour.rgb * pow(saturate(dot(direction, sun_direction.xyz)), 24.0) * 0.35;
    return sky;
}

float4 main(float2 screen_uv : TEXCOORD0) : COLOR0
{
    const bool ownership_view = ocean_detail.w > 0.5;
    // Never paint over a frame that renders its own 3D - see scene_depth.hlsli.
    if (IsExcludedPixel(screen_uv)) discard;
    const float depth = tex2D(camera_depth, SceneDepthUV(screen_uv)).r;
    const bool scene_is_sky = IsSkyDepth(depth);

    // The view ray, built from the two ends of this pixel's clip-space line rather than from the
    // sampled depth, so it exists even where the scene is sky. That is what lets the water reach
    // the horizon without relying on depth precision out there.
    const float2 clip_xy = float2(screen_uv.x * 2.0 - 1.0, 1.0 - screen_uv.y * 2.0);
    float4 near_point = mul(inverse_view_projection, float4(clip_xy, 0.0, 1.0));
    float4 far_point = mul(inverse_view_projection, float4(clip_xy, 1.0, 1.0));
    near_point /= near_point.w;
    far_point /= far_point.w;
    const float3 view_ray = normalize(far_point.xyz - near_point.xyz);

    // GW's up is -z, so the plane lies ahead only when the ray points down from above it. That
    // one test is also the horizon.
    const float plane_z = water_plane.x;
    if (abs(view_ray.z) < 1e-6) discard;
    const float distance_to_plane = (plane_z - camera_position.z) / view_ray.z;
    if (distance_to_plane <= 0.0) {
        if (ownership_view) return float4(0.0, 0.0, 1.0, 1.0);
        discard;
    }
    float3 surface_position = camera_position.xyz + view_ray * distance_to_plane;

    // Where the scene sits, and how deep the water is over it. With no scene behind this pixel
    // the column is as deep as it gets - open sea to the horizon.
    float distance_to_scene = 1e9;
    float water_depth = 1e6;
    if (!scene_is_sky) {
        float4 scene_position = mul(inverse_view_projection, float4(clip_xy, depth, 1.0));
        scene_position /= scene_position.w;
        distance_to_scene = length(scene_position.xyz - camera_position.xyz);
        water_depth = scene_position.z - plane_z;
    }

    // Anything nearer than the surface is in front of the water - a beach, a hill, a boat - so it
    // occludes and the water is simply not visible here. Tested against the flat plane first,
    // which is the FURTHEST the surface can be; the marched hit below can only come closer, and
    // is re-tested afterwards.
    if (distance_to_scene < distance_to_plane || water_depth <= 0.0) {
        if (ownership_view) return float4(1.0, 0.0, 1.0, 1.0);
        discard;
    }
    if (ownership_view) return float4(0.0, 1.0, 0.0, 1.0);

    // Find where the ray meets the WAVE SURFACE, not the flat plane.
    //
    // This is what lets waves stand up on screen. Drawing the water only where the ray crosses the
    // still-water plane silently flattens the sea: a crest between the camera and that crossing is
    // simply never tested, so however tall the waves are made, their tops cannot rise above the
    // line the flat plane would have drawn - and the swell height slider mostly moves shading
    // around instead of moving water.
    //
    // The waves live in a band of +-envelope about the plane, so the search starts where the ray
    // enters that band (its ceiling) rather than at the plane, walks forward until it passes below
    // the surface, and bisects the crossing. Beyond the marched range the waves are far enough to
    // be sub-pixel and the plane hit is used as before.
    float hit_distance = distance_to_plane;
    const int march_steps = (int)ocean_detail.y;
    const float envelope = ocean_detail.z;
    if (march_steps > 0 && envelope > 0.01) {
        // Up is -z, so the crests are at a SMALLER z than the plane and the troughs a larger one.
        const float ceiling_z = plane_z - envelope;
        const float floor_z = plane_z + envelope;
        const float entry = camera_position.z < ceiling_z
            ? (ceiling_z - camera_position.z) / view_ray.z
            : 0.0;
        // Grazing rays cross the band over an enormous distance; stepping across all of it would
        // stride straight over the waves. Cap the search and let the far field stay flat.
        const float exit = min(
            (floor_z - camera_position.z) / view_ray.z, entry + kMaxMarchDistance);
        if (exit > entry) {
            const float step = (exit - entry) / march_steps;
            float previous = entry;
            [loop] for (int i = 1; i <= march_steps; i++) {
                const float t = entry + step * i;
                const float3 probe = camera_position.xyz + view_ray * t;
                if (probe.z >= plane_z - SurfaceHeight(probe.xy)) {
                    // Crossed it somewhere in the last step - close in on where.
                    float low = previous;
                    float high = t;
                    [loop] for (int b = 0; b < 4; b++) {
                        const float mid = (low + high) * 0.5;
                        const float3 sample = camera_position.xyz + view_ray * mid;
                        if (sample.z >= plane_z - SurfaceHeight(sample.xy)) high = mid;
                        else low = mid;
                    }
                    hit_distance = (low + high) * 0.5;
                    break;
                }
                previous = t;
            }
        }
    }
    surface_position = camera_position.xyz + view_ray * hit_distance;
    const float distance_to_camera = hit_distance;

    // The local surface rides up and down, so the depth over a given patch of sand changes with
    // every swell and the waterline advances and retreats on its own - which is what reads as the
    // sea washing in and out rather than as a static edge drawn around the island.
    float swell_height;
    float2 swell_slope;
    SwellSurface(surface_position.xy, swell_height, swell_slope);
    const float wave_height =
        SampleOcean(surface_position.xy).g * ocean_sample.y + swell_height;
    const float shore_depth = water_depth + wave_height;
    if (shore_depth <= 0.0) discard; // the swell has drawn back off this sand

    // A crest can stand between the camera and something the flat plane would have been behind.
    if (distance_to_scene < hit_distance) discard;

    const float3 gradient_foam = SampleOceanGradient(surface_position.xy);
    // The simulated gradient is dimensionless (height and spacing share units), so it needs no
    // conversion; the swell's is analytic and already in the same terms. GW's up is -z, hence the
    // negative up component.
    const float2 slope = gradient_foam.rg + swell_slope;
    float3 normal = normalize(float3(-slope, -1.0));

    // Flatten with distance: past a few thousand units the wave detail turns into shimmering
    // noise rather than surface.
    const float detail_fade = saturate(distance_to_camera / 9000.0);
    normal = normalize(lerp(normal, float3(0.0, 0.0, -1.0), detail_fade * 0.8));

    float3 reflected = reflect(view_ray, normal);
    reflected.z = -abs(reflected.z); // keep it pointing skyward
    const float3 reflection = SampleSky(reflected);

    // Cubic rather than Schlick: keeping some of the water's own colour at grazing angles is what
    // stops a distant sea turning into a mirror of the sky.
    const float facing = saturate(dot(-view_ray, normal));
    const float fresnel = pow(1.0 - facing, 3.0);

    const float3 specular = sun_colour.rgb
        * pow(saturate(dot(reflected, sun_direction.xyz)), sun_colour.w)
        * sun_direction.w;

    // Absorption: shallow water shows the sand through it, deep water does not. Driven by the
    // real column depth, so the colour varies with the ground beneath instead of being uniform.
    const float fade_depth = max(water_depth_params.x, 1.0);
    const float absorbed = saturate(shore_depth / fade_depth);
    const float diffuse = lerp(0.25, 1.0, saturate(dot(normal, sun_direction.xyz)));
    const float3 refracted = lerp(water_shallow.rgb, water_deep.rgb, absorbed) * diffuse;
    float3 colour = lerp(refracted, 0.9 * reflection, fresnel) + specular;

    // Light carried through the thin crest of a wave, brightest where the surface stands highest
    // above the mean and only close enough to see. Cheap stand-in for subsurface scattering.
    const float scatter_reach = max(1.0 - distance_to_camera * distance_to_camera * 1e-8, 0.0);
    const float crest_lift = saturate(wave_height / max(ocean_sample.z, 0.01));
    colour += water_shallow.rgb * crest_lift * water_depth_params.w * scatter_reach;

    // Transparency follows the same depth, so the water thins to nothing exactly where the land
    // reaches the waterline - no translucent fringe left lying over the beach. Reaching full
    // opacity well before the colour saturates keeps the shallows from looking like glass.
    float alpha = water_plane.w * saturate(shore_depth / (fade_depth * 0.35));

    // Foam: the surf line where the sheet is thinnest, plus the whitecaps the simulation itself
    // reports - faces that have folded over, rather than merely steep ones. The simulated foam
    // also breaks up the surf line, so it reads as spray instead of as a contour line.
    // Foam keys on shallow water, and the silhouette of anything standing in the sea is a
    // one-pixel sliver of exactly that: across that edge the depth buffer jumps from the seabed
    // behind the object to the object's own surface, which near the waterline reads as a shore
    // one pixel wide. It painted a hard white rim around every hull, rock and character in the
    // water - an outline rather than surf, because a real shore is never one pixel across.
    //
    // So take the DEEPEST of this pixel and its four neighbours: a genuine shore is shallow over
    // a whole neighbourhood, while an edge always has deep water on one side of it. ddx/ddy give
    // the one-pixel step in UV, so this needs no viewport size of its own.
    const float2 pixel_dx = ddx(screen_uv);
    const float2 pixel_dy = ddy(screen_uv);
    float neighbour_depth = water_depth;
    neighbour_depth = max(neighbour_depth, WaterDepthAt(screen_uv + pixel_dx, plane_z));
    neighbour_depth = max(neighbour_depth, WaterDepthAt(screen_uv - pixel_dx, plane_z));
    neighbour_depth = max(neighbour_depth, WaterDepthAt(screen_uv + pixel_dy, plane_z));
    neighbour_depth = max(neighbour_depth, WaterDepthAt(screen_uv - pixel_dy, plane_z));
    const float guarded_depth = neighbour_depth + wave_height;
    const float foam_edge = 1.0 - saturate(guarded_depth / max(water_depth_params.y, 0.5));
    const float surf = foam_edge * foam_edge * (0.55 + 0.75 * gradient_foam.b);
    float foam = saturate((surf + gradient_foam.b) * water_depth_params.z);
    foam *= 1.0 - detail_fade * 0.5; // distant surf is not resolvable, only shimmer
    colour = lerp(colour, water_foam_colour.rgb, foam);
    alpha = saturate(alpha + foam);

    return float4(colour, alpha);
}
