// (45) Ray Tracing (prototype): merges the traced sun visibility into the engine's shadow mask.
//
//   mask.r = lerp(mask.r, min(mask.r, rt), w)
//
// rt is the traced visibility after a small depth-aware filter; w is 1 inside the traced area
// and fades to 0 over the hand-off band at its edge, where the cascades / terrain shadows take
// over again. mask.gba (local shadowed lights) are copied unchanged.
// Screen Space Shadows inside the traced area (Lighting multiplies them with the mask):
//   hand-off (45i): faded out by w (their texture set towards 1);
//   combine (45j, default): kept, rewritten as min(mask, sss) / mask so that mask * sss' =
//     min(mask, sss): contact shadows of everything the ray tracer does not see (grass, leaves,
//     characters left to the shadow map) stay, and where both see the same shadow it is not
//     darkened twice.
// (45j) Optional history blend (Temporal): the filtered visibility is blended with last frame's,
// reprojected with last frame's camera, clamped to this frame's 3x3 range; history whose distance
// does not match is dropped, and pixels shadowed by a character lean on the new frame.
// The debug views are written as colour + opacity for OverlayCS.

#include "RayTracing/Common.hlsli"

Texture2D<float4> Mask : register(t0);
Texture2D<unorm float4> Visibility : register(t1);  // r visibility, g coverage, w flags (R8G8B8A8_UNORM)
Texture2D<float> DepthMask : register(t2);  // signed depth, see PrepareCS (R32_FLOAT)
Texture2D<float2> HistoryIn : register(t3);  // (45j) r visibility, g distance to the camera (-1 = none)
Texture2D<float2> Denoised : register(t4);   // (45k) Penumbra: spatially filtered visibility, radius (px)
Texture2D<float4> History4In : register(t5); // (45k) Penumbra: visibility, distance (-1 = none), history length (frames)

RWTexture2D<unorm float4> OutMask : register(u0);
RWTexture2D<unorm float> ScreenSpaceShadows : register(u1);
RWTexture2D<unorm float4> DebugColor : register(u2);
RWTexture2D<float2> HistoryOut : register(u3);
RWTexture2D<float4> History4Out : register(u4);  // (45k)

bool TracedPixel(int2 q, uint flags)
{
	const float sq = DepthMask[q];
	const float dq = abs(sq);
	if (RTIsSky(dq) || ((flags & kFlagTraceSkipped) && sq < 0.0))
		return false;
	const float3 Pq = RTPosition(q, dq);
	return Pq.x >= Rect.x && Pq.y >= Rect.y && Pq.x <= Rect.z && Pq.y <= Rect.w;
}

// (45k) Denoiser Penumbra, after the spatial filter of this frame (DenoiseSpatialCS): history
// reprojected with the camera (bilinear, each tap checked against the expected distance), clamped
// to this frame's 3x3 mean +- k sigma, blended with 1 / length; the length grows to Denoise.z
// frames, less while the camera moves fast and where a character casts the shadow.
float PenumbraTemporal(int2 p, float3 P, float rt, uint flags, uint anyFlags, out float historyLength)
{
	historyLength = 1.0;
	float mean = 0.0, m2 = 0.0, n = 0.0;
	[unroll] for (int dy = -1; dy <= 1; ++dy)
	{
		[unroll] for (int dx = -1; dx <= 1; ++dx)
		{
			const int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(Size.xy) - 1);
			if (!TracedPixel(q, flags))
				continue;
			const float v = Denoised[q].x;
			mean += v;
			m2 += v * v;
			n += 1.0;
		}
	}
	if (n <= 0.0 || !(flags & kFlagHistoryValid))
		return rt;
	mean /= n;
	const float sigma = sqrt(max(m2 / n - mean * mean, 0.0));
	const float lo = mean - Denoise.w * sigma - 0.02, hi = mean + Denoise.w * sigma + 0.02;

	const float4 prevClip = mul(PrevViewProj, float4(P + CameraDelta.xyz, 1.0));
	if (prevClip.w <= 1e-4)
		return rt;
	const float2 prevUv = float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5);
	if (any(prevUv < 0.0) || any(prevUv >= 1.0))
		return rt;
	const float2 prevPos = prevUv * float2(Size.xy) - 0.5;
	const int2 base = int2(floor(prevPos));
	const float2 f = prevPos - float2(base);
	const float expected = length(P + CameraDelta.xyz);
	const float tolerance = expected * 0.02 + 8.0;
	float hv = 0.0, hl = 0.0, hw = 0.0;
	[unroll] for (int t = 0; t < 4; ++t)
	{
		const int2 o = int2(t & 1, t >> 1);
		const int2 q = clamp(base + o, int2(0, 0), int2(Size.xy) - 1);
		const float4 h = History4In[q];
		const float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
		if (h.y > 0.0 && abs(h.y - expected) <= tolerance && bw > 0.0) {
			hv += h.x * bw;
			hl += h.z * bw;
			hw += bw;
		}
	}
	if (hw < 0.05)
		return rt;  // disoccluded: this frame alone
	hv = clamp(hv / hw, lo, hi);
	hl /= hw;
	const float motion = length(prevPos + 0.5 - (float2(p) + 0.5));
	float maxLength = lerp(Denoise.z, 3.0, saturate((motion - 1.0) / 24.0));
	if (anyFlags & kOutCharacter)
		maxLength = min(maxLength, 3.0);
	historyLength = clamp(hl + 1.0, 1.0, max(maxLength, 1.0));
	return lerp(hv, rt, 1.0 / historyLength);
}

uint VisFlags(int2 q)
{
	return uint(Visibility[q].w * 255.0 + 0.5);
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(dtid.xy >= Size.xy))
		return;

	float4 mask = Mask[p];
	const float engineMask = mask.x;
	const float signedDepth = DepthMask[p];
	const float depth = abs(signedDepth);
	const uint flags = asuint(Band.z);
	const uint view = Size.z;

	float4 debug = 0;
	float w = 0;
	float rt = 1;
	float sss = 1;
	float2 history = float2(1.0, -1.0);
	float4 history4 = float4(1.0, -1.0, 0.0, 0.0);
	float historyLength = 0.0;

	if (!RTIsSky(depth)) {
		const float3 P = RTPosition(p, depth);
		const float edge = min(min(P.x - Rect.x, Rect.z - P.x), min(P.y - Rect.y, Rect.w - P.y));
		// (45k) Extra.x fades the traced result in after a loading screen (1 otherwise).
		w = saturate(edge / max(Band.x, 1.0)) * saturate(Extra.x);

		[branch] if (w > 0.0 && (flags & kFlagPenumbra)) {
			// (45k) Denoiser Penumbra: this frame's penumbra filter, then the clamped history.
			uint anyFlags = 0u;
			[unroll] for (int dy = -1; dy <= 1; ++dy)
			{
				[unroll] for (int dx = -1; dx <= 1; ++dx)
				{
					anyFlags |= VisFlags(clamp(p + int2(dx, dy), int2(0, 0), int2(Size.xy) - 1));
				}
			}
			const float dist = length(P);
			rt = Denoised[p].x;
			rt = PenumbraTemporal(p, P, rt, flags, anyFlags, historyLength);
			history4 = float4(rt, dist, historyLength, 0.0);

			if (signedDepth < 0.0 && (flags & kFlagTraceSkipped))
				rt = 0.0;
			mask.x = lerp(engineMask, min(engineMask, rt), w);
			if (flags & kFlagHandOffSSS) {
				sss = ScreenSpaceShadows[p];
				const bool keep = (flags & kFlagKeepSSSUnderFoliage) && (anyFlags & kOutAlphaCandidate);
				ScreenSpaceShadows[p] = keep ? sss : lerp(sss, 1.0, w);
			} else if (flags & kFlagCombineSSS) {
				sss = ScreenSpaceShadows[p];
				const float m = mask.x;
				const float ratio = m > (1.0 / 255.0) ? saturate(min(m, sss) / m) : 1.0;
				ScreenSpaceShadows[p] = lerp(sss, ratio, w);
			}
		} else if (w > 0.0) {
			// Depth-aware filter of the traced visibility (the trace is 1-2 jittered rays per pixel).
			// (45j) Pixels the trace skipped (engine mask already 0) hold a placeholder 0: they are
			// left out, so their shadow no longer bleeds into lit neighbours.
			const bool skipped = (flags & kFlagTraceSkipped) != 0u;
			const float dist = length(P);
			const int radius = int(Band.y);
			const float sigma = float(radius * radius) + 0.5;
			float sum = 0.0, weights = 0.0;
			float lo = 1.0, hi = 0.0;
			uint anyFlags = 0u;
			for (int dy = -radius; dy <= radius; ++dy) {
				for (int dx = -radius; dx <= radius; ++dx) {
					const int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(Size.xy) - 1);
					const float sq = DepthMask[q];
					const float dq = abs(sq);
					if (RTIsSky(dq) || (skipped && sq < 0.0))
						continue;
					const float distQ = length(RTPosition(q, dq));
					const float wt = exp(-abs(distQ - dist) / (dist * 0.01 + 2.0)) * exp(-float(dx * dx + dy * dy) / sigma);
					const float v = Visibility[q].x;
					sum += v * wt;
					weights += wt;
					if (abs(dx) <= 1 && abs(dy) <= 1) {
						lo = min(lo, v);
						hi = max(hi, v);
						anyFlags |= VisFlags(q);
					}
				}
			}
			rt = weights > 0.0 ? sum / weights : Visibility[p].x;
			if (lo > hi) {
				lo = 0.0;
				hi = 1.0;
			}

			if (flags & kFlagTemporal) {
				float blend = 1.0;
				if (flags & kFlagHistoryValid) {
					const float4 prevClip = mul(PrevViewProj, float4(P + CameraDelta.xyz, 1.0));
					if (prevClip.w > 1e-4) {
						const float2 prevUv = float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5);
						const int2 prevP = int2(prevUv * float2(Size.xy));
						if (all(prevUv >= 0.0) && all(prevUv < 1.0)) {
							const float2 h = HistoryIn[prevP];
							const float expected = length(P + CameraDelta.xyz);
							if (h.y > 0.0 && abs(h.y - expected) <= expected * 0.02 + 8.0) {
								const float clamped = clamp(h.x, lo, hi);
								blend = (anyFlags & kOutCharacter) ? 0.6 : Band.w;
								rt = lerp(clamped, rt, blend);
							}
						}
					}
				}
			}
			history = float2(rt, dist);

			if (signedDepth < 0.0 && (flags & kFlagTraceSkipped))
				rt = 0.0;  // the engine's mask is 0 here: min() keeps it, the debug views show the shadow
			mask.x = lerp(engineMask, min(engineMask, rt), w);

			if (flags & kFlagHandOffSSS) {
				sss = ScreenSpaceShadows[p];
				// (45j) Under leaves the traced shadow can be thinner than the contact shadow it replaces.
				const bool keep = (flags & kFlagKeepSSSUnderFoliage) && (anyFlags & kOutAlphaCandidate);
				ScreenSpaceShadows[p] = keep ? sss : lerp(sss, 1.0, w);
			} else if (flags & kFlagCombineSSS) {
				sss = ScreenSpaceShadows[p];
				const float m = mask.x;
				const float ratio = m > (1.0 / 255.0) ? saturate(min(m, sss) / m) : 1.0;
				ScreenSpaceShadows[p] = lerp(sss, ratio, w);
			}
		}
	}

	OutMask[p] = mask;
	if (flags & kFlagTemporal)
		HistoryOut[p] = history;
	if (flags & kFlagPenumbra)
		History4Out[p] = history4;
	if (flags & kFlagDebug) {
		if (view == kViewShadowSource) {
			if (w <= 0.0)
				debug = engineMask < 0.5 ? float4(0.15, 0.35, 1.0, 0.55) : float4(0.5, 0.5, 0.5, 0.2);  // outside: cascades / terrain
			else if (rt < 0.5)
				debug = float4(1.0, 0.1, 0.05, 0.6);  // ray-traced shadow
			else if (engineMask < 0.5)
				debug = float4(0.15, 0.35, 1.0, 0.6);  // shadow map: casters the ray tracer does not see
			else
				debug = float4(0.1, 1.0, 0.2, 0.15);  // lit
			if (w > 0.0 && w < 1.0)
				debug = lerp(float4(1.0, 0.85, 0.0, 0.5), debug, w);  // hand-off band
			if ((flags & kFlagHandOffSSS) && sss < 0.5 && w > 0.0)
				debug = float4(1.0, 0.5, 0.0, 0.6);  // contact shadow kept (should not happen with hand-off)
		} else if (view == kViewDisagreement) {
			if (w > 0.0) {
				const bool rtShadow = rt < 0.5, smShadow = engineMask < 0.5;
				if (rtShadow && !smShadow)
					debug = float4(1.0, 0.1, 0.05, 0.65);  // only RT says shadow
				else if (!rtShadow && smShadow)
					debug = float4(0.15, 0.35, 1.0, 0.65);  // only the shadow map says shadow
				else if (rtShadow)
					debug = float4(0.1, 0.1, 0.1, 0.3);  // both shadow
			}
		} else if (view == kViewCoverage) {
			// (45e) Primary-ray test (TraceShadowsCS): green = the visible surface is in the ray-traced
			// scene, magenta = it is not, orange = the scene has a surface in FRONT of the visible one
			// (geometry in the wrong place, or something the game hides).
			// (45h) cyan = an alpha-tested surface traced with its cut-out (0.8, Alpha-tested casters).
			// (45i) purple = a ray-traced character surface (0.65, Characters).
			if (w > 0.0) {
				const float c = Visibility[p].y;
				debug = c > 0.9 ? float4(0.1, 1.0, 0.2, 0.45) :
				        (c > 0.7 ? float4(0.0, 0.85, 1.0, 0.5) :
				        (c > 0.6 ? float4(0.55, 0.35, 1.0, 0.6) : (c > 0.375 ? float4(1.0, 0.55, 0.0, 0.6) : float4(1.0, 0.1, 1.0, 0.45))));
			}
		} else if (view == kViewHitDepth) {
			// (45e) Ray-traced first hit minus the game's depth along the view ray: white = match,
			// red = the ray tracer's surface is in front (full red at 256 units), blue = behind,
			// black = nothing within reach.
			if (w > 0.0) {
				const float2 vis = Visibility[p].yz;
				if (vis.x <= 0.0)
					debug = float4(0.0, 0.0, 0.0, 0.7);
				else {
					const float d = (vis.y - 0.5) * 2.0;  // -1 .. 1 = -256 .. 256 units, > 0 in front
					debug = d >= 0.0 ? float4(1.0, 1.0 - d, 1.0 - d, 0.7) : float4(1.0 + d, 1.0 + d, 1.0, 0.7);
				}
			}
		} else if (view == kViewRaw) {
			if (w > 0.0)
				debug = float4(Visibility[p].xxx, 1.0);
		} else if (view == kViewPenumbra) {
			// (45k) Penumbra filter radius: none (clear), small = blue, up to the maximum = red.
			if (w > 0.0 && (flags & kFlagPenumbra)) {
				const float r = Denoised[p].y / max(Denoise.y, 1.0);
				if (r > 0.0)
					debug = float4(saturate(r * 2.0), saturate(1.0 - abs(r - 0.5) * 2.0), saturate(1.0 - r * 2.0), 0.6);
			}
		} else if (view == kViewHistory) {
			// (45k) History length: red = this frame only (new / disoccluded), green = full history.
			if (w > 0.0 && (flags & kFlagPenumbra)) {
				const float h = saturate((historyLength - 1.0) / max(Denoise.z - 1.0, 1.0));
				debug = float4(1.0 - h, h, 0.1, 0.55);
			}
		} else if (view == kViewAdaptive) {
			// (45k) Magenta = adaptive extra rays traced here.
			if (w > 0.0 && (VisFlags(p) & kOutExtraRays))
				debug = float4(1.0, 0.1, 1.0, 0.6);
		} else if (view == kViewRtSurface) {
			// (45k) Screen Space Shadows mask: yellow = a ray-traced caster (its contact shadow is limited).
			if (VisFlags(p) & kOutRtSurface)
				debug = float4(1.0, 0.9, 0.1, 0.5);
		}
		DebugColor[p] = debug;
	}
}
