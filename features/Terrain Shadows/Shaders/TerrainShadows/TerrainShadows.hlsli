#ifndef TERRAIN_SHADOW_REGISTER
#	define TERRAIN_SHADOW_REGISTER t60
#endif

namespace TerrainShadows
{
	Texture2D<float2> ShadowHeightTexture : register(TERRAIN_SHADOW_REGISTER);

	float2 GetTerrainShadowUV(float2 xy)
	{
		return xy * SharedData::terraOccSettings.Scale.xy + SharedData::terraOccSettings.Offset.xy;
	}

	// (batch 40) The bias that lowers shadow heights to hide self-shadowing from the coarse
	// heightmap comes from the CPU: 1024 (the old constant) with Stable Soft Edges off, 256
	// (upstream #2729) with it on.
	float GetTerrainZ(float norm_z)
	{
		return lerp(SharedData::terraOccSettings.ZRange.x, SharedData::terraOccSettings.ZRange.y, norm_z) - SharedData::terraOccSettings.SelfShadowBias;
	}

	float2 GetTerrainZ(float2 norm_z)
	{
		return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
	}

	float GetTerrainShadow(const float3 worldPos, SamplerState samp)
	{
		// (batch 38) single exit: the early return made fxc warn X4000 at every call site.
		float shadowFraction = 1.0;
		if (SharedData::terraOccSettings.EnableTerrainShadow) {
			float2 terraOccUV = GetTerrainShadowUV(worldPos.xy);
			[branch] if (SharedData::terraOccSettings.StablePenumbrae)
			{
				// (batch 40) upstream #2729 (df687ca41): outside the heightmap is lit rather than
				// the clamped edge texel, and the transition is widened in z by about one heightmap
				// step of light descent (ZBlur, capped by the bias so lit flat ground stays lit),
				// which hides the heightmap's texel grid in the penumbra.
				if (all(terraOccUV >= 0.0) && all(terraOccUV <= 1.0)) {
					float2 shadowHeight = GetTerrainZ(ShadowHeightTexture.SampleLevel(samp, terraOccUV, 0));
					float zBlur = min(SharedData::terraOccSettings.ZBlur, SharedData::terraOccSettings.SelfShadowBias);
					float lowerHeight = shadowHeight.y - zBlur;
					float transitionHeight = shadowHeight.x + zBlur - lowerHeight;
					shadowFraction = transitionHeight > 0.0 ?
					                     saturate((worldPos.z - lowerHeight) / transitionHeight) :
					                     (worldPos.z >= shadowHeight.x ? 1.0 : 0.0);
				}
			}
			else
			{
				float2 shadowHeight = GetTerrainZ(ShadowHeightTexture.SampleLevel(samp, terraOccUV, 0));
				shadowFraction = saturate((worldPos.z - shadowHeight.y) / (shadowHeight.x - shadowHeight.y));
			}
		}

		return shadowFraction;
	}
}
