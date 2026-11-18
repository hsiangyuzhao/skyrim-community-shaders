#ifndef __RT_FOG_VISIBILITY_HLSLI__
#define __RT_FOG_VISIBILITY_HLSLI__

// (49b) Ray-traced visibility in the volumetric fog's near froxel grid, temporally filtered (VolumetricFogRTVisibilityCS), read by
// the fog's light scattering pass (VolumetricFogLightScatteringCS) and by lit effects (Effect.hlsl, "Particles Use RT Shadows").
// Not included by Lighting (keeps the Lighting shaders untouched).
//
// t47 RTFogData[0]: which parts apply this frame (kFog* for the fog, kParticle* for effects). Unbound = all zero = off.
// t48 RTFogSun (near grid): x = sun visibility (1 lit), y = how much to trust it (0 = no ray-traced data: keep the shadow map alone).
// t49 RTFogLightVis / t52 RTFogLightId (half resolution in x / y, full z): per slot k the visibility of the point-light source whose
//     slot + 1 is RTFogLightId[k] (0 = empty). A light whose id is not among the four keeps its old, unshadowed contribution.
// t53 RTFogLightIds[i] (Light Limit Fix, per light of its clustered buffer): the source id (slot + 1) of light i: its Ray Tracing shadow tag's
//     slot, or for a window beam (an untagged spot shadow light) its beam source; 0 = none. Unbound = 0.

namespace RTFogVis
{
	static const uint kFogSun = 1u;
	static const uint kFogLights = 2u;
	static const uint kParticleSun = 4u;
	static const uint kParticleLights = 8u;

	struct Data
	{
		uint Flags;
		uint3 Pad;
	};

	StructuredBuffer<Data> RTFogData : register(t47);
	Texture3D<float2> RTFogSun : register(t48);
	Texture3D<float4> RTFogLightVis : register(t49);
	Texture3D<uint4> RTFogLightId : register(t52);
	StructuredBuffer<uint> RTFogLightIds : register(t53);

	/// This frame's kFog* / kParticle* bits (0 when unbound).
	uint Flags()
	{
		return RTFogData[0].Flags;
	}

	/// The sun factor to multiply into the shadow map's: 1 where there is no ray-traced data.
	float SunFactor(float2 visTrust)
	{
		return lerp(1.0, saturate(visTrust.x), saturate(visTrust.y));
	}

	/// The source id (slot + 1) of Light Limit Fix light `a_lightIndex` (its index in the clustered light buffer), 0 = none.
	uint LightId(uint a_lightIndex)
	{
		return RTFogLightIds[a_lightIndex] & 0xFFu;
	}

	/// A froxel's four slots packed into two words (8 bits each): ids, visibility x 255 (fewer registers across the light loops).
	uint2 PackSlots(uint4 a_ids, float4 a_vis)
	{
		const uint4 v = uint4(saturate(a_vis) * 255.0 + 0.5);
		return uint2((a_ids.x & 0xFFu) | ((a_ids.y & 0xFFu) << 8) | ((a_ids.z & 0xFFu) << 16) | ((a_ids.w & 0xFFu) << 24),
			v.x | (v.y << 8) | (v.z << 16) | (v.w << 24));
	}

	/// The visibility of source `a_id` among packed slots; a_found = it is one of them.
	float LightVisibility(uint2 a_slots, uint a_id, out bool a_found)
	{
		float v = 1.0;
		a_found = false;
		[unroll] for (uint k = 0u; k < 4u; ++k)
		{
			if (a_id != 0u && ((a_slots.x >> (8u * k)) & 0xFFu) == a_id) {
				v = float((a_slots.y >> (8u * k)) & 0xFFu) / 255.0;
				a_found = true;
			}
		}
		return v;
	}
}

#endif
