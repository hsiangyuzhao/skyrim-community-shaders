// (47l, RT foliage sway) Poses a tree-animated mesh's vertices on the ray tracer's D3D12 queue
// (cs_6_5, compiled at build time with the Windows SDK's dxc like SkinCharactersCS).
//
// The game's tree animation, replayed exactly from the rest pose (the shadow map's Utility.hlsl VS, ~line 153, and the
// Lighting.hlsl VS GetTreeShiftVector, ~line 185, are the same formula):
//   t      = (TreeParams.w * TreeParams.y) * timer          leaf frequency x wind magnitude x the tree timer
//   s      = smoothstep(|2 frac((0.1, 0.25) t + dot(p, 1) + 0.5) - 1|)
//   offset = normal * (s.x + 0.1 s.y) * (vertex alpha * TreeParams.z)   amplitude; normal = 2 n - 1 (UNORM8)
// The offset is always along +normal, in [0, 1.1 amplitude alpha]: the rest pose is one end of the sway, not its centre.
// Mode 0 (a first build) poses every vertex at the offset's mean over time, 0.55 amplitude alpha (E[s] = 0.5): the BVH is
// built in the middle of the sway and every later in-place refit stays within half its range of it.
//
// One dispatch per mesh, 64 vertices per group. Root bindings (Backend::CreateWindPipeline):
//   b0 constants: x vertex count, y mode (0 mean pose, 1 the timer's pose), z amplitude, w leaf frequency (float bits),
//                 then wind magnitude, timer (float bits), 2 unused
//   t0 Rest (raw): per vertex 16 bytes = float3 position (the mesh's own space, as the frozen BLAS), uint normal xyz (UNORM8,
//                  as stored by the engine) | vertex alpha << 24
//   u0 Out  (raw): per vertex float3 position (the mesh's own space) = the wind BLAS vertex buffer
// A non-finite result falls back to the rest position (the CPU checked those), so a BLAS never holds NaN vertices.

cbuffer WindConstants : register(b0)
{
	uint VertexCount;
	uint Mode;
	float Amplitude;
	float Frequency;
	float Magnitude;
	float Timer;
	uint2 Unused;
};

ByteAddressBuffer Rest : register(t0);
RWByteAddressBuffer Out : register(u0);

float2 SmoothSaturate(float2 value)
{
	return value * value * (3.0 - 2.0 * value);
}

[numthreads(64, 1, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	const uint v = dtid.x;
	if (v >= VertexCount)
		return;
	const uint base = v * 16u;
	precise float3 position = asfloat(Rest.Load3(base));
	const uint packed = Rest.Load(base + 12u);
	const float3 normal = float3(uint3(packed, packed >> 8u, packed >> 16u) & 0xFFu) * (2.0 / 255.0) - 1.0;
	const float alpha = float(packed >> 24u) * (1.0 / 255.0);

	precise float wave = 0.55;
	if (Mode != 0u) {
		precise float t = (Frequency * Magnitude) * Timer;
		precise float2 s = SmoothSaturate(abs(2.0 * frac(float2(0.1, 0.25) * t + dot(position, 1.0.xxx) + 0.5) - 1.0));
		wave = s.x + 0.1 * s.y;
	}
	precise float3 result = position + normal * (wave * (alpha * Amplitude));
	if (!all(isfinite(result)))
		result = position;
	Out.Store3(v * 12u, asuint(result));
}
