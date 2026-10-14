// (45l) Ray Tracing, Denoiser "NRD SIGMA": the inputs of NVIDIA's shadow denoiser, from this
// frame's trace and depth (all at render resolution, before the opaque pass, so nothing of the
// G-buffer exists yet):
//   u0 IN_VIEWZ            linear view depth (the formula of the NRD guides); sky and pixels outside
//                          the traced area far beyond the denoising range (SIGMA leaves them alone)
//   u1 IN_NORMAL_ROUGHNESS world-space normal from depth (per axis the neighbour with the smaller depth
//                          step, as in the trace), roughness 1, NRD_NORMAL_ENCODING 2 / ROUGHNESS_ENCODING 1
//   u2 IN_MV               camera-only motion, uv(previous, unjittered) - uv(now, unjittered)
//   u3 IN_PENUMBRA         SIGMA_FrontEnd_PackPenumbra(blocker distance, tan(sun cone)): the first ray's
//                          closest hit; NRD_FP16_MAX where it is lit; 0 where N.L <= 0 or the engine's mask
//                          is already 0 (trace skipped)

#include "NRD/NRDReblurSH.hlsli"
#include "RayTracing/Common.hlsli"

Texture2D<unorm float4> Visibility : register(t0);
Texture2D<float> DepthMask : register(t1);
RWTexture2D<float> OutViewZ : register(u0);
RWTexture2D<unorm float4> OutNormalRoughness : register(u1);
RWTexture2D<float2> OutMotion : register(u2);
RWTexture2D<float> OutPenumbra : register(u3);

static const float kFarViewZ = 3.402823466e+38;

float PackPenumbra(float distanceToOccluder, float tanOfLightAngularRadius)
{
	// SIGMA_FrontEnd_PackPenumbra (NRD 4.17, directional light).
	const float penumbraRadius = distanceToOccluder * tanOfLightAngularRadius * 0.5;
	return distanceToOccluder >= NRD_FP16_MAX ? NRD_FP16_MAX : min(penumbraRadius, 32768.0);
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(dtid.xy >= Size.xy))
		return;
	const uint flags = asuint(Band.z);
	const float sd = DepthMask[p];
	const float depth = abs(sd);
	float viewZ = kFarViewZ;
	float4 normal = NRD_FrontEnd_PackNormalAndRoughness(float3(0, 0, 1), 1.0, 0.0);
	float2 motion = 0.0;
	float penumbra = NRD_FP16_MAX;

	if (!RTIsSky(depth)) {
		const float3 P = RTPosition(p, depth);
		const bool inside = P.x >= Rect.x && P.y >= Rect.y && P.x <= Rect.z && P.y <= Rect.w;
		if (inside) {
			viewZ = CameraData.w / (-depth * CameraData.z + CameraData.x);

			const int2 maxP = int2(Size.xy) - 1;
			const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
			const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
			const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
			const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - RTPosition(pl, dl) : RTPosition(pr, dr) - P;
			const float3 dy = abs(du - depth) < abs(dd - depth) ? P - RTPosition(pu, du) : RTPosition(pd, dd) - P;
			const float dist = length(P);
			const float3 V = dist > 0.0 ? -P / dist : float3(0, 0, 1);
			float3 N = cross(dy, dx);
			const float len = length(N);
			N = len > 1e-12 ? N / len : V;
			if (dot(N, V) < 0.0)
				N = -N;
			normal = NRD_FrontEnd_PackNormalAndRoughness(N, 1.0, 0.0);

			const float4 clipNow = mul(ViewProjUnjittered, float4(P, 1.0));
			const float4 clipPrev = mul(PrevViewProjUnjittered, float4(P + CameraDelta.xyz, 1.0));
			if (clipNow.w > 1e-4 && clipPrev.w > 1e-4) {
				const float2 uvNow = float2(clipNow.x / clipNow.w * 0.5 + 0.5, 0.5 - clipNow.y / clipNow.w * 0.5);
				const float2 uvPrev = float2(clipPrev.x / clipPrev.w * 0.5 + 0.5, 0.5 - clipPrev.y / clipPrev.w * 0.5);
				motion = uvPrev - uvNow;
			}

			const float4 v = Visibility[p];
			if (((flags & kFlagTraceSkipped) && sd < 0.0) || dot(N, SunDirection.xyz) <= 0.0) {
				penumbra = 0.0;  // in shadow, hard
			} else if (v.x < 0.5) {
				const float t = v.z > 0.0 ? v.z * v.z * kBlockerRange : 1.0;
				penumbra = PackPenumbra(t, SunDirection.w);
			}
		}
	}
	OutViewZ[p] = viewZ;
	OutNormalRoughness[p] = normal;
	OutMotion[p] = motion;
	OutPenumbra[p] = penumbra;
}
