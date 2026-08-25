#include "TerrainHelper.h"

#include "ShaderCache.h"
#include "State.h"

void TerrainHelper::DataLoaded()
{
	// Get the default landscape texture set for terrain helper
	const auto defaultLandTextureSet = RE::TESForm::LookupByEditorID<RE::BGSTextureSet>("LandscapeDefault");
	if (defaultLandTextureSet != nullptr) {
		logger::info("[Terrain Helper] LandscapeDefault EDID texture set found");
		defaultLandTexture = defaultLandTextureSet;
		// only enable if TerrainHelper.esp is loaded
		enabled = true;
	} else {
		logger::warn("[Terrain Helper] LandscapeDefault EDID texture set from TerrainHelper.esp not found. Terrain helper is disabled.");
		enabled = false;
	}
}

// (batch 16, item 4) See the doc comment on the declaration for the full argument. Runs from
// TESObjectLAND_SetupMaterial rather than from a per-frame hook: that is the only place the map
// grows, and it is called continuously as land streams in, so a worldspace change is always
// followed by more calls here within a second or two.
void TerrainHelper::PruneStaleExtendedSlots()
{
	// How long after a worldspace change we wait before evicting, so that anything still being
	// rendered has had many frames to stamp lastUsed.
	static constexpr auto pruneGrace = std::chrono::seconds(10);

	auto tes = RE::TES::GetSingleton();
	auto worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	if (!worldspace)
		return;  // interior, or no worldspace yet: never a safe moment to evict anything

	const std::uint32_t current = worldspace->formID;
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();

	{
		const std::unique_lock lock(extendedSlotsMutex);

		if (current != trackedWorldspace) {
			// Arm, do not fire. The entries for the *new* worldspace mostly do not exist yet,
			// and the ones for the old one need a grace period in which to prove they are not
			// still being drawn.
			trackedWorldspace = current;
			worldspaceChangedAt = now;
			return;
		}

		if (worldspaceChangedAt == 0)
			return;  // nothing pending

		if (now - worldspaceChangedAt < std::chrono::duration_cast<std::chrono::steady_clock::duration>(pruneGrace).count())
			return;  // still inside the grace period

		const size_t before = extendedSlots.size();
		for (auto it = extendedSlots.begin(); it != extendedSlots.end();) {
			const bool foreign = it->second.worldspace != 0 && it->second.worldspace != trackedWorldspace;
			const bool unusedSinceChange = it->second.lastUsed.load(std::memory_order_relaxed) < worldspaceChangedAt;
			it = (foreign && unusedSinceChange) ? extendedSlots.erase(it) : std::next(it);
		}
		worldspaceChangedAt = 0;

		if (before != extendedSlots.size())
			logger::info("[Terrain Helper] Released {} terrain material entries left behind by a worldspace change, {} remain", before - extendedSlots.size(), extendedSlots.size());
	}
}

bool TerrainHelper::TESObjectLAND_SetupMaterial(RE::TESObjectLAND* land)
{
	if (!enabled) {
		// terrain helper is not enabled
		return false;
	}

	PruneStaleExtendedSlots();

	auto tes = RE::TES::GetSingleton();
	auto currentWorldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	const std::uint32_t currentWorldspaceID = currentWorldspace ? currentWorldspace->formID : 0u;

	if (land == nullptr || land->loadedData == nullptr || land->loadedData->mesh[0] == nullptr) {
		// this is not terrain or vanilla material failed
		return false;
	}

	for (uint32_t quadI = 0; quadI < 4; ++quadI) {
		// Get hash key of vanilla material
		uint32_t hashKey = 0;

		if (land->loadedData->mesh[quadI] == nullptr) {
			// continue if cannot find mesh
			continue;
		}

		const auto& children = land->loadedData->mesh[quadI]->GetChildren();
		auto geometry = children.empty() ? nullptr : static_cast<RE::BSGeometry*>(children[0].get());
		if (geometry != nullptr) {
			const auto shaderProp = static_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().properties[1].get());
			if (shaderProp != nullptr) {
				hashKey = shaderProp->GetBaseMaterial()->hashKey;
			}
		}

		if (hashKey == 0) {
			// continue if cannot find hash key
			continue;
		}

		// Create array of texture sets (6 tiles)
		std::array<RE::BGSTextureSet*, 6> textureSets;
		auto defTexture = land->loadedData->defQuadTextures[quadI];
		if (defTexture != nullptr && defTexture->formID != 0) {
			textureSets[0] = Util::GetSeasonalSwap(defTexture->textureSet);
		} else {
			// this is a default texture
			textureSets[0] = Util::GetSeasonalSwap(defaultLandTexture);
		}
		for (uint32_t textureI = 0; textureI < 5; ++textureI) {
			auto curTexture = land->loadedData->quadTextures[quadI][textureI];
			if (curTexture == nullptr) {
				textureSets[textureI + 1] = nullptr;
				continue;
			}

			if (curTexture->formID == 0) {
				// this is a default texture
				textureSets[textureI + 1] = Util::GetSeasonalSwap(defaultLandTexture);
			} else {
				textureSets[textureI + 1] = Util::GetSeasonalSwap(land->loadedData->quadTextures[quadI][textureI]->textureSet);
			}
		}

		// Assign textures to material
		{
			const std::unique_lock lock(extendedSlotsMutex);
			auto& slot = extendedSlots.try_emplace(hashKey).first->second;
			// Tag with the worldspace this land belongs to; PruneStaleExtendedSlots uses it.
			// Re-stamped rather than set-once so that an entry re-registered under a new
			// worldspace is not left carrying the old tag.
			slot.worldspace = currentWorldspaceID;
			slot.lastUsed.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);

			for (uint32_t textureI = 0; textureI < 6; ++textureI) {
				if (textureSets[textureI] == nullptr) {
					continue;
				}

				auto txSet = textureSets[textureI];
				if (txSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(3)) != nullptr) {
					txSet->SetTexture(static_cast<RE::BSTextureSet::Texture>(3), slot.parallax[textureI]);
				}
			}
		}
	}

	return true;
}

struct THExtendedRendererState
{
	static constexpr uint32_t NumPSTextures = 6;
	static constexpr uint32_t FirstPSTexture = 92;

	uint32_t PSResourceModifiedBits = 0;
	std::array<ID3D11ShaderResourceView*, NumPSTextures> PSTexture;

	void SetPSTexture(size_t textureIndex, RE::BSGraphics::Texture* newTexture)
	{
		ID3D11ShaderResourceView* resourceView = newTexture ? newTexture->resourceView : nullptr;

		PSTexture[textureIndex] = resourceView;
		PSResourceModifiedBits |= (1 << textureIndex);
	}

	THExtendedRendererState()
	{
		std::fill(PSTexture.begin(), PSTexture.end(), nullptr);
	}
} thExtendedRendererState;

void TerrainHelper::SetShaderResouces(ID3D11DeviceContext* a_context)
{
	uint32_t mask = thExtendedRendererState.PSResourceModifiedBits;

	if (mask == 0) [[likely]] {
		return;  // Nothing to update
	}

	constexpr uint32_t firstTexture = THExtendedRendererState::FirstPSTexture;
	auto& textures = thExtendedRendererState.PSTexture;

	while (mask) {
		// Find the position of the first set bit
		uint32_t batchStart = std::countr_zero(mask);

		// Count consecutive 1s starting from batchStart
		uint32_t shiftedMask = mask >> batchStart;
		uint32_t batchCount = std::countr_one(shiftedMask);

		a_context->PSSetShaderResources(
			firstTexture + batchStart,
			batchCount,
			&textures[batchStart]);

		// Clear the processed bits
		uint32_t clearMask = ((1u << batchCount) - 1u) << batchStart;
		mask &= ~clearMask;
	}

	thExtendedRendererState.PSResourceModifiedBits = 0;
}

void TerrainHelper::BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material)
{
	if (!enabled) {
		// terrain helper is not enabled
		return;
	}

	if (material == nullptr) {
		return;
	}

	// (batch 16, item 4) Only the parallax array is copied out, not the whole entry -- the entry
	// now also carries an atomic recency stamp, and copying it under a shared_lock would be both
	// wrong and pointless. The copy itself stays: it is what lets the lock be released before the
	// rest of this function runs, and it is what makes the pointers safe against a concurrent
	// prune.
	std::array<RE::NiSourceTexturePtr, 6> materialParallax;
	{
		const std::shared_lock lock(extendedSlotsMutex);

		auto it = extendedSlots.find(material->hashKey);
		if (it == extendedSlots.end()) {
			// hash does not exists
			return;
		}
		// Recency stamp. Relaxed is enough: the prune only compares it against a timestamp it
		// took before the grace period, so a stale-by-microseconds read cannot flip the result.
		it->second.lastUsed.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
		materialParallax = it->second.parallax;
	}

	const auto state = globals::state;
	const auto& stateData = globals::game::graphicsState->GetRuntimeData();

	state->permutationData.ExtraFeatureDescriptor &= ~uint(State::ExtraFeatureDescriptors::THLandHasDisplacement);

	// Populate extended slots
	// Bits 0-5 track individual texture displacement; THLandHasDisplacement (bit 9) tracks if any texture has displacement
	for (uint32_t textureI = 0; textureI < 6; ++textureI) {
		if (materialParallax[textureI] != nullptr && materialParallax[textureI] != stateData.defaultTextureNormalMap) {
			thExtendedRendererState.SetPSTexture(textureI, materialParallax[textureI]->rendererTexture);
			state->permutationData.ExtraFeatureDescriptor |= 1 << textureI;
			state->permutationData.ExtraFeatureDescriptor |= uint(State::ExtraFeatureDescriptors::THLandHasDisplacement);
		} else {
			thExtendedRendererState.SetPSTexture(textureI, nullptr);
			state->permutationData.ExtraFeatureDescriptor &= ~(1 << textureI);
		}
	}
}