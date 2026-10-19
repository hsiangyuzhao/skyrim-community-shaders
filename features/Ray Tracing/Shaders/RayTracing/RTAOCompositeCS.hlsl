// (46f) Ray-traced ambient occlusion: the mix with Screen Space GI's AO, written into the texture SSGI's AO
// consumers read this frame (DeferredComposite t10, SSRT t9: occlusion, they take 1 - value as visibility).
//
//   w      = the RT weight: traced, inside the traced area, fading back to SSGI's AO with distance (TraceAOCS .g),
//            times the RT fade-in after a loading screen
//   rtVis  = 1 - (1 - filtered RTAO) * strength
//   (46t)  two scales (kFlagDetail): rtVis = (1 - (1 - filtered far term) * strength) x (1 - (1 - filtered detail) * detail
//          strength) -- the far term is the visibility beyond the detail length given the near range is open, so each
//          occluder is counted once (see TraceAOCS)
//   mode "RTAO (keeps SSGI contact AO)": rtVis * contact (the contact AO's visibility; SSGI's main AO is replaced)
//   mode "Darker of RTAO and SSGI AO":   min(rtVis, 1 - SSGI's whole AO)
//   out    = lerp(SSGI's AO, 1 - mixed, w)
// Split (debug): the right half keeps SSGI's AO as before, a line marks the middle.
// Terrain and grass are not in the TLAS: their own occlusion comes from the contact AO (or SSGI in "Darker").

#include "RayTracing/RTAOCommon.hlsli"

Texture2D<unorm float> RTAO : register(t0);    // filtered (and history-blended) visibility (two scales: the far term)
Texture2D<float4> Raw : register(t1);          // (47h float: RGBA16F or RGBA8) TraceAOCS: r this frame's ray, g RT weight, (46t) b far weight, a near visibility
Texture2D<unorm float> SsgiAo : register(t2);  // SSGI's own AO this frame (occlusion; contact AO folded in)
Texture2D<unorm float> Contact : register(t3); // SSGI's contact AO visibility (kFlagContact)
Texture2D<unorm float> DetailAO : register(t4); // (46t) filtered (and history-blended) detail visibility (kFlagDetail)
Texture2D<unorm float4> NormalDebug : register(t5);  // (47k) AONormalPrepCS's Smooth Normals debug view (kFlagNormalDebug)
RWTexture2D<unorm float> Output : register(u0);
RWTexture2D<unorm float4> Debug : register(u1);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 p = int2(dtid.xy);
	const float ssgiOcclusion = saturate(SsgiAo[p]);
	const float4 raw = Raw[p];
	const float weight = saturate(raw.g * Params.w);
	const float largeVis = 1.0 - saturate((1.0 - RTAO[p]) * Params.x);
	const bool detail = (Size.z & kFlagDetail) != 0;
	const float detailVis = detail ? 1.0 - saturate((1.0 - DetailAO[p]) * Detail.x) : 1.0;
	const float rtVis = largeVis * detailVis;
	float mixed;
	[branch] if (Size.z & kFlagDarker)
	{
		mixed = min(rtVis, 1.0 - ssgiOcclusion);
	}
	else
	{
		const float contact = (Size.z & kFlagContact) ? saturate(Contact[p]) : 1.0;
		mixed = rtVis * contact;
	}
	float occlusion = lerp(ssgiOcclusion, 1.0 - mixed, weight);
	const bool rightHalf = (Size.z & kFlagSplit) && dtid.x >= Size.x / 2u;
	if (rightHalf)
		occlusion = ssgiOcclusion;
	Output[p] = occlusion;

	[branch] if (Size.w != 0u)
	{
		const float4 notTraced = float4(0.1, 0.2, 1.0, 0.35);
		float4 debug = 0.0;
		if (Size.w == kViewRaw) {
			// White = the ray found nothing, black = blocked; blue tint = not traced (SSGI's AO there).
			// (46t) Two scales: the far segment; orange = no far sample (the near segment of that direction was blocked).
			if (weight <= 0.0)
				debug = notTraced;
			else if ((Size.z & kFlagValidity) && RawFarWeight(raw.b) <= 0.0)
				debug = float4(1.0, 0.5, 0.1, 1.0);
			else
				debug = float4(raw.rrr, 1.0);
		} else if (Size.w == kViewFiltered) {
			// The (large-scale) RTAO visibility after the filter (and history), with Strength, as it enters the mix.
			debug = weight > 0.0 ? float4(largeVis.xxx, 1.0) : notTraced;
		} else if (Size.w == kViewSplit) {
			const uint middle = Size.x / 2u;
			debug = (dtid.x + 1u >= middle && dtid.x <= middle + 1u) ? float4(1.0, 1.0, 0.2, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
		} else if (Size.w == kViewRawDetail) {
			// (46t) This frame's near segments: the open share (white all open, black all blocked); magenta = detail off.
			debug = !detail ? float4(0.8, 0.1, 0.8, 0.35) : (weight > 0.0 ? float4(raw.aaa, 1.0) : notTraced);
		} else if (Size.w == kViewFilteredDetail) {
			// (46t) The detail term after its filter (and history), with Detail Strength.
			debug = !detail ? float4(0.8, 0.1, 0.8, 0.35) : (weight > 0.0 ? float4(detailVis.xxx, 1.0) : notTraced);
		} else if (Size.w == kViewFinal) {
			// (46t) What the lighting gets: 1 - the final occlusion (RTAO mixed with SSGI's AO), everywhere.
			debug = float4((1.0 - occlusion).xxx, 1.0);
		} else if (Size.w >= kViewNormalConfidence && Size.w <= kViewNormalNg) {
			// (47k) Smooth Normals: written by AONormalPrepCS this frame (blue tint: it did not run, e.g. Smooth Normals off).
			debug = (Size.z & kFlagNormalDebug) ? NormalDebug[p] : notTraced;
		} else if (Size.w == kViewHistory) {
			// (47n) Written by RTAOTemporalCS (Temporal v2) this frame; without v2 a blue tint.
			if (Size.z & kFlagTemporalV2)
				return;
			debug = notTraced;
		}
		Debug[p] = debug;
	}
}
