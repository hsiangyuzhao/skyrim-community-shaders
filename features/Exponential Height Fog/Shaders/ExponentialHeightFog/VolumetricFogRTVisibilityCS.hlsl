// (49b) RT fog visibility: last frame's ray-traced froxel visibility (Ray Tracing's TraceFogCS, a 2D atlas shared with D3D12) brought
// into this frame's near grid with the fog's own history reprojection (the previous unjittered view-projection, as
// VolumetricFogLightScatteringCS reprojects its history), blended with this pass's own history, written as 3D textures that the light
// scattering pass and lit effects read (RTFogVisibility.hlsli).
//
// Sun (every froxel): RawSun atlas texel (x, y + gridY * (z / 2)), rg = slice z even, ba = odd; r / b visibility, g / a trust. Bilinear in
// x / y and linear in z, weighted by the trust (texels without data do not pull the visibility toward 1). Without new data the history's
// trust fades (back to the shadow map in about a third of a second).
// Point lights (threads with even x and y: one per half-resolution texel): RawLightVis / RawLightId atlas texel (x, y + ceil(gridY / 2) z),
// nearest texel (the ids cannot be interpolated); a source found in the history at the same texel is blended with it.

#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

SamplerState LinearSampler : register(s0);
Texture2D<float4> RawSun : register(t0);
Texture2D<float4> RawLightVis : register(t1);
Texture2D<float4> RawLightId : register(t2);
Texture3D<float2> HistSun : register(t3);
Texture3D<float4> HistLightVis : register(t4);
Texture3D<uint4> HistLightId : register(t5);
RWTexture3D<float2> OutSun : register(u0);
RWTexture3D<float4> OutLightVis : register(u1);
RWTexture3D<uint4> OutLightId : register(u2);

cbuffer RTFogFilterCB : register(b1)
{
	uint4 RTFilterFlags;    // x flags (kRaw* / kHist*), yzw the raw atlas grid (last frame's near grid; must equal this one)
	float4 RTFilterParams;  // x sun history weight, y light history weight, z sun trust kept per frame without new data
};

static const uint kRawSun = 1u;
static const uint kRawLights = 2u;
static const uint kHistSun = 4u;
static const uint kHistLights = 8u;

// Last frame's volume uvw of a camera-relative position (VolumetricFogLightScatteringCS ComputeHistoryVolumeUVAndDepth, eye 0).
bool PreviousVolumeUVW(float3 positionWS, out float3 uvw)
{
	const float3 previousPositionWS = positionWS + FrameBuffer::CameraPosAdjust[0].xyz - FrameBuffer::CameraPreviousPosAdjust[0].xyz;
	const float4 previousClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[0], float4(previousPositionWS, 1.0f));
	uvw = 0.0f.xxx;
	if (previousClip.w <= 0.0f)
		return false;
	const float2 uv = previousClip.xy / previousClip.w * float2(0.5f, -0.5f) + 0.5f;
	uvw = float3(uv, ExponentialHeightFog::ComputeVolumetricNormalizedSlice(abs(previousClip.w)));
	return all(uvw >= 0.0f) && all(uvw < 1.0f);
}

// One slice of the raw sun atlas, bilinear in x / y: (sum of weight x trust x visibility, sum of weight x trust).
float2 RawSunSlice(float2 xy, uint slice, uint3 grid)
{
	const int2 maxXY = int2(grid.xy) - 1;
	const float2 f = frac(xy);
	const int2 p0 = int2(floor(xy));
	const uint row = grid.y * (slice / 2u);
	const bool odd = (slice & 1u) != 0u;
	float2 acc = 0.0f.xx;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const int2 o = int2(i & 1u, i >> 1u);
		const int2 p = clamp(p0 + o, 0, maxXY);
		const float w = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y);
		const float4 t = RawSun.Load(int3(p.x, p.y + int(row), 0));
		const float2 vt = odd ? t.zw : t.xy;
		acc += float2(vt.x * vt.y, vt.y) * w;
	}
	return acc;
}

[numthreads(8, 8, 4)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (!ExponentialHeightFog::IsInsideVolumetricGrid(dtid))
		return;
	const uint flags = RTFilterFlags.x;
	const uint3 rawGrid = RTFilterFlags.yzw;
	const uint3 grid = VolumetricFogGridSize;

	// Sun.
	{
		uint eyeIndex;
		float viewDepth;
		const float3 positionWS = ExponentialHeightFog::ComputeCellWorldPosition(dtid, 0.5f.xxx, eyeIndex, viewDepth);
		float3 uvw;
		const bool inside = PreviousVolumeUVW(positionWS, uvw);
		float2 raw = float2(1.0f, 0.0f);
		bool rawValid = false;
		[branch] if (inside && (flags & kRawSun))
		{
			const float3 p = uvw * float3(rawGrid) - 0.5f;
			const float zf = clamp(p.z, 0.0f, float(rawGrid.z) - 1.0f);
			const uint z0 = uint(floor(zf));
			const uint z1 = min(z0 + 1u, rawGrid.z - 1u);
			const float fz = zf - float(z0);
			const float2 acc = RawSunSlice(p.xy, z0, rawGrid) * (1.0f - fz) + RawSunSlice(p.xy, z1, rawGrid) * fz;
			raw = float2(acc.y > 1e-4f ? acc.x / acc.y : 1.0f, acc.y);
			rawValid = true;
		}
		float2 hist = float2(1.0f, 0.0f);
		bool histValid = false;
		[branch] if (inside && (flags & kHistSun))
		{
			hist = HistSun.SampleLevel(LinearSampler, uvw, 0);
			histValid = true;
		}
		float2 result = float2(1.0f, 0.0f);
		if (rawValid && histValid) {
			const float a = RTFilterParams.x;
			const float trust = lerp(raw.y, hist.y, a);
			const float weighted = lerp(raw.x * raw.y, hist.x * hist.y, a);
			result = float2(trust > 1e-4f ? weighted / trust : 1.0f, trust);
		} else if (rawValid) {
			result = raw;
		} else if (histValid) {
			result = float2(hist.x, hist.y * RTFilterParams.z);
		}
		if (result.y < 0.01f)
			result = float2(1.0f, 0.0f);  // no trust left (8-bit rounding would keep 1/255 for ever)
		OutSun[dtid] = saturate(result);
	}

	// Point lights: one thread per half-resolution texel.
	[branch] if (((dtid.x | dtid.y) & 1u) == 0u)
	{
		const uint3 h = uint3(dtid.xy / 2u, dtid.z);
		uint eyeIndex;
		float viewDepth;
		const float3 positionWS = ExponentialHeightFog::ComputeCellWorldPosition(dtid, float3(1.0f, 1.0f, 0.5f), eyeIndex, viewDepth);
		float3 uvw;
		const bool inside = PreviousVolumeUVW(positionWS, uvw);
		uint4 ids = 0u.xxxx;
		float4 vis = 1.0f.xxxx;
		[branch] if (inside && (flags & kRawLights))
		{
			const uint2 rawHalf = (rawGrid.xy + 1u) / 2u;
			const uint3 q = min(uint3(uvw * float3(rawHalf, rawGrid.z)), uint3(rawHalf, rawGrid.z) - 1u);
			const int3 atlas = int3(q.x, q.y + rawHalf.y * q.z, 0);
			ids = uint4(RawLightId.Load(atlas) * 255.0f + 0.5f);
			vis = RawLightVis.Load(atlas);
			[branch] if (flags & kHistLights)
			{
				const uint3 halfGrid = uint3((grid.xy + 1u) / 2u, grid.z);
				const uint3 hq = min(uint3(uvw * float3(halfGrid)), halfGrid - 1u);
				const uint4 histIds = HistLightId.Load(int4(hq, 0));
				const float4 histVis = HistLightVis.Load(int4(hq, 0));
				const float a = RTFilterParams.y;
				[unroll] for (uint k = 0u; k < 4u; ++k)
				{
					[unroll] for (uint m = 0u; m < 4u; ++m)
					{
						if (ids[k] != 0u && histIds[m] == ids[k])
							vis[k] = lerp(vis[k], histVis[m], a);
					}
				}
			}
		}
		OutLightId[h] = ids;
		OutLightVis[h] = saturate(vis);
	}
}
