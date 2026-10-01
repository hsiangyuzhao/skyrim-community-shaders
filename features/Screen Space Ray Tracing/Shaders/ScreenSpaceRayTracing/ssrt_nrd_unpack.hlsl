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
//
// (batch 36b) SSRT_UNPACK_SPEC_EFFICIENCY: the efficiency-mode specular unpack. Two extra jobs:
//   * the DLSS-RR specular hit-distance guide (texHitDistance, R32) is rebuilt here for every
//     pixel. The checkerboard ray march only traced half of them this frame, so it no longer
//     writes that surface; REBLUR's denoised normalized hit distance is denormalized back to
//     game units with the same (A, B, C) curve the ray march packed it with.
//   * if the merged REBLUR dispatch did not complete, t0 is the compact checkerboard input
//     (SSRT_COMPOSITE_FLAG_CHECKER_INPUT) and each pixel takes the traced sample of its own
//     horizontal pair -- this frame's radiance, undenoised, the same S1.2 contract as before.
// Without the define this file compiles to exactly what it did before.

#include "NRD/NRDReblurSH.hlsli"

#ifdef SSRT_UNPACK_SPEC_EFFICIENCY
#	include "ScreenSpaceRayTracing/ssrt_common.hlsli"
#	include "ScreenSpaceRayTracing/ssrt_cb.hlsli"
Texture2D<float> DepthTexture : register(t1);
// t2 is NormalRoughnessTexture, declared by ssrt_common.hlsli.
RWTexture2D<float> HitDistanceOutput : register(u1);
#endif

Texture2D<float4> PackedTexture : register(t0);

RWTexture2D<float4> RadianceOutput : register(u0);

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	float3 radiance;
	float normHitDist;
#ifdef SSRT_UNPACK_SPEC_EFFICIENCY
	uint2 src = dtid;
	if ((CompositeFlags & SSRT_COMPOSITE_FLAG_CHECKER_INPUT) != 0)
		src.x >>= 1;
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(PackedTexture[src], radiance, normHitDist);
	RadianceOutput[dtid] = float4(radiance, normHitDist);

	// 65536 is the ray march's own "no hit" sentinel, which it packs as normHitDist 1; sky and
	// far plane keep it too, exactly as the full-resolution ray march writes them.
	const float depth = DepthTexture[dtid];
	float hitDist = 65536.0;
	[branch] if (!SSRT_IS_FAR_PLANE(depth) && normHitDist < 0.999)
	{
		float3 normalVS;
		float roughness;
		GetNormalRoughness(dtid, normalVS, roughness);
		// The ray march's SSRT_NRDViewZ and REBLUR_FrontEnd_GetNormHitDist, inverted.
		const float viewZ = SharedData::CameraData.w / (-depth * SharedData::CameraData.z + SharedData::CameraData.x);
		const float smc = _NRD_GetSpecMagicCurve(saturate(roughness), 0.5);
		const float f = (NRDHitDistA + abs(viewZ) * NRDHitDistB) * lerp(NRDHitDistC, 1.0, smc);
		hitDist = max(normHitDist, 0.0) * f;
		hitDist = (asuint(hitDist) & 0x7F800000u) == 0x7F800000u ? 65536.0 : hitDist;
	}
	HitDistanceOutput[dtid] = hitDist;
#else
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(PackedTexture[dtid], radiance, normHitDist);
	RadianceOutput[dtid] = float4(radiance, normHitDist);
#endif
}
