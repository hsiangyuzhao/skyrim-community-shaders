// (45) Ray Tracing (prototype): copies what the ray tracer reads into its shared texture.
//   .x = the depth the engine's sun shadow mask was computed from (after the depth prepass)
//   .y = the engine's sun shadow mask (red channel), so the tracer can skip pixels already at 0

#include "RayTracing/Common.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float4> Mask : register(t1);
RWTexture2D<float2> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	Output[dtid.xy] = float2(Depth[dtid.xy], Mask[dtid.xy].x);
}
