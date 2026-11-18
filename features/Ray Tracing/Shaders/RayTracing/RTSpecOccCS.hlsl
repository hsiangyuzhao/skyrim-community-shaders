// (49a) RT Specular Occlusion (S1): the bent-normal blur (see RTSpecOcc.hlsli). Two separable passes at render resolution, this frame's
// rays only (hard constraint #3, RTAO exception: spatial filter, no NRD, no history), edge-aware like the RTAO filters (linear depth,
// G-buffer normal). 9 taps 2 pixels apart per axis (17 x 17 pixels in all).
//   horizontal (default): t0 = TraceAOCS's bent output (rgb = mean open direction x 0.5 + 0.5, a = 1 traced / 0 not)
//   RTSO_VERTICAL:        t0 = the horizontal result; t3 = this frame's final AO occlusion (RTAOCompositeCS output), u1 = the RTAO
//                         debug overlay (view kViewSpecOcc: the specular occlusion the composite applies)
// Output: rgb = the blurred vector x 0.5 + 0.5, a = the share of the blur's weight that had traced data.

#include "Common/GBuffer.hlsli"
#include "RayTracing/RTAOCommon.hlsli"
#include "RayTracing/RTSpecOcc.hlsli"

Texture2D<unorm float4> Input : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<unorm float3> NormalRoughness : register(t2);
RWTexture2D<unorm float4> Output : register(u0);
#if defined(RTSO_VERTICAL)
Texture2D<unorm float> FinalOcclusion : register(t3);
RWTexture2D<unorm float4> Debug : register(u1);
#endif

static const uint kViewSpecOcc = 11u;  // RayTracing::AODebugView::SpecularOcclusion
static const int kTaps = 4;            // each side
static const int kStride = 2;

float LinearDepth(float a_depth)
{
	return Camera.w / max(Camera.x - a_depth * Camera.z, 1e-6);
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 p = int2(dtid.xy);
	const float depth = Depth[p];
	[branch] if (!(depth < 0.999999 && depth > 0.0))
	{
		Output[p] = float4(0.5, 0.5, 0.5, 0.0);
#if defined(RTSO_VERTICAL)
		if (Size.w == kViewSpecOcc)
			Debug[p] = float4(0.1, 0.2, 1.0, 0.35);
#endif
		return;
	}
	const float z = LinearDepth(depth);
	const float3 n = GBuffer::DecodeNormal(NormalRoughness[p].xy);
	const float depthScale = 0.02 * z + 0.5;
#if defined(RTSO_VERTICAL)
	const int2 axis = int2(0, 1);
#else
	const int2 axis = int2(1, 0);
#endif
	const int2 maxP = int2(Size.xy) - 1;
	float3 sumVector = 0.0;
	float sumData = 0.0;
	float sumWeight = 0.0;
	[unroll] for (int i = -kTaps; i <= kTaps; ++i)
	{
		const int2 q = clamp(p + axis * (i * kStride), int2(0, 0), maxP);
		const float dq = Depth[q];
		if (!(dq < 0.999999 && dq > 0.0))
			continue;
		const float g = exp(-float(i * i) / (2.0 * 2.5 * 2.5));
		const float wd = exp(-abs(LinearDepth(dq) - z) / depthScale);
		const float3 nq = GBuffer::DecodeNormal(NormalRoughness[q].xy);
		const float nd = saturate(dot(n, nq));
		const float wn = nd * nd * nd * nd;
		const float w = g * wd * wn * wn;  // dot^8, as the RTAO filters' normal weight
		const float4 t = Input[q];
		const float data = saturate(t.a);
		sumWeight += w;
		sumData += w * data;
		sumVector += w * data * (t.rgb * 2.0 - 1.0);
	}
	const float share = sumWeight > 1e-6 ? sumData / sumWeight : 0.0;
	const float3 m = sumData > 1e-6 ? sumVector / sumData : 0.0;
	const float4 result = float4(saturate(m * 0.5 + 0.5), share);
	Output[p] = result;

#if defined(RTSO_VERTICAL)
	[branch] if (Size.w == kViewSpecOcc)
	{
		// What DeferredCompositeCS applies to the cubemap reflection here (white = none, black = all), strength 1; blue tint = no data.
		// The Lagarde fallback with the composite's own inputs (its view vector points from the camera, as it passes it).
		const float2 uv = (float2(p) + 0.5) / float2(Size.xy);
		float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
		float4 world = mul(InvViewProj, clip);
		const float3 V = normalize(world.xyz / world.w);
		const float3 N = normalize(mul((float3x3)ViewInverse, n));
		const float roughness = 1.0 - NormalRoughness[p].z;
		const float visibility = 1.0 - saturate(FinalOcclusion[p]);
		const float lagarde = saturate(pow(abs(saturate(dot(N, V)) + visibility), exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility);
		const float occ = RTSpecOcc::Occlusion(result, visibility, lagarde, N, reflect(V, N), roughness, 1.0);
		Debug[p] = result.a > 0.0 ? float4(occ.xxx, 1.0) : float4(0.1, 0.2, 1.0, 0.35);
	}
#endif
}
