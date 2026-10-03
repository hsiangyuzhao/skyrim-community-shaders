#include "ScreenSpaceRayTracing/ssrt_common.hlsli"
#include "ScreenSpaceRayTracing/ssrt_b36g.hlsli"
#include "NRD/NRDReblurSH.hlsli"

// (batch 36g, pattern C) REFLECTION RESOLVE: our own hole filling for the complementary
// checkerboard. The reflection ray march traced the batch 12 checkerboard's *other* cell of every
// horizontal pair (diffuse took the first), compact and linear. This pass writes the full-resolution
// reflection the denoiser reads -- REBLUR's packed layout or the linear one, per SSRTCB::NRDFrontEndPack
// -- and the full-resolution DLSS-RR hit-distance guide.
//
// A traced pixel is copied through exactly; with the packed layout it then holds the bits the
// full-resolution ray march would have written for the same ray (the same pack of the same fp16
// radiance and the same normalized hit distance).
//
// A skipped pixel reuses its four neighbours' rays (left, right, up, down: all four traced the
// reflection this frame under a checkerboard). Frostbite-style ("Stochastic Screen-Space
// Reflections", Stachowiak 2015): a neighbour's ray direction L -- towards its hit point, or its
// direction if it found nothing -- is re-weighted with this pixel's own GGX lobe, D_i(H) * N.L over
// the pdf the neighbour sampled L with, times a depth/normal similarity weight so nothing crosses a
// silhouette. On near-mirror pixels the lobe is too narrow to re-weight a neighbour's direction
// meaningfully, so only the similarity weight is used there.

Texture2D<float4> SparseSpecColor : register(t0);    // compact: linear radiance, .w confidence
Texture2D<float> SparseSpecHitDist : register(t1);   // compact: world hit distance (65536 = none)
// NormalRoughnessTexture (t2) comes from ssrt_common.hlsli.
Texture2D<float4> SparseSpecHitInfo : register(t3);  // compact: hit point / direction + pdf
Texture2D<float> DepthTexture : register(t4);         // full resolution

RWTexture2D<float4> ResolvedSpecular : register(u0);  // texNRDPackInput / texNRDSpecInput / texSSRColor
RWTexture2D<float> ResolvedHitDistance : register(u1);  // texHitDistance (DLSS-RR guide)

// Prefix of ScreenSpaceRayTracing::SSRTCB (ssrt_raymarch.hlsl) through row 4.
cbuffer SSRTCB : register(b1)
{
	uint MaxSteps;
	uint MaxMips;
	uint UseDynamicCubemapsAsFallback;
	float Thickness;
	float NormalBias;
	float BRDFBias;
	float OcclusionStrength;
	float CubemapNormalization;
	uint FreezeNoisePhase;
	uint UseBlueNoise;
	uint TemporalAmbientConfidence;
	float AmbientConfidenceInvMaxFrames;
	float CubemapFillBlend;
	float NRDHitDistA;
	float NRDHitDistB;
	float NRDHitDistC;
	uint NRDFrontEndPack;
	float SpecularMaxRoughness;
	float DistanceCapStart;
	float DistanceCapEnd;
};

#define B36G_SPEC_NO_HIT 65536.0f
#define B36G_SPEC_DEPTH_SCALE (1.0f * SSRT_DEPTH_WEIGHT_SCALE)
#define B36G_SPEC_NORMAL_PHI 32.0f
// Below this roughness the GGX lobe is narrower than the angle between two neighbours' rays.
#define B36G_SPEC_BRDF_MIN_ROUGHNESS 0.1f

void B36G_StoreSpec(uint2 px, float4 linearColor, float hitDistance, float roughness, float depth)
{
	if (NRDFrontEndPack != 0) {
		const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(
			hitDistance, SSRT_NRDViewZ(depth), float3(NRDHitDistA, NRDHitDistB, NRDHitDistC), roughness);
		ResolvedSpecular[px] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(SSRT_Fp16RoundTrip3(linearColor.rgb), normHitDist, true);
	} else {
		ResolvedSpecular[px] = linearColor;
	}
	ResolvedHitDistance[px] = hitDistance;
}

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
	const uint2 renderExtent = SSRT_GetRenderExtent();
	const uint2 px = DTid.xy;
	if (any(px >= renderExtent)) {
		ResolvedSpecular[px] = 0.0f;
		ResolvedHitDistance[px] = B36G_SPEC_NO_HIT;
		return;
	}

	const float depth = DepthTexture[px];
	float3 normalVS;
	float roughness;
	GetNormalRoughness(px, normalVS, roughness);

	if (SSRT_IS_FAR_PLANE(depth)) {
		B36G_StoreSpec(px, 0.0f, B36G_SPEC_NO_HIT, roughness, depth);
		return;
	}

	const uint compactWidth = max(1u, renderExtent.x >> 1);
	if ((px.x >> 1) < compactWidth && B36G_SpecularTraced(px)) {
		const uint2 own = uint2(px.x >> 1, px.y);
		B36G_StoreSpec(px, SparseSpecColor[own], SparseSpecHitDist[own], roughness, depth);
		return;
	}

	// This pixel's surface point, view vector and normal in camera-relative world space -- the same
	// construction the ray march uses for positionWS and world_space_hit.
	const float2 uv = (float2(px) + 0.5f) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	const float3 P = positionWS.xyz / positionWS.w;
	const float3 V = normalize(-P);
	const float3 N = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);
	const float linearDepth = SharedData::GetScreenDepth(depth);

	const float alpha = Pow2(clamp(roughness, 0.02f, 1.0f));
	const float a2 = alpha * alpha;
	const bool useBrdf = roughness >= B36G_SPEC_BRDF_MIN_ROUGHNESS;

	float4 colorBrdf = 0.0f;
	float hitBrdf = 0.0f;
	float weightBrdf = 0.0f;
	float4 colorGeom = 0.0f;
	float hitGeom = 0.0f;
	float weightGeom = 0.0f;

	[unroll] for (int k = 0; k < 4; k++)
	{
		const int2 n = int2(px) + int2(k == 0 ? -1 : (k == 1 ? 1 : 0), k == 2 ? -1 : (k == 3 ? 1 : 0));
		if (any(n < int2(0, 0)) || any(n >= int2(renderExtent)))
			continue;
		const uint2 tap = uint2(uint(n.x) >> 1, uint(n.y));
		if (tap.x >= compactWidth || !B36G_SpecularTraced(uint2(n)))
			continue;

		const float tapDepth = DepthTexture[n];
		if (SSRT_IS_FAR_PLANE(tapDepth))
			continue;
		float3 tapNormal;
		float tapRoughness;
		GetNormalRoughness(uint2(n), tapNormal, tapRoughness);
		const float relative = abs(SharedData::GetScreenDepth(tapDepth) - linearDepth) / max(linearDepth, 1e-5f);
		const float geom = exp(-relative / B36G_SPEC_DEPTH_SCALE) * pow(max(0.0f, dot(normalVS, tapNormal)), B36G_SPEC_NORMAL_PHI);

		const float4 c = SparseSpecColor[tap];
		const float h = SparseSpecHitDist[tap];
		colorGeom += c * geom;
		hitGeom += h * geom;
		weightGeom += geom;

		if (useBrdf) {
			const float4 info = SparseSpecHitInfo[tap];
			const float3 L = info.w > 0.0f ? normalize(info.xyz - P) : normalize(info.xyz);
			const float NoL = dot(N, L);
			const float3 H = normalize(V + L);
			const float NoH = saturate(dot(N, H));
			const float d = (NoH * a2 - NoH) * NoH + 1.0f;
			const float D = a2 / (Math::PI * d * d);
			float w = NoL > 0.0f ? geom * D * NoL / max(abs(info.w), 1e-4f) : 0.0f;
			if (!isFiniteSafe(w))
				w = 0.0f;
			colorBrdf += c * w;
			hitBrdf += h * w;
			weightBrdf += w;
		}
	}

	// The last column of an odd render width has no compact lane at all (the compact extent is
	// floor(W / 2), as batch 12's), so neither signal was traced there and its left neighbour may be
	// a diffuse cell. Its diagonal neighbours on the left are then reflection cells: use them, by
	// similarity only, when the first ring found nothing.
	if (weightGeom <= 1e-5f) {
		[unroll] for (int d = 0; d < 4; d++)
		{
			const int2 n = int2(px) + int2((d & 1) ? 1 : -1, (d & 2) ? 1 : -1);
			if (any(n < int2(0, 0)) || any(n >= int2(renderExtent)))
				continue;
			const uint2 tap = uint2(uint(n.x) >> 1, uint(n.y));
			if (tap.x >= compactWidth || !B36G_SpecularTraced(uint2(n)))
				continue;
			const float tapDepth = DepthTexture[n];
			if (SSRT_IS_FAR_PLANE(tapDepth))
				continue;
			float3 tapNormal;
			float tapRoughness;
			GetNormalRoughness(uint2(n), tapNormal, tapRoughness);
			const float relative = abs(SharedData::GetScreenDepth(tapDepth) - linearDepth) / max(linearDepth, 1e-5f);
			const float geom = exp(-relative / B36G_SPEC_DEPTH_SCALE) * pow(max(0.0f, dot(normalVS, tapNormal)), B36G_SPEC_NORMAL_PHI);
			colorGeom += SparseSpecColor[tap] * geom;
			hitGeom += SparseSpecHitDist[tap] * geom;
			weightGeom += geom;
		}
	}

	float4 color = 0.0f;
	float hitDistance = B36G_SPEC_NO_HIT;
	if (weightBrdf > 1e-8f && isFiniteSafe(weightBrdf)) {
		color = colorBrdf / weightBrdf;
		hitDistance = hitBrdf / weightBrdf;
	} else if (weightGeom > 1e-5f) {
		color = colorGeom / weightGeom;
		hitDistance = hitGeom / weightGeom;
	}
	color = isFiniteSafe(color) ? color : 0.0f;
	hitDistance = isFiniteSafe(hitDistance) ? hitDistance : B36G_SPEC_NO_HIT;
	B36G_StoreSpec(px, color, hitDistance, roughness, depth);
}
