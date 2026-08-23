// (batch C1) REBLUR front-end pack: adapts the SSRT ray-march outputs into the
// IN_DIFF_RADIANCE_HITDIST / IN_SPEC_RADIANCE_HITDIST layout NRD expects
// (YCoCg radiance + normalized hit distance, RGBA16F), without touching the
// ray-march shaders themselves. Two permutations:
//
//   (default)       diffuse — decodes the R8 reciprocal hit-distance surface
//                   (u = t / (t + 16), t = correlation length in *render texels*
//                   at this pixel's depth; see SSRT_HITT_REF_TEXELS in
//                   ssrt_common.hlsli) back to world units before normalizing.
//   SSRT_SPECULAR   specular — reads the R32 world-space hit distance the
//                   ray march already writes for DLSS-RR (65536 = miss) and the
//                   G-buffer roughness, exactly like the reference integration.
//
// The (A, B, C) normalization constants arrive in NRDPackCB and are the same
// values the C++ side hands nrd::ReblurSettings::hitDistanceParameters — REBLUR
// denormalizes with them internally, so front end and back end must agree.
//
// fxc cs_5_0 notes: no early returns, no conditional second blocks; the only
// non-finite hazard is the radiance input and REBLUR_FrontEnd_PackRadianceAndNormHitDist
// sanitizes it (that helper ships with the upstream-proven NRDReblurSH.hlsli).

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "NRD/NRDReblurSH.hlsli"

Texture2D<float4> RadianceTexture : register(t0);
Texture2D<float> HitDistanceTexture : register(t1);
Texture2D<float> DepthTexture : register(t2);
#if defined(SSRT_SPECULAR)
Texture2D<float4> NormalRoughnessTexture : register(t3);
#endif

RWTexture2D<float4> PackedOutput : register(u0);

cbuffer NRDPackCB : register(b1)
{
	float HitDistA;
	float HitDistB;
	float HitDistC;
	float nrdPackPad0;
};

// Must stay equal to SSRT_HITT_REF_TEXELS in ssrt_common.hlsli — it is the other
// half of the encoding this shader decodes. Not included from there because that
// header carries the whole traversal library, and the tracing side of this batch
// is deliberately untouched.
#define NRD_PACK_HITT_REF_TEXELS 16.0f

float ScreenToViewDepth(float screenDepth)
{
	// Same guard as prepareNRDGuides.cs.hlsl: sky and far plane resolve to a viewZ
	// far outside NRD's denoisingRange, so REBLUR treats the pixel as "no surface".
	if (screenDepth >= 1.0 - 1e-6 || screenDepth <= 0.0)
		return 3.402823466e+38;
	return (SharedData::CameraData.w / (-screenDepth * SharedData::CameraData.z + SharedData::CameraData.x));
}

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	const float depth = DepthTexture[dtid];
	const float viewZ = ScreenToViewDepth(depth);

#if defined(SSRT_SPECULAR)
	// World-space hit length in game units straight off the ray march; 65536 is its
	// "no hit" sentinel, which GetNormHitDist saturates to 1 — REBLUR's own meaning
	// for "as far as the encoding can say".
	const float hitDistWorld = HitDistanceTexture[dtid];
	const float roughness = saturate(1.0 - NormalRoughnessTexture[dtid].z);
#else
	// Reciprocal texel-space encoding: u = t / (t + 16). Decode t, then convert
	// texels to world units through the same texel-footprint construction the ray
	// march used to encode it: texelWorld = |viewZ| * 2 / (P00 * renderWidth).
	// u = 1.0 is the literal "no screen-space hit" and decodes, through the clamped
	// denominator, to a distance large enough that the normalization saturates.
	const float u = HitDistanceTexture[dtid];
	const float tTexels = NRD_PACK_HITT_REF_TEXELS * u / max(1.0 - u, 1e-4);
	const float renderWidth = SharedData::BufferDim.x * FrameBuffer::DynamicResolutionParams1.x;
	// viewZ is clamped before the footprint multiply: a sky pixel carries the
	// 3.4e38 sentinel, and 2 * 3.4e38 overflows to +inf, which a u = 0 texel would
	// then turn into 0 * inf = NaN. 1e7 game units is far beyond the denoising
	// range, so the clamp changes nothing for any pixel REBLUR actually reads.
	const float texelWorld = min(abs(viewZ), 1e7) * 2.0 /
	                         max(abs(FrameBuffer::CameraProj[0][0][0]) * renderWidth, 1e-6);
	const float hitDistWorld = tTexels * texelWorld;
	const float roughness = 1.0;
#endif

	const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(
		hitDistWorld, viewZ, float3(HitDistA, HitDistB, HitDistC), roughness);

	PackedOutput[dtid] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
		RadianceTexture[dtid].rgb, normHitDist, true);
}
