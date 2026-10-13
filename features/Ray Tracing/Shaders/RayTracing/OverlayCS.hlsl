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
	color.rgb = lerp(color.rgb, debug.rgb * 0.6, debug.a);
	Scene[dtid.xy] = color;
}
