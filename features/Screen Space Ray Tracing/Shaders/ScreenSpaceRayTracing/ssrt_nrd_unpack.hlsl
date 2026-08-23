// (batch C1) REBLUR back-end unpack: converts an OUT_*_RADIANCE_HITDIST surface
// (YCoCg + normalized hit distance) back to linear RGB and writes it where the
// rest of the pipeline expects the denoised signal — texSSRTDiffuseColor for the
// diffuse chain, texSSRColor for the specular one. Shared by both chains: the
// layout is identical, only the bindings differ.
//
// .w carries the denoised normalized hit distance through. Neither composite
// reads .w of these surfaces (ssrt_diffuse_composite ignores it; the deferred
// composite reads only .rgb of the specular surface), and the SVGF chain is not
// running to interpret it as variance, so the channel is free.

#include "NRD/NRDReblurSH.hlsli"

Texture2D<float4> PackedTexture : register(t0);

RWTexture2D<float4> RadianceOutput : register(u0);

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	float3 radiance;
	float normHitDist;
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(PackedTexture[dtid], radiance, normHitDist);
	RadianceOutput[dtid] = float4(radiance, normHitDist);
}
