#ifndef RT_POINT_LIGHT_SHADOWS_HLSLI
#define RT_POINT_LIGHT_SHADOWS_HLSLI

// (46b) Ray-traced point-light shadows in the Lighting / Water pixel shaders (define RT_POINT_SHADOWS:
// Ray Tracing is installed; whether it runs is per light, see below).
//
// Light Limit Fix writes each light's tag into Light::rtShadow (RayTracing/PointLights.h): 0 = no
// ray-traced shadow this frame (Ray Tracing off, interior, not selected, the trace did not run), so
// with the feature off every light takes exactly the old path. Otherwise the low 7 bits are the light's
// shadow source + 1; this pixel's trace (TracePointLightsCS + PointDenoiseCS, bound to t40 / t41)
// lists the up to four sources it traced. The light's visibility is used when
//   - its source is among them (the K strongest of this pixel), and
//   - the shaded fragment is the surface that was traced (its depth matches the depth buffer; water,
//     glass and other surfaces drawn after the depth prepass are not).
// Else the light keeps Screen Space Point Light Shadows / its shadow map as before.

#include "Common/SharedData.hlsli"

namespace RTPointShadows
{
	Texture2D<unorm float4> Visibility : register(t40);  // denoised visibility per slot
	Texture2D<unorm float4> Sources : register(t41);     // per slot: source + 1 (low 7 bits)

	static const uint kSourceMask = 0x7Fu;
	static const uint kNative = 1u << 8;     // an engine shadow-map light: RT replaces its shadow map
	static const uint kKeepSSPLS = 1u << 9;  // Screen Space Point Light Shadows stay on as contact shadows (min with RT)
	static const uint kSplit = 1u << 10;     // debug A/B: RT on the left half of the screen only

	/// True when this pixel has a ray-traced visibility for the light with this tag (o_visibility, 0..1).
	/// a_position: SV_Position.xy (render-target pixel); a_screenUV: the shader's screen UV; a_viewDepth:
	/// the fragment's view-space depth (viewPosition.z).
	bool Get(uint a_tag, float2 a_position, float2 a_screenUV, float a_viewDepth, uint a_eyeIndex, out float o_visibility)
	{
		// One exit (fxc warns about early returns from a function called inside a condition).
		o_visibility = 1.0;
		bool found = false;
		const uint source = a_tag & kSourceMask;
		[branch] if (source != 0u && !((a_tag & kSplit) && a_screenUV.x >= 0.5))
		{
			const int3 pixel = int3(int2(a_position), 0);
			const uint4 ids = uint4(Sources.Load(pixel) * 255.0 + 0.5) & kSourceMask;
			const bool4 match = ids == source.xxxx;
			[branch] if (any(match))
			{
				// The traced surface is the one in the depth buffer.
				const float sceneDepth = SharedData::GetScreenDepth(a_screenUV, a_eyeIndex);
				[branch] if (abs(sceneDepth - a_viewDepth) <= a_viewDepth * 0.02 + 4.0)
				{
					const float4 visibility = Visibility.Load(pixel);
					o_visibility = match.x ? visibility.x : (match.y ? visibility.y : (match.z ? visibility.z : visibility.w));
					found = true;
				}
			}
		}
		return found;
	}
}

#endif  // RT_POINT_LIGHT_SHADOWS_HLSLI
