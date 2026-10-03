#ifndef SSRT_B36G_HLSLI
#define SSRT_B36G_HLSLI

// (batch 36g) Tracing patterns of the checkerboard / merged-denoiser diagnostic matrix.
//
// Included only by the batch 36g permutations (SSRT_B36G, SSRT_CONF_PATTERN, the specular resolve
// and the debug pass), so no default permutation declares b3 and none of their bytecode changes.
// Needs ssrt_common.hlsli first (SSRT_SparseCheckerPhase, GetNormalRoughness).
//
// The patterns, per full-resolution render pixel p = (x, y):
//   Full  both signals traced at every pixel (batch 36f).
//   A     strict complementary checkerboard, filled by NRD (CheckerboardMode::BLACK).
//         NRD: Sequence::CheckerBoard(p, frameIndex) = (x ^ y ^ frameIndex) & 1 (MathLib);
//         BLACK gives gDiffCheckerboard = 0, gSpecCheckerboard = 1 (Reblur.cpp), and REBLUR reads
//         the signal of a pixel that has data at (x >> 1, y): the left half of the input texture.
//         So diffuse owns x with (x & 1) == ((y + frameIndex) & 1) and specular the other x.
//   B     probabilistic lobe selection (NRD's recommended mode): diffuse iff
//         frac(Bayer4x4(p, frameIndex) + weyl) < pDiffuse(roughness), pDiffuse clamped to
//         [1/4, 3/4]. The untraced lobe gets hitT = 0 (invalid) and radiance 0, the traced one is
//         divided by its probability. Every aligned 2x2 block holds Bayer values c, c+4, c+8, c+12
//         (mod 16), i.e. four points 1/4 apart after any global shift, so both lobes are traced at
//         least once in every aligned 2x2 block and therefore in every 3x3 window -- the condition
//         HitDistanceReconstructionMode::AREA_3X3 needs.
//   C     complementary checkerboard on batch 12's phase (SharedData::FrameCount & 1), filled by
//         our own resolves: diffuse = batch 12's ssrt_sparse_resolve, specular = ssrt_spec_resolve.
//
// B36G_FrameIndex is the value NRD receives as CommonSettings::frameIndex (State::frameCount),
// never SharedData::FrameCount, which stays 0 with temporal effects off.

cbuffer SSRTPatternCB : register(b3)
{
	uint B36G_Pattern;          // this dispatch's pattern: B36G_PATTERN_*
	uint B36G_DiffuseMapping;   // diffuse ray march lane mapping: B36G_MAP_*
	uint B36G_FrameIndex;       // nrd::CommonSettings::frameIndex
	uint B36G_Flags;            // B36G_FLAG_*
	// --- row 1 ---
	float B36G_WeylShift;       // frac(frameIndex / sqrt(7)), computed on the CPU in double
	uint B36G_DebugMode;        // B36G_DEBUG_*
	uint B36G_DebugFlags;       // B36G_DBGF_*
	uint B36G_SpecularMapping;  // specular ray march lane mapping: B36G_MAP_*
};

#define B36G_PATTERN_FULL 0u
#define B36G_PATTERN_A 1u
#define B36G_PATTERN_B 2u
#define B36G_PATTERN_C 3u

// Lane mappings of the ray march dispatch.
#define B36G_MAP_IDENTITY 0u  // one lane per render pixel
#define B36G_MAP_NRD 1u       // compact, NRD checkerboard phase (pattern A); extent ceil(W / 2) x H
#define B36G_MAP_BATCH12 2u   // compact, batch 12 phase (pattern C); extent max(1, W >> 1) x H
#define B36G_MAP_PROB 3u      // one lane per render pixel, probabilistic lobe selection (pattern B)

// The diffuse hit-distance channel carries REBLUR visibility (confidence source 2, batch 36b).
#define B36G_FLAG_VISIBILITY 1u

#define B36G_DEBUG_OFF 0u
#define B36G_DEBUG_PATTERN 1u
#define B36G_DEBUG_FILL 2u

// Debug fill view: layout of the "before" (t0) and "after" (t1) sources.
#define B36G_DBGF_BEFORE_NRD_COMPACT 1u
#define B36G_DBGF_BEFORE_BATCH12_COMPACT 2u
#define B36G_DBGF_BEFORE_PACKED 4u
#define B36G_DBGF_AFTER_PACKED 8u
#define B36G_DBGF_EMULATE_NRD_FILL 16u
#define B36G_DBGF_SPECULAR 32u
#define B36G_DBGF_AFTER_SAME_AS_BEFORE 64u

// MathLib Sequence::CheckerBoard, verbatim.
uint B36G_NrdCheckerBoard(uint2 p)
{
	return ((p.x ^ p.y) ^ B36G_FrameIndex) & 1u;
}

// MathLib Sequence::Bayer4x4 (ML_BAYER_DEFAULT), verbatim: [0, 1).
float B36G_Bayer4x4(uint2 samplePos, uint frameIndex)
{
	uint2 p = samplePos & 3u;
	uint b = ((p.y & 1u) << 2) | ((p.x & 1u) << 3) | ((p.y & 2u) >> 1) | (p.x & 2u);
	return (float((b + frameIndex) & 15u) + 0.5f) / 16.0f;
}

// Probability of tracing diffuse rather than specular. Rough surfaces get more diffuse rays,
// glossy ones more specular rays; clamped to [1/4, 3/4] as AREA_3X3 requires. The roughness is the
// un-clamped 1 - glossiness the ray march reads (GetNormalRoughness), so every pass that asks gets
// the same answer for the same pixel.
float B36G_DiffuseProbability(float roughness)
{
	return clamp(0.25f + 0.5f * saturate(roughness), 0.25f, 0.75f);
}

bool B36G_ProbSelectsDiffuse(uint2 px, float roughness)
{
	const float rnd = frac(B36G_Bayer4x4(px, B36G_FrameIndex) + B36G_WeylShift);
	return rnd < B36G_DiffuseProbability(roughness);
}

float B36G_PixelRoughness(uint2 px)
{
	float3 n;
	float r;
	GetNormalRoughness(px, n, r);
	return r;
}

// Whether the diffuse / specular signal was traced at full-resolution pixel px this frame.
bool B36G_DiffuseTraced(uint2 px)
{
	if (B36G_Pattern == B36G_PATTERN_A)
		return B36G_NrdCheckerBoard(px) == 0u;
	if (B36G_Pattern == B36G_PATTERN_B)
		return B36G_ProbSelectsDiffuse(px, B36G_PixelRoughness(px));
	if (B36G_Pattern == B36G_PATTERN_C)
		return SSRT_SparseCheckerIsTraced(px);
	return true;
}

bool B36G_SpecularTraced(uint2 px)
{
	if (B36G_Pattern == B36G_PATTERN_A)
		return B36G_NrdCheckerBoard(px) == 1u;
	if (B36G_Pattern == B36G_PATTERN_B)
		return !B36G_ProbSelectsDiffuse(px, B36G_PixelRoughness(px));
	if (B36G_Pattern == B36G_PATTERN_C)
		return !SSRT_SparseCheckerIsTraced(px);
	return true;
}

// Compact lane -> the full-resolution pixel it traces. a_complement selects the other cell of the
// pair (the specular signal).
uint2 B36G_NrdCompactToPixel(uint2 c, uint a_complement)
{
	const uint parity = ((c.y + B36G_FrameIndex) & 1u) ^ a_complement;
	return uint2((c.x << 1) | parity, c.y);
}

uint2 B36G_Batch12CompactToPixel(uint2 c, uint a_complement)
{
	const uint parity = ((c.y + SSRT_SparseCheckerPhase()) & 1u) ^ a_complement;
	return uint2((c.x << 1) | parity, c.y);
}

uint2 B36G_CompactExtent(uint a_mapping, uint2 renderExtent)
{
	if (a_mapping == B36G_MAP_NRD)
		return uint2((renderExtent.x + 1u) >> 1, renderExtent.y);
	if (a_mapping == B36G_MAP_BATCH12)
		return uint2(max(1u, renderExtent.x >> 1), renderExtent.y);
	return renderExtent;
}

uint2 B36G_LaneToPixel(uint a_mapping, uint2 lane, uint a_complement)
{
	if (a_mapping == B36G_MAP_NRD)
		return B36G_NrdCompactToPixel(lane, a_complement);
	if (a_mapping == B36G_MAP_BATCH12)
		return B36G_Batch12CompactToPixel(lane, a_complement);
	return lane;
}

#endif  // SSRT_B36G_HLSLI
