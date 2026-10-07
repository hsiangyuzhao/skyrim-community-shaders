#ifndef __SHARED_DATA_DEPENDENCY_HLSL__
#define __SHARED_DATA_DEPENDENCY_HLSL__

#include "Common/FrameBuffer.hlsli"
#include "Common/VR.hlsli"

namespace SharedData
{

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
	cbuffer SharedData : register(b5)
	{
		float4 WaterData[25];
		row_major float3x4 DirectionalAmbient;
		float4 DirLightDirection;
		float4 DirLightColor;
		float4 CameraData;
		float4 BufferDim;
		float Timer;
		uint FrameCount;
		uint FrameCountAlwaysActive;
		bool InInterior;  // If the area lacks a directional shadow light e.g. the sun or moon
		bool InMapMenu;   // If the world/local map is open (note that the renderer is still deferred here)
		bool HideSky;     // HideSky flag in WorldSpace, e.g. Blackreach
		float MipBias;    // Offset to mip level for TAA sharpness#
	};

	struct GrassLightingSettings
	{
		float Glossiness;
		float SpecularStrength;
		float SubsurfaceScatteringAmount;
		bool OverrideComplexGrassSettings;

		float BasicGrassBrightness;
		// Only read by the GRASS_OPTIMIZATIONS permutation, for grass drawn with an LOD mesh.
		float MidLODBrightness;
		float FarLODBrightness;
		float pad0;
	};

	struct CPMSettings
	{
		bool EnableComplexMaterial;
		bool EnableParallax;
		bool EnableTerrainParallax;
		bool EnableHeightBlending;
		bool EnableShadows;
		bool ExtendShadows;
		bool EnableParallaxWarpingFix;
		float1 pad0;
	};

	struct CubemapCreatorSettings
	{
		uint Enabled;
		float3 pad0;

		float4 CubemapColor;
	};

	struct TerraOccSettings
	{
		bool EnableTerrainShadow;
		float3 Scale;
		float2 ZRange;
		float2 Offset;
	};

	struct LightLimitFixSettings
	{
		uint EnableLightsVisualisation;
		uint LightsVisualisationMode;
		float2 pad0;
		uint4 ClusterSize;
	};

	struct WetnessEffectsSettings
	{
		row_major float4x4 OcclusionViewProj;

		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;

		bool EnableWetnessEffects;
		float MaxRainWetness;
		float MaxPuddleWetness;
		float MaxShoreWetness;

		uint ShoreRange;
		float PuddleRadius;
		float PuddleMaxAngle;
		float PuddleMinWetness;

		float MinRainWetness;
		float SkinWetness;
		float WeatherTransitionSpeed;
		bool EnableRaindropFx;

		bool EnableSplashes;
		bool EnableRipples;
        uint EnableVanillaRipples;
        float RaindropFxRange;

		float RaindropGridSizeRcp;
		float RaindropIntervalRcp;
		float RaindropChance;
		float SplashesLifetime;

		float SplashesStrength;
		float SplashesMinRadius;
		float SplashesMaxRadius;
		float RippleStrength;

		float RippleRadius;
		float RippleBreadth;
		float RippleLifetimeRcp;
		float pad0;
	};

	struct SkylightingSettings
	{
		row_major float4x4 OcclusionViewProj;
		float4 OcclusionDir;

		float4 PosOffset;   // xyz: cell origin in camera model space
		uint4 ArrayOrigin;  // xyz: array origin
		int4 ValidMargin;

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint2 pad0;
	};

	struct CloudShadowsSettings
	{
		float Opacity;
		float3 pad0;
	};

	struct LODBlendingSettings
	{
		float LODTerrainBrightness;
		float LODObjectBrightness;
		float LODObjectSnowBrightness;
		bool DisableTerrainVertexColors;
		float LODTerrainGamma;
		float LODObjectGamma;
		float LODObjectSnowGamma;
		float LODGrassGamma;
	};

	struct HairSpecularSettings
	{
		uint Enabled;
		float HairGlossiness;
		float SpecularMult;
		float DiffuseMult;
		uint EnableTangentShift;
		float PrimaryTangentShift;
		float SecondaryTangentShift;
		float HairSaturation;
		float SpecularIndirectMult;
		float DiffuseIndirectMult;
		float BaseColorMult;
		float Transmission;
		uint EnableSelfShadow;
		float SelfShadowStrength;
		float SelfShadowExponent;
		float SelfShadowScale;
		uint HairMode;  // 0: Kajiya-Kay, 1: Marschner
		uint3 pad;
	};

	struct TerrainVariationSettings
	{
		uint enableTilingFix;
		uint enableLODTerrainTilingFix;
		float2 pad0;
	};

	struct IBLSettings
	{
		uint EnableDiffuseIBL;
		uint PreserveFogLuminance;
		uint UseStaticIBL;
		uint EnableInterior;
		float DiffuseIBLScale;
		float DALCAmount;
		float IBLSaturation;
		float FogAmount;
		uint EffectNormalization;
		float EffectNormalizationMult;
		float MinEffectMult;
		// (B7) Mirrors IBL.h's Settings tail exactly. Per-source trim for the environment and
		// sky halves of the ambient probe; all four default to 1.0 and are exact no-ops there.
		float EnvIBLScale;
		float SkyIBLScale;
		float EnvIBLSaturation;
		float SkyIBLSaturation;
		float pad;
	};

	struct ExtendedTranslucencySettings
	{
		uint MaterialModel;  // [0,1,2,3] The MaterialModel
		float Reduction;     // [0, 1.0] The factor to reduce the transparency to matain the average transparency [0,1]
		float Softness;      // [0, 2.0] The soft remap upper limit [0,2]
		float Strength;      // [0, 1.0] The inverse blend weight of the effect
	};

	struct LinearLightingSettings
	{
		uint enableLinearLighting;
		uint enableGammaCorrection;
		uint isDirLightLinear;
		float dirLightMult;
		float lightGamma;
		float colorGamma;
		float emitColorGamma;
		float glowmapGamma;
		float ambientGamma;
		float fogGamma;
		float fogAlphaGamma;
		float effectGamma;
		float effectAlphaGamma;
		float skyGamma;
		float waterGamma;
		float vlGamma;
		float vanillaDiffuseMult;
		float vanillaSpecularMult;
		float grassDiffuseMult;
		float grassSpecularMult;
		float vanillaDiffuseColorMult;
		float lightMult;
		float directionalLightMult;
		float pointLightMult;
		float emitColorMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
	};

	struct PostProcessingSettings
	{
		uint DisableVanillaTonemapping;
		uint3 pad0;
	};

	struct SkinData
	{
		float4 skinParams;
		float4 skinParams2;
		float4 skinDetailParams;
		float4 sssParams;
		float4 fuzzParams;
		float4 physicalParams;
		float4 wetParams;
	};

	struct SSPLSSettings
	{
		uint Enable;
		float Strength;
		uint StepLimit;
		float RayLength;
		float CompareToleranceScale;
		float MaxDistance;
		uint EnableSoftShadows;
		float SoftShadowScale;
	};

	struct VanillaFresnelSettings
	{
		uint Enable;
		uint EnableGGX;
		uint EnableGGXOnGrass;
		uint EnableDynamicCubemapsConversion;
		uint EnableEyeSpecialHandling;
		float RoughnessMultiplier;
		float SpecularRoughnessBlend;
		float BaseF0Multiplier;
		float MinF0;
		float CubemapToF0Multiplier;
		float ComplexMaterialF0Multiplier;
		// (batch 10b) Mirrors VanillaFresnel::Settings. Sixteen 4-byte slots = 64 bytes; the
		// pads are spelled out because FeatureData is a naked concatenation of every
		// feature's struct (src/FeatureBuffer.cpp), so this side has to state the same size
		// the C++ side gets from alignas(16). Growing this struct by 16 bytes shifts
		// physSkyData, ssrtSettings, exponentialHeightFogSettings and ssgiSettings by 16 --
		// consistently on both sides, which is what makes it safe.
		//
		// (batch 13) EyeDirectRoughness was appended into what used to be pad0, so this stayed
		// at 64 bytes and nothing after it moved. EyeRoughness now drives only the environment
		// reflection and the G-buffer glossiness; EyeDirectRoughness drives the direct-light
		// GGX lobe (see the split at Lighting.hlsl's directSpecularRoughness).
		//
		// (batch 15) EyeDirectSpecularMode is the 15th slot and there was no pad left to take,
		// so this struct is now 20 slots = 80 bytes, and physSkyData, ssrtSettings,
		// exponentialHeightFogSettings and ssgiSettings all sit 16 bytes later than before.
		// The C++ side grew by exactly the same 16 (VanillaFresnel::Settings' static_assert).
		//
		// (batch 17) EyeSoftLightingScale and ShadowSoftLighting took pad3 and pad4, so the
		// struct is still 20 slots = 80 bytes and nothing after it moved.
		float EyeRoughness;
		float EnvMaskStrength;
		float EyeDirectRoughness;
		uint EyeDirectSpecularMode;
		float EyeSoftLightingScale;
		uint ShadowSoftLighting;
		float pad0;
		float pad1;
		float pad2;
	};

	// (batch 15) Values for VanillaFresnelSettings::EyeDirectSpecularMode. Mirrors
	// VanillaFresnel::EyeDirectSpecular in src/Features/VanillaFresnel.h.
	//
	// Only the *direct* highlight on eye materials is affected. The environment reflection
	// path (EyeRoughness, DynamicCubemaps::GetDynamicCubemap, psout.NormalGlossiness) is
	// untouched by all three -- batch 13 separated the two and they stay separated.
	static const uint EyeDirectSpecularModeVanillaPhong = 0;  // vanilla phong lobe + both gates
	static const uint EyeDirectSpecularModeGGXGated = 1;      // GGX lobe + both gates restored
	static const uint EyeDirectSpecularModeGGXRaw = 2;        // GGX lobe, no gates (batch 9-14)

	struct PhysSkyData
	{
		
		// DYNAMIC
		float2 texDim;
		float2 rcpTexDim;  //
		float2 frameDim;
		float2 rcpFrameDim;  //

		float zCameraPlanet;
		float3 sunDir;  //
		float3 sunlightColor;
		float trMix;  //
		float3 masserDir;
		float apLumMix;  //
		float3 masserColor;
		float apTrMix;  //
		float3 secundaDir;
		float sunDiskCos;  //
		float3 secundaColor;

		// GENERAL
		uint enabled;  //
		int tonemapper;
		float vanillaMix;

		// WORLD
		float zBottom;
		float rPlanet;  //
		float rAtmosphere;
		float3 groundAlbedo;  //

		// ATMOSPHERE
		float2 cloudShadowRemapRange;

		float aerosolFalloff;
		float aerosolPhaseG; //
		float3 aerosolScatter;
		float _pad5;  //
		float3 aerosolAbsorption;
		
		float rayleighFalloff;
		float3 rayleighScatter;  //

		float ozoneAltitude;  //
		float ozoneThickness;
		float3 ozoneAbsorption;  //
		
		// CLOUDS (VANILLA)
		float cloudRelightMix;
		float cloudOriginalMix;
		float silverLiningMix;
		float silverLiningSpread;  //
	};

	// Mirrors ScreenSpaceRayTracing::SharedData. This struct sits inside FeatureData with
	// ExponentialHeightFogSettings behind it, so its size is load bearing: two whole float4
	// rows on both sides, or everything after it shifts. Row 1 is full - its last two slots hold
	// the SSGI contact pair below - so the row still needs no explicit padding member.
	struct SSRTSettings
    {
        uint EnableSpecular;
        float SpecularMult;
        float DiffuseMult;
		float AmbientMult;
		// --- row 1 ---
		/// Non-zero switches the diffuse energy model from "SSRT replaces the forward ambient
		/// wholesale" to "SSRT displaces it in proportion to hit confidence".
		uint AmbientReinjection;
		float AmbientReinjectionStrength;
		/// (contact AO) Screen Space GI's contact-occlusion pass, described from SSRT's point of
		/// view. These two are the *only* thing the ray march needs to know about that pass -- its
		/// output reaches the march through the SSGI AO texture like any other occlusion -- but the
		/// near-field vote suppression in ssrt_raymarch.hlsl has to know whether a deterministic
		/// term is standing in for the votes it drops, and over what range.
		///
		/// They live in this block rather than in one of SSGI's own because SSGI publishes no
		/// FeatureData block at all, and adding one to carry two scalars would move every offset
		/// behind it. Filled by ScreenSpaceRayTracing::GetCommonBufferData from
		/// ScreenSpaceGI::settings; they occupy the two slots the retired reinjection contact pair
		/// used, so the buffer layout is unchanged.
		///
		/// Non-zero iff SSGI is loaded, enabled, and its contact pass is on.
		uint SsgiContactAoActive;
		/// Contact search radius in centimetres, i.e. the range the suppression hands over. Same
		/// value the kernel measures itself in, so the handover cannot drift out of alignment with
		/// the kernel's own falloff.
		float SsgiContactRadius;
    };

	struct ExponentialHeightFogSettings
	{
		uint enabled;
		uint useDynamicCubemaps;
		float startDistance;
		float fogHeight;
		float fogHeightFalloff;
		float fogDensity;
		float directionalInscatteringMultiplier;
		float directionalInscatteringExponent;
		float4 inscatteringTint;
		float cubemapMipLevel;
		// (batch 38, A1, upstream #2831) second stacked layer; occupies the former float3 pad,
		// so nothing moves. Density 0 (default) = no second layer = 37c fog.
		float fogHeight2;
		float fogHeightFalloff2;
		float fogDensity2;
	};

	// (batch 37b) Mirrors VolumetricLighting::CommonBufferData. Appended after every older
	// block so nothing before it moves (FeatureBuffer.cpp pins the offset at 1440).
	struct VolumetricLightingSettings
	{
		float WorldShadowPower;  // 0 = no cloud/terrain occlusion of light shafts (37a)
		uint LinearizeColor;     // linearise the shaft colour when the dir light is not linear
		float2 pad0;
	};

	// (batch 37b) Mirrors PhysicalSky::ExtCbData, after VolumetricLightingSettings (offset 1456).
	// Every flag is a 37b switch ANDed with the master; Flags == 0 runs the 37a code paths.
	struct PhysSkyExtData
	{
		uint Flags;
		float SunRadianceCap;
		float SunGlowIntensity;  // (batch 37c) glow around the procedural sun, x a sunlit white wall; 0 = off
		float SunGlowWidth;      // (batch 37c) glow falloff width, radians
	};

	// (batch 38, A1) Mirrors ExponentialHeightFog::VolumetricFogPSData: what a pixel shader needs to
	// look the froxel volumes up. Appended after PhysSkyExtData (offset 1472). Enabled == 0
	// whenever the volumes were not built this frame (fog off, master off, interior map ...).
	struct VolumetricFogSettings
	{
		uint Enabled;               // near volume valid this frame
		uint FarEnabled;            // far volume valid this frame
		float StartDistance;        // no volumetric fog closer than this (view depth)
		float EndDistance;          // view depth where the far volume ends
		float4 NearGridZParams;     // log2(depth * x + y) * z / w = normalized slice (w = slices)
		float4 FarGridZParams;      // same for the far volume
		float NearGridEndDistance;  // view depth where the near volume hands over to the far one
		float UpsampleJitter;       // screen-space jitter of the lookup, in froxels
		float2 pad0;
	};

	// (batch 38, A2) Mirrors VolumetricShadows::CommonBufferData, after VolumetricFogSettings
	// (offset 1536). Non-zero only while the VSM was built this frame.
	struct VolumetricShadowsSettings
	{
		uint ParticleShadows;
		uint ForwardSoftShadows;
		uint2 pad0;
	};

	namespace PhysSkyExtFlags
	{
		static const uint SunReplace = (1 << 0);           // disk replaces the vanilla sun quad
		static const uint SunSoftEdge = (1 << 1);          // upstream fec65ed15
		static const uint SunPhysicalRadiance = (1 << 2);  // upstream 728eedd61 + d08484aef
		static const uint HideSunGlare = (1 << 3);         // clear the vanilla glare quad
		static const uint SkyAlphaOpaque = (1 << 4);       // upstream 5846ad833
		static const uint TrLutEdgeFix = (1 << 5);         // upstream 9fbd052ad (+ clamp sampler)
		static const uint ApShadowDepthFix = (1 << 6);     // depth read of upstream 224312a11
		static const uint ReflectionSkyFix = (1 << 7);     // upstream 23156dc5f
		static const uint MultiScatterFix = (1 << 8);      // LutGen part of upstream c14664115
	}

	cbuffer FeatureData : register(b6)
	{
		GrassLightingSettings grassLightingSettings;
		CPMSettings extendedMaterialSettings;
		CubemapCreatorSettings cubemapCreatorSettings;
		TerraOccSettings terraOccSettings;
		LightLimitFixSettings lightLimitFixSettings;
		WetnessEffectsSettings wetnessEffectsSettings;
		SkylightingSettings skylightingSettings;
		CloudShadowsSettings cloudShadowsSettings;
		LODBlendingSettings lodBlendingSettings;
		HairSpecularSettings hairSpecularSettings;
		TerrainVariationSettings terrainVariationSettings;
		IBLSettings iblSettings;
		ExtendedTranslucencySettings extendedTranslucencySettings;
		LinearLightingSettings linearLightingSettings;
		PostProcessingSettings postProcessingSettings;
		SkinData skinData;
		SSPLSSettings ssplsSettings;
		VanillaFresnelSettings vanillaFresnelSettings;
		PhysSkyData physSkyData;
		SSRTSettings ssrtSettings;
		ExponentialHeightFogSettings exponentialHeightFogSettings;
		VolumetricLightingSettings volumetricLightingSettings;
		PhysSkyExtData physSkyExtData;
		VolumetricFogSettings volumetricFogSettings;
		VolumetricShadowsSettings volumetricShadowsSettings;
	};

	Texture2D<float4> DepthTexture : register(t17);

	// Get a int3 to be used as texture sample coord. [0,1] in uv space
	int3 ConvertUVToSampleCoord(float2 uv, uint a_eyeIndex)
	{
		uv = Stereo::ConvertToStereoUV(uv, a_eyeIndex);
		uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
		return int3(uv * BufferDim.xy, 0);
	}

	// Get a raw depth from the depth buffer. [0,1] in uv space
	float GetDepth(float2 uv, uint a_eyeIndex = 0)
	{
		return DepthTexture.Load(ConvertUVToSampleCoord(uv, a_eyeIndex)).x;
	}

	float GetScreenDepth(float depth)
	{
		return (CameraData.w / (-depth * CameraData.z + CameraData.x));
	}

	float4 GetScreenDepths(float4 depths)
	{
		return (CameraData.w / (-depths * CameraData.z + CameraData.x));
	}

	float GetScreenDepth(float2 uv, uint a_eyeIndex = 0)
	{
		float depth = GetDepth(uv, a_eyeIndex);
		return GetScreenDepth(depth);
	}

	float4 GetWaterData(float3 worldPosition)
	{
		float2 cellF = (((worldPosition.xy + FrameBuffer::CameraPosAdjust[0].xy)) / 4096.0) + 64.0;  // always positive
		int2 cellInt;
		float2 cellFrac = modf(cellF, cellInt);

		cellF = worldPosition.xy / float2(4096.0, 4096.0);  // remap to cell scale
		cellF += 2.5;                                       // 5x5 cell grid
		cellF -= cellFrac;                                  // align to cell borders
		cellInt = round(cellF);

		uint waterTile = (uint)clamp(cellInt.x + (cellInt.y * 5), 0, 24);  // remap xy to 0-24

		float4 waterData = float4(1.0, 1.0, 1.0, -2147483648);

		[flatten] if (cellInt.x < 5 && cellInt.x >= 0 && cellInt.y < 5 && cellInt.y >= 0)
			waterData = WaterData[waterTile];
		return waterData;
	}

#endif  // PSHADER
}
#endif  // __SHARED_DATA_DEPENDENCY_HLSL__