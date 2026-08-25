#pragma once

struct TerrainHelper : Feature
{
private:
	static constexpr std::string_view MOD_ID = "143149";

public:
	virtual inline std::string GetName() override { return "Terrain Helper"; }
	virtual inline std::string GetShortName() override { return "TerrainHelper"; }
	virtual inline std::string_view GetShaderDefineName() override { return "TERRAIN_HELPER"; }
	virtual std::string_view GetCategory() const override { return "Landscape & Textures"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Provides enhanced terrain material support for terrain mods that require additional texture slots and parallax mapping capabilities.",
			{ "Extended texture slot support for terrain materials",
				"Parallax mapping integration for terrain textures",
				"Automatic terrain material detection and setup",
				"Support for advanced terrain modifications",
				"Compatibility layer for terrain enhancement mods" }
		};
	}

	struct Settings
	{
	} settings;

	struct ExtendedSlots
	{
		std::array<RE::NiSourceTexturePtr, 6> parallax;

		/// @brief FormID of the worldspace that was current when this entry was created.
		///
		/// (batch 16, item 4) Half of the eviction predicate. 0 means "created with no
		/// worldspace known", which is never evicted.
		std::uint32_t worldspace = 0;

		/// @brief steady_clock tick of the last draw that read this entry.
		///
		/// The other half of the predicate. Atomic because the read path
		/// (BSLightingShader_SetupMaterial) holds only a shared_lock, while the prune holds the
		/// unique one.
		std::atomic<std::int64_t> lastUsed{ 0 };
	};

	std::shared_mutex extendedSlotsMutex;
	std::unordered_map<uint32_t, ExtendedSlots> extendedSlots;

	/// @brief Worldspace this map is currently considered to describe. 0 until the first land.
	std::uint32_t trackedWorldspace = 0;

	/// @brief steady_clock tick at which trackedWorldspace last changed, or 0 if no prune is
	/// pending.
	std::int64_t worldspaceChangedAt = 0;

	/// @brief Evict entries left behind by a worldspace change.
	///
	/// (batch 16, item 4) extendedSlots pins six RE::NiSourceTexturePtr per distinct LAND
	/// material -- the displacement maps for the six blend tiles -- and nothing ever removed an
	/// entry. Every terrain material you walk past stays pinned in VRAM for the session, and the
	/// game unloading the cell does not help, because ours is a strong reference.
	///
	/// Eviction here is *not* free the way it is for Skin::skinExtraTextures. The read path,
	/// BSLightingShader_SetupMaterial, treats a miss as "this material has no extended slots"
	/// and returns; it has no way to rebuild the entry, because rebuilding needs the six
	/// BGSTextureSets that only TESObjectLAND_SetupMaterial sees. So dropping an entry whose
	/// land is still loaded means that terrain silently loses its displacement maps until the
	/// cell is reloaded. Not a crash -- but a visual regression with no recovery inside the
	/// session, which is why this is deliberately conservative rather than an LRU.
	///
	/// The predicate is therefore two conditions, and an entry has to fail both to be dropped:
	///
	///   1. It was created under a different worldspace than the one now loading land. Exterior
	///      worldspaces do not coexist, so its cells cannot still be loaded.
	///   2. It has not been drawn at all since that worldspace change, and the change was at
	///      least `pruneGrace` ago. This is the backstop for (1) being wrong -- a worldspace
	///      that inherits land records from a parent, say. Anything actually being rendered
	///      stamps lastUsed every frame and can never satisfy this.
	///
	/// What this deliberately does not do is bound growth *within* one worldspace. Walking
	/// across Tamriel for an hour still accumulates, because there is no signal available here
	/// that distinguishes "cell unloaded" from "cell loaded but behind the camera", and getting
	/// that wrong costs the user visible terrain detail.
	void PruneStaleExtendedSlots();
	RE::BGSTextureSet* defaultLandTexture;
	bool enabled = false;

	virtual void DataLoaded() override;
	virtual bool SupportsVR() override { return true; };
	virtual std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }

	void SetShaderResouces(ID3D11DeviceContext* a_context);
	bool TESObjectLAND_SetupMaterial(RE::TESObjectLAND* land);
	void BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material);
};