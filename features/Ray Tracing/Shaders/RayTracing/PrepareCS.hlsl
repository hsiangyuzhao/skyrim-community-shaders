// (45) Ray Tracing (prototype): copies what the ray tracer reads into its shared texture
// (R32_FLOAT: two-channel formats cannot be shared with D3D12, 45d):
//   |value| = the depth the engine's sun shadow mask was computed from (after the depth prepass)
//   sign    = negative where the engine's sun shadow mask (red channel) is already 0, so the
//             tracer can skip those pixels. Readers take abs() for the depth.

#include "RayTracing/Common.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float4> Mask : register(t1);
RWTexture2D<float> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const float depth = Depth[dtid.xy];
	Output[dtid.xy] = Mask[dtid.xy].x <= (0.5 / 255.0) ? -depth : depth;
}
