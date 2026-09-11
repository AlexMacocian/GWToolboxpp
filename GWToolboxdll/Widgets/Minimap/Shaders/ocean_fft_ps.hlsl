// One butterfly stage of an inverse FFT, run as a fullscreen pass and ping-ponged: log2(N) passes
// across, then log2(N) down. This is what turns a spectrum of thousands of wave components into a
// height field in a fixed number of passes - summing them directly would cost N^2 per texel.
//
// Stockham auto-sort formulation, so no bit-reversal permutation pass is needed.
//
// Both halves of the RGBA texel are transformed at once, which is what carries the height and
// both horizontal displacement components through the same sixteen passes.
//
// Adapted from https://github.com/dli/waves (MIT).
#include "ocean_common.hlsli"

sampler2D fft_input : register(s0);

// x = resolution, y = subtransform size for this stage, z = 1 when transforming across
float4 ocean_fft : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    const float resolution = ocean_fft.x;
    const float subtransform_size = ocean_fft.y;
    const bool horizontal = ocean_fft.z > 0.5;
    const float2 texel = vpos - 0.5;

    const float index = horizontal ? texel.x : texel.y;
    const float half_subtransform = subtransform_size * 0.5;
    const float even_index = floor(index / subtransform_size) * half_subtransform
        + fmod(index, half_subtransform);

    float2 even_uv;
    float2 odd_uv;
    if (horizontal) {
        even_uv = float2(even_index + 0.5, texel.y + 0.5) / resolution;
        odd_uv = float2(even_index + resolution * 0.5 + 0.5, texel.y + 0.5) / resolution;
    }
    else {
        even_uv = float2(texel.x + 0.5, even_index + 0.5) / resolution;
        odd_uv = float2(texel.x + 0.5, even_index + resolution * 0.5 + 0.5) / resolution;
    }

    const float4 even = tex2D(fft_input, even_uv);
    const float4 odd = tex2D(fft_input, odd_uv);

    const float twiddle_argument = -2.0 * kPi * (index / subtransform_size);
    const float2 twiddle = float2(cos(twiddle_argument), sin(twiddle_argument));

    return float4(
        even.xy + ComplexMul(twiddle, odd.xy),
        even.zw + ComplexMul(twiddle, odd.zw));
}
