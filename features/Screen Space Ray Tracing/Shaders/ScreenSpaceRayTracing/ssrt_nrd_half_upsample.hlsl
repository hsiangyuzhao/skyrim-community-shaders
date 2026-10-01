// (batch 36) Half-resolution diffuse REBLUR, step 3 of 3: bring the denoised half-resolution
// result back to every render pixel and unpack it into the chain's linear radiance surface
// (texSSRTDiffuseColor), i.e. exactly what ssrt_nrd_unpack.hlsl leaves there on the
// full-resolution path, so the diffuse composite and everything after it are unchanged.
//
// Half texel h was built around render pixel 2h (ssrt_nrd_half_downsample.hlsl), so render pixel
// p sits at p / 2 in half-texel units: on a representative itself for even coordinates, halfway
// between two (or four) representatives for odd ones. The taps are those bilinear neighbours,
// each weighted by its bilinear weight times how well its viewZ matches this pixel's own:
//
//     w_k = bilinear_k * saturate(1 - |z_k - z| / (TOL * z))
//
// so a representative on another surface contributes nothing, and a pixel straddling a
// silhouette takes its light only from its own side. If every tap is on another surface -- a
// thin object (grass blade, railing) that no representative landed on -- the tap whose depth is
// nearest is used alone: NRD's own recommendation for upsampling denoised output ("nearest Z"),
// and the only choice that cannot invent a value between two surfaces.
//
// The packed payload is linear (YCoCg + normalized hit distance), so interpolating it before the
// unpack is the same as interpolating the unpacked radiance.

#include "Common/SharedData.hlsli"
#include "NRD/NRDReblurSH.hlsli"

Texture2D<float4> PackedHalf : register(t0);  // REBLUR OUT_DIFF_RADIANCE_HITDIST, half resolution
Texture2D<float> ViewZHalf : register(t1);    // the half-resolution IN_VIEWZ guide
Texture2D<float> ViewZFull : register(t2);    // the full-resolution IN_VIEWZ guide

RWTexture2D<float4> RadianceOutput : register(u0);  // texSSRTDiffuseColor

// Relative viewZ difference at which a tap's weight reaches zero. Taps are up to 2 render pixels
// from this one, so a same-surface tap differs by at most ~2e-2 even at grazing angles (see
// SSRT_NRD_HALF_DEPTH_TOLERANCE in the downsample); a linear falloff to 0.1 keeps those near full
// weight and drops a real depth step (0.3+) to zero.
#define SSRT_NRD_HALF_UPSAMPLE_TOLERANCE 0.1f

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	const uint2 renderExtent = max(uint2(1, 1), uint2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy));
	if (any(dtid >= renderExtent))
		return;

	const int2 halfMax = int2((renderExtent + 1) >> 1) - 1;
	const int2 base = int2(dtid >> 1);
	// 0 on a representative, 0.5 between two.
	const float2 f = float2(dtid & 1u) * 0.5f;

	const float z = ViewZFull[dtid];
	// Relative error is measured against this pixel's depth, floored so a (never expected)
	// non-positive viewZ cannot divide by zero. FLT_MAX sky stays finite: |FLT_MAX - z_k| / FLT_MAX
	// is at most 1, and a sky tap seen from geometry gives a large finite or +inf ratio, which
	// saturate() turns into a zero weight either way -- no NaN can arise (no inf - inf, no 0 / 0).
	const float zRef = max(z, 1e-6f);

	float4 sum = 0.0f;
	float weightSum = 0.0f;
	float4 nearest = 0.0f;
	float nearestRel = 3.402823466e+38f;

	[unroll] for (uint k = 0; k < 4; k++)
	{
		const int2 o = int2(k & 1u, k >> 1u);
		const int2 h = min(base + o, halfMax);
		const float bilinear = (o.x != 0 ? f.x : 1.0f - f.x) * (o.y != 0 ? f.y : 1.0f - f.y);

		const float rel = abs(ViewZHalf[h] - z) / zRef;
		const float4 v = PackedHalf[h];
		const float w = bilinear * saturate(1.0f - rel / SSRT_NRD_HALF_UPSAMPLE_TOLERANCE);

		sum += v * w;
		weightSum += w;

		[flatten] if (rel < nearestRel)
		{
			nearestRel = rel;
			nearest = v;
		}
	}

	const float4 packed = weightSum > 1e-4f ? sum / weightSum : nearest;

	float3 radiance;
	float normHitDist;
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed, radiance, normHitDist);
	RadianceOutput[dtid] = float4(radiance, normHitDist);
}
