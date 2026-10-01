// (batch 36) Half-resolution diffuse REBLUR, step 1 of 3: build the half-resolution denoiser
// input and the three half-resolution guides from their full-resolution counterparts.
//
// One thread per half-resolution texel h, which stands for the 2x2 block of render pixels whose
// top-left corner is 2h (NRD's own advice for reduced-resolution denoising: address the full-
// resolution guides with pixelPos * 2, not with a rotating "pick one of four" pattern, which
// raises the entropy of the noise-free guides and with it the disocclusion rate).
//
//   * Guides (viewZ, packed normal+roughness, motion): copied from the representative pixel 2h.
//     Never averaged -- an averaged viewZ across a silhouette is a surface that does not exist,
//     and the normal is NRD-packed (octahedral), where averaging the encoding is meaningless.
//     Motion is UV-space (CommonSettings::motionVectorScale = 1, 1), so it needs no rescale.
//   * Radiance (REBLUR's packed YCoCg + normalized hit distance, from the ray march or the sparse
//     resolve): the mean of the block's pixels that lie on the representative's surface, judged
//     by relative viewZ. YCoCg is a linear transform of RGB, so averaging it is averaging the
//     radiance; the representative always passes its own test, so the mean is never empty. This
//     is what lets the half-resolution denoiser start from 4x the ray samples per input pixel
//     instead of throwing three quarters of the traced rays away.
//
// The render extent is never assumed even: the half extent is ceil(render / 2), and the last
// row/column of an odd extent clamps to the render edge, so every half texel NRD's rectSize
// covers is written.

#include "Common/SharedData.hlsli"

Texture2D<float4> PackedFull : register(t0);          // texNRDPackInput (REBLUR front-end layout)
Texture2D<float> ViewZFull : register(t1);            // NRD IN_VIEWZ guide
Texture2D<float4> NormalRoughnessFull : register(t2); // NRD IN_NORMAL_ROUGHNESS guide (R10G10B10A2)
Texture2D<float2> MotionFull : register(t3);          // NRD IN_MV (game target or its snapshot)

RWTexture2D<float4> PackedHalf : register(u0);
RWTexture2D<float> ViewZHalf : register(u1);
RWTexture2D<float4> NormalRoughnessHalf : register(u2);
RWTexture2D<float2> MotionHalf : register(u3);

// Relative viewZ difference above which a block pixel is a different surface. The relative depth
// change between adjacent pixels on one surface is pixelAngularSize * |slope|: ~1e-3 face-on and
// ~1e-2 at grazing angles (the derivation behind SSRT_CONF_DEPTH_TOLERANCE), and the block spans
// at most one pixel diagonally, so 0.05 accepts every same-surface neighbour while a real depth
// step is 0.3 or more. A hard test rather than a falloff: there are only three candidates.
#define SSRT_NRD_HALF_DEPTH_TOLERANCE 0.05f

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	// Same truncation Util::ConvertToDynamic performs on the CPU side, which is the rectSize the
	// C++ side halves for the half-resolution REBLUR instance.
	const uint2 renderExtent = max(uint2(1, 1), uint2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy));
	const uint2 halfExtent = (renderExtent + 1) >> 1;
	if (any(dtid >= halfExtent))
		return;

	const uint2 maxPixel = renderExtent - 1;
	const uint2 rep = min(dtid * 2, maxPixel);

	// Sky / far plane is FLT_MAX in the viewZ guide (prepareNRDGuides.cs.hlsl). The comparison
	// below stays finite for it either way: |FLT_MAX - z| <= 0.05 * FLT_MAX is false against any
	// real surface and true between two sky pixels, so neither side mixes with the other.
	const float repZ = ViewZFull[rep];
	float4 sum = PackedFull[rep];
	float count = 1.0f;

	[unroll] for (uint i = 1; i < 4; i++)
	{
		const uint2 p = min(dtid * 2 + uint2(i & 1u, i >> 1u), maxPixel);
		const float z = ViewZFull[p];
		[flatten] if (abs(z - repZ) <= SSRT_NRD_HALF_DEPTH_TOLERANCE * repZ)
		{
			sum += PackedFull[p];
			count += 1.0f;
		}
	}

	PackedHalf[dtid] = sum / count;
	ViewZHalf[dtid] = repZ;
	NormalRoughnessHalf[dtid] = NormalRoughnessFull[rep];
	MotionHalf[dtid] = MotionFull[rep];
}
