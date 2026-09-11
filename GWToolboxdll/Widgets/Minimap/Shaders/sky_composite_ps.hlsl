// Single-pass atmospheric sky for GWToolbox, ported to ps_3_0.
//
// Model: O'Neil "Accurate Atmospheric Scattering" (GPU Gems 2, chapter 16), via FMS_Cat's
// ShaderToy port (MIT). One pass, no LUT: Rayleigh + Mie in-scattering integrated analytically
// along the view ray with a 2-sample approximation. A crisp sun disk is added on top (the raw
// Mie forward-scatter glow alone is too soft), fading out below the horizon.
//
// The interpolated ray_dir arrives already rotated by the CPU into the atmosphere "sky frame"
// (zenith = +z, the sun lying in the x-z plane), so no per-pixel basis change is needed and the
// sun stays anchored in world space as the camera orbits.
//
// c0: sun_dir.xyz              sun direction in the sky frame (zenith = +z)
// c1: (exposure_ev, sun_disk_size_deg, sun_disk_intensity, eye_altitude_km)
// c2: (debug_mode, 0, 0, 0)    debug_mode 1 = show ray dir, 2 = ownership paint (flat orange)
// c3: night_sky_colour.rgb     colour the sky fades to once the Sun has set
// c4: (star_brightness, star_density, star_threshold, star_visibility)
// c5: (time_seconds, twinkle_speed, 0, 0)
// c6: moon_dir.xyz             moon direction in the sky frame (anti-solar)
// c7: (moon_size_deg, moon_intensity, moon_visibility, moon_glow)
// c8: moon_colour.rgb          the disk itself
// c9: moon_glow_colour.rgb     the halo, warmer than the disk
// c10: (coverage, scale, scroll.x, sharpness)
// c11: (light_absorption, phase_g, opacity, scroll.y)
// c12: cloud_light_colour.rgb  direct sun/moon light reaching the cloud
// c13: cloud_ambient_colour.rgb sky light filling the shadowed side
// c14: (flash_intensity, flash_angular_radius, bolt_seed, bolt_intensity)
// c15: flash_dir.xyz           sky-frame direction of the current strike

float4 sun_dir_p : register(c0);
float4 sky_params : register(c1);
float4 sky_debug : register(c2);
float4 night_sky_colour : register(c3);
float4 star_params : register(c4);
float4 star_motion : register(c5);
float4 moon_dir_p : register(c6);
float4 moon_params : register(c7);
float4 moon_colour : register(c8);
float4 moon_glow_colour : register(c9);
float4 cloud_light_colour : register(c12);
float4 cloud_ambient_colour : register(c13);
float4 lightning_params : register(c14);
float4 lightning_dir : register(c15);
float4 aurora_params : register(c16); // (intensity, spread_radians, drift_speed, step_count)
float4 aurora_dir : register(c17);    // sky-frame direction the display is centred on
float4 godray_params : register(c21); // (intensity, extinction, cos(spread), step count)
float4 godray_colour : register(c22); // (rgb tint of the shafts, cos(full-strength angle))
float4 godray_reach : register(c23);  // (march length, baseline, glow contrast, glow strength)
float4 rainbow_params : register(c24); // (intensity, radius_rad, width_rad, secondary)
float4 rainbow_dir : register(c25);    // antisolar direction in sky frame, w = saturation
float4 aurora_colour : register(c18); // rgb tint, w = altitude of the lowest sheet

// === aurora ===
// Ported from "Auroras" by nimitz (2017), CC BY-NC-SA 3.0. https://www.shadertoy.com/view/XtGGRt
//
// The original marches 50 samples of a 5-octave noise, which is far beyond a ps_3_0 budget when
// the sky already carries scattering, clouds and stars. Two changes make it affordable: the step
// count is a constant the CPU sets (so it can be traded against quality), and the loop index is
// rescaled by 50/steps so a shorter march still spans the same altitudes and the same emission
// decay - dropping steps thins the sheets rather than truncating them.
//
// Axis note: the original is written with +y up. Our sky frame has +z up, so its "rd.y" is our
// star_dir.z and its horizontal pair maps to star_dir.xy.
float2x2 AuroraRotate(const float angle)
{
    float s, c;
    sincos(angle, s, c);
    return float2x2(c, s, -s, c);
}

float AuroraTri(const float x)
{
    return clamp(abs(frac(x) - 0.5), 0.01, 0.49);
}

float2 AuroraTri2(const float2 p)
{
    return float2(AuroraTri(p.x) + AuroraTri(p.y), AuroraTri(p.y + AuroraTri(p.x)));
}

// The band-like field that gives the curtains their structure: a few octaves of triangle noise,
// each displaced by the one below it and rotated over time so the sheets appear to stream.
float AuroraNoise(float2 p, const float speed, const float t)
{
    static const float2x2 kFold = float2x2(0.95534, 0.29552, -0.29552, 0.95534);
    float z = 1.8;
    float z2 = 2.5;
    float rz = 0.0;
    // GLSL's `v * m` is HLSL's `mul(m, v)`, so the rotations keep their original handedness.
    p = mul(AuroraRotate(p.x * 0.06), p);
    float2 bp = p;
    for (int i = 0; i < 4; i++) {
        float2 dg = AuroraTri2(bp * 1.85) * 0.75;
        dg = mul(AuroraRotate(t * speed), dg);
        p -= dg / z2;
        bp *= 1.3;
        z2 *= 0.45;
        z *= 0.42;
        p *= 1.21 + (rz - 1.0) * 0.02;
        rz += AuroraTri(p.x + AuroraTri(p.y)) * z;
        p = mul(-kFold, p);
    }
    return clamp(1.0 / pow(max(rz * 29.0, 1e-4), 1.3), 0.0, 0.55);
}

float AuroraHash(const float2 n)
{
    return frac(sin(dot(n, float2(12.9898, 4.1414))) * 43758.5453);
}

// Marches upward through a stack of emissive sheets. Returns rgb premultiplied by its own
// coverage in a, so the caller can composite it over whatever is already behind it.
float4 AuroraSample(const float3 dir, const float t)
{
    const float steps = max(aurora_params.w, 4.0);
    // Preserve the original's altitude span and falloff whatever the step count.
    const float index_scale = 50.0 / steps;
    // Break up the banding a short march would otherwise show, using the ray itself as the
    // dither source (there is no fragment coordinate bound here).
    const float dither = AuroraHash(dir.xy * 512.0);

    float4 col = 0.0;
    float4 average = 0.0;
    // Dynamic: the step count is a runtime constant, so this must not unroll.
    [loop] for (int i = 0; i < (int)steps; i++) {
        const float fi = (float)i * index_scale;
        const float offset = 0.006 * dither * smoothstep(0.0, 15.0, fi);
        // Distance along the ray to this sheet. The altitudes climb polynomially so the sparse
        // upper reaches cost as little as the dense base.
        const float travel =
            ((aurora_colour.w + pow(fi, 1.4) * 0.002) / (dir.z * 2.0 + 0.4)) - offset;
        const float2 plane = travel * dir.xy;
        const float density = AuroraNoise(plane, aurora_params.z, t);
        float4 sheet;
        // The colour cycles with altitude, which is what gives the green base and violet crown.
        sheet.rgb = (sin(1.0 - float3(2.15, -0.5, 1.2) + fi * 0.043) * 0.5 + 0.5) * density;
        sheet.a = density;
        average = lerp(average, sheet, 0.5);
        col += average * exp2(-fi * 0.065 - 2.5) * smoothstep(0.0, 5.0, fi);
    }
    // Fade out toward the horizon, where the march degenerates.
    col *= saturate(dir.z * 15.0 + 0.4);
    return col * 1.8;
}

#include "clouds.hlsli"
#include "scene_depth.hlsli"

float CloudHenyeyGreenstein(float cos_angle, float g)
{
    // 4*pi inline: PI is declared further down the file than this helper.
    float gg = g * g;
    return (1.0 - gg)
        / (12.56637061 * pow(max(1.0 + gg - 2.0 * g * cos_angle, 1e-4), 1.5));
}

// === lightning channel ===
// The visible bolt is a 1D fractal displacement: x = f(y). Drawing it needs a
// flat frame around the strike, so the view ray is projected onto the plane
// facing it and the path is evaluated against that.
float BoltHash(float x)
{
    return frac(sin(x) * 75154.32912);
}

float BoltValueNoise(float x)
{
    float i = floor(x);
    return lerp(BoltHash(i), BoltHash(i + 1.0), frac(x));
}

float BoltFractal(float x)
{
    float total = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    for (int i = 0; i < 6; i++) {
        frequency *= 2.0;
        amplitude *= 0.5;
        total += amplitude * BoltValueNoise(frequency * x);
    }
    return total;
}

float BoltPath(float height, float seed)
{
    return 0.4 * (BoltFractal(2.0 * height + seed) - 0.5);
}

// Spectral colour across a bow, t running 0 at the violet end to 1 at the red end.
//
// Three overlapping triangles standing in for the eye's cone responses. A proper
// wavelength -> CIE XYZ -> RGB chain is the correct thing, but over a band this narrow the two
// are very hard to tell apart, and this is a handful of instructions in a shader that is
// already close to the ps_3_0 register ceiling.
float3 RainbowSpectrum(const float t)
{
    return saturate(float3(
        1.5 - abs(4.0 * t - 3.0),
        1.5 - abs(4.0 * t - 2.0),
        1.5 - abs(4.0 * t - 1.0)));
}

// Smooth 0..1..0 across a band, used for both bows' radial profile.
float RainbowBand(const float t)
{
    const float b = saturate(1.0 - abs(t * 2.0 - 1.0));
    return b * b * (3.0 - 2.0 * b);
}

// Standard 3D gradient (Perlin-style) noise: hash each lattice corner to a
// pseudo-random gradient, dot it with the offset to the sample point, then
// smoothstep between them. Sampled along the view ray, so the star field is
// fixed to the sky rather than sliding around with the screen.
float3 StarHash(float3 p)
{
    p = float3(
        dot(p, float3(127.1, 311.7, 74.7)),
        dot(p, float3(269.5, 183.3, 246.1)),
        dot(p, float3(113.5, 271.9, 124.6)));
    return -1.0 + 2.0 * frac(sin(p) * 43758.5453123);
}

float StarNoise(float3 p)
{
    float3 cell = floor(p);
    float3 offset = frac(p);
    float3 weight = offset * offset * (3.0 - 2.0 * offset);

    float c000 = dot(StarHash(cell + float3(0, 0, 0)), offset - float3(0, 0, 0));
    float c100 = dot(StarHash(cell + float3(1, 0, 0)), offset - float3(1, 0, 0));
    float c010 = dot(StarHash(cell + float3(0, 1, 0)), offset - float3(0, 1, 0));
    float c110 = dot(StarHash(cell + float3(1, 1, 0)), offset - float3(1, 1, 0));
    float c001 = dot(StarHash(cell + float3(0, 0, 1)), offset - float3(0, 0, 1));
    float c101 = dot(StarHash(cell + float3(1, 0, 1)), offset - float3(1, 0, 1));
    float c011 = dot(StarHash(cell + float3(0, 1, 1)), offset - float3(0, 1, 1));
    float c111 = dot(StarHash(cell + float3(1, 1, 1)), offset - float3(1, 1, 1));

    return lerp(
        lerp(lerp(c000, c100, weight.x), lerp(c010, c110, weight.x), weight.y),
        lerp(lerp(c001, c101, weight.x), lerp(c011, c111, weight.x), weight.y),
        weight.z);
}

float StarField(float3 direction)
{
    // Normalise against the noise peak before the power, so raising sparsity
    // thins the field without also dimming it into nothing: a peak sample always
    // lands at full brightness, everything below it falls away steeply.
    float peak = saturate(StarNoise(direction * star_params.y) / 0.7);
    float field = pow(peak, star_params.z);
    // A second, coarser sample drifting in time twinkles them individually.
    float twinkle = StarNoise(
        direction * 100.0 + star_motion.x * star_motion.y);
    return field * lerp(0.4, 1.4, twinkle);
}

static const float PI = 3.14159265358979323846;

static const float INNER_RADIUS = 1.0;
static const float OUTER_RADIUS = 1.025;
// 1 / pow(wavelength, 4) for R,G,B - makes the sky blue and sunsets red.
static const float3 INV_WAVE_LENGTH = float3(5.60204474633241, 9.4732844379203038, 19.643802610477206);
static const float ESUN = 10.0;   // sun brightness
static const float KR = 0.0025;   // Rayleigh scattering constant
static const float KM = 0.0015;   // Mie scattering constant
static const float SCALE_DEPTH = 0.25;
static const float G = -0.99;     // Mie asymmetry: strong forward scatter -> bright sun region
static const int SAMPLES = 2;

// Optical depth approximation fit (O'Neil).
float optical_scale(float fCos)
{
    float x = 1.0 - fCos;
    return SCALE_DEPTH * exp(-0.00287 + x * (0.459 + x * (3.83 + x * (-6.80 + x * 5.25))));
}

// Near/far intersection distances of a ray with a sphere centred at the origin.
float2 get_intersections(float3 pos, float3 dir, float dist2, float rad2)
{
    float B = 2.0 * dot(pos, dir);
    float C = dist2 - rad2;
    float det = max(0.0, B * B - 4.0 * C);
    return 0.5 * float2(-B - sqrt(det), -B + sqrt(det));
}

float rayleigh_phase(float fCos2)
{
    return 0.75 * (2.0 + 0.5 * fCos2);
}

float mie_phase(float fCos, float fCos2, float g, float g2)
{
    return 1.5 * ((1.0 - g2) / (2.0 + g2)) * (1.0 + fCos2)
        / pow(abs(1.0 + g2 - 2.0 * g * fCos), 1.5);
}

struct PS_INPUT {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 ray_dir : TEXCOORD1;
};

// (start z, band width, floor multiplier) for the horizon band fade above.
float4 horizon_band : register(c29);

// The camera depth copy (INTZ). Sampled only when sky_debug.y selects copy-based sky
// classification, so this pass agrees with the water/fog/tint passes on what counts as sky.
sampler2D camera_depth : register(s0);

float4 main(PS_INPUT input) : COLOR
{
    // Never paint over a frame that renders its own 3D - see scene_depth.hlsli.
    if (IsExcludedPixel(input.uv)) discard;

    // Optional: occlude the sky from the SAME depth copy the water/fog/tint passes read, rather
    // than the hardware Z-test, so all five passes share one sky test and the horizon band the
    // two disagree over is no longer left for GW's own pixel to fill. See Skybox.cpp.
    [branch] if (sky_debug.y > 0.5) {
        if (!IsSkyDepth(tex2D(camera_depth, SceneDepthUV(input.uv)).r)) discard;
    }

    float3 ray_dir = normalize(input.ray_dir);
    float3 light_dir = normalize(sun_dir_p.xyz);
    // Kept before the horizon floor below, so stars are not smeared along it.
    float3 star_dir = ray_dir;

    if (sky_debug.x > 0.5 && sky_debug.x < 1.5) {
        return float4(ray_dir * 0.5 + 0.5, 1.0);
    }

    // Ownership paint: flat orange on every pixel the sky pass actually writes. Unlike the water
    // pass's ownership view, this sees through the REAL Z-buffer the sky tests against, not the
    // RESZ depth copy - so the two can be compared. If the thin sea-horizon line turns orange the
    // sky owns it (and its real colour there is the bug); if it stays light blue while the rest
    // of the sky goes orange, nothing of ours paints it and GW's own pixel is surviving.
    if (sky_debug.x > 1.5 && sky_debug.x < 2.5) {
        return float4(1.0, 0.5, 0.0, 1.0);
    }

    // Floor below-horizon rays to a hair above the horizon. Otherwise grazing/downward rays take a
    // degenerate near-zero scattering path and render as a black band right at the horizon; GW's
    // terrain covers the real ground anyway, so a thin horizon haze there is exactly what we want.
    // How far BELOW the horizon this ray really pointed, before the floor below erases it.
    // Over land GW's terrain covers these pixels so the floored haze is never seen, but out at
    // sea the water stops short of the true horizon and this pass owns the gap - where a constant
    // band of bright horizon haze reads as a thin light blue line above dark water.
    // Ramps from just ABOVE the horizon down through it, not only below. The first attempt
    // covered only negative z and changed nothing, which is the measurement that matters: the
    // bright line is the atmosphere's real horizon band, where scattering peaks as z approaches
    // 0, and not the floored region underneath it. Over land terrain hides that band; over sea
    // it meets dark water directly and reads as a hard light blue line.
    const float below_horizon = saturate((horizon_band.x - ray_dir.z) / max(horizon_band.y, 1e-4));
    ray_dir.z = max(ray_dir.z, 0.005);
    ray_dir = normalize(ray_dir);

    float exposure_ev = sky_params.x;
    float sun_size_deg = sky_params.y;
    float sun_intensity = sky_params.z;
    float eye_altitude_km = sky_params.w;

    float fInnerRadius2 = INNER_RADIUS * INNER_RADIUS;
    float fOuterRadius2 = OUTER_RADIUS * OUTER_RADIUS;
    float fKrESun = KR * ESUN;
    float fKmESun = KM * ESUN;
    float fKr4PI = KR * 4.0 * PI;
    float fKm4PI = KM * 4.0 * PI;
    float fScale = 1.0 / (OUTER_RADIUS - INNER_RADIUS);
    float fScaleOverScaleDepth = fScale / SCALE_DEPTH;
    float fG2 = G * G;

    // Eye height above the surface, on the zenith axis (+z in the sky frame). The altitude lever
    // raises the camera, dipping the horizon so the sky "starts" lower on screen.
    float3 ray_ori = float3(0.0, 0.0, INNER_RADIUS + eye_altitude_km * 0.001 + 1e-6);
    float fCameraHeight = length(ray_ori);
    float fCameraHeight2 = fCameraHeight * fCameraHeight;

    float2 inner_isects = get_intersections(ray_ori, ray_dir, fCameraHeight2, fInnerRadius2);
    float2 outer_isects = get_intersections(ray_ori, ray_dir, fCameraHeight2, fOuterRadius2);
    bool isGround = 0.0 < inner_isects.x;

    float fNear = max(0.0, outer_isects.x);
    float fFar = isGround ? inner_isects.x : outer_isects.y;
    float3 far_pos = ray_ori + ray_dir * fFar;
    float3 far_pos_norm = normalize(far_pos);

    float3 start_pos = ray_ori + ray_dir * fNear;
    float fStartHeight = length(start_pos);
    float3 start_pos_norm = start_pos / fStartHeight;
    float fStartAngle = dot(ray_dir, start_pos_norm);
    float fStartDepth = exp(fScaleOverScaleDepth * (INNER_RADIUS - fStartHeight));
    float fStartOffset = fStartDepth * optical_scale(fStartAngle);

    float fCameraAngle = dot(-ray_dir, far_pos_norm);
    float fCameraScale = optical_scale(fCameraAngle);
    float fCameraOffset = exp((INNER_RADIUS - fCameraHeight) / SCALE_DEPTH) * fCameraScale;

    float fTemp = optical_scale(dot(far_pos_norm, light_dir)) + optical_scale(dot(far_pos_norm, -ray_dir));

    float fSampleLength = (fFar - fNear) / float(SAMPLES);
    float fScaledLength = fSampleLength * fScale;
    float3 sample_dir = ray_dir * fSampleLength;
    float3 sample_point = start_pos + sample_dir * 0.5;

    float3 front_color = float3(0.0, 0.0, 0.0);
    float3 attenuate = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < SAMPLES; ++i) {
        float fHeight = length(sample_point);
        float fDepth = exp(fScaleOverScaleDepth * (INNER_RADIUS - fHeight));
        float fLightAngle = dot(light_dir, sample_point) / fHeight;
        float fSampleAngle = dot(ray_dir, sample_point) / fHeight;
        float fScatter = isGround
            ? (fDepth * fTemp - fCameraOffset)
            : (fStartOffset + fDepth * (optical_scale(fLightAngle) - optical_scale(fSampleAngle)));
        attenuate = exp(-fScatter * (INV_WAVE_LENGTH * fKr4PI + fKm4PI));
        front_color += attenuate * (fDepth * fScaledLength);
        sample_point += sample_dir;
    }

    front_color = clamp(front_color, 0.0, 3.0);
    float3 rayleigh = front_color * (INV_WAVE_LENGTH * fKrESun);
    float3 mie = front_color * fKmESun;

    float3 col;
    if (isGround) {
        // GW's own terrain covers the ground; just render the sky-ward scattering here.
        col = rayleigh + mie;
    } else {
        float fCos = dot(-light_dir, ray_dir);
        float fCos2 = fCos * fCos;
        col = rayleigh_phase(fCos2) * rayleigh + mie_phase(fCos, fCos2, G, fG2) * mie;

        // Crisp sun disk on top of the soft Mie glow. Fades out below the horizon.
        float sun_cos = dot(ray_dir, light_dir);
        float outer_cos = cos(radians(sun_size_deg));
        float inner_cos = cos(radians(max(sun_size_deg - 0.35, 0.0)));
        float disk = smoothstep(outer_cos, inner_cos, sun_cos);
        float above_horizon = smoothstep(-0.05, 0.05, light_dir.z);
        col += disk * sun_intensity * float3(1.0, 0.96, 0.9) * above_horizon;
    }

    col *= exp2(exposure_ev);
    col = 1.0 - exp(-col);                 // filmic-ish rolloff, keeps the bright sun from clipping hard

    // The single-scattering integral has no notion of the planet occluding the
    // Sun, so it keeps lighting the atmosphere from below the horizon. Fade to a
    // night sky manually across civil twilight instead.
    float night_blend = smoothstep(-0.28, 0.06, light_dir.z);
    col = lerp(night_sky_colour.rgb, col, night_blend);

    // Stars come in with the night, on the same curve as the world tint, and fade
    // out near the horizon where the air is thickest.
    float horizon_fade = smoothstep(0.02, 0.25, star_dir.z);

    // The moon sits opposite the Sun, so it is always full. Drawn before the
    // stars so it can mask them out: stars twinkling through the disk would give
    // away that it is a flat sprite.
    float moon_disk = 0.0;
    if (moon_params.z > 0.001) {
        float3 moon_dir = normalize(moon_dir_p.xyz);
        float moon_cos = dot(star_dir, moon_dir);
        float outer_cos = cos(radians(moon_params.x));
        float inner_cos = cos(radians(max(moon_params.x - 0.12, 0.0)));
        moon_disk = smoothstep(outer_cos, inner_cos, moon_cos);

        // Mare-like patches: sample noise across the disk rather than along the
        // ray, so the markings stay put instead of sliding as the camera turns.
        // Two octaves read as blotches rather than an even wobble.
        float3 across = star_dir - moon_dir * moon_cos;
        float coarse = StarNoise(across * 90.0 + 11.0) * 0.5 + 0.5;
        float fine = StarNoise(across * 190.0 + 31.0) * 0.5 + 0.5;
        float surface = lerp(0.68, 1.04, saturate(coarse * 0.65 + fine * 0.35));

        // Inverse-power halo: saturates around the limb and falls away fast, with
        // a wide exponential tail so it never visibly stops. Warmer than the disk,
        // which is what sells it against a cold night sky.
        float angle = acos(clamp(moon_cos, -1.0, 1.0));
        float radii = angle / max(radians(moon_params.x), 1e-4);
        float core = saturate(pow(1.0 + radii * 0.572, -7.0) * 20.4);
        float wide = exp(-radii * 0.09) * 0.12;

        float moon_horizon = smoothstep(-0.02, 0.12, moon_dir.z);
        float3 lit = moon_disk * surface * moon_colour.rgb
            + (core + wide) * moon_params.w * moon_glow_colour.rgb;
        col += lit * moon_params.y * moon_params.z * moon_horizon;
    }

    if (star_params.w > 0.001 && star_params.x > 0.0) {
        col += saturate(
            StarField(star_dir) * star_params.x * star_params.w * horizon_fade)
            * (1.0 - moon_disk);
    }

    // Aurora sits above the cloud deck but below the stars it veils, so it goes on after them
    // and before the clouds.
    [branch] if (aurora_params.x > 0.001 && star_dir.z > 0.0) {
        // Confine the display to one part of the sky. The mask runs on the HORIZONTAL bearing
        // only, so an aurora placed to the north stays north however high the curtains reach.
        float2 bearing = star_dir.xy;
        float bearing_length = length(bearing);
        float mask = 1.0;
        if (bearing_length > 1e-4) {
            float angle = acos(clamp(dot(bearing / bearing_length, aurora_dir.xy), -1.0, 1.0));
            mask = 1.0 - smoothstep(aurora_params.y * 0.5, aurora_params.y, angle);
        }
        // Per-pixel, so sky outside the display never pays for the march.
        [branch] if (mask > 0.001) {
            float4 aurora = smoothstep(0.0, 1.5, AuroraSample(star_dir, star_motion.x));
            aurora *= aurora_params.x * mask;
            // Emissive, so it is composited over the sky rather than lit by it - and like the
            // lightning it is added after the exposure, so a dark sky does not dim it.
            col = col * (1.0 - saturate(aurora.a)) + aurora.rgb * aurora_colour.rgb;
        }
    }

    // Clouds go on last so they occlude the stars and moon behind them.
    //
    // How much the cloud deck can be trusted at this elevation. The deck is a plane sampled
    // through 1/z, so as the view ray flattens the sample point runs away to the horizon and
    // the projection degenerates: past the 1/z clamp the deck coordinate stops changing with
    // elevation altogether and the field smears into vertical bars. Everything that reads the
    // deck fades out over this range, so nothing is drawn from the part of the projection that
    // has stopped carrying information.
    const float deck_fade = smoothstep(0.02, 0.16, star_dir.z);
    float cloud_alpha = 0.0;
    if (cloud_light.z > 0.001 && star_dir.z > 0.02) {
        // Project onto a plane at cloud height: distance grows toward the horizon
        // so the layer converges there. Capped, since past that the noise turns
        // into aliasing rather than detail.
        float distance_along = min(1.0 / star_dir.z, 24.0);
        // cloud_origin carries the camera's own position on the deck, which is what gives the
        // layer a place in the world instead of leaving it stuck to the viewer. Without it the
        // pattern is indexed by view direction alone, so the deck sits at infinity, never
        // parallaxes, and - the reason this matters here - has no world position for a shadow to
        // be cast from.
        float2 plane = star_dir.xy * distance_along * cloud_shape.y
            + cloud_origin.xy
            + float2(cloud_shape.z, cloud_light.w);

        // A solid deck has no gaps. The noise still thins out in places even at
        // full coverage, and every one of those thin spots let a star through, so
        // the sky read as broken when it should have been sealed. Floor the
        // density with how overcast it is: the deck fills in and goes featureless,
        // which is what a real overcast sky looks like.
        // Warp and weather are sampled ONCE here and shared: both are far lower frequency than
        // the distance between the shadow taps below, so re-sampling them per tap would cost
        // three times as much for a value that has barely moved.
        plane += CloudWarp(plane);
        const float weather = CloudWeather(plane);

        float overcast_solid = cloud_light_colour.w;
        float density = max(CloudDensity(plane, 4, weather), overcast_solid);
        if (density > 0.001) {
            // Step toward the Sun through the same field: what it finds is roughly
            // how much cloud sits between this point and the light.
            float2 sun_step = normalize(light_dir.xy + 1e-5) * 0.35;
            float shadow = CloudDensity(plane + sun_step, 2, weather) * 0.6
                + CloudDensity(plane + sun_step * 2.2, 2, weather) * 0.4;
            float transmittance = exp(-shadow * cloud_light.x * 4.0);

            // Beer-powder: thin edges scatter more light than the flat interior,
            // which is what gives the bright rim.
            float powder = 1.0 - exp(-density * 4.0);
            float phase = CloudHenyeyGreenstein(
                dot(star_dir, light_dir), cloud_light.y);

            float3 lit = cloud_light_colour.rgb * transmittance * powder
                * (0.4 + phase * 2.5)
                + cloud_ambient_colour.rgb;

            // Lightning lights the cloud from within, so it scales with density
            // and falls off with angle from the strike rather than being a flat
            // screen flash.
            if (lightning_params.x > 0.001) {
                float strike_angle = acos(
                    clamp(dot(star_dir, normalize(lightning_dir.xyz)), -1.0, 1.0));
                float reach = exp(-strike_angle / max(lightning_params.y, 1e-3));
                lit += lightning_params.x * reach * (0.35 + density * 1.6)
                    * float3(0.85, 0.90, 1.0);
            }

            float coverage_fade = deck_fade;
            // Beer-Lambert, not a linear fade. The blend used to be
            // saturate(density * opacity), which made a cloud's opacity
            // PROPORTIONAL to its density - so a core at density 1 with opacity
            // 0.7 still let 30% of the sky through, and stars sat visibly on top
            // of solid cloud. Real clouds do not work that way: extinction
            // accumulates exponentially along the path, so anything past a modest
            // depth is completely opaque and only the wispy edges are
            // translucent. cloud_light.z is the extinction coefficient, so it now
            // controls how quickly edges seal rather than capping how opaque the
            // thickest part can ever be.
            float alpha = 1.0 - exp(-density * cloud_light.z * 8.0);
            cloud_alpha = saturate(alpha) * coverage_fade;
            col = lerp(col, lit, cloud_alpha);
        }
    }

    // === crepuscular rays ===
    //
    // Sunlight scattering off haze in the air between the viewer and the cloud deck, which is
    // what makes shafts appear where the deck is broken and none where it is solid or clear.
    //
    // Done by marching the cloud field from this pixel TOWARD the sun and asking how much of
    // that path is open. That is the same idea as the classic DX9 radial blur (GPU Gems 3), but
    // taken against the density field directly rather than against a rendered occlusion buffer -
    // which costs no second render target or downsample, holds up at full resolution, and cannot
    // disagree with the clouds actually drawn, since it reads the very same field.
    //
    // It also degrades gracefully when the sun leaves the screen, where a screen-space blur
    // breaks down: this works in direction space, so the angular term simply falls to zero.
    [branch] if (godray_params.x > 0.001 && light_dir.z > 0.0) {
        // Confine to the neighbourhood of the sun. Shafts are a forward-scattering effect and
        // only show near the source, so most of the sky pays nothing for this.
        //
        // The window reaches full strength well INSIDE the cone rather than only at the sun
        // itself. Ramping all the way to the centre laid a smooth radial gradient over
        // everything, and since that gradient is far stronger than the cloud structure it is
        // what the eye actually read - a bullseye with the clouds as a faint modulation. Making
        // it a cutoff with a soft rim instead leaves the clouds as the only thing varying
        // across the core.
        const float sun_cos_angle = dot(star_dir, light_dir);
        const float angular = smoothstep(godray_params.z, godray_colour.w, sun_cos_angle);
        // Fades out with the same curve the clouds themselves do. Using a shorter fade here was
        // what made shadows drop straight down at the horizon: the deck's 1/z projection
        // degenerates as the view ray flattens, so the field smears into vertical bars, and the
        // shadow term was still at full strength through a band where the cloud casting it had
        // already faded to a few per cent. The shadow cannot outlive its cloud.
        const float horizon = deck_fade;
        [branch] if (angular * horizon > 0.002) {
            const float steps = max(godray_params.w, 2.0);
            // The deck coordinates every cloud sample is taken in. This must carry the same
            // scroll the deck is drawn with: the scroll accumulates without bound, so sampling
            // any part of this effect without it drifts a little further out of step with the
            // sky every frame.
            const float2 deck_offset =
                cloud_origin.xy + float2(cloud_shape.z, cloud_light.w);

            // Where the march actually runs.
            //
            // A shaft seen at this pixel is light scattered by haze at points along the VIEW
            // ray, and each of those points is lit or shadowed by whatever cloud its own line
            // to the Sun passes through. Walking the view ray to distance s and following the
            // Sun from there, the deck at height h is pierced at
            //
            //     s * star_dir.xy + (h - s * star_dir.z) / light_dir.z * light_dir.xy
            //
            // which is AFFINE in s - the pierce points trace a straight line across the deck.
            // Its two ends are ones we already know how to project: s = 0, the eye, giving the
            // point the Sun stands over, and s = h / star_dir.z, where the view ray meets the
            // deck, giving this pixel's own cloud point. So the march is a plain lerp between
            // them and no direction has to be interpolated at all.
            //
            // Interpolating directions instead - as this did - bends that straight line into
            // an arc, and worse, runs every intermediate direction through the 1/z projection
            // where the near-horizon clamp bites. A low Sun is exactly when shafts are worth
            // drawing and exactly when that clamp is hit, so the shafts anchored nowhere near
            // the gaps that cast them.
            const float2 pixel_plane =
                star_dir.xy * min(1.0 / max(star_dir.z, 0.02), 24.0) * cloud_shape.y
                + deck_offset;
            const float2 sun_full =
                light_dir.xy * min(1.0 / max(light_dir.z, 0.02), 24.0) * cloud_shape.y
                + deck_offset;

            // How far along that line to actually walk. Running the whole way to the Sun's own
            // foot point is what the geometry says, but it is also self-defeating: with a low
            // Sun that segment is tens of cloud widths long, every pixel's copy of it shares
            // the same far end, and averaging over it returns much the same number everywhere -
            // structure sampled away to a constant. Stopping after a few cloud widths keeps the
            // part of the column that differs from pixel to pixel, and is what a finite haze
            // depth would do anyway.
            const float2 to_sun = sun_full - pixel_plane;
            const float2 sun_plane = pixel_plane
                + to_sun * min(godray_reach.x / max(length(to_sun), 1e-5), 1.0);

            // Weather and warp both vary far more slowly than this segment is long, so they are
            // taken at the ends and interpolated rather than resampled per step. Sampling the
            // warp inside the loop is the honest version, but it costs more temporaries than
            // ps_3_0 has registers - and dropping it entirely, as this did, slid the shafts off
            // the very gaps they come from, so interpolating it is the compromise that fits.
            const float2 warp_near = CloudWarp(pixel_plane);
            const float2 warp_far = CloudWarp(sun_plane);
            const float weather_near = CloudWeather(pixel_plane + warp_near);
            const float weather_far = CloudWeather(sun_plane + warp_far);
            const float2 plane_near = pixel_plane + warp_near;
            const float2 plane_far = sun_plane + warp_far;

            float open = 0.0;
            [loop] for (int gi = 0; gi < (int)steps; gi++) {
                const float t = ((float)gi + 0.5) / steps;
                const float shaft_density = max(
                    CloudDensity(lerp(plane_near, plane_far, t), 2,
                        lerp(weather_near, weather_far, t)),
                    cloud_light_colour.w);
                open += exp(-shaft_density * godray_params.y);
            }
            open /= steps;

            // How the result is applied, which is the whole difference between this being
            // visible and not.
            //
            // It used to be ADDED to the sky. That cannot work here: the sky has already been
            // through the `1 - exp(-col)` rolloff a few lines up, so near the Sun it sits at
            // 0.98-1.0 with essentially no headroom left. Adding light to a value the tonemap
            // has already driven to white is a no-op, which is why the effect stayed faint no
            // matter how hard the sliders were pushed - and the flat core made it worst exactly
            // where the sky is brightest.
            //
            // What is seen in a real crepuscular ray is the DARK lane, not the bright shaft:
            // the air in cloud shadow scatters no sunlight, while the air beside it does. That
            // is also what the sky calculation above got wrong - it assumed every point along
            // the view ray sees the Sun. So the correction is a multiply by how much of the
            // column is actually lit, which darkens the shadowed lanes and leaves the open ones
            // alone. A multiply has full range on a saturated sky, where an add has none.
            //
            // Weighted by the sky that shows through, so the shafts appear between the clouds
            // rather than dimming the clouds themselves - those are already self-shadowed.
            const float sky_weight = angular * horizon * (1.0 - cloud_alpha);
            col *= saturate(1.0 - godray_params.x * sky_weight * (1.0 - open));

            // A little light added back in the lanes that are clearer than the sky's average
            // column, which is what gives the shaft a glow rather than only carving a shadow.
            // Small by nature, since this is the term with no headroom to work in.
            col += godray_colour.rgb * godray_reach.w * sky_weight
                * saturate((open - godray_reach.y) * godray_reach.z);
        }
    }

    // === rainbow ===
    //
    // No borrowed noise here, unlike the aurora: the geometry is exact and falls out of one
    // fact. A ray entering a spherical raindrop, reflecting once off the far wall and leaving
    // again is turned through a total deviation whose MINIMUM is about 138 degrees. Because it
    // is a stationary point, rays crowd into it - so the sky carries a bright ring
    // 180 - 138 = 42 degrees out from the antisolar point, and is darker inside it, where no
    // once-reflected ray can emerge at all. That minimum sits at a slightly different angle for
    // each wavelength, and that is the whole reason the ring is coloured rather than white.
    //
    // Everything therefore keys off the angle from the antisolar point, and the bow is a circle
    // centred there by construction. That also gives the effect its behaviour for free: the
    // antisolar point is as far below the horizon as the Sun is above it, so a high Sun puts
    // the whole ring underground and the bow simply is not there - which is why rainbows are a
    // morning and evening thing. No special case is needed to get that.
    [branch] if (rainbow_params.x > 0.001) {
        const float theta = acos(clamp(dot(star_dir, rainbow_dir.xyz), -1.0, 1.0));
        const float width = max(rainbow_params.z, 1e-4);
        const float r1 = rainbow_params.y;
        // The secondary comes from rays that reflect TWICE inside the drop. That puts its
        // minimum deviation on the other side, so it sits further out and its colours run the
        // other way - red innermost. The extra reflection also loses most of the light, which
        // is why it is always the fainter of the two.
        const float r2 = r1 * 1.214;

        const float t1 = saturate((theta - (r1 - width * 0.5)) / width);
        const float t2 = saturate((theta - (r2 - width * 0.9)) / (width * 1.8));

        float3 bow = RainbowSpectrum(t1) * RainbowBand(t1)
            + RainbowSpectrum(1.0 - t2) * RainbowBand(t2) * rainbow_params.w;
        // Real bows are washed out rather than saturated, because each drop returns a spread of
        // angles and neighbouring colours overlap.
        bow = lerp(dot(bow, float3(0.333, 0.333, 0.333)).xxx, bow, rainbow_dir.w);

        // Alexander's band. Between the two bows no once- or twice-reflected ray can leave a
        // drop at all, so that gap is genuinely darker than the sky on either side of it - and
        // being a darkening it survives the tonemap where added light would not.
        const float alexander =
            smoothstep(r1, r1 + width, theta) * smoothstep(r2, r2 - width, theta);
        const float visible = rainbow_params.x * saturate(star_dir.z * 20.0);
        col *= 1.0 - alexander * visible * 0.15;
        col += bow * visible;
    }

    // The strike lights the SKY, not only the cloud. The in-cloud glow above needs cloud
    // density to scatter in, so with a broken deck a strike lit nothing where the sky showed
    // through - and adding rain (which thickens the deck via overcast) appeared to "switch
    // lightning on". This term is what makes lightning stand on its own.
    if (lightning_params.x > 0.001) {
        float sky_angle = acos(
            clamp(dot(star_dir, normalize(lightning_dir.xyz)), -1.0, 1.0));
        float sky_reach = exp(-sky_angle / max(lightning_params.y, 1e-3));
        col += lightning_params.x * sky_reach * 0.30 * float3(0.85, 0.90, 1.0);
    }

    // The channel itself, drawn over the clouds. Only a fraction of strikes show
    // one - the rest stay buried in the cloud as a glow, which is what most real
    // strikes look like from a distance.
    if (lightning_params.w > 0.001) {
        float3 strike_dir = normalize(lightning_dir.xyz);
        float facing = dot(star_dir, strike_dir);
        if (facing > 0.15) {
            // Gnomonic projection onto the plane facing the strike, giving a flat
            // frame to draw the path in.
            float3 across = normalize(cross(float3(0.0, 0.0, 1.0), strike_dir));
            float3 along = cross(strike_dir, across);
            float3 offset = star_dir / facing - strike_dir;
            float u = dot(offset, across);
            float v = dot(offset, along);

            float path = BoltPath(v, lightning_params.z);
            // Widen where the path turns sharply, so corners do not thin out.
            float turn = abs(BoltPath(v + 0.002, lightning_params.z) - path);
            float distance_to_path = abs(u - path);
            float core = smoothstep(0.004 + turn * 4.0, 0.0, distance_to_path);
            float glow = smoothstep(0.05, 0.0, distance_to_path);
            // Hangs downward from the strike point and frays out at the bottom.
            float extent = smoothstep(0.03, -0.04, v)
                * smoothstep(-0.95, -0.55, v);
            col += (core + glow * 0.22) * extent * lightning_params.w
                * float3(0.82, 0.87, 1.0);
        }
    }

    col = pow(max(col, 0.0), 1.0 / 2.2);   // gamma
    // Settle the floored band toward the ground rather than leaving it at full horizon
    // brightness, so it meets the sea as a fade instead of an edge.
    col *= lerp(1.0, horizon_band.z, below_horizon);
    return float4(col, 1.0);
}
