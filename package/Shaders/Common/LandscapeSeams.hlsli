#ifndef __LANDSCAPE_SEAMS_HLSLI__
#define __LANDSCAPE_SEAMS_HLSLI__

// (49d) Landscape Seams: land texture weights blended across quad borders, plus up to two textures borrowed from the
// neighbouring quads there. Port of official Community Shaders #2841 (from InTheBottle's Bottled-Shaders): Load() and the
// data layout as upstream; the borrowed-layer blending is rewritten for this fork's unrolled landscape code in Lighting.hlsl
// (same sampling, Terrain Variation, PBR / vanilla conversion and RMAOS rules as the engine's six layers there).
// Borrowed layers blend colour, normal and material only; parallax height stays on the engine's six layers (as upstream).
// Bound by LandscapeSeamsFix::Bind (C++) at t104-t111; only MTLand / MTLandLODBlend pixel shaders define LANDSCAPE_SEAMS.
namespace LandscapeSeams
{
	// Mirrors LandscapeSeamsFix::QuadData (96 bytes, structured buffer).
	struct QuadData
	{
		float2 Origin;
		uint Flags;
		uint ExtraCount;
		float2 IsSnow;     // borrowed layer is snow (Dynamic Snow; vanilla snow mask)
		float2 SpecPower;  // vanilla land only: the layer's specular power
		float4 PBRParams[2];
		float4 GlintParams[2];
	};

	static const uint FlagPbr = 1u << 0;
	static const uint FlagGlint = 1u << 8;
	static const uint FlagValid = 1u << 31;

	static const float QuadSize = 2048.0;
	static const float GridCells = 16.0;

	Texture2DArray<float4> Weights : register(t104);
	StructuredBuffer<QuadData> Quad : register(t105);

	Texture2D<float4> Color0 : register(t106);
	Texture2D<float4> Normal0 : register(t107);
	Texture2D<float4> Rmaos0 : register(t108);

	Texture2D<float4> Color1 : register(t109);
	Texture2D<float4> Normal1 : register(t110);
	Texture2D<float4> Rmaos1 : register(t111);

	static QuadData Data;
	static float2 ExtraWeights = 0;

	bool HasFlag(uint flag, uint layer)
	{
		return (Data.Flags & (flag << layer)) != 0;
	}

	float4 FetchSlice(int2 cell, float2 blend, int slice)
	{
		float4 w00 = Weights.Load(int4(cell, slice, 0));
		float4 w10 = Weights.Load(int4(cell + int2(1, 0), slice, 0));
		float4 w01 = Weights.Load(int4(cell + int2(0, 1), slice, 0));
		float4 w11 = Weights.Load(int4(cell + int2(1, 1), slice, 0));
		return lerp(lerp(w00, w10, blend.x), lerp(w01, w11, blend.x), blend.y);
	}

	// Replaces the engine's six vertex weights with the quad's healed weight map and reads the two borrowed weights.
	// Without resources (Quad reads 0, no FlagValid) the engine weights stay and the borrowed weights are 0.
	void Load(float2 worldXY, inout float4 weights1, inout float4 weights2)
	{
		Data = Quad[0];
		ExtraWeights = 0;
		[branch] if ((Data.Flags & FlagValid) != 0)
		{
			float2 grid = saturate((worldXY - Data.Origin) / QuadSize) * GridCells;
			float2 cellF = min(floor(grid), GridCells - 1.0);
			float2 blend = grid - cellF;
			int2 cell = int2(cellF);

			float4 slice0 = FetchSlice(cell, blend, 0);
			float4 slice1 = FetchSlice(cell, blend, 1);

			weights1 = slice0;
			weights2.xy = slice1.xy;
			ExtraWeights = slice1.zw;
		}
	}
}

// Sampling of a borrowed texture, as the engine layers in Lighting.hlsl sample theirs (Terrain Variation's stochastic
// sampling when its tiling fix is on). Expects uv and, with TERRAIN_VARIATION, useTerrainVariation / sharedOffset / dx / dy.
#if defined(TERRAIN_VARIATION)
#	define LANDSCAPE_SEAMS_SAMPLE(RESULT, TEX)                                         \
		[branch] if (useTerrainVariation)                                              \
		{                                                                              \
			RESULT = StochasticEffect(TEX, SampColorSampler, uv, sharedOffset, dx, dy); \
		}                                                                              \
		else                                                                           \
		{                                                                              \
			RESULT = TEX.SampleBias(SampColorSampler, uv, SharedData::MipBias);         \
		}
#else
#	define LANDSCAPE_SEAMS_SAMPLE(RESULT, TEX) RESULT = TEX.SampleBias(SampColorSampler, uv, SharedData::MipBias);
#endif

// Per-permutation material part of a borrowed layer (seamColor / seamColorRGB are the sampled colour):
//  - True PBR: a vanilla (non-PBR) texture is converted to linear as the engine layers' non-PBR tiles; RMAOS and glint from
//    the texture set's PBR data, else the engine layers' non-PBR RMAOS expression (kept identical so both sides of a seam match);
//  - vanilla with SNOW: the layer's snow mask, as the engine layers accumulate theirs.
#if defined(TRUE_PBR)
#	define LANDSCAPE_SEAMS_MATERIAL(INDEX, RMAOS_TEX, WEIGHT)                                                                     \
		[branch] if (!LandscapeSeams::HasFlag(LandscapeSeams::FlagPbr, INDEX))                                                  \
		{                                                                                                                       \
			seamColorRGB = Color::GammaToTrueLinear(seamColorRGB / Color::PBRLightingScale);                                    \
		}                                                                                                                       \
		float4 seamRMAOS;                                                                                                       \
		[branch] if (LandscapeSeams::HasFlag(LandscapeSeams::FlagPbr, INDEX))                                                   \
		{                                                                                                                       \
			LANDSCAPE_SEAMS_SAMPLE(seamRMAOS, RMAOS_TEX)                                                                        \
			seamRMAOS *= float4(LandscapeSeams::Data.PBRParams[INDEX].x, 1, 1, LandscapeSeams::Data.PBRParams[INDEX].z);        \
			[branch] if (LandscapeSeams::HasFlag(LandscapeSeams::FlagGlint, INDEX))                                             \
			{                                                                                                                   \
				glintParameters += (WEIGHT) * LandscapeSeams::Data.GlintParams[INDEX];                                          \
			}                                                                                                                   \
		}                                                                                                                       \
		else                                                                                                                    \
		{                                                                                                                       \
			seamRMAOS = (WEIGHT) * float4(1 - glossiness.x, 0, 1, 0);                                                           \
		}                                                                                                                       \
		blendedRMAOS += seamRMAOS * (WEIGHT);
#elif defined(SNOW)
#	define LANDSCAPE_SEAMS_MATERIAL(INDEX, RMAOS_TEX, WEIGHT) \
		landSnowMask += LandscapeSeams::Data.IsSnow[INDEX] * (WEIGHT) * GetLandSnowMaskValue(seamColor.w);
#else
#	define LANDSCAPE_SEAMS_MATERIAL(INDEX, RMAOS_TEX, WEIGHT)
#endif

// One borrowed layer, accumulated like an engine layer (blendedRGB / blendedAlpha / blendedNormalRGB / blendedNormalAlpha).
#define LANDSCAPE_SEAMS_BLEND_EXTRA(INDEX, COLOR_TEX, NORMAL_TEX, RMAOS_TEX, WEIGHT_VALUE) \
	[branch] if ((WEIGHT_VALUE) > 0.01)                                                  \
	{                                                                                    \
		float seamWeight = WEIGHT_VALUE;                                                 \
		float4 seamColor;                                                                \
		LANDSCAPE_SEAMS_SAMPLE(seamColor, COLOR_TEX)                                     \
		float3 seamColorRGB = seamColor.rgb;                                             \
		float4 seamNormal;                                                               \
		LANDSCAPE_SEAMS_SAMPLE(seamNormal, NORMAL_TEX)                                   \
		LANDSCAPE_SEAMS_MATERIAL(INDEX, RMAOS_TEX, seamWeight)                           \
		blendedRGB += seamColorRGB * seamWeight;                                         \
		blendedAlpha += seamColor.a * seamWeight;                                        \
		blendedNormalRGB += seamNormal.rgb * seamWeight;                                 \
		blendedNormalAlpha += seamNormal.a * seamWeight;                                 \
	}

#define LANDSCAPE_SEAMS_BLEND_EXTRAS                                                                                          \
	LANDSCAPE_SEAMS_BLEND_EXTRA(0, LandscapeSeams::Color0, LandscapeSeams::Normal0, LandscapeSeams::Rmaos0, LandscapeSeams::ExtraWeights.x) \
	LANDSCAPE_SEAMS_BLEND_EXTRA(1, LandscapeSeams::Color1, LandscapeSeams::Normal1, LandscapeSeams::Rmaos1, LandscapeSeams::ExtraWeights.y)

#endif  // __LANDSCAPE_SEAMS_HLSLI__
