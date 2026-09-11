// Wave energy per wave vector: the Elfouhaily et al. unified directional spectrum, which unlike
// the Phillips spectrum covers both the long wind-driven swell and the short capillary ripples,
// and carries a directional spread rather than a bare cos^2 term.
//
// Run once, and again whenever the wind or the patch size changes: this is the SHAPE of the sea,
// and only the phases move from frame to frame.
//
// Adapted from https://github.com/dli/waves (MIT).
#include "ocean_common.hlsli"

// x = resolution, y = patch size (world units), z unused, w unused
float4 ocean_params : register(c0);
// xy = wind velocity (m/s), z = upwind energy left in place.
//
// The SPEED here sets the shape of the sea only - where the spectrum peaks, and therefore how
// long the waves are. It is derived from the wanted wavelength rather than from the world's
// weather, so that the wind can change how BIG the sea is without changing what KIND of sea it
// is: a gust should raise the water, not re-scale it. The height the wind asks for is applied
// per-frame in ocean_spectrum_ps.hlsl.
float4 ocean_wind : register(c1);

float4 main(float2 vpos : VPOS) : COLOR0
{
    const float resolution = ocean_params.x;
    const float patch_size = ocean_params.y;
    const float2 texel = vpos - 0.5;
    const float2 wave_vector = OceanWaveVector(texel, resolution, patch_size);
    const float k = length(wave_vector);
    if (k < 1e-6) return float4(0.0, 0.0, 0.0, 0.0); // no DC term: a constant sea level, not a wave

    const float u10 = max(length(ocean_wind.xy), 0.5);
    const float inverse_wave_age = 0.84; // fully developed sea
    const float kp = kGravity * Square(inverse_wave_age / u10); // peak of the spectrum

    const float c = OceanOmega(k) / k;
    const float cp = OceanOmega(kp) / kp;

    // Long-wave (gravity) side: Pierson-Moskowitz shape with a JONSWAP peak enhancement.
    const float lpm = exp(-1.25 * Square(kp / k));
    const float gamma = 1.7;
    const float sigma = 0.08 * (1.0 + 4.0 * pow(inverse_wave_age, -3.0));
    const float peak = exp(-Square(sqrt(k / kp) - 1.0) / (2.0 * Square(sigma)));
    const float jp = pow(gamma, peak);
    const float fp = lpm * jp
        * exp(-inverse_wave_age / sqrt(10.0) * (sqrt(k / kp) - 1.0));
    const float alphap = 0.006 * sqrt(inverse_wave_age);
    const float bl = 0.5 * alphap * cp / c * fp;

    // Short-wave (capillary) side, driven by the friction velocity rather than by U10 directly.
    const float z0 = 0.000037 * Square(u10) / kGravity * pow(u10 / cp, 0.9);
    const float friction = 0.41 * u10 / log(10.0 / z0);
    const float alpham = 0.01 * (friction < kCapillaryC
        ? 1.0 + log(friction / kCapillaryC)
        : 1.0 + 3.0 * log(friction / kCapillaryC));
    const float fm = exp(-0.25 * Square(k / kCapillaryK - 1.0));
    const float bh = 0.5 * alpham * kCapillaryC / c * fm * lpm;

    // Directional spread: how much energy travels across the wind rather than along it. This is
    // what stops the sea looking like corrugated iron.
    const float a0 = log(2.0) / 4.0;
    const float am = 0.13 * friction / kCapillaryC;
    const float spread = Tanh(
        a0 + 4.0 * pow(c / cp, 2.5) + am * pow(kCapillaryC / c, 2.5));
    const float cos_phi = dot(normalize(ocean_wind.xy), wave_vector / k);

    float s = (1.0 / (2.0 * kPi)) * pow(k, -4.0) * (bl + bh)
        * (1.0 + spread * (2.0 * cos_phi * cos_phi - 1.0));

    // Stop waves travelling INTO the wind.
    //
    // The spread term above is cos(2*phi), which is unchanged by turning a wave around: downwind
    // and upwind components come out with exactly the same energy. Superposed, those pairs form
    // standing waves - the surface heaves in place instead of marching - and the sea ends up with
    // no direction at all no matter how hard the wind blows. It is a real property of the
    // formulation, not a bug in it: the spectrum describes how much energy sits at each
    // wavelength and orientation, and an orientation is a line, not an arrow.
    //
    // Wind waves are driven by the wind, so they go where it goes. Keeping the downwind half and
    // leaving a little of the other (a real sea is never perfectly ordered) is what turns the
    // heaving into travelling swell.
    s *= lerp(ocean_wind.z, 1.0, saturate(cos_phi * 0.5 + 0.5));

    // Drop the waves this grid cannot actually show.
    //
    // The spectrum runs all the way down to capillary ripples a centimetre long. The transform
    // can represent anything down to two texels, but a wave that small is reconstructed from a
    // couple of bilinear samples spread over metres of screen - so it never reads as a wave, only
    // as sparkle that crawls when the camera moves. Thousands of them at once IS white noise, and
    // that is what the sea looked like at any patch size small enough to contain real swell.
    //
    // Six texels per wavelength is about where a sampled wave still looks like one, so energy is
    // faded out from a quarter of the Nyquist wavenumber and gone by half. The visible waves keep
    // exactly the energy the spectrum gives them; only what could never be drawn is removed.
    const float k_nyquist = kPi * resolution / patch_size;
    const float resolvable = 1.0 - smoothstep(k_nyquist * 0.25, k_nyquist * 0.5, k);

    // Amplitude of this component. The randomness lives in the initial phases, so this stays a
    // real number and the whole spectrum is reproducible.
    const float dk = 2.0 * kPi / patch_size;
    const float amplitude = sqrt(max(s, 0.0) / 2.0) * dk * resolvable;
    return float4(amplitude, 0.0, 0.0, 0.0);
}
