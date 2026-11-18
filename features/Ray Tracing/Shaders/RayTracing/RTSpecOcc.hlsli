// (49a) RT Specular Occlusion (S1): bent normal + cone intersection.
//
// TraceAOCS (kFlagBent) writes, per traced pixel, the mean of its near AO rays' directions weighted by whether each was open
// (cosine-weighted rays: an unoccluded pixel gives 2/3 x N). RTSpecOccCS blurs that vector edge-aware (this frame only, no history)
// into RGBA8: rgb = blurred vector x 0.5 + 0.5, a = the share of the blur's weight that had traced data (0 = none).
//
// DeferredCompositeCS (RT_SPECOCC) then replaces the scalar specular AO (Lagarde) by the share of the reflection cone that lies inside
// the visibility cone:
//   visibility cone:  axis = the bent normal B, half-angle from the final AO visibility v (cosine-weighted: cos = sqrt(1 - v));
//   reflection cone:  axis = R, half-angle from the roughness (Jimenez et al. 2016: cos = exp2(-3.32193 r^2));
//   share  = cap intersection / reflection cap (UE4's approximation of the spherical cap intersection), divided by the same share
//            for an unoccluded pixel (B = N, the hemisphere) so an open surface is never darkened by this term (the horizon is
//            already handled elsewhere).
// Where the direction cannot be trusted it falls back to the Lagarde value: no data (blur weight a), nearly fully occluded (the
// visibility cone narrower than ~6 degrees: UE4's 0.1 - 0.3 rad ramp) and rough surfaces (roughness 0.6 -> 1: at 1 the old value
// exactly). Strength blends between the two (1 = the cone result where trusted).

#ifndef RT_SPEC_OCC_HLSLI
#define RT_SPEC_OCC_HLSLI

namespace RTSpecOcc
{
	static const float kPI = 3.1415926535897932384626433832795f;

	float SafeAcos(float x)
	{
		return acos(clamp(x, -1.0, 1.0));
	}

	/// Solid angle of the intersection of two spherical caps (half-angles from their cosines, axes cosB apart); UE4's approximation.
	float CapIntersection(float cosC1, float cosC2, float cosB)
	{
		const float r1 = SafeAcos(cosC1);
		const float r2 = SafeAcos(cosC2);
		const float rd = SafeAcos(cosB);
		const float smallCap = 2.0 * kPI - 2.0 * kPI * max(cosC1, cosC2);
		// rd <= |r1 - r2| (one cap inside the other): x = 1, the smaller cap; rd >= r1 + r2: x = 0, none (saturate covers both).
		const float diff = abs(r1 - r2);
		const float x = 1.0 - saturate((rd - diff) / max(r1 + r2 - diff, 1e-4));
		return smoothstep(0.0, 1.0, x) * smallCap;
	}

	/// The share of the reflection cone (cosS around R) inside the visibility cone (cosV around B).
	float ConeShare(float cosV, float3 B, float cosS, float3 R)
	{
		const float specCap = max(2.0 * kPI * (1.0 - cosS), 1e-4);
		return saturate(CapIntersection(cosV, cosS, dot(B, R)) / specCap);
	}

	/// @param a_bent       the blurred bent texel (RTSpecOccCS output)
	/// @param a_visibility the final AO visibility (1 - the occlusion the composite reads)
	/// @param a_lagarde    the composite's scalar specular AO (the fallback, and the result where the direction is not trusted)
	/// @param a_N          world normal; a_R the reflection direction (world); a_roughness 1 - glossiness
	float Occlusion(float4 a_bent, float a_visibility, float a_lagarde, float3 a_N, float3 a_R, float a_roughness, float a_strength)
	{
		const float3 m = a_bent.xyz * 2.0 - 1.0;
		const float ml = length(m);
		// No data (or no direction): weight 0 below, i.e. the scalar term exactly. One exit (fxc X4000 with an early return).
		const float dataWeight = ml > 1e-3 ? saturate(a_bent.w) : 0.0;
		const float3 B = ml > 1e-3 ? m / ml : a_N;
		const float cosV = sqrt(saturate(1.0 - saturate(a_visibility)));
		const float r = saturate(a_roughness);
		const float cosS = clamp(exp2(-3.32193 * r * r), 0.02, 0.9995);
		const float3 R = normalize(a_R);
		const float3 N = normalize(a_N);
		const float open = ConeShare(0.0, N, cosS, R);  // the unoccluded reference: the hemisphere around N
		const float share = saturate(ConeShare(cosV, B, cosS, R) / max(open, 0.05));
		const float coneTrust = saturate((SafeAcos(cosV) - 0.1) / 0.2);
		const float roughTrust = 1.0 - smoothstep(0.6, 1.0, r);
		const float w = saturate(a_strength) * dataWeight * coneTrust * roughTrust;
		return lerp(a_lagarde, share, w);
	}
}

#endif  // RT_SPEC_OCC_HLSLI
