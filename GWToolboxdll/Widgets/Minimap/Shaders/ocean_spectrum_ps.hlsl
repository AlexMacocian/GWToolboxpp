// Turns the static spectrum plus this frame's phases into the complex field that the inverse FFT
// transforms into an actual surface.
//
// Two fields are packed into one RGBA texture so a single FFT produces both. An inverse FFT of a
// Hermitian-symmetric input is real, so two such inputs can travel as the real and imaginary parts
// of one complex signal and come back out separated:
//   RG = horizontal displacement x + i * height
//   BA = horizontal displacement y
// After the transform: R = displacement x, G = height, B = displacement y.
//
// The horizontal displacement is what makes crests sharp and troughs broad instead of leaving the
// surface a sum of round sinusoids - it is the difference between a swell and a ripple pattern.
//
// Adapted from https://github.com/dli/waves (MIT).
#include "ocean_common.hlsli"

sampler2D initial_spectrum : register(s0);
sampler2D phases : register(s1);

// x = resolution, y = patch size, z = amplitude scale, w = choppiness
float4 ocean_params : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    const float resolution = ocean_params.x;
    const float2 texel = vpos - 0.5;
    const float2 uv = (texel + 0.5) / resolution;
    const float2 wave_vector = OceanWaveVector(texel, resolution, ocean_params.y);
    const float k = length(wave_vector);
    if (k < 1e-6) return float4(0.0, 0.0, 0.0, 0.0);

    const float phase = tex2D(phases, uv).r;
    const float2 phase_vector = float2(cos(phase), sin(phase));

    // A real surface needs h(-k) to be the conjugate of h(k); building it from the mirrored texel
    // is what guarantees the transform comes back real rather than leaving an imaginary residue.
    const float2 h0 = tex2D(initial_spectrum, uv).rg;
    float2 h0_conj = tex2D(initial_spectrum, 1.0 - uv + 1.0 / resolution).rg;
    h0_conj.y = -h0_conj.y;

    const float2 h = ComplexMul(h0, phase_vector)
        + ComplexMul(h0_conj, float2(phase_vector.x, -phase_vector.y));

    // Wind-driven height is applied HERE rather than baked into the spectrum, so that changing it
    // costs nothing and lands smoothly: the spectrum texture describes the SHAPE of the sea and is
    // rebuilt only when that shape changes, while the amount of it varies every frame with the
    // wind. Scaling before the displacements keeps crest sharpening in proportion to wave size.
    const float2 scaled = h * ocean_params.z;

    const float choppiness = ocean_params.w;
    const float2 displacement_x =
        -ComplexMulI(scaled * (wave_vector.x / k)) * choppiness;
    const float2 displacement_y =
        -ComplexMulI(scaled * (wave_vector.y / k)) * choppiness;

    return float4(displacement_x + ComplexMulI(scaled), displacement_y);
}
