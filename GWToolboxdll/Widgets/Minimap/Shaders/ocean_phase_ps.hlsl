// Advances every wave's phase by its own angular frequency, so long swells and short chop move at
// the speeds their wavelengths imply. Ping-ponged between two textures.
//
// The phase is carried rather than recomputed from an absolute clock so the sea never jumps when
// the wave speed changes: a new rate applies from here on, exactly like the sky's cloud scroll.
//
// Adapted from https://github.com/dli/waves (MIT).
#include "ocean_common.hlsli"

sampler2D phase_input : register(s0);

// x = resolution, y = patch size, z = delta time (seconds), w = seed (1 = write random phases)
float4 ocean_params : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    const float resolution = ocean_params.x;
    const float2 texel = vpos - 0.5;

    // First frame, and after a device reset: start from noise. Without a random start every wave
    // crosses zero together and the first seconds show one flat pulse instead of a sea.
    if (ocean_params.w > 0.5) {
        return float4(OceanHash(texel) * 2.0 * kPi, 0.0, 0.0, 0.0);
    }

    const float2 wave_vector = OceanWaveVector(texel, resolution, ocean_params.y);
    const float phase = tex2D(phase_input, (texel + 0.5) / resolution).r;
    const float advanced = phase + OceanOmega(length(wave_vector)) * ocean_params.z;
    // Kept in 0..2pi so the value cannot grow until float precision eats the small increments.
    return float4(fmod(advanced, 2.0 * kPi), 0.0, 0.0, 0.0);
}
