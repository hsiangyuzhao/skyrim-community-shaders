// (45h) Ray Tracing, Alpha-tested casters: the 256x256 alpha copy of a game texture the ray
// tracer's D3D12 device cannot open directly (loaded before the switch was turned on, or not
// shareable). One dispatch per mip of the copy layer; the source is read at the mip closest to
// the copy mip's size. The copies live in a texture array shared with D3D12 (TraceShadowsCS t4).

Texture2D<float4> Source : register(t0);
SamplerState LinearWrap : register(s0);

#if defined(ALPHA_RGBA)
RWTexture2DArray<unorm float4> Layers : register(u0);
#else
RWTexture2DArray<unorm float> Layers : register(u0);
#endif

cbuffer AlphaMaskCB : register(b0)
{
	uint Layer;
	uint Side;    // this mip's size in texels
	float Lod;    // source mip to read
	uint Unused;
};

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (dtid.x >= Side || dtid.y >= Side)
		return;
	const float2 uv = (float2(dtid.xy) + 0.5) / float(Side);
	const float alpha = Source.SampleLevel(LinearWrap, uv, Lod).a;
#if defined(ALPHA_RGBA)
	Layers[uint3(dtid.xy, Layer)] = alpha.xxxx;
#else
	Layers[uint3(dtid.xy, Layer)] = alpha;
#endif
}
