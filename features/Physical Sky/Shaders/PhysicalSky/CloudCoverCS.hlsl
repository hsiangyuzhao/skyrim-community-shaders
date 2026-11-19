// (49h stage 2) "Overcast From Measured Clouds": Cloud Shadows' occlusion cube (R8, 0 clear .. 1 opaque cloud, camera-centred world
// directions) reduced to four numbers, read back on the CPU 2-3 frames later (PhysicalSky::DispatchCloudCover):
//   [0] cloud cover: the cosine-weighted mean over the upper hemisphere (64 directions);
//   [1..3] the cloud in front of the sun, Masser and Secunda (a 5-tap cone of ~3 degrees around each).

TextureCube<float> CloudOcclusion : register(t0);
SamplerState LinearSampler : register(s0);
RWStructuredBuffer<float> Result : register(u0);

cbuffer CloudCoverCB : register(b0)
{
	float4 SunDir;      // xyz unit, w unused
	float4 MasserDir;
	float4 SecundaDir;
};

groupshared float coverShared[64];

float ConeOcclusion(float3 a_dir)
{
	const float3 dir = normalize(a_dir);
	const float3 up = abs(dir.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0);
	const float3 t = normalize(cross(up, dir));
	const float3 b = cross(dir, t);
	const float r = 0.05;  // ~3 degrees
	float sum = CloudOcclusion.SampleLevel(LinearSampler, dir, 0);
	sum += CloudOcclusion.SampleLevel(LinearSampler, dir + t * r, 0);
	sum += CloudOcclusion.SampleLevel(LinearSampler, dir - t * r, 0);
	sum += CloudOcclusion.SampleLevel(LinearSampler, dir + b * r, 0);
	sum += CloudOcclusion.SampleLevel(LinearSampler, dir - b * r, 0);
	return saturate(sum * 0.2);
}

[numthreads(64, 1, 1)] void main(uint gidx : SV_GroupIndex)
{
	// Cosine-weighted directions over the upper hemisphere (a Fibonacci spiral on the projected disc): their plain mean is the
	// irradiance-weighted cover.
	const float u = (float(gidx) + 0.5) / 64.0;
	const float r = sqrt(u);
	const float phi = float(gidx) * 2.39996323;
	const float3 dir = float3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u, 0.0)));
	coverShared[gidx] = CloudOcclusion.SampleLevel(LinearSampler, dir, 0);
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (gidx < stride)
			coverShared[gidx] += coverShared[gidx + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (gidx == 0)
		Result[0] = saturate(coverShared[0] / 64.0);
	else if (gidx == 1)
		Result[1] = ConeOcclusion(SunDir.xyz);
	else if (gidx == 2)
		Result[2] = ConeOcclusion(MasserDir.xyz);
	else if (gidx == 3)
		Result[3] = ConeOcclusion(SecundaDir.xyz);
}
