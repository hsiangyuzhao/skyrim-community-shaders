// (46f) Ray-traced ambient occlusion: the optional history blend, AFTER the spatial filter (hard constraint #3:
// the AO multiplies the ambient light, so it comes from this frame's rays through a spatial bilateral filter; a
// history may only refine that, clamped to this frame's filtered neighbourhood -- never a per-pixel average of
// unfiltered values, never anything of NRD).
//
// Current: this frame's filtered visibility (full render size). History: last frame's output of this pass,
// reprojected with the game's motion vectors (render-extent screen space, as Screen Space GI's contact AO).
// The history is clamped into the min / max of the current 3x3 neighbourhood (plus a small slack) before the
// blend, so a moving occluder leaves no dark trail beyond one frame's filtered range.
// (46t) The same pass for the detail term (RTAO_DETAIL_HISTORY: its own history, its own flag).

#include "RayTracing/RTAOCommon.hlsli"

#if defined(RTAO_DETAIL_HISTORY)
static const uint kHistoryFlag = kFlagDetailHistory;
#else
static const uint kHistoryFlag = kFlagHistory;
#endif

Texture2D<unorm float> Current : register(t0);
Texture2D<unorm float> History : register(t1);
Texture2D<float2> MotionVectors : register(t2);
SamplerState LinearClamp : register(s0);
RWTexture2D<unorm float> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 p = int2(dtid.xy);
	const int2 maxP = int2(Size.xy) - 1;
	const float current = Current[p];
	float lo = current;
	float hi = current;
	[unroll] for (int dy = -1; dy <= 1; ++dy)
	{
		[unroll] for (int dx = -1; dx <= 1; ++dx)
		{
			const float s = Current[clamp(p + int2(dx, dy), int2(0, 0), maxP)];
			lo = min(lo, s);
			hi = max(hi, s);
		}
	}
	float result = current;
	[branch] if (Size.z & kHistoryFlag)
	{
		const float2 uv = (float2(p) + 0.5) / float2(Size.xy);
		const float2 prevUv = uv + MotionVectors[p];
		if (all(prevUv >= 0.0) && all(prevUv <= 1.0)) {
			const float history = History.SampleLevel(LinearClamp, prevUv, 0);
			const float clamped = clamp(history, max(lo - Params.z, 0.0), min(hi + Params.z, 1.0));
			result = lerp(clamped, current, Params.y);
		}
	}
	Output[p] = result;
}
