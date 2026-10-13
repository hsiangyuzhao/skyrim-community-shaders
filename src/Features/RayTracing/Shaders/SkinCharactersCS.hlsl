// (45i, RT characters) Skins a character shape's vertices on the ray tracer's D3D12 queue
// (cs_6_5, compiled at build time with the Windows SDK's dxc like TraceShadowsCS).
//
// One dispatch per shape, 64 vertices per group. Root bindings (Backend::CreateSkinPipeline):
//   b0 constants: x vertex count, y first bone of this shape in Bones, z bone count, w unused
//   t0 Rest  (raw): per vertex 32 bytes = float3 position (the mesh's own space), uint 4 x u8 bone
//                   index, float4 weights (normalized on the CPU)
//   t1 Bones (raw): per bone 48 bytes = row-major float3x4, bone world * skin-to-bone, with the
//                   translation relative to the shape's draw origin (the actor), filled per frame
//   u0 Out   (raw): per vertex float3 position relative to the same origin = the BLAS vertex buffer
// Bone indices were checked against the bone count on the CPU; they are clamped here as well.
// A non-finite result (a bad bone) falls back to the first bone alone, then to the origin, so a
// BLAS never holds NaN vertices (an update may not turn triangles inactive).

cbuffer SkinConstants : register(b0)
{
	uint VertexCount;
	uint BoneBase;
	uint BoneCount;
	uint Unused;
};

ByteAddressBuffer Rest : register(t0);
ByteAddressBuffer Bones : register(t1);
RWByteAddressBuffer Out : register(u0);

float3 Transform(uint bone, float4 position)
{
	const uint offset = (BoneBase + bone) * 48u;
	const float4 r0 = asfloat(Bones.Load4(offset));
	const float4 r1 = asfloat(Bones.Load4(offset + 16u));
	const float4 r2 = asfloat(Bones.Load4(offset + 32u));
	return float3(dot(r0, position), dot(r1, position), dot(r2, position));
}

[numthreads(64, 1, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	const uint v = dtid.x;
	if (v >= VertexCount || BoneCount == 0u)
		return;
	const uint base = v * 32u;
	const float4 position = float4(asfloat(Rest.Load3(base)), 1.0);
	const uint bones = Rest.Load(base + 12u);
	const float4 weights = asfloat(Rest.Load4(base + 16u));
	const uint last = BoneCount - 1u;

	float3 result = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const uint bone = min((bones >> (8u * i)) & 0xFFu, last);
		if (weights[i] != 0.0)
			result += weights[i] * Transform(bone, position);
	}
	if (!all(isfinite(result))) {
		result = Transform(min(bones & 0xFFu, last), position);
		if (!all(isfinite(result)))
			result = 0.0;
	}
	Out.Store3(v * 12u, asuint(result));
}
