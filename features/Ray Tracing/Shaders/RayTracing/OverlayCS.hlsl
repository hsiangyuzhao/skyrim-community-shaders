// (45) Ray Tracing (prototype): blends the debug view colours over the lit scene (after the
// deferred composite, before transparency and post-processing).

#include "RayTracing/Common.hlsli"

Texture2D<unorm float4> DebugColor : register(t0);
RWTexture2D<float4> Scene : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const float4 debug = DebugColor[dtid.xy];
	if (debug.a <= 0.0)
		return;
	float4 color = Scene[dtid.xy];
	// (45g) A light tint (about 25-40 % at most, slightly desaturated) so the geometry underneath
	// stays readable in screenshots; the views' own opacities keep their relative weight.
	// Opaque entries (Raw RT Visibility) still replace the scene.
	if (debug.a >= 0.999) {
		color.rgb = debug.rgb * 0.6;
	} else {
		const float3 tint = lerp(dot(debug.rgb, float3(0.3333, 0.3333, 0.3333)).xxx, debug.rgb, 0.8) * 0.6;
		color.rgb = lerp(color.rgb, tint, saturate(debug.a * 0.6));
	}
	Scene[dtid.xy] = color;
}
