// (batch 38, item 3) Skin SSS upgrade pre-pass (upstream 98ffc7f66 + 2f93cb9a1): turns the lit
// image into linear light with the skin colour taken out, so the blur spreads light, not texture.
RWTexture2D<float4> OutputRW : register(u0);

Texture2D<float4> ColorTexture : register(t0);
Texture2D<float4> MaskTexture : register(t2);
Texture2D<float4> AlbedoTexture : register(t3);

#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"
#include "SubsurfaceScattering/SSSCommon.hlsli"

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	if (any(DTid.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	// Burley only reads pixels whose mask is above zero (centre and taps alike), so the rest
	// need not be written. Separable reads every neighbour and clears this flag.
	if (PrepassMaskOnly && MaskTexture[DTid.xy].x <= 0.0)
		return;

	float4 color = ColorTexture[DTid.xy];
	float3 linearColor = Color::IrradianceToLinear(color.rgb);
	float3 albedo = SSSDecodeAlbedo(AlbedoTexture[DTid.xy].rgb);
	color.rgb = SSSRemoveAlbedo(linearColor, albedo, ScatterMode);
	OutputRW[DTid.xy] = color;
}
