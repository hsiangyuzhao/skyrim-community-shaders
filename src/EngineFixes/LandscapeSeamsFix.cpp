#include "LandscapeSeamsFix.h"

#include <algorithm>
#include <chrono>
#include <imgui.h>

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "TruePBR/BSLightingShaderMaterialPBRLandscape.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

namespace
{
	constexpr uint32_t SeamsFlag = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::LandscapeSeams);
	// Bits 3-5 of a lighting technique: TruePbr, Deferred, LandscapeSeams (the engine's light count outside the descriptors).
	constexpr uint32_t CommunityShadersBits = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr) |
	                                          static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred) | SeamsFlag;

	constexpr uint32_t FlagPbr = 1u << 0;
	constexpr uint32_t FlagGlint = 1u << 8;
	constexpr uint32_t FlagValid = 1u << 31;

	constexpr int GridMax = static_cast<int>(LandscapeSeamsFix::GridSize) - 1;
	constexpr float QuadSize = 2048.0f;
	constexpr float MinDelta = 6.0f / 255.0f;
	constexpr float MinLayerWeight = 4.0f / 255.0f;
	constexpr size_t MaxFieldTextures = 56;

	RE::TESLandTexture* GetDefaultLandTexture()
	{
		static const auto address = REL::Relocation<RE::TESLandTexture**>(RELOCATION_ID(514783, 400936));
		return *address;
	}

	/// (49g) CreateResources failed: logged once (that piece draws as normal terrain; the menu readout says so for the piece under the player).
	std::shared_ptr<LandscapeSeamsFix::Resources> ResourceFailure(const char* a_what, HRESULT a_hr)
	{
		static std::atomic<bool> logged{ false };
		if (!logged.exchange(true))
			logger::warn("[Landscape Seams] Could not create a land piece's blend resources ({} failed, HRESULT {:08X}): such pieces draw as normal terrain (logged once)",
				a_what, static_cast<uint32_t>(a_hr));
		return nullptr;
	}

	/// (49g) A land texture's name for the menu: its editor ID, else its texture set's, else the diffuse file name, else the form ID.
	std::string LandTextureName(const RE::TESLandTexture* a_texture)
	{
		if (a_texture == nullptr)
			return "-";
		if (const char* id = a_texture->GetFormEditorID(); id != nullptr && *id != '\0')
			return id;
		if (const auto* set = a_texture->textureSet) {
			if (const char* id = set->GetFormEditorID(); id != nullptr && *id != '\0')
				return id;
			if (const char* path = set->textures[0].textureName.c_str(); path != nullptr && *path != '\0') {
				std::string_view name(path);
				if (const auto slash = name.find_last_of("\\/"); slash != std::string_view::npos)
					name.remove_prefix(slash + 1);
				if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos)
					name = name.substr(0, dot);
				if (!name.empty())
					return std::string(name);
			}
		}
		return std::format("LTEX {:08X}", a_texture->GetFormID());
	}

	const char* DropReasonText(LandscapeSeamsFix::DropReason a_reason)
	{
		switch (a_reason) {
		case LandscapeSeamsFix::kDropNotLoaded:
			return "neighbour not loaded";
		case LandscapeSeamsFix::kDropPbrOnVanilla:
			return "PBR texture beside vanilla land";
		default:
			return "no room: more than 2 foreign textures";
		}
	}

	const char* TechniqueDropText(LandscapeSeamsFix::TechniqueDrop a_reason)
	{
		using enum LandscapeSeamsFix::TechniqueDrop;
		switch (a_reason) {
		case kCacheOff:
			return "the shader cache is off";
		case kLightingOff:
			return "Lighting shaders are not replaced";
		case kSwitchOff:
			return "Blend Terrain Seams is off";
		case kVertexShaderMissing:
			return "its vertex shader is not ready (still compiling, or it failed to compile)";
		case kPixelShaderMissing:
			return "its pixel shader is not ready (still compiling, or it failed to compile)";
		default:
			return "-";
		}
	}

	bool IsLandscapeTechnique(uint32_t a_descriptor)
	{
		const auto type = static_cast<SIE::ShaderCache::LightingShaderTechniques>((a_descriptor >> 24) & 0x3F);
		return type == SIE::ShaderCache::LightingShaderTechniques::MTLand || type == SIE::ShaderCache::LightingShaderTechniques::MTLandLODBlend;
	}

	constexpr int PerimeterIndex(int a_x, int a_y)
	{
		if (a_y == 0)
			return a_x;
		if (a_y == GridMax)
			return 17 + a_x;
		if (a_x == 0)
			return 33 + a_y;
		return 48 + a_y;
	}

	constexpr int VertexIndex(int a_x, int a_y)
	{
		return a_y * static_cast<int>(LandscapeSeamsFix::GridSize) + a_x;
	}

	RE::BSGeometry* GetQuadGeometry(RE::TESObjectLAND* a_land, uint32_t a_quad)
	{
		auto* node = a_land->loadedData->mesh[a_quad];
		if (node == nullptr)
			return nullptr;
		const auto& children = node->GetChildren();
		return children.empty() ? nullptr : static_cast<RE::BSGeometry*>(children[0].get());
	}

	void ReadQuad(RE::TESObjectLAND* a_land, uint32_t a_quad, LandscapeSeamsFix::Quad& a_out)
	{
		const auto& data = *a_land->loadedData;

		auto* base = data.defQuadTextures[a_quad];
		a_out.slots[0] = base != nullptr ? base : GetDefaultLandTexture();
		for (uint32_t layer = 0; layer + 1 < LandscapeSeamsFix::EngineLayers; ++layer) {
			a_out.slots[layer + 1] = data.quadTextures[a_quad][layer];
		}

		a_out.grid = std::make_unique<LandscapeSeamsFix::WeightGrid>();
		auto& grid = *a_out.grid;
		for (uint32_t vertex = 0; vertex < LandscapeSeamsFix::GridVertices; ++vertex) {
			int total = 0;
			for (uint32_t layer = 0; layer < LandscapeSeamsFix::EngineLayers; ++layer) {
				total += static_cast<uint8_t>(data.percents[a_quad][vertex][layer]);
			}
			grid[0][vertex] = static_cast<uint8_t>(std::clamp(255 - total, 0, 255));
			for (uint32_t layer = 0; layer + 1 < LandscapeSeamsFix::EngineLayers; ++layer) {
				grid[layer + 1][vertex] = a_out.slots[layer + 1] != nullptr ? static_cast<uint8_t>(data.percents[a_quad][vertex][layer]) : uint8_t{ 0 };
			}
		}

		for (int y = 0; y <= GridMax; ++y) {
			for (int x = 0; x <= GridMax; ++x) {
				if (x != 0 && x != GridMax && y != 0 && y != GridMax)
					continue;
				for (uint32_t layer = 0; layer < LandscapeSeamsFix::EngineLayers; ++layer) {
					a_out.border[layer][PerimeterIndex(x, y)] = grid[layer][VertexIndex(x, y)];
				}
			}
		}
	}

	/**
	 * @brief Copies what a neighbour may borrow from each slot: the texture pointers and PBR values the engine (vanilla land) or
	 * True PBR (PBR land) has just put into this quad's material, plus the land texture's snow / specular values. Pointer copies
	 * and field reads only: no texture lookup or load. Runs in the land setup hook, after both material setups.
	 *
	 * (49e) The material type is proven before any field is read: True PBR land (a_pbr) must carry exactly a
	 * BSLightingShaderMaterialPBRLandscape, vanilla land exactly the engine's BSLightingShaderMaterialLandscape (vtable compares).
	 * GetFeature() cannot tell them apart: the PBR material returns kMultiTexLandLODBlend too (49d read PBR land with the vanilla
	 * layout and crashed). Anything else: no slot information (the quad still blends, nothing is borrowed from it).
	 */
	void ReadSlotInfo(RE::BSGeometry* a_geometry, bool a_pbr, LandscapeSeamsFix::Quad& a_out)
	{
		auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect].get());
		const auto* material = property != nullptr ? property->material : nullptr;
		static const std::uintptr_t vanillaLandscapeVtable = RE::VTABLE_BSLightingShaderMaterialLandscape[0].address();
		const std::uintptr_t vtable = material != nullptr ? *reinterpret_cast<const std::uintptr_t*>(material) : 0;
		const bool pbrMaterial = a_pbr && BSLightingShaderMaterialPBRLandscape::IsInstance(material);
		const bool vanillaMaterial = !a_pbr && vtable != 0 && vtable == vanillaLandscapeVtable;
		if (material != nullptr && !pbrMaterial && !vanillaMaterial) {
			static std::atomic<bool> logged{ false };
			if (!logged.exchange(true))
				logger::warn("[Landscape Seams] Land material of an unexpected type ({}, vtable {:X}): its textures are not borrowed", a_pbr ? "True PBR land" : "vanilla land", vtable);
		}

		for (uint32_t slot = 0; slot < LandscapeSeamsFix::EngineLayers; ++slot) {
			auto& info = a_out.info[slot];
			info = {};
			auto* landTexture = a_out.slots[slot];
			if (landTexture == nullptr)
				continue;

			if (pbrMaterial) {
				const auto* pbr = static_cast<const BSLightingShaderMaterialPBRLandscape*>(material);
				info.color = pbr->landscapeBaseColorTextures[slot];
				info.normal = pbr->landscapeNormalTextures[slot];
				info.isPbr = pbr->isPbr[slot];
				if (info.isPbr) {
					info.rmaos = pbr->landscapeRMAOSTextures[slot];
					info.pbrParams = { pbr->roughnessScales[slot], pbr->displacementScales[slot], pbr->specularLevels[slot], 0.0f };
					const auto& glint = pbr->glintParameters[slot];
					info.glint = glint.enabled;
					info.glintParams = { glint.screenSpaceScale, 40.0f - glint.logMicrofacetDensity, glint.microfacetRoughness, glint.densityRandomization };
				}
			} else if (vanillaMaterial) {
				const auto* vanilla = static_cast<const RE::BSLightingShaderMaterialLandscape*>(material);
				info.color = slot == 0 ? vanilla->diffuseTexture : vanilla->landscapeDiffuseTexture[slot - 1];
				info.normal = slot == 0 ? vanilla->normalTexture : vanilla->landscapeNormalTexture[slot - 1];
			} else {
				continue;
			}

			info.isSnow = TruePBR::IsSnowLandTexture(*landTexture, Util::GetSeasonalSwap(landTexture->textureSet)) ? 1.0f : 0.0f;
			info.isSnowVanilla = landTexture->shaderTextureIndex != 0 ? 1.0f : 0.0f;
			info.specPower = static_cast<float>(static_cast<uint8_t>(landTexture->specularExponent));
			info.valid = info.color != nullptr;
		}
	}

	struct FieldTexture
	{
		RE::TESLandTexture* texture = nullptr;
		const LandscapeSeamsFix::SlotInfo* info = nullptr;  // first loaded quad slot showing this texture (null: cannot be borrowed)
		int slot = -1;
		std::array<float, LandscapeSeamsFix::PerimeterVertices> delta{};
	};

	struct FieldTextures
	{
		std::array<FieldTexture, MaxFieldTextures> entries;
		size_t count = 0;

		int FindOrAdd(RE::TESLandTexture* a_texture, const LandscapeSeamsFix::SlotInfo& a_info)
		{
			const auto* info = a_info.valid ? &a_info : nullptr;
			for (size_t i = 0; i < count; ++i) {
				if (entries[i].texture == a_texture) {
					if (entries[i].info == nullptr)
						entries[i].info = info;
					return static_cast<int>(i);
				}
			}
			if (count == MaxFieldTextures)
				return -1;
			entries[count].texture = a_texture;
			entries[count].info = info;
			return static_cast<int>(count++);
		}
	};
}

bool LandscapeSeamsFix::IsBlended(RE::BSGeometry* a_geometry)
{
	const std::shared_lock lock(mutex);
	const auto loadedIt = loadedQuads.find(a_geometry);
	if (loadedIt == loadedQuads.end())
		return false;
	const auto quadIt = quads.find(loadedIt->second);
	return quadIt != quads.end() && quadIt->second.resources;
}

bool LandscapeSeamsFix::ShouldTag(RE::BSGeometry* a_geometry)
{
	return a_geometry != nullptr && IsEnabled() && IsBlended(a_geometry);
}

void LandscapeSeamsFix::Bind(RE::BSGeometry* a_geometry)
{
	std::shared_ptr<Resources> resources;
	{
		const std::shared_lock lock(mutex);
		if (const auto loadedIt = loadedQuads.find(a_geometry); loadedIt != loadedQuads.end()) {
			if (const auto quadIt = quads.find(loadedIt->second); quadIt != quads.end())
				resources = quadIt->second.resources;
		}
	}

	// Without resources every view is null: QuadData reads as zero (no FlagValid), so the shader keeps the engine weights.
	std::array<ID3D11ShaderResourceView*, NumPSTextures> views{};
	if (resources) {
		views[0] = resources->weights.get();
		views[1] = resources->data.get();
		for (uint32_t extra = 0; extra < MaxExtraLayers; ++extra) {
			for (uint32_t texture = 0; texture < TexturesPerExtra; ++texture) {
				const auto& source = resources->extras[extra][texture];
				if (source != nullptr && source->rendererTexture != nullptr)
					views[2 + extra * TexturesPerExtra + texture] = source->rendererTexture->resourceView;
			}
		}
	}

	globals::d3d::context->PSSetShaderResources(FirstPSTexture, NumPSTextures, views.data());

	// (49g) Menu readout: the piece under the player was bound for a seams pass this frame.
	if (resources && a_geometry == diagGeometry.load(std::memory_order_relaxed))
		diagBoundFrame.store(globals::state->frameCount, std::memory_order_relaxed);
}

void LandscapeSeamsFix::NoteTechnique(TechniqueDrop a_reason)
{
	const uint32_t frame = globals::state->frameCount;
	if (a_reason == TechniqueDrop::kNone) {
		lastSeamsDrawFrame.store(frame, std::memory_order_relaxed);
		return;
	}
	lastSeamsDropFrame.store(frame, std::memory_order_relaxed);
	lastSeamsDropReason.store(static_cast<uint8_t>(a_reason), std::memory_order_relaxed);
	const uint32_t bit = 1u << static_cast<uint32_t>(a_reason);
	if ((loggedDropReasons.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
		if (a_reason == TechniqueDrop::kSwitchOff)
			logger::info("[Landscape Seams] A land piece drew as normal terrain instead of with the seams shader: {} (logged once per reason)", TechniqueDropText(a_reason));
		else
			logger::warn("[Landscape Seams] A land piece drew as normal terrain instead of with the seams shader: {} (logged once per reason)", TechniqueDropText(a_reason));
	}
}

void LandscapeSeamsFix::Heal(const QuadKey& a_key, const Quad& a_quad, Healed& a_out) const
{
	a_out.active = false;
	a_out.extraCount = 0;
	a_out.diag = {};
	a_out.diag.healed = true;

	const auto& grid = *a_quad.grid;

	FieldTextures textures;
	std::array<int, EngineLayers> slotTexture{};
	for (uint32_t slot = 0; slot < EngineLayers; ++slot) {
		slotTexture[slot] = -1;
		if (a_quad.slots[slot] == nullptr)
			continue;
		const int index = textures.FindOrAdd(a_quad.slots[slot], a_quad.info[slot]);
		slotTexture[slot] = index;
		if (index >= 0 && textures.entries[index].slot < 0)
			textures.entries[index].slot = static_cast<int>(slot);
	}

	float maxDelta = 0.0f;
	for (int y = 0; y <= GridMax; ++y) {
		for (int x = 0; x <= GridMax; ++x) {
			if (x != 0 && x != GridMax && y != 0 && y != GridMax)
				continue;

			const int perimeter = PerimeterIndex(x, y);
			const int vertex = VertexIndex(x, y);

			std::array<float, MaxFieldTextures> own{};
			std::array<float, MaxFieldTextures> sum{};

			float ownTotal = 0.0f;
			for (uint32_t slot = 0; slot < EngineLayers; ++slot) {
				if (slotTexture[slot] >= 0)
					ownTotal += grid[slot][vertex];
			}
			if (ownTotal <= 0.0f)
				continue;
			for (uint32_t slot = 0; slot < EngineLayers; ++slot) {
				if (slotTexture[slot] >= 0)
					own[slotTexture[slot]] += grid[slot][vertex] / ownTotal;
			}
			sum = own;

			int sharing = 1;
			const int dxMin = x == 0 ? -1 : 0;
			const int dxMax = x == GridMax ? 1 : 0;
			const int dyMin = y == 0 ? -1 : 0;
			const int dyMax = y == GridMax ? 1 : 0;
			for (int dy = dyMin; dy <= dyMax; ++dy) {
				for (int dx = dxMin; dx <= dxMax; ++dx) {
					if (dx == 0 && dy == 0)
						continue;

					const auto neighbourIt = quads.find(QuadKey{ a_key.worldSpace, a_key.x + dx, a_key.y + dy });
					if (neighbourIt == quads.end())
						continue;
					const auto& neighbour = neighbourIt->second;

					const int neighbourX = dx < 0 ? GridMax : (dx > 0 ? 0 : x);
					const int neighbourY = dy < 0 ? GridMax : (dy > 0 ? 0 : y);
					const int neighbourPerimeter = PerimeterIndex(neighbourX, neighbourY);

					float neighbourTotal = 0.0f;
					for (uint32_t slot = 0; slot < EngineLayers; ++slot) {
						if (neighbour.slots[slot] != nullptr)
							neighbourTotal += neighbour.border[slot][neighbourPerimeter];
					}
					if (neighbourTotal <= 0.0f)
						continue;

					bool complete = true;
					std::array<float, MaxFieldTextures> contribution{};
					for (uint32_t slot = 0; slot < EngineLayers; ++slot) {
						const auto weight = neighbour.border[slot][neighbourPerimeter];
						if (neighbour.slots[slot] == nullptr || weight == 0)
							continue;
						const int index = textures.FindOrAdd(neighbour.slots[slot], neighbour.info[slot]);
						if (index < 0) {
							complete = false;
							a_out.diag.fieldOverflow = true;
							break;
						}
						contribution[index] += weight / neighbourTotal;
					}
					if (!complete)
						continue;

					for (size_t i = 0; i < textures.count; ++i) {
						sum[i] += contribution[i];
					}
					++sharing;
				}
			}

			const float inverse = 1.0f / static_cast<float>(sharing);
			for (size_t i = 0; i < textures.count; ++i) {
				const float delta = sum[i] * inverse - own[i];
				textures.entries[i].delta[perimeter] = delta;
				maxDelta = std::max(maxDelta, std::abs(delta));
			}
		}
	}

	if (maxDelta < MinDelta)
		return;

	std::array<float, GridSize> falloff{};
	for (int distance = 0; distance <= GridMax; ++distance) {
		const float t = std::min(1.0f, static_cast<float>(distance) / static_cast<float>(BlendRadius));
		falloff[distance] = 1.0f - t * t * (3.0f - 2.0f * t);
	}

	std::vector<std::array<float, GridVertices>> fields(textures.count);
	std::array<float, MaxFieldTextures> fieldSum{};
	std::array<float, MaxFieldTextures> fieldMax{};

	for (size_t i = 0; i < textures.count; ++i) {
		const auto& delta = textures.entries[i].delta;
		const float corner00 = delta[PerimeterIndex(0, 0)];
		const float corner10 = delta[PerimeterIndex(GridMax, 0)];
		const float corner01 = delta[PerimeterIndex(0, GridMax)];
		const float corner11 = delta[PerimeterIndex(GridMax, GridMax)];

		auto& field = fields[i];
		for (int y = 0; y <= GridMax; ++y) {
			const float fy0 = falloff[y];
			const float fy1 = falloff[GridMax - y];
			const float west = delta[PerimeterIndex(0, y)];
			const float east = delta[PerimeterIndex(GridMax, y)];
			for (int x = 0; x <= GridMax; ++x) {
				const float fx0 = falloff[x];
				const float fx1 = falloff[GridMax - x];
				const int vertex = VertexIndex(x, y);

				float raw = 0.0f;
				float rawTotal = 0.0f;
				for (uint32_t slot = 0; slot < EngineLayers; ++slot) {
					if (slotTexture[slot] < 0)
						continue;
					rawTotal += grid[slot][vertex];
					if (slotTexture[slot] == static_cast<int>(i))
						raw += grid[slot][vertex];
				}
				raw = rawTotal > 0.0f ? raw / rawTotal : 0.0f;

				float value = fx0 * west + fx1 * east + fy0 * delta[PerimeterIndex(x, 0)] + fy1 * delta[PerimeterIndex(x, GridMax)];
				value -= fx0 * fy0 * corner00 + fx1 * fy0 * corner10 + fx0 * fy1 * corner01 + fx1 * fy1 * corner11;
				value = std::max(0.0f, raw + value);

				field[vertex] = value;
				fieldSum[i] += value;
				fieldMax[i] = std::max(fieldMax[i], value);
			}
		}
	}

	std::array<int, MaxFieldTextures> layerOf{};
	layerOf.fill(-1);

	std::array<int, MaxFieldTextures> candidates{};
	size_t candidateCount = 0;
	for (size_t i = 0; i < textures.count; ++i) {
		const auto& entry = textures.entries[i];
		if (entry.slot >= 0) {
			layerOf[i] = entry.slot;
		} else if (fieldMax[i] >= MinLayerWeight && entry.info != nullptr) {
			// The vanilla terrain shader would read a PBR base color as an sRGB diffuse.
			if (!a_out.pbr && entry.info->isPbr)
				continue;
			candidates[candidateCount++] = static_cast<int>(i);
		}
	}
	std::sort(candidates.begin(), candidates.begin() + candidateCount, [&](int a_left, int a_right) { return fieldSum[a_left] > fieldSum[a_right]; });

	for (size_t i = 0; i < candidateCount && a_out.extraCount < MaxExtraLayers; ++i) {
		layerOf[candidates[i]] = static_cast<int>(EngineLayers + a_out.extraCount);
		a_out.diag.borrowed[a_out.extraCount] = textures.entries[candidates[i]].texture;
		a_out.extras[a_out.extraCount++] = *textures.entries[candidates[i]].info;  // reference copies, under the shared lock
	}
	a_out.diag.borrowedCount = a_out.extraCount;

	// (49g) Foreign textures that reach the piece but got no layer (their half of the border stays a hard line), strongest first.
	{
		std::array<DroppedTexture, MaxFieldTextures> dropped{};
		uint32_t droppedCount = 0;
		for (size_t i = 0; i < textures.count; ++i) {
			const auto& entry = textures.entries[i];
			if (entry.slot >= 0 || layerOf[i] >= 0 || fieldMax[i] < MinLayerWeight)
				continue;
			auto& out = dropped[droppedCount++];
			out.texture = entry.texture;
			out.peak = std::min(fieldMax[i], 1.0f);
			out.reason = entry.info == nullptr ? kDropNotLoaded : ((!a_out.pbr && entry.info->isPbr) ? kDropPbrOnVanilla : kDropNoRoom);
		}
		std::sort(dropped.begin(), dropped.begin() + droppedCount, [](const DroppedTexture& a_left, const DroppedTexture& a_right) { return a_left.peak > a_right.peak; });
		a_out.diag.droppedTotal = droppedCount;
		a_out.diag.droppedShown = std::min<uint32_t>(droppedCount, static_cast<uint32_t>(MaxDroppedShown));
		std::copy_n(dropped.begin(), a_out.diag.droppedShown, a_out.diag.dropped.begin());
	}

	for (auto& layer : a_out.weights) {
		layer.fill(0);
	}

	for (uint32_t vertex = 0; vertex < GridVertices; ++vertex) {
		float total = 0.0f;
		for (size_t i = 0; i < textures.count; ++i) {
			if (layerOf[i] >= 0)
				total += fields[i][vertex];
		}
		if (total <= 0.0f) {
			a_out.weights[0][vertex] = 255;
			continue;
		}
		for (size_t i = 0; i < textures.count; ++i) {
			if (layerOf[i] >= 0)
				a_out.weights[layerOf[i]][vertex] = static_cast<uint8_t>(std::lround(fields[i][vertex] / total * 255.0f));
		}
	}

	a_out.active = true;
	a_out.diag.active = true;
}

std::shared_ptr<LandscapeSeamsFix::Resources> LandscapeSeamsFix::CreateResources(const Healed& a_healed) const
{
	auto resources = std::make_shared<Resources>();
	const auto& defaults = globals::game::graphicsState->GetRuntimeData();

	QuadData data{};
	data.Origin = { static_cast<float>(a_healed.key.x) * QuadSize, static_cast<float>(a_healed.key.y) * QuadSize };
	data.Flags = FlagValid;
	data.ExtraCount = a_healed.extraCount;

	for (uint32_t extra = 0; extra < MaxExtraLayers; ++extra) {
		data.PBRParams[extra] = { 1.0f, 1.0f, 0.04f, 0.0f };
	}

	static_assert(MaxExtraLayers == 2, "IsSnow and SpecPower hold one component per borrowed layer");
	for (uint32_t extra = 0; extra < a_healed.extraCount; ++extra) {
		// Copied from the source quad's material by the setup hook: no engine call here (worker thread).
		const auto& info = a_healed.extras[extra];
		auto& layer = resources->extras[extra];
		layer[0] = info.color;
		layer[1] = info.normal;

		float isSnow = 0.0f;
		if (a_healed.pbr) {
			// Same rule as the quad's own tiles (TruePBR.cpp, PBRFlags bits 24-29 read by Dynamic Snow).
			isSnow = info.isSnow;
			if (info.isPbr) {
				layer[2] = info.rmaos;
				data.Flags |= FlagPbr << extra;
				if (info.glint)
					data.Flags |= FlagGlint << extra;
				data.PBRParams[extra] = info.pbrParams;
				data.GlintParams[extra] = info.glintParams;
				if (layer[2] == nullptr)
					layer[2] = defaults.defaultTextureWhite;
			}
		} else {
			// The vanilla material's textureIsSnow / specular power per tile (LTEX INAM / SNAM).
			isSnow = info.isSnowVanilla;
			(extra == 0 ? data.SpecPower.x : data.SpecPower.y) = info.specPower;
		}
		(extra == 0 ? data.IsSnow.x : data.IsSnow.y) = isSnow;

		if (layer[0] == nullptr)
			layer[0] = defaults.defaultTextureBlack;
		if (layer[1] == nullptr)
			layer[1] = defaults.defaultTextureNormalMap;
	}

	constexpr uint32_t sliceCount = (MaxLayers + 3) / 4;
	constexpr uint32_t sliceBytes = GridVertices * 4;
	std::array<std::array<uint8_t, sliceBytes>, sliceCount> pixels{};
	for (uint32_t layer = 0; layer < MaxLayers; ++layer) {
		auto& slice = pixels[layer / 4];
		const uint32_t channel = layer % 4;
		for (uint32_t vertex = 0; vertex < GridVertices; ++vertex) {
			slice[vertex * 4 + channel] = a_healed.weights[layer][vertex];
		}
	}

	auto* device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC textureDesc{};
	textureDesc.Width = GridSize;
	textureDesc.Height = GridSize;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = sliceCount;
	textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureDesc.SampleDesc = { 1, 0 };
	textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	std::array<D3D11_SUBRESOURCE_DATA, sliceCount> textureData{};
	for (uint32_t slice = 0; slice < sliceCount; ++slice) {
		textureData[slice].pSysMem = pixels[slice].data();
		textureData[slice].SysMemPitch = GridSize * 4;
	}

	winrt::com_ptr<ID3D11Texture2D> texture;
	if (const HRESULT hr = device->CreateTexture2D(&textureDesc, textureData.data(), texture.put()); FAILED(hr))
		return ResourceFailure("weight texture", hr);
	Util::SetResourceName(texture.get(), "LandscapeSeamsFix::Weights");

	D3D11_SHADER_RESOURCE_VIEW_DESC textureViewDesc{};
	textureViewDesc.Format = textureDesc.Format;
	textureViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	textureViewDesc.Texture2DArray.MipLevels = 1;
	textureViewDesc.Texture2DArray.ArraySize = sliceCount;
	if (const HRESULT hr = device->CreateShaderResourceView(texture.get(), &textureViewDesc, resources->weights.put()); FAILED(hr))
		return ResourceFailure("weight view", hr);
	Util::SetResourceName(resources->weights.get(), "LandscapeSeamsFix::Weights SRV");

	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.ByteWidth = sizeof(QuadData);
	bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
	bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bufferDesc.StructureByteStride = sizeof(QuadData);

	D3D11_SUBRESOURCE_DATA bufferData{};
	bufferData.pSysMem = &data;

	winrt::com_ptr<ID3D11Buffer> buffer;
	if (const HRESULT hr = device->CreateBuffer(&bufferDesc, &bufferData, buffer.put()); FAILED(hr))
		return ResourceFailure("piece data buffer", hr);
	Util::SetResourceName(buffer.get(), "LandscapeSeamsFix::QuadData");

	D3D11_SHADER_RESOURCE_VIEW_DESC bufferViewDesc{};
	bufferViewDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	bufferViewDesc.Buffer.NumElements = 1;
	if (const HRESULT hr = device->CreateShaderResourceView(buffer.get(), &bufferViewDesc, resources->data.put()); FAILED(hr))
		return ResourceFailure("piece data view", hr);
	Util::SetResourceName(resources->data.get(), "LandscapeSeamsFix::QuadData SRV");

	return resources;
}

void LandscapeSeamsFix::Rebuild(const std::vector<QuadKey>& a_keys)
{
	const auto start = std::chrono::steady_clock::now();

	std::vector<Healed> pending;
	pending.reserve(a_keys.size());
	{
		const std::unique_lock lock(mutex);
		for (const auto& key : a_keys) {
			const auto it = quads.find(key);
			if (it == quads.end() || !it->second.grid || !it->second.geometry)
				continue;

			auto& quad = it->second;
			auto& healed = pending.emplace_back();
			healed.key = key;
			healed.generation = ++quad.generation;
			healed.pbr = quad.pbr;
		}
	}

	{
		const std::shared_lock lock(mutex);
		for (auto& healed : pending) {
			const auto it = quads.find(healed.key);
			if (it != quads.end() && it->second.grid && it->second.generation == healed.generation)
				Heal(healed.key, it->second, healed);
		}
	}

	std::vector<std::shared_ptr<Resources>> built(pending.size());
	for (size_t i = 0; i < pending.size(); ++i) {
		if (pending[i].active) {
			built[i] = CreateResources(pending[i]);
			pending[i].diag.resourceFailed = built[i] == nullptr;
		}
	}

	// (49g) One line per batch: how many pieces have textures that did not fit (their half of the border stays a hard line).
	uint32_t blendedCount = 0;
	uint32_t withDropped = 0;
	uint32_t droppedTextures = 0;
	uint32_t overflowed = 0;
	for (const auto& healed : pending) {
		blendedCount += healed.active && !healed.diag.resourceFailed ? 1u : 0u;
		withDropped += healed.diag.droppedTotal > 0 ? 1u : 0u;
		droppedTextures += healed.diag.droppedTotal;
		overflowed += healed.diag.fieldOverflow ? 1u : 0u;
	}

	{
		const std::unique_lock lock(mutex);
		for (size_t i = 0; i < pending.size(); ++i) {
			const auto it = quads.find(pending[i].key);
			if (it != quads.end() && it->second.generation == pending[i].generation) {
				std::swap(it->second.resources, built[i]);
				it->second.diag = pending[i].diag;
			}
		}
	}
	// `built` now holds the replaced resources. They and the texture references taken by Heal go to the release list: the
	// worker never drops the last reference to an engine texture (Reset releases them on the render thread).
	{
		const std::scoped_lock lock(releaseMutex);
		for (auto& resources : built) {
			if (resources)
				releaseResources.push_back(std::move(resources));
		}
		for (auto& healed : pending) {
			for (auto& extra : healed.extras) {
				for (auto* texture : { &extra.color, &extra.normal, &extra.rmaos }) {
					if (*texture)
						releaseTextures.push_back(std::move(*texture));
				}
			}
		}
	}

	rebuiltQuads.fetch_add(static_cast<uint32_t>(pending.size()), std::memory_order_relaxed);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	lastRebuildMs.store(ms, std::memory_order_relaxed);
	if (!pending.empty())
		logger::info("[Landscape Seams] Batch: {} land pieces, {} with blended edges, {} with textures that did not fit ({} textures dropped){}, {:.2f} ms",
			pending.size(), blendedCount, withDropped, droppedTextures, overflowed ? std::format(", {} with too many textures around them", overflowed) : std::string(), ms);
}

void LandscapeSeamsFix::Enqueue(const std::vector<QuadKey>& a_keys)
{
	{
		const std::scoped_lock lock(queueMutex);
		for (const auto& key : a_keys) {
			if (std::find(queuedKeys.begin(), queuedKeys.end(), key) == queuedKeys.end())
				queuedKeys.push_back(key);
		}
		if (!worker.joinable())
			worker = std::jthread([this](std::stop_token a_stop) { WorkerLoop(a_stop); });
	}
	queueCondition.notify_one();
}

void LandscapeSeamsFix::WorkerLoop(std::stop_token a_stop)
{
	while (!a_stop.stop_requested()) {
		std::vector<QuadKey> keys;
		{
			std::unique_lock lock(queueMutex);
			if (!queueCondition.wait(lock, a_stop, [this] { return !queuedKeys.empty(); }))
				return;
			keys.swap(queuedKeys);
		}
		try {
			Rebuild(keys);
		} catch (const std::exception& e) {
			logger::error("[Landscape Seams] Rebuild failed: {}", e.what());
		}
	}
}

void LandscapeSeamsFix::OnLandSetup(RE::TESObjectLAND* a_land, bool a_pbr)
{
	if (a_land == nullptr || a_land->loadedData == nullptr || a_land->parentCell == nullptr)
		return;

	auto* cell = a_land->parentCell;
	if (!cell->IsExteriorCell())
		return;

	const auto* coordinates = cell->GetCoordinates();
	const auto* worldSpace = cell->GetRuntimeData().worldSpace;
	if (coordinates == nullptr || worldSpace == nullptr)
		return;

	struct LoadedQuad
	{
		QuadKey key;
		Quad quad;
	};
	std::vector<LoadedQuad> loadedNow;
	loadedNow.reserve(4);

	for (uint32_t quadIndex = 0; quadIndex < 4; ++quadIndex) {
		auto* geometry = GetQuadGeometry(a_land, quadIndex);
		if (geometry == nullptr)
			continue;

		auto& entry = loadedNow.emplace_back();
		entry.key = {
			worldSpace->GetFormID(),
			coordinates->cellX * 2 + static_cast<int32_t>(quadIndex & 1),
			coordinates->cellY * 2 + static_cast<int32_t>(quadIndex >> 1)
		};
		ReadQuad(a_land, quadIndex, entry.quad);
		ReadSlotInfo(geometry, a_pbr, entry.quad);
		geometry->IncRefCount();
		entry.quad.geometry = geometry;
		entry.quad.pbr = a_pbr;
	}

	if (loadedNow.empty())
		return;

	std::vector<QuadKey> keys;
	keys.reserve(16);
	std::vector<std::shared_ptr<Resources>> staleResources;
	{
		const std::unique_lock lock(mutex);

		// Unloaded quads stay cached so new neighbours can still blend against their borders.
		// Drop the unloaded ones of other worldspaces so the cache holds at most one worldspace.
		const uint32_t worldSpaceID = worldSpace->GetFormID();
		if (worldSpaceID != currentWorldSpace) {
			currentWorldSpace = worldSpaceID;
			std::erase_if(quads, [&](const auto& a_entry) { return a_entry.first.worldSpace != worldSpaceID && a_entry.second.geometry == nullptr; });
		}

		for (auto& entry : loadedNow) {
			auto& quad = quads[entry.key];
			if (quad.geometry != nullptr) {
				loadedQuads.erase(quad.geometry);
				retiredGeometry.push_back(quad.geometry);
			}
			loadedQuads[entry.quad.geometry] = entry.key;

			quad.slots = entry.quad.slots;
			quad.border = entry.quad.border;
			quad.info = std::move(entry.quad.info);
			quad.grid = std::move(entry.quad.grid);
			quad.geometry = entry.quad.geometry;
			quad.pbr = entry.quad.pbr;
			quad.diag = {};  // (49g) until the worker has looked at the new data
			// Healed from the previous data, so they must not be bound until the worker replaces them.
			staleResources.push_back(std::move(quad.resources));
			// Invalidates a Rebuild of this quad already in flight on the worker.
			++quad.generation;

			keys.push_back(entry.key);
		}

		for (const auto& entry : loadedNow) {
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					if (dx == 0 && dy == 0)
						continue;
					const QuadKey neighbourKey{ entry.key.worldSpace, entry.key.x + dx, entry.key.y + dy };
					if (std::find(keys.begin(), keys.end(), neighbourKey) != keys.end())
						continue;
					const auto it = quads.find(neighbourKey);
					if (it != quads.end() && it->second.grid && it->second.geometry)
						keys.push_back(neighbourKey);
				}
			}
		}
	}

	{
		const std::scoped_lock lock(releaseMutex);
		for (auto& resources : staleResources) {
			if (resources)
				releaseResources.push_back(std::move(resources));
		}
	}

	// The weight work (healing, weight maps, buffers) runs on the worker, never in this thread.
	Enqueue(keys);
}

void LandscapeSeamsFix::Reset()
{
	// What the worker let go of (replaced resources, texture references): released here, on the render thread, at function exit.
	std::vector<std::shared_ptr<Resources>> drainedResources;
	std::vector<RE::NiSourceTexturePtr> drainedTextures;
	{
		const std::unique_lock releaseLock(releaseMutex, std::try_to_lock);
		if (releaseLock.owns_lock()) {
			drainedResources.swap(releaseResources);
			drainedTextures.swap(releaseTextures);
		}
	}
	std::vector<RE::BSGeometry*> released;
	std::vector<std::shared_ptr<Resources>> releasedResources;
	{
		const std::unique_lock lock(mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		if (loadedQuads.empty() && retiredGeometry.empty())
			return;
		released.swap(retiredGeometry);

		for (auto it = loadedQuads.begin(); it != loadedQuads.end();) {
			const auto quadIt = quads.find(it->second);
			if (quadIt == quads.end()) {
				it = loadedQuads.erase(it);
				continue;
			}

			auto& quad = quadIt->second;
			if (quad.geometry == nullptr || quad.geometry != it->first) {
				it = loadedQuads.erase(it);
				continue;
			}

			if (quad.geometry->GetRefCount() <= 1) {
				released.push_back(quad.geometry);
				releasedResources.push_back(std::move(quad.resources));
				quad.geometry = nullptr;
				quad.grid.reset();
				quad.info = {};  // an unloaded quad keeps its border weights, not its textures
				++quad.generation;
				it = loadedQuads.erase(it);
				continue;
			}

			++it;
		}
	}

	for (auto* geometry : released) {
		geometry->DecRefCount();
	}
}

LandscapeSeamsFix& LandscapeSeamsFix::GetInstance()
{
	// Never destroyed: it holds engine textures and a worker thread, neither of which may be torn down at process exit.
	static auto* instance = new LandscapeSeamsFix();
	return *instance;
}

void LandscapeSeamsFix::Install()
{
	// The hooks it needs already exist: Hooks.cpp (TESObjectLAND::SetupMaterial, BSShader::BeginTechnique) and TruePBR.cpp
	// (BSLightingShaderProperty::GetRenderPasses, BSLightingShader::SetupGeometry) call in here.
	logger::info("[Landscape Seams] Ready (switch {}), slots t{}-t{}", IsEnabled() ? "on" : "off", FirstPSTexture, FirstPSTexture + NumPSTextures - 1);
}

void LandscapeSeamsFix::GenerateShaderPermutations(RE::BSShader* a_shader)
{
	if (a_shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return;

	// The True PBR landscape seams permutations come from TruePBR::GenerateShaderPermutations (landFlags).
	auto state = globals::state;
	auto shaderCache = globals::shaderCache;
	uint32_t requested = 0;
	for (const auto& entry : a_shader->pixelShaders) {
		if (!IsLandscapeTechnique(entry->id) || (entry->id & CommunityShadersBits) != 0)
			continue;

		auto vertexShaderDescriptor = entry->id;
		auto pixelShaderDescriptor = entry->id | SeamsFlag;
		state->ModifyShaderLookup(*a_shader, vertexShaderDescriptor, pixelShaderDescriptor);
		std::ignore = shaderCache->GetPixelShader(*a_shader, pixelShaderDescriptor);
		vertexShaderDescriptor = entry->id;
		pixelShaderDescriptor = entry->id | SeamsFlag;
		state->ModifyShaderLookup(*a_shader, vertexShaderDescriptor, pixelShaderDescriptor, true);
		std::ignore = shaderCache->GetPixelShader(*a_shader, pixelShaderDescriptor);
		++requested;
	}
	logger::info("[Landscape Seams] Requested seams pixel shaders for {} vanilla landscape permutations (forward + deferred)", requested);
}

void LandscapeSeamsFix::Load(const nlohmann::json& a_featureJson)
{
	if (!a_featureJson.is_object())
		return;
	if (a_featureJson.contains(kSettingKey) && a_featureJson[kSettingKey].is_boolean())
		SetEnabled(a_featureJson[kSettingKey].get<bool>());
	if (a_featureJson.contains(kRMAOSFixSettingKey) && a_featureJson[kRMAOSFixSettingKey].is_boolean())
		SetRMAOSFixEnabled(a_featureJson[kRMAOSFixSettingKey].get<bool>());
}

void LandscapeSeamsFix::LoadLegacy(const nlohmann::json& a_json)
{
	if (a_json.is_object() && a_json.contains("Enabled") && a_json["Enabled"].is_boolean())
		SetEnabled(a_json["Enabled"].get<bool>());
}

void LandscapeSeamsFix::Save(nlohmann::json& a_featureJson) const
{
	a_featureJson[kSettingKey] = IsEnabled();
	a_featureJson[kRMAOSFixSettingKey] = IsRMAOSFixEnabled();
}

void LandscapeSeamsFix::DrawSettings()
{
	ImGui::SeparatorText("Terrain Seams");

	bool on = IsEnabled();
	if (ImGui::Checkbox("Blend Terrain Seams", &on))
		enabled.store(on, std::memory_order_relaxed);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Land is built from square pieces (four per cell), each painted with at most six ground textures of its own. "
			"Where two pieces use different textures the ground changes in a straight line along the piece edge.\n"
			"On: the textures fade into each other over a few metres at those edges, and a piece may borrow up to two "
			"textures from its neighbour there. Off: exactly the old ground.\n"
			"The blending is prepared while land loads, never during a frame; takes effect immediately.");
	}

	size_t loaded = 0;
	size_t blended = 0;
	{
		const std::shared_lock lock(mutex);
		loaded = loadedQuads.size();
		for (const auto& loadedEntry : loadedQuads) {
			if (const auto it = quads.find(loadedEntry.second); it != quads.end() && it->second.resources)
				++blended;
		}
	}
	ImGui::TextDisabled("Loaded land pieces: %zu, with blended edges: %zu (prepared so far: %u, last batch %.2f ms on a background thread)",
		loaded, blended, rebuiltQuads.load(std::memory_order_relaxed), lastRebuildMs.load(std::memory_order_relaxed));

	// (49g) The land piece under the player: what it has, what it borrowed, what did not fit, and whether the seams shader draws it.
	struct Readout
	{
		bool found = false;
		bool loadedPiece = false;
		bool pbr = false;
		bool hasResources = false;
		RE::BSGeometry* geometry = nullptr;
		std::array<RE::TESLandTexture*, EngineLayers> slots{};
		Diagnostics diag;
	} readout;
	QuadKey key{};
	const char* where = nullptr;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player != nullptr ? player->GetParentCell() : nullptr;
	auto* worldSpace = player != nullptr ? player->GetWorldspace() : nullptr;
	if (cell != nullptr && cell->IsInteriorCell()) {
		where = "Under you: indoors (no land pieces).";
	} else if (cell == nullptr || worldSpace == nullptr) {
		where = "Under you: no game loaded.";
	} else {
		const auto position = player->GetPosition();
		key = { worldSpace->GetFormID(), static_cast<int32_t>(std::floor(position.x / QuadSize)), static_cast<int32_t>(std::floor(position.y / QuadSize)) };
		const std::shared_lock lock(mutex);
		if (const auto it = quads.find(key); it != quads.end()) {
			const auto& quad = it->second;
			readout.found = true;
			readout.loadedPiece = quad.geometry != nullptr;
			readout.pbr = quad.pbr;
			readout.hasResources = quad.resources != nullptr;
			readout.geometry = quad.geometry;
			readout.slots = quad.slots;
			readout.diag = quad.diag;
		}
	}
	diagGeometry.store(readout.geometry, std::memory_order_relaxed);

	if (ImGui::TreeNodeEx("Land Piece Under You", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"What the seam blending did for the land piece you stand on (a piece is a quarter of a cell, about 29 m square).\n"
				"Stand 10-20 m from a straight texture line to judge it. From far away (beyond about 100 m) the ground is the baked "
				"LOD texture, which this cannot change.");
		}
		if (where != nullptr) {
			ImGui::TextDisabled("%s", where);
		} else if (!readout.found || !readout.loadedPiece) {
			ImGui::TextDisabled("Under you: land piece %d, %d - not loaded yet.", key.x, key.y);
		} else {
			ImGui::TextDisabled("Under you: land piece %d, %d (cell %d, %d), %s land", key.x, key.y, key.x >> 1, key.y >> 1, readout.pbr ? "True PBR" : "vanilla");

			std::string own;
			for (const auto* texture : readout.slots) {
				if (texture == nullptr)
					continue;
				if (!own.empty())
					own += ", ";
				own += LandTextureName(texture);
			}
			ImGui::TextWrapped("Its own textures: %s", own.empty() ? "-" : own.c_str());

			const auto& diag = readout.diag;
			std::string borrowed;
			for (uint32_t i = 0; i < diag.borrowedCount; ++i) {
				if (!borrowed.empty())
					borrowed += ", ";
				borrowed += LandTextureName(diag.borrowed[i]);
			}
			ImGui::TextWrapped("Borrowed from neighbours: %s", borrowed.empty() ? "none" : borrowed.c_str());

			std::string dropped;
			for (uint32_t i = 0; i < diag.droppedShown; ++i) {
				if (!dropped.empty())
					dropped += "; ";
				dropped += std::format("{} (up to {:.0f}%, {})", LandTextureName(diag.dropped[i].texture), diag.dropped[i].peak * 100.0f, DropReasonText(diag.dropped[i].reason));
			}
			if (diag.droppedTotal > diag.droppedShown)
				dropped += std::format("; and {} more", diag.droppedTotal - diag.droppedShown);
			if (diag.fieldOverflow)
				dropped += dropped.empty() ? "a neighbour was left out (too many textures around)" : "; a neighbour was left out (too many textures around)";
			if (dropped.empty()) {
				ImGui::TextWrapped("Did not fit: none");
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
				ImGui::TextWrapped("Did not fit (a hard line stays for these): %s", dropped.c_str());
				ImGui::PopStyleColor();
			}

			const char* blendText = "yes";
			if (!diag.healed)
				blendText = "not yet (being prepared)";
			else if (!diag.active)
				blendText = "no (its edges match its neighbours: nothing to blend)";
			else if (diag.resourceFailed || !readout.hasResources)
				blendText = "no (could not create its resources, see the log)";
			ImGui::TextDisabled("Blended edges: %s", blendText);

			const uint32_t frame = globals::state->frameCount;
			const auto recent = [frame](uint32_t a_frame) { return a_frame != 0 && frame - a_frame <= 2; };
			std::string shaderText;
			if (!readout.hasResources)
				shaderText = "not used (nothing blended on this piece)";
			else if (!IsEnabled())
				shaderText = "not used (Blend Terrain Seams is off)";
			else if (recent(lastSeamsDropFrame.load(std::memory_order_relaxed)))
				shaderText = std::format("not used: {}", TechniqueDropText(static_cast<TechniqueDrop>(lastSeamsDropReason.load(std::memory_order_relaxed))));
			else if (recent(diagBoundFrame.load(std::memory_order_relaxed)))
				shaderText = "in use";
			else
				shaderText = "not drawn this frame (look at the ground)";
			ImGui::TextDisabled("Seams shader: %s", shaderText.c_str());
		}
		ImGui::TreePop();
	}
}

void LandscapeSeamsFix::DrawPbrTerrainSettings()
{
	ImGui::SeparatorText("PBR Terrain");

	bool on = IsRMAOSFixEnabled();
	if (ImGui::Checkbox("Fix Dark Bands Between Textures", &on))
		SetRMAOSFixEnabled(on);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"True PBR land only. Where a normal (non-PBR) ground texture fades into another one, the old code counted its share twice, "
			"so half-way through the fade the ground got darker and shinier in a band.\n"
			"On: each texture counts once, and a non-PBR texture takes its shininess from its own normal map (as official Community "
			"Shaders does). Off: exactly the old ground. Takes effect immediately.");
	}
}
