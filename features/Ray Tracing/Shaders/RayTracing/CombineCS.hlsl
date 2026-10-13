// (45) Ray Tracing (prototype): merges the traced sun visibility into the engine's shadow mask.
//
//   mask.r = lerp(mask.r, min(mask.r, rt), w)
//
// rt is the traced visibility after a small depth-aware filter; w is 1 inside the traced area
// and fades to 0 over the hand-off band at its edge, where the cascades / terrain shadows take
// over again. mask.gba (local shadowed lights) are copied unchanged. With the hand-off flag,
// Screen Space Shadows are faded out by the same w (their texture is set towards 1).
// The debug views are written as colour + opacity for OverlayCS.

#include "RayTracing/Common.hlsli"

Texture2D<float4> Mask : register(t0);
Texture2D<unorm float4> Visibility : register(t1);  // r visibility, g coverage (R8G8B8A8_UNORM)
Texture2D<float> DepthMask : register(t2);  // signed depth, see PrepareCS (R32_FLOAT)

RWTexture2D<unorm float4> OutMask : register(u0);
RWTexture2D<unorm float> ScreenSpaceShadows : register(u1);
RWTexture2D<unorm float4> DebugColor : register(u2);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(dtid.xy >= Size.xy))
		return;

	float4 mask = Mask[p];
	const float engineMask = mask.x;
	const float depth = abs(DepthMask[p]);
	const uint flags = asuint(Band.z);
	const uint view = Size.z;

	float4 debug = 0;
	float w = 0;
	float rt = 1;
	float sss = 1;

	if (!RTIsSky(depth)) {
		const float3 P = RTPosition(p, depth);
		const float edge = min(min(P.x - Rect.x, Rect.z - P.x), min(P.y - Rect.y, Rect.w - P.y));
		w = saturate(edge / max(Band.x, 1.0));

		if (w > 0.0) {
			// Depth-aware filter of the traced visibility (the trace is 1-2 jittered rays per pixel).
			const float dist = length(P);
			const int radius = int(Band.y);
			const float sigma = float(radius * radius) + 0.5;
			float sum = 0.0, weights = 0.0;
			for (int dy = -radius; dy <= radius; ++dy) {
				for (int dx = -radius; dx <= radius; ++dx) {
					const int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(Size.xy) - 1);
					const float dq = abs(DepthMask[q]);
					if (RTIsSky(dq))
						continue;
					const float distQ = length(RTPosition(q, dq));
					const float wt = exp(-abs(distQ - dist) / (dist * 0.01 + 2.0)) * exp(-float(dx * dx + dy * dy) / sigma);
					sum += Visibility[q].x * wt;
					weights += wt;
				}
			}
			rt = weights > 0.0 ? sum / weights : Visibility[p].x;
			mask.x = lerp(engineMask, min(engineMask, rt), w);

			if (flags & kFlagHandOffSSS) {
				sss = ScreenSpaceShadows[p];
				ScreenSpaceShadows[p] = lerp(sss, 1.0, w);
			}
		}
	}

	OutMask[p] = mask;

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
			if (sss < 0.5 && w > 0.0)
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
			if (w > 0.0) {
				const float c = Visibility[p].y;
				debug = c > 0.75 ? float4(0.1, 1.0, 0.2, 0.45) : (c > 0.375 ? float4(1.0, 0.55, 0.0, 0.6) : float4(1.0, 0.1, 1.0, 0.45));
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
		}
		DebugColor[p] = debug;
	}
}
