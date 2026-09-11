// Shared cloud density field.
//
// Included by BOTH the sky (which draws the clouds) and the shadow receive pass (which projects
// them onto the ground), so the two are the same field by construction rather than by two
// implementations that have to be kept in agreement.
//
// The constants below sit at the same registers in both shaders and are uploaded from one place,
// so coverage, scale, wind scroll and the variety controls cannot drift apart between the cloud
// you see and the shadow it casts.
#ifndef REBIRTH_CLOUDS_INCLUDED
#define REBIRTH_CLOUDS_INCLUDED

float4 cloud_shape : register(c10);   // (coverage, scale, scroll.x, sharpness)
float4 cloud_light : register(c11);   // (absorption, phase g, opacity, scroll.y)
float4 cloud_variety : register(c19); // (weather amount, weather scale, warp strength, billow)
// Where the camera currently stands on the deck, in pattern units. ACCUMULATED from the camera's
// movement rather than computed from its absolute position - see Skybox.cpp for why that
// distinction is the whole difference between the deck sitting still and sliding about.
float4 cloud_origin : register(c20);

// === clouds ===
// A single lit layer rather than a full volumetric march: the density field is
// 2D, projected onto a plane at cloud height so it converges at the horizon,
// and the lighting samples the same field a couple of steps toward the Sun to
// approximate self-shadowing. That keeps it to a handful of noise evaluations
// per pixel instead of the hundreds a stepped volume would need.
static const float2x2 kCloudRotate = float2x2(1.6, 1.2, -1.2, 1.6);

float2 CloudHash(float2 p)
{
    p = float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)));
    return -1.0 + 2.0 * frac(sin(p) * 43758.5453123);
}

float CloudNoise(float2 p)
{
    float2 cell = floor(p);
    float2 offset = frac(p);
    float2 weight = offset * offset * (3.0 - 2.0 * offset);
    return lerp(
        lerp(dot(CloudHash(cell + float2(0, 0)), offset - float2(0, 0)),
             dot(CloudHash(cell + float2(1, 0)), offset - float2(1, 0)), weight.x),
        lerp(dot(CloudHash(cell + float2(0, 1)), offset - float2(0, 1)),
             dot(CloudHash(cell + float2(1, 1)), offset - float2(1, 1)), weight.x),
        weight.y);
}

// Four octaves for what is seen directly, two for the shadow taps: the light
// term only needs the bulk of the cloud, not its detail.
float CloudFbm(float2 p, const int octaves)
{
    float total = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < octaves; i++) {
        float n = CloudNoise(p);
        // Billow: folding the noise about zero turns smooth rolling hills into the stacked
        // cauliflower lobes a cumulus is built from. Blended rather than switched, because a
        // fully billowed field reads as bubbling foam.
        total += lerp(n, abs(n) * 2.0 - 1.0, cloud_variety.w) * amplitude;
        p = mul(kCloudRotate, p);
        amplitude *= 0.5;
    }
    return total;
}

// The weather field: a very low frequency sample that decides how much cloud belongs HERE,
// as opposed to how much belongs in the sky overall.
//
// This is the fix for every cloud coming out the same size. Thresholding a normalised fBm is
// what produced that: the field's values cluster about its mean, so a single global cut line
// crosses it at a roughly constant contour spacing and the blobs it carves are all about as big
// as each other - popcorn, whatever the coverage. Detail octaves cannot help, because they only
// add structure INSIDE a blob, never change how large it is.
//
// Varying the cut line across the sky is what breaks it. Where the weather runs high the line
// drops and neighbouring blobs merge into one mass; where it runs low only the tallest peaks
// clear it and you get a few small wisps - so one sky holds both. This is the "weather map" from
// Guerrilla's Nubis, reduced to a couple of noise samples rather than an authored texture.
float CloudWeather(const float2 p)
{
    const float2 wp = p * cloud_variety.y;
    float w = CloudNoise(wp) * 0.65 + CloudNoise(wp * 2.3 + 11.7) * 0.35;
    return saturate(w + 0.5);
}

// Displace the sample point by a low frequency field. Noise laid on a rotated lattice betrays
// its grid in the form of blobs that are all subtly the same shape; pushing the coordinates
// around beforehand shears them into the drawn-out, folded forms weather actually takes.
float2 CloudWarp(const float2 p)
{
    const float2 wp = p * cloud_variety.y * 1.7;
    return float2(CloudNoise(wp), CloudNoise(wp + 41.3)) * cloud_variety.z;
}

// `weather` is supplied by the caller so it can be sampled once per pixel and shared with the
// self-shadowing taps, which sit far closer together than the weather field varies.
float CloudDensity(const float2 p, const int octaves, const float weather)
{
    const float shape = CloudFbm(p, octaves) * 0.5 + 0.5;
    // Coverage becomes a local quantity. At variety 0 this is the old global threshold exactly.
    const float coverage = saturate(
        cloud_shape.x * lerp(1.0, weather * 2.0, cloud_variety.x));
    return saturate((shape - (1.0 - coverage)) * cloud_shape.w);
}


#endif
