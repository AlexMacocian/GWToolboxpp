float4x4 view_matrix : register(c0);
float4x4 projection_matrix : register(c4);

struct VS_INPUT {
    float3 position : POSITION0;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 depth : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    float4 position = mul(float4(input.position, 1.0), view_matrix);
    output.position = mul(position, projection_matrix);
    output.depth = output.position.zw;
    return output;
}
