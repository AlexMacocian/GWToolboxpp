float4x4 camera_view_matrix : register(c0);
float4x4 camera_projection_matrix : register(c4);
float4x4 light_view_matrix : register(c8);
float4x4 light_projection_matrix : register(c12);

struct VS_INPUT {
    float3 position : POSITION0;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float4 light_position : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    const float4 world_position = float4(input.position, 1.0);
    output.position = mul(mul(world_position, camera_view_matrix), camera_projection_matrix);
    output.light_position = mul(mul(world_position, light_view_matrix), light_projection_matrix);
    return output;
}
