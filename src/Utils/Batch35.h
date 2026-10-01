#pragma once

#include <array>
#include <cstdint>

/**
 * @brief Batch 35 (Lighting path optimisations): one master switch plus one switch per item.
 *
 * Everything here is a runtime A/B switch, flipped from Advanced -> Batch 35 without a
 * restart or a cache clear:
 *
 * - The GPU items are bits in SharedData::Batch35Flags (SharedDataCB, b5). The Lighting
 *   shader reads them through Batch35::IsOn() in Common/SharedData.hlsli and branches on
 *   them uniformly, so both the old and the new code path are compiled into every
 *   permutation and the switch only chooses between them.
 * - The CPU items are plain C++ branches at their call sites.
 *
 * With the master switch off every item takes its pre-batch-35 path, whatever its own
 * switch says.
 */
namespace Batch35
{
	/// Bits of SharedData::Batch35Flags. Must match the Batch35 namespace in Common/SharedData.hlsli.
	enum GpuFlag : uint32_t
	{
		EarlyAlphaTest = 1u << 0,          // B1
		WetnessSpecularBranch = 1u << 1,   // B5a
		ExclusiveSpecularLobe = 1u << 2,   // B5b
		SkipOccludedSunShadows = 1u << 3,  // B2
		ReuseTerrainHeight = 1u << 4,      // B3a
		HoistTerrainVariationPow = 1u << 5  // B3b
	};

	enum class CpuItem : uint32_t
	{
		LinearLightingUploadDedup = 0,  // C1
		SkylightingStaticBSXName,       // C2
		LightLimitFixRoomCache,         // C3
		SkinBindOncePerFrame,           // C4
		SharedDataUploadDedup,          // C7
		Count
	};

	struct ItemInfo
	{
		const char* key;      // settings JSON key, stable
		const char* label;    // menu label
		const char* tooltip;  // menu tooltip
	};

	inline constexpr std::array<std::pair<GpuFlag, ItemInfo>, 6> kGpuItems{ {
		{ EarlyAlphaTest, { "B1 Early Alpha Test", "B1: early alpha test",
							  "Alpha-tested materials (leaves, foliage cards, hair, fences) run their alpha test\n"
							  "right after the base texture is read instead of at the very end, so a cut-out\n"
							  "pixel no longer pays for shadows, sky lighting, IBL and every point light first.\n"
							  "The picture must be identical either way. Watch: Geometry (opaque) in a forest." } },
		{ WetnessSpecularBranch, { "B5a Wetness Branch", "B5a: skip dry wetness specular",
									 "The rain/wetness highlight of each light is now skipped on dry pixels instead\n"
									 "of being computed and thrown away. Identical picture. Watch: Geometry (opaque)\n"
									 "indoors with many lights, once dry and once in the rain." } },
		{ ExclusiveSpecularLobe, { "B5b Exclusive Lobe", "B5b: compute only the specular lobe in use",
									 "With Vanilla Fresnel's GGX lobe on, the old Phong highlight of every light was\n"
									 "still computed and then overwritten. Now only the lobe in use is computed.\n"
									 "Identical picture. Watch: Geometry (opaque) indoors with many lights." } },
		{ SkipOccludedSunShadows, { "B2 Skip Occluded Sun", "B2: skip sun shadow work where the sun is already blocked",
									  "Where the sun's contribution is already exactly zero (inside the engine's shadow),\n"
									  "the terrain/cloud shadow lookups and the parallax self-shadow for the sun are\n"
									  "skipped: they would be multiplied by zero anyway. Identical picture. Watch:\n"
									  "Geometry (opaque) outdoors on a sunny day, looking at a shaded area." } },
		{ ReuseTerrainHeight, { "B3a Terrain Height Reuse", "B3a: reuse the terrain parallax height",
								  "Terrain with Terrain Variation and parallax shadows sampled the same height twice;\n"
								  "the second lookup now reuses the first. Identical picture. Watch: Geometry\n"
								  "(opaque) over open terrain." } },
		{ HoistTerrainVariationPow, { "B3b Terrain Variation Pow", "B3b: Terrain Variation blend weights once",
										"Terrain Variation recomputed the same blend weights for every texture of every\n"
										"layer; they are now computed once per pixel. Identical picture. Watch: Geometry\n"
										"(opaque) over open terrain." } },
	} };

	inline constexpr std::array<ItemInfo, static_cast<size_t>(CpuItem::Count)> kCpuItems{ {
		{ "C1 LinearLighting Upload", "C1: LinearLighting skips unchanged per-draw uploads",
			"Linear Lighting uploaded one float to the GPU before every Lighting draw. It is now\n"
			"only uploaded when the value changes. CPU only; identical picture." },
		{ "C2 Skylighting BSX", "C2: Skylighting reuses the BSX lookup name",
			"The Skylighting occlusion pass built a new engine string for every object it\n"
			"checked. CPU only; identical picture." },
		{ "C3 LLF Room Cache", "C3: Light Limit Fix caches room lookups per frame",
			"Indoors, every draw walked up the scene graph to find its room. The result is now\n"
			"remembered for the rest of the frame. CPU only; identical picture." },
		{ "C4 Skin Bind", "C4: Skin binds its buffer once per frame",
			"Skin re-bound the same constant buffer before every draw. It is now bound once per\n"
			"frame. CPU only; identical picture." },
		{ "C7 Shared Data Upload", "C7: shared data skips unchanged uploads",
			"The shared constant buffers were re-uploaded 3-4 times per frame. A call whose\n"
			"contents are byte-identical to what the GPU already has now skips the upload. CPU\n"
			"only; identical picture." },
	} };

	struct Settings
	{
		bool master = true;
		std::array<bool, kGpuItems.size()> gpu{ true, true, true, true, true, true };
		std::array<bool, static_cast<size_t>(CpuItem::Count)> cpu{ true, true, true, true, true };
	};

	inline Settings settings{};

	/// Bumped by Deferred's Renderer_ResetState hook. A binding that is made once and then
	/// trusted for the rest of the frame (C4) is redone after an engine state reset as well.
	inline uint32_t rendererResetGeneration = 0;

	/// @brief The value uploaded as SharedData::Batch35Flags this frame.
	inline uint32_t GetGpuFlags()
	{
		if (!settings.master)
			return 0;
		uint32_t flags = 0;
		for (size_t i = 0; i < kGpuItems.size(); ++i) {
			if (settings.gpu[i])
				flags |= kGpuItems[i].first;
		}
		return flags;
	}

	/// @brief Whether a CPU item takes its batch 35 path right now.
	inline bool IsOn(CpuItem a_item)
	{
		return settings.master && settings.cpu[static_cast<size_t>(a_item)];
	}

	inline void Load(const json& a_json)
	{
		if (!a_json.is_object())
			return;
		if (a_json.contains("Master") && a_json["Master"].is_boolean())
			settings.master = a_json["Master"].get<bool>();
		for (size_t i = 0; i < kGpuItems.size(); ++i) {
			const char* key = kGpuItems[i].second.key;
			if (a_json.contains(key) && a_json[key].is_boolean())
				settings.gpu[i] = a_json[key].get<bool>();
		}
		for (size_t i = 0; i < kCpuItems.size(); ++i) {
			const char* key = kCpuItems[i].key;
			if (a_json.contains(key) && a_json[key].is_boolean())
				settings.cpu[i] = a_json[key].get<bool>();
		}
	}

	inline json Save()
	{
		json o;
		o["Master"] = settings.master;
		for (size_t i = 0; i < kGpuItems.size(); ++i)
			o[kGpuItems[i].second.key] = settings.gpu[i];
		for (size_t i = 0; i < kCpuItems.size(); ++i)
			o[kCpuItems[i].key] = settings.cpu[i];
		return o;
	}
}
