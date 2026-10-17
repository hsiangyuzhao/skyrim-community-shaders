// (47i) Point Light Shadows on Water: after this frame's water draws, the water surface's distance from the camera per render
// pixel, where the water lies in front of the opaque surface the trace saw (DepthMask, |value| = depth after the prepass);
// 0 elsewhere. Next frame's point-light trace (TracePointLightsCS kFlagWater) traces the pixel's sources from that distance
// along its own view ray, and the Water shader uses the result where its fragment is at that distance (PointLightShadows.hlsli).

cbuffer WaterPrepCB : register(b0)
{
	row_major float4x4 InvViewProj;  // this frame's (the trace's, camera-relative)
	float4 RenderSize;               // xy size, zw 1 / size
};

Texture2D<float> WaterDepth : register(t0);   // the depth after the water (a copy)
Texture2D<float> OpaqueDepth : register(t1);  // the trace's DepthMask
RWTexture2D<float> Output : register(u0);

float DistanceAt(uint2 p, float depth)
{
	const float2 uv = (float2(p) + 0.5) * RenderSize.zw;
	const float4 pos = mul(InvViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
	return length(pos.xyz / pos.w);
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= uint2(RenderSize.xy)))
		return;
	const float water = WaterDepth[dtid.xy];
	const float opaque = abs(OpaqueDepth[dtid.xy]);
	float result = 0.0;
	[branch] if (water > 0.0 && water < 0.999999 && water < opaque)
	{
		const float d = DistanceAt(dtid.xy, water);
		const float behind = opaque < 0.999999 ? DistanceAt(dtid.xy, opaque) : 1e30;
		if (d < behind - max(1.0, 0.002 * behind))
			result = d;
	}
	Output[dtid.xy] = result;
}
