// (46f) Ray-traced ambient occlusion: the mix with Screen Space GI's AO, written into the texture SSGI's AO
// consumers read this frame (DeferredComposite t10, SSRT t9: occlusion, they take 1 - value as visibility).
//
//   w      = the RT weight: traced, inside the traced area, fading back to SSGI's AO with distance (TraceAOCS .g),
//            times the RT fade-in after a loading screen
//   rtVis  = 1 - (1 - filtered RTAO) * strength
//   mode "RTAO (keeps SSGI contact AO)": rtVis * contact (the contact AO's visibility; SSGI's main AO is replaced)
//   mode "Darker of RTAO and SSGI AO":   min(rtVis, 1 - SSGI's whole AO)
//   out    = lerp(SSGI's AO, 1 - mixed, w)
// Split (debug): the right half keeps SSGI's AO as before, a line marks the middle.
// Terrain and grass are not in the TLAS: their own occlusion comes from the contact AO (or SSGI in "Darker").

cbuffer RTAOCB : register(b0)
{
	uint4 Size;     // render width, height, flags, debug view (0 none, 1 raw, 2 filtered, 3 split line)
	float4 Params;  // x strength, y this frame's weight in the history, z history clamp slack, w RT fade-in (0..1)
};

static const uint kFlagContact = 1u;
static const uint kFlagDarker = 2u;
static const uint kFlagSplit = 8u;
static const uint kViewRaw = 1u;
static const uint kViewFiltered = 2u;
static const uint kViewSplit = 3u;

Texture2D<unorm float> RTAO : register(t0);    // filtered (and history-blended) visibility
Texture2D<unorm float4> Raw : register(t1);    // TraceAOCS: r this frame's ray, g RT weight
Texture2D<unorm float> SsgiAo : register(t2);  // SSGI's own AO this frame (occlusion; contact AO folded in)
Texture2D<unorm float> Contact : register(t3); // SSGI's contact AO visibility (kFlagContact)
RWTexture2D<unorm float> Output : register(u0);
RWTexture2D<unorm float4> Debug : register(u1);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 p = int2(dtid.xy);
	const float ssgiOcclusion = saturate(SsgiAo[p]);
	const float4 raw = Raw[p];
	const float weight = saturate(raw.g * Params.w);
	const float rtVis = 1.0 - saturate((1.0 - RTAO[p]) * Params.x);
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
		float4 debug = 0.0;
		if (Size.w == kViewRaw) {
			// White = the ray found nothing, black = blocked; blue tint = not traced (SSGI's AO there).
			debug = weight > 0.0 ? float4(raw.rrr, 1.0) : float4(0.1, 0.2, 1.0, 0.35);
		} else if (Size.w == kViewFiltered) {
			// The RTAO visibility after the filter (and history), as it enters the mix.
			debug = weight > 0.0 ? float4(rtVis.xxx, 1.0) : float4(0.1, 0.2, 1.0, 0.35);
		} else if (Size.w == kViewSplit) {
			const uint middle = Size.x / 2u;
			debug = (dtid.x + 1u >= middle && dtid.x <= middle + 1u) ? float4(1.0, 1.0, 0.2, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
		}
		Debug[p] = debug;
	}
}
