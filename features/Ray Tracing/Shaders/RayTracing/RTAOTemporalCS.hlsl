// (46f) Ray-traced ambient occlusion: the optional history blend, AFTER the spatial filter (hard constraint #3:
// the AO multiplies the ambient light, so it comes from this frame's rays through the 1/4-resolution bilateral
// filter; a history may only refine that, clamped to this frame's filtered neighbourhood -- never a per-pixel
// average of unfiltered values, never anything of NRD).
//
// Current: this frame's filtered visibility (full render size). History: last frame's output of this pass,
// reprojected with the game's motion vectors (render-extent screen space, as Screen Space GI's contact AO).
// The history is clamped into the min / max of the current 3x3 neighbourhood (plus a small slack) before the
// blend, so a moving occluder leaves no dark trail beyond one frame's filtered range.

cbuffer RTAOCB : register(b0)
{
	uint4 Size;     // render width, height, flags, debug view
	float4 Params;  // x strength, y this frame's weight in the history, z history clamp slack, w RT fade-in (0..1)
};

static const uint kFlagHistory = 4u;

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
	[branch] if (Size.z & kFlagHistory)
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
