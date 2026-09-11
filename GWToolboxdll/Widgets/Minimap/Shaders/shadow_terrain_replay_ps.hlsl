float4 main(float2 depth : TEXCOORD0) : COLOR0
{
    const float linear_depth = saturate(depth.x / depth.y);
    return float4(linear_depth, linear_depth, linear_depth, 1.0);
}
