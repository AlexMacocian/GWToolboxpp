// Fullscreen pass-through vertex shader shared by the atmospheric sky passes.
// Positions arrive already in clip space (a screen-covering quad), so they are emitted
// untouched. uv is used by the LUT render passes; ray_dir (a world/sky-frame view ray for
// each screen corner, built on the CPU) is interpolated and consumed by the composite pass.
struct VS_INPUT {
    float4 position : POSITION;
    float2 uv : TEXCOORD0;
    float3 ray_dir : TEXCOORD1;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 ray_dir : TEXCOORD1;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    output.position = input.position;
    output.uv = input.uv;
    output.ray_dir = input.ray_dir;
    return output;
}
