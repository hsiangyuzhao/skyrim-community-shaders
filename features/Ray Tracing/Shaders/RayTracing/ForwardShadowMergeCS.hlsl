// (47z) "Keep Buildings in the Game's Sun Shadow Map": the sun cascades the forward readers sample (ShadowSampling t18, the shared
// capture of Volumetric Shadows / fog / Physical Sky), per texel the nearer depth of
//   - the engine's cascades (every caster the ray tracer does not cover; also what the engine's shadow mask and so every deferred
//     surface use, unchanged), and
//   - the kept cascades (RayTracingCasterFilter: the large casters the ray tracer covers, drawn there instead of being left out),
//     only for the slices it holds (the near ones); the other slices are the engine's.
// Depth as the engine stores it (1 = far, a texel is lit where its depth is greater than the receiver's).

Texture2DArray<float> Cascades : register(t0);
Texture2DArray<float> Kept : register(t1);
RWTexture2DArray<float> Merged : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	uint width, height, slices;
	Merged.GetDimensions(width, height, slices);
	if (dtid.x >= width || dtid.y >= height || dtid.z >= slices)
		return;
	uint keptWidth, keptHeight, keptSlices;
	Kept.GetDimensions(keptWidth, keptHeight, keptSlices);
	float depth = Cascades[dtid];
	if (dtid.z < keptSlices)
		depth = min(depth, Kept[dtid]);
	Merged[dtid] = depth;
}
