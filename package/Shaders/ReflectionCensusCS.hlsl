// (49h stage 1, S9) Reflection census: run once when the Performance Overlay saves a frame (F12 freeze + save), never otherwise.
// One thread per render pixel over this frame's G-buffer, right before the deferred composite (the same inputs it reads):
//   sky (depth 1) / no specular (reflectance luminance < 0.002) / smooth (roughness < RoughStart) / semi / rough (> RoughEnd);
//   the 48d RT reflection importance ((1 - smoothstep(RoughStart, RoughEnd, roughness)) x saturate(reflectance luminance / 0.04),
//   counted where >= 1/32 as in AONormalPrepCS: the share of pixels that would get a ray, x 1024 fixed point);
//   with SSRT specular this frame: beyond its distance limit / skipped by Specular Max Roughness / hit (screen or RT fill: hit
//   distance < 65536) / missed (cubemap, or left the screen: SSRT writes 65536 for both) / not traced (cleared, 0).
// Counts per group in groupshared memory, one atomic per counter per group. Nothing on screen changes.

Texture2D<unorm float4> NormalRoughness : register(t0);  // .z glossiness (roughness = 1 - z)
Texture2D<float4> Reflectance : register(t1);           // the composite's reflectance (t5 there)
Texture2D<float> Depth : register(t2);                  // kPOST_ZPREPASS_COPY
Texture2D<float> SSRTHitDistance : register(t3);        // SSRT specular hit distance (bound only when it ran this frame)

RWByteAddressBuffer Counters : register(u0);

cbuffer CensusCB : register(b0)
{
	uint2 RenderSize;
	uint SSRTValid;      // t3 holds this frame's specular hit distance
	uint SSRTCapOn;      // SSRT's distance limit applies
	float RoughStart;    // 0.30
	float RoughEnd;      // 0.65
	float SSRTCapEnd;    // view depth, game units
	float SSRTMaxRoughness;
	float4 CameraData;   // SharedData::CameraData (x far, y near... as SSRTCompositeViewZ uses it)
};

static const uint kCounters = 12;
// 0 pixels, 1 sky, 2 no specular, 3 smooth, 4 semi, 5 rough, 6 needs-a-ray x1024, 7 SSR beyond, 8 SSR rough skip, 9 SSR hit, 10 SSR miss,
// 11 SSR not traced
groupshared uint gCount[kCounters];

float ViewZ(float a_depth)
{
	return CameraData.w / (-a_depth * CameraData.z + CameraData.x);
}

[numthreads(16, 16, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
	if (gi < kCounters)
		gCount[gi] = 0u;
	GroupMemoryBarrierWithGroupSync();

	[branch] if (all(dtid.xy < RenderSize))
	{
		InterlockedAdd(gCount[0], 1u);
		const float depth = Depth[dtid.xy];
		[branch] if (depth >= 1.0 - 1e-6)
		{
			InterlockedAdd(gCount[1], 1u);
		}
		else
		{
			const float3 reflectance = max(Reflectance[dtid.xy].rgb, 0.0);
			const float reflectanceY = dot(reflectance, float3(0.2126, 0.7152, 0.0722));
			const float roughness = saturate(1.0 - NormalRoughness[dtid.xy].z);
			[branch] if (reflectanceY < 0.002)
			{
				InterlockedAdd(gCount[2], 1u);
			}
			else
			{
				InterlockedAdd(gCount[roughness < RoughStart ? 3 : (roughness > RoughEnd ? 5 : 4)], 1u);
				const float importance = (1.0 - smoothstep(RoughStart, RoughEnd, roughness)) * saturate(reflectanceY / 0.04);
				if (importance >= 1.0 / 32.0)
					InterlockedAdd(gCount[6], uint(round(importance * 1024.0)));
				[branch] if (SSRTValid != 0u)
				{
					if (SSRTCapOn != 0u && ViewZ(depth) >= SSRTCapEnd)
						InterlockedAdd(gCount[7], 1u);
					else if (roughness > SSRTMaxRoughness)
						InterlockedAdd(gCount[8], 1u);
					else
					{
						const float hit = SSRTHitDistance[dtid.xy];
						InterlockedAdd(gCount[hit <= 0.0 ? 11 : (hit < 65000.0 ? 9 : 10)], 1u);
					}
				}
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi < kCounters && gCount[gi] != 0u)
		Counters.InterlockedAdd(gi * 4u, gCount[gi]);
}
