#include "ScreenSpaceRayTracing/ssrt_common.hlsli"
#include "ScreenSpaceRayTracing/ssrt_b36g.hlsli"
#include "NRD/NRDReblurSH.hlsli"

// (batch 36g) Debug views of the diagnostic matrix, written into texB36gDebug and shown in
// Advanced > Batch 36g. Diagnostic only; nothing reads the surface but the menu.
//
// B36G_DEBUG_PATTERN: per pixel, red = diffuse traced this frame, green = reflection traced
// (yellow = both, black = neither or sky). Uses the same functions the ray march decides with.
//
// B36G_DEBUG_FILL: the denoiser's input for one signal, before (left half of the screen) and after
// (right half) the holes are filled:
//   before: the pixel's own traced sample, black where the pattern skipped it;
//   after:  A -- an emulation of NRD's checkerboard resolve (left/right neighbours, depth-weighted;
//              the real one runs inside REBLUR and cannot be read back);
//           B -- the input as NRD receives it (radiance holes stay: AREA_3X3 rebuilds hitT only);
//           C -- our resolve's full-resolution output; Full -- the input itself.

Texture2D<float4> BeforeTexture : register(t0);
Texture2D<float4> AfterTexture : register(t1);
// NormalRoughnessTexture (t2) comes from ssrt_common.hlsli.
Texture2D<float> DepthTexture : register(t4);

RWTexture2D<unorm float4> DebugOutput : register(u0);

float3 B36G_DebugDecode(float4 v, bool packed)
{
	if (packed) {
		float3 radiance;
		float normHitDist;
		REBLUR_BackEnd_UnpackRadianceAndNormHitDist(v, radiance, normHitDist);
		return radiance;
	}
	return v.rgb;
}

float3 B36G_DebugDisplay(float3 radiance)
{
	radiance = max(filterInf(filterNaN(radiance)), 0.0f);
	return sqrt(radiance / (1.0f + radiance));
}

bool B36G_DebugTraced(uint2 px)
{
	return (B36G_DebugFlags & B36G_DBGF_SPECULAR) != 0u ? B36G_SpecularTraced(px) : B36G_DiffuseTraced(px);
}

float3 B36G_DebugBefore(uint2 px)
{
	if (!B36G_DebugTraced(px))
		return 0.0f;
	const bool compact = (B36G_DebugFlags & (B36G_DBGF_BEFORE_NRD_COMPACT | B36G_DBGF_BEFORE_BATCH12_COMPACT)) != 0u;
	const uint2 src = compact ? uint2(px.x >> 1, px.y) : px;
	return B36G_DebugDecode(BeforeTexture[src], (B36G_DebugFlags & B36G_DBGF_BEFORE_PACKED) != 0u);
}

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
	const uint2 renderExtent = SSRT_GetRenderExtent();
	const uint2 px = DTid.xy;
	if (any(px >= uint2(SharedData::BufferDim.xy)))
		return;
	if (any(px >= renderExtent)) {
		DebugOutput[px] = 0.0f;
		return;
	}
	const float depth = DepthTexture[px];
	const bool sky = SSRT_IS_FAR_PLANE(depth);

	if (B36G_DebugMode == B36G_DEBUG_PATTERN) {
		DebugOutput[px] = sky ? float4(0.0f, 0.0f, 0.15f, 1.0f) :
		                        float4(B36G_DiffuseTraced(px) ? 1.0f : 0.0f, B36G_SpecularTraced(px) ? 1.0f : 0.0f, 0.0f, 1.0f);
		return;
	}

	const uint split = renderExtent.x >> 1;
	if (px.x == split) {
		DebugOutput[px] = 1.0f;
		return;
	}
	if (sky) {
		DebugOutput[px] = float4(0.0f, 0.0f, 0.15f, 1.0f);
		return;
	}

	float3 radiance;
	if (px.x < split) {
		radiance = B36G_DebugBefore(px);
	} else if ((B36G_DebugFlags & B36G_DBGF_EMULATE_NRD_FILL) != 0u) {
		if (B36G_DebugTraced(px)) {
			radiance = B36G_DebugBefore(px);
		} else {
			// REBLUR_PrePass / TemporalAccumulation: x - 1 and x + 1, disocclusion-weighted by viewZ.
			const float z = SharedData::GetScreenDepth(depth);
			float3 sum = 0.0f;
			float wsum = 0.0f;
			[unroll] for (int k = 0; k < 2; k++)
			{
				const int nx = int(px.x) + (k == 0 ? -1 : 1);
				if (nx < 0 || nx >= int(renderExtent.x))
					continue;
				const uint2 n = uint2(nx, px.y);
				const float zn = SharedData::GetScreenDepth(DepthTexture[n]);
				const float w = abs(zn - z) / max(z, 1e-5f) < 0.02f ? 1.0f : 0.0f;
				sum += B36G_DebugBefore(n) * w;
				wsum += w;
			}
			radiance = wsum > 0.0f ? sum / wsum : 0.0f;
		}
	} else if ((B36G_DebugFlags & B36G_DBGF_AFTER_SAME_AS_BEFORE) != 0u) {
		radiance = B36G_DebugDecode(BeforeTexture[px], (B36G_DebugFlags & B36G_DBGF_BEFORE_PACKED) != 0u);
	} else {
		radiance = B36G_DebugDecode(AfterTexture[px], (B36G_DebugFlags & B36G_DBGF_AFTER_PACKED) != 0u);
	}
	DebugOutput[px] = float4(B36G_DebugDisplay(radiance), 1.0f);
}
