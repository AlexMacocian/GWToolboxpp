// Shared maths for the FFT ocean - see ocean_spectrum_ps.hlsl for what the pipeline does.
//
// Adapted from David Li's WebGL wave simulation (https://github.com/dli/waves, MIT), which is a
// GPU implementation of Tessendorf's "Simulating Ocean Water" using the Elfouhaily et al. unified
// directional spectrum.

static const float kPi = 3.14159265359;
static const float kGravity = 9.81;
// Wavenumber where surface tension takes over from gravity, and the phase speed at that point.
static const float kCapillaryK = 370.0;
static const float kCapillaryC = 0.23;

float2 ComplexMul(const float2 a, const float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.y * b.x + a.x * b.y);
}

float2 ComplexMulI(const float2 z)
{
    return float2(-z.y, z.x);
}

float Square(const float x) { return x * x; }

float Tanh(const float x)
{
    const float e = exp(-2.0 * x);
    return (1.0 - e) / (1.0 + e);
}

// Dispersion, including the surface-tension term that lifts the smallest ripples.
//
// NB the reference carries this as `1.0 + k * k / KM * KM` in two of its three shaders, where the
// multiply cancels the divide and leaves `1 + k^2`; only its spectrum pass has `1 + (k/KM)^2`.
// Written correctly here, and identically everywhere, because the phase advance and the spectrum
// must agree or the waves drift against their own amplitudes.
float OceanOmega(const float k)
{
    return sqrt(kGravity * k * (1.0 + Square(k / kCapillaryK)));
}

// The wave vector for a texel of the spectrum. The FFT's output is periodic, so the second half
// of each axis carries the NEGATIVE frequencies - which is what the wrap below expresses.
float2 OceanWaveVector(const float2 texel, const float resolution, const float patch_size)
{
    const float n = texel.x < resolution * 0.5 ? texel.x : texel.x - resolution;
    const float m = texel.y < resolution * 0.5 ? texel.y : texel.y - resolution;
    return (2.0 * kPi * float2(n, m)) / patch_size;
}

// Deterministic per-texel randomness, so the initial phases need no upload from the CPU and
// survive a device reset unchanged.
float OceanHash(const float2 texel)
{
    return frac(sin(dot(texel, float2(12.9898, 78.233))) * 43758.5453);
}
