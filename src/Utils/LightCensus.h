#pragma once

#include <cstdint>
#include <d3d11.h>
#include <string>
#include <utility>
#include <vector>

namespace RE
{
	class BSLight;
}

/**
 * @brief (45o) Light Census: a one-shot snapshot of every light the renderer used this frame.
 *
 * Diagnostic only, default off, nothing changes what the game or the GPU draws.
 *
 * One capture = one JSON in Documents/My Games/Skyrim Special Edition/SKSE/CommunityShaders/Perf/
 * light-census-<time>-<location>.json with:
 *  - every light Light Limit Fix gathered from ShadowSceneNode::activeLights / activeShadowLights
 *    (the list the renderer really uses: placed lights, Light Placer mesh lights, player and
 *    actor lights, magic and effect lights), plus the ones it skipped and why;
 *  - per light: kind, radius / colour / fade (raw and as LLF uploads them), ISL data, flags,
 *    position and distance, provenance (node name chain, owning reference and base form with
 *    plugins, the LIGH base), and a category: player / actor / fixture / fill / window /
 *    effect / standalone, with the reasons that decided it, and a stable ID for matching captures;
 *  - scene context (cell, weather, hour, interior lighting, sun light, relevant CS settings);
 *  - distribution from LLF's cluster grid (read back from the GPU once): per cluster, per screen
 *    tile and per pixel (sampled through the depth buffer) light counts, and each light's share
 *    of the screen;
 *  - cost: LLF GPU time, engine shadow-map times and counts when they are being measured.
 *
 * Capture: render thread, inside Light Limit Fix's prepass. The CPU walk and the GPU copies
 * happen in the frame of the request; the read-back is polled on the following frames (a
 * blocking map after a few frames), then the file is written on a worker thread.
 *
 * While an F7 frame trace records, the census also hands the trace a few cheap per-frame
 * numbers (light totals, shadow lights, a fill-light estimate, player lights, the max lights in
 * one cluster from a small ring of grid read-backs). Nothing of this runs otherwise.
 */
namespace Util::LightCensus
{
	/// Why Light Limit Fix left a light out of its buffer.
	enum class DropReason : std::uint8_t
	{
		None = 0,
		NoNiLight,
		Hidden,
		InactiveShadow,  ///< shadow light without a shadow-map slot this frame (index 255)
		FadedOut,        ///< colour * fade or radius ~0: almost always lodDimmer 0 (faded by distance / room); (46a) was ZeroIntensity
		OverLimit,       ///< beyond LLF's 1024-light buffer
	};

	/// Queues a capture for the next frame (any thread).
	void RequestCapture();
	/// True from the request until the file has been written.
	bool Busy();
	/// Last file written or last error, for the menu.
	std::string StatusLine();

	/// Render thread, per frame: Light Limit Fix should hand over the light pointers this frame.
	bool WantsSources();

	/// Render thread: the lights LLF gathered (a_used[i] produced a_data[i]) and the ones it skipped.
	/// a_data points at LightLimitFix::LightData; a_uploaded = how many of them went to the GPU.
	void OnLightsGathered(const std::vector<RE::BSLight*>& a_used, const std::vector<std::pair<RE::BSLight*, DropReason>>& a_dropped,
		const void* a_data, std::size_t a_dataStride, std::uint32_t a_uploaded);

	struct ClusterInputs
	{
		ID3D11Buffer* grid = nullptr;       ///< LightGrid per cluster (offset, count, pad, pad)
		ID3D11Buffer* indexList = nullptr;  ///< light indices
		ID3D11Buffer* counter = nullptr;    ///< used entries of indexList
		std::uint32_t clusterSize[3]{};
		float renderWidth = 0.0f;
		float renderHeight = 0.0f;
		std::uint32_t lightCount = 0;
	};
	/// Render thread, after LLF's culling dispatch: GPU copies for a capture or the trace ring, and polling.
	void OnClustersCulled(const ClusterInputs& a_inputs);

	/// The "Diagnostics" node contents (Light Limit Fix page). a_key / a_capturingKey: the hotkey setting.
	void DrawSettings(std::uint32_t& a_key, bool& a_capturingKey);

	// ---- (46b) shared per-frame classifier ------------------------------------------------------------

	/// The census categories (the same decision as a capture's "category" field).
	enum class Category : std::uint8_t
	{
		Unknown = 0,
		Player,
		Actor,
		Effect,
		WindowBeam,     ///< Window Shadows Ultimate's shadowed sun spot through a window
		WindowGlow,     ///< the glow of a lit pane seen from outside
		WindowFill,     ///< the light a window spreads indoors
		FixtureShadow,  ///< placed shadow-casting light without a mesh: a fixture's shadow stand-in
		Fill,
		Fixture,
		Standalone,
		MeshAttached,
		Count
	};
	const char* CategoryName(Category a_category);

	/**
	 * (46b) The census classifier for every frame (RT point-light shadows): one census-style read of the
	 * light's engine data (owner, LIGH, node chain, glowing neighbours; SEH-protected) the first time a light
	 * is asked for, cached per light (validated by its NiLight and owner on every lookup). Window lights are
	 * re-split by the current radius on each call. The nearby-fixture scan of a capture is not run: a placed
	 * light without a mesh that is not named like a fill light comes out as Standalone (a capture may call it
	 * Fixture); both are lit fixtures for the shadow selection.
	 *
	 * Render thread only. a_budget: new lights that may be read this call (decremented); a light that is not
	 * cached and finds no budget returns false (asked again next frame).
	 */
	bool ClassifyCached(RE::BSLight* a_light, float a_radius, std::uint32_t a_frame, std::uint32_t& a_budget, Category& o_category);
	/// Drops cache entries not asked for since a_maxAge frames (call once per frame; cheap when nothing is old).
	void TrimClassifierCache(std::uint32_t a_frame, std::uint32_t a_maxAge);
	/// Lights in the cache, and lights read so far (for the page).
	std::uint32_t ClassifierCacheSize();
	std::uint64_t ClassifierReads();
}
