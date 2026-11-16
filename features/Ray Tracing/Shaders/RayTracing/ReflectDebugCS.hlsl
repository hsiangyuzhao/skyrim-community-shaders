// (48a) RT reflections step 1: the debug views of the reflection rays (TraceReflectCS output, render size) into the debug colour texture
// (drawn over the frame by OverlayCS like the other Ray Tracing views). Not traced pixels stay transparent.
//   1 Hit Type: green static, orange object, cyan foliage / cut-out, purple character, brown terrain, blue miss (left the scene / too far),
//     (48d) grey: nothing within the shortened range of a rough ray (Rough Rays: Leaves Solid; the gap fill keeps the old cubemap there);
//   2 Hit Distance: white near -> dark far (log scale up to Params.x units); a miss is blue;
//   3 Normal Confidence: how sure the reprojected last-frame normal is (green sure .. red fell back to the depth normal).

cbuffer ReflectDebugCB : register(b0)
{
	uint4 Size;     // render width, height, view (1..3), unused
	float4 Params;  // x max ray length (units)
};

Texture2D<uint> ReflectOut : register(t0);
RWTexture2D<unorm float4> DebugColor : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const uint v = ReflectOut[dtid.xy];
	float4 color = 0.0;
	if (v & 0x80u) {
		const uint type = v & 7u;
		const float confidence = float((v >> 3) & 7u) / 7.0;
		const float distance = f16tof32(v >> 16);
		if (Size.z == 1u) {
			if (type == 0u)
				color = float4(0.2, 0.4, 1.0, 0.6);
			else if (type == 1u)
				color = float4(0.1, 0.9, 0.2, 0.6);
			else if (type == 2u)
				color = float4(1.0, 0.55, 0.1, 0.6);
			else if (type == 3u)
				color = float4(0.1, 0.9, 0.9, 0.6);
			else if (type == 4u)
				color = float4(0.75, 0.2, 1.0, 0.6);
			else if (type == 6u)
				color = float4(0.5, 0.5, 0.5, 0.6);  // (48d) range end
			else
				color = float4(0.55, 0.35, 0.15, 0.7);
		} else if (Size.z == 2u) {
			if (type == 0u) {
				color = float4(0.2, 0.4, 1.0, 0.6);
			} else if (type == 6u) {
				color = float4(0.5, 0.5, 0.5, 0.6);  // (48d) range end
			} else {
				const float t = saturate(log2(1.0 + distance / 70.0) / log2(1.0 + max(Params.x, 70.0) / 70.0));
				color = float4((1.0 - t).xxx, 0.8);
			}
		} else {
			color = float4(lerp(float3(1.0, 0.15, 0.1), float3(0.1, 1.0, 0.2), confidence), 0.6);
		}
	}
	DebugColor[dtid.xy] = color;
}
