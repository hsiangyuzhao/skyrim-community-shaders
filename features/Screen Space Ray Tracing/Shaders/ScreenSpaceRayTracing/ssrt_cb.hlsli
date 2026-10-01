#ifndef SSRT_CB_HLSLI
#define SSRT_CB_HLSLI

// (batch 36b) The whole of ScreenSpaceRayTracing::SSRTCB, in one place. ssrt_raymarch.hlsl, the
// batch 36b permutations of ssrt_diffuse_composite.hlsl and the efficiency-mode specular unpack
// all read fields past the third row, so they share this declaration instead of each mirroring a
// prefix. The C++ struct's static_assert pins the size; keep the two in lockstep.
cbuffer SSRTCB : register(b1)
{
	uint MaxSteps;
	uint MaxMips;
	uint UseDynamicCubemapsAsFallback;
	float Thickness;
	// --- row 1 ---
	float NormalBias;
	float BRDFBias;
	float OcclusionStrength;
	float CubemapNormalization;
	// --- row 2 ---
	// (diagnostic T2) Non-zero freezes the per-frame phase of the ray-direction noise.
	uint FreezeNoisePhase;
	// (S3.10) Non-zero takes the sample scramble from the baked blue-noise array.
	uint UseBlueNoise;
	// (reinjection noise) Read by ssrt_diffuse_composite.hlsl only.
	uint TemporalAmbientConfidence;
	float AmbientConfidenceInvMaxFrames;
	// --- row 3 ---
	// (batch 8) beta, the cubemap share of the unresolved hemisphere. Diffuse ray march only.
	float CubemapFillBlend;
	// (batch 11, item A) REBLUR's hit-distance normalization (A, B, C). Producer and denoiser
	// must agree, so these are the values nrd::ReblurSettings::hitDistanceParameters receives.
	float NRDHitDistA;
	float NRDHitDistB;
	float NRDHitDistC;
	// --- row 4 ---
	// (batch 11, item A) Non-zero: u0 is written in REBLUR's front-end layout.
	uint NRDFrontEndPack;
	// (batch 28) Roughness above which the specular march is skipped.
	float SpecularMaxRoughness;
	// (batch 36b) Non-zero: this dispatch is a checkerboard trace (SSRT_CHECKERBOARD). Informational
	// for the shaders that are not themselves the checkerboard permutation.
	uint CheckerboardTrace;
	// (batch 36b) NRD's own CommonSettings::frameIndex for this frame. The checkerboard phase is
	// derived from this and from nothing else, so the ray march and REBLUR cannot disagree about
	// which half of the pixels holds data -- SharedData::FrameCount is 0 with temporal effects off.
	uint NRDFrameIndex;
	// --- row 5 ---
	// (batch 36b) Non-zero: the AO texture at t9 holds *last* frame's denoiser AO, so it is read at
	// this pixel's motion-reprojected position. Zero: it is this frame's, read in place.
	uint AoFetchReprojected;
	// (batch 36b) Non-zero: the diffuse front-end hit distance is the per-sample mean of
	// 1 - coverage * (1 - normHitDist), i.e. REBLUR's AO convention, instead of the batch 11 mean of
	// the texel-space encoding. See SSRT_DiffuseVisibilitySample in ssrt_raymarch.hlsl.
	uint HitDistIsVisibility;
	// (batch 36b) Non-zero: under ambient reinjection the traced radiance is weighted by the same
	// distance-attenuated coverage the hit distance channel carries (efficiency mode, deviation 3).
	uint ProximityCoverage;
	// (batch 36b) SSRT_RAYMARCH_FLAG_* bits.
	uint RaymarchFlags;
	// --- row 6 ---
	// (batch 36b) SSRT_COMPOSITE_FLAG_* bits, read by the batch 36b composite and unpack permutations.
	uint CompositeFlags;
	float ssrtPad6a;
	float ssrtPad6b;
	float ssrtPad6c;
};

// RaymarchFlags
#define SSRT_RAYMARCH_FLAG_PREV_FRAME_COLOR 1u  // specular: hit colour from last frame's image, reprojected
#define SSRT_RAYMARCH_FLAG_SPEC_MISS_COMPOSITE 2u  // specular: misses take the deferred composite's own cubemap (deviation 5)
#define SSRT_RAYMARCH_FLAG_CHECKER_DEBUG 4u  // checkerboard: mark traced pixels in the debug surface
#define SSRT_RAYMARCH_FLAG_MISS_BENT 8u  // diffuse: publish the miss bent normal (deviation 2)

// CompositeFlags
#define SSRT_COMPOSITE_FLAG_CONF_FROM_DENOISER 1u  // confidence = 1 - denoised visibility (efficiency mode)
#define SSRT_COMPOSITE_FLAG_WRITE_AO 2u  // publish the denoiser AO surface
#define SSRT_COMPOSITE_FLAG_AO_CONTACT 4u  // fold Screen Space GI's contact visibility into it
#define SSRT_COMPOSITE_FLAG_TRACED_SKIPS_AO 8u  // pre-compensate the traced light for the composite's sqrt(AO) (deviation 4)
#define SSRT_COMPOSITE_FLAG_CHECKER_INPUT 16u  // t0 is a compact checkerboard input (REBLUR dispatch failed)
#define SSRT_COMPOSITE_FLAG_MISS_BENT 32u  // accumulate the miss bent normal (deviation 2)

#endif  // SSRT_CB_HLSLI
