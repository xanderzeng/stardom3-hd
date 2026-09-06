// Each outline pass contributes only the coverage missing from the union of
// previous passes. With SRCALPHA/INVSRCALPHA, the result is max(a0,...,a3),
// rather than 1-product(1-ai). The foreground keeps its original coverage.
sampler2D glyph : register(s0);
float4 shift[4] : register(c0); // xy: UV offset, z: active, w: vertex alpha
float4 bounds[4] : register(c4); // previous quad's min/max UV
float4 mode : register(c8); // x: foreground

float previousCoverage(float2 uv, int i) {
    float2 p = uv + shift[i].xy;
    float inside = all(p >= bounds[i].xy) && all(p < bounds[i].zw);
    return tex2D(glyph, p).a * inside * shift[i].z * shift[i].w;
}

float4 main(float2 uv : TEXCOORD0, float4 color : COLOR0) : COLOR0 {
    float a = tex2D(glyph, uv).a * color.a;
    float prior = max(max(previousCoverage(uv, 0), previousCoverage(uv, 1)),
                      max(previousCoverage(uv, 2), previousCoverage(uv, 3)));
    float extra = saturate((a - prior) / max(1.0 - prior, 0.00001));
    return float4(color.rgb, lerp(extra, a, mode.x));
}
