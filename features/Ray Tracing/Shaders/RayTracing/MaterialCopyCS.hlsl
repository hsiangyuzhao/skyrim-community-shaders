// (48f) Ray Tracing, RT reflections, Capture Materials: the 128x128 RGBA8 copy (all 8 mips) of a game diffuse texture in a layer of a
// texture-copy page (Texture2DArray, Backend::UpdateMaterials). One dispatch per texture: z = the copy mip; each mip reads the source at
// the mip closest to its own size (bilinear from there), so the last mip (1x1) is the texture's average colour (terrain layers use it).
// The values are copied as the game stores them (no colour-space change).

Texture2D<float4> Source : register(t0);
SamplerState LinearWrap : register(s0);

RWTexture2DArray<unorm float4> Mip0 : register(u0);
RWTexture2DArray<unorm float4> Mip1 : register(u1);
RWTexture2DArray<unorm float4> Mip2 : register(u2);
RWTexture2DArray<unorm float4> Mip3 : register(u3);
RWTexture2DArray<unorm float4> Mip4 : register(u4);
RWTexture2DArray<unorm float4> Mip5 : register(u5);
RWTexture2DArray<unorm float4> Mip6 : register(u6);
RWTexture2DArray<unorm float4> Mip7 : register(u7);

cbuffer MaterialCopyCB : register(b0)
{
	uint Layer;     // layer inside the page
	float Lod0;     // source mip matching the copy's 128x128 level (log2(source size / 128), >= 0)
	float MaxLod;   // the source's last mip
	uint Unused;
};

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const uint mip = dtid.z;
	const uint side = 128u >> mip;
	if (mip >= 8u || dtid.x >= side || dtid.y >= side)
		return;
	const float2 uv = (float2(dtid.xy) + 0.5) / float(side);
	const float4 color = Source.SampleLevel(LinearWrap, uv, min(Lod0 + float(mip), MaxLod));
	const uint3 at = uint3(dtid.xy, Layer);
	[branch] switch (mip)
	{
	case 0u:
		Mip0[at] = color;
		break;
	case 1u:
		Mip1[at] = color;
		break;
	case 2u:
		Mip2[at] = color;
		break;
	case 3u:
		Mip3[at] = color;
		break;
	case 4u:
		Mip4[at] = color;
		break;
	case 5u:
		Mip5[at] = color;
		break;
	case 6u:
		Mip6[at] = color;
		break;
	default:
		Mip7[at] = color;
		break;
	}
}
