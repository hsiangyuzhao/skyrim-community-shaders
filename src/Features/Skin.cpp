#include "Skin.h"
#include <DirectXTex.h>

#include "Deferred.h"
#include "Hooks.h"
#include "JiayeStatement.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Features/WetSkin.h"

#include "DynamicWetness_PublicAPI.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skin::Settings,
	EnableSkin,
	SkinMainRoughness,
	SkinSecondRoughness,
	SkinSpecularTexMultiplier,
	SecondarySpecularStrength,
	F0,
	BaseColorMultiplier,
	PhysicalMainRoughnessMultiplier,
	PhysicalSecondRoughnessMultiplier,
	PhysicalSpecularStrength,
	ExtraEdgeRoughness,
	EnableSkinDetail,
	SkinDetailStrength,
	SkinDetailTiling,
	BodyTilingMultiplier,
	WetFadeTime,
	StartSweat,
	FullSweat,
	Translucency,
	sssWidth,
	UseSSS,
	FuzzStrength,
	FuzzRoughness,
	FuzzF0,
	UseDynamicWetness);

void Skin::DrawSettings()
{
	ImGui::Checkbox("Enable Advanced Skin", &settings.EnableSkin);

	ImGui::Text("Advanced Skin Shader using dual specular lobes.");

	ImGui::Spacing();
	ImGui::SliderFloat("Primary Roughness", &settings.SkinMainRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Controls microscopic roughness of stratum corneum layer");
	}

	ImGui::SliderFloat("Secondary Roughness", &settings.SkinSecondRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Smoothness of epidermal cell layer reflections");
		ImGui::BulletText("Should be 30-50%% lower than Primary");
	}

	ImGui::SliderFloat("Specular Texture Multiplier", &settings.SkinSpecularTexMultiplier, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Multiplier for specular map");
		ImGui::BulletText("A multiplier for the vanilla specular map, applied to the first layer's roughness");
	}

	ImGui::SliderFloat("Secondary Specular Strength", &settings.SecondarySpecularStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Intensity of secondary specular highlights");
	}

	ImGui::SliderFloat("Fresnel F0", &settings.F0, 0.0f, 0.1f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Fresnel reflectance");
	}
	
	ImGui::SliderFloat("Base Color Multiplier", &settings.BaseColorMultiplier, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Multiplier for the base color texture");
	}

	ImGui::Spacing();
	ImGui::Text("Options for additional roughness and specular maps.");

	ImGui::SliderFloat("Physical Main Roughness Multiplier", &settings.PhysicalMainRoughnessMultiplier, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("Physical Second Roughness Multiplier", &settings.PhysicalSecondRoughnessMultiplier, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("Physical Specular Multiplier", &settings.PhysicalSpecularStrength, 0.0f, 2.0f, "%.2f");

	ImGui::Spacing();

	ImGui::SliderFloat("Extra Edge Roughness", &settings.ExtraEdgeRoughness, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Extra roughness at the edges of the skin, to approximate peach fuzz on the face.");
	}

	ImGui::SliderFloat("Fuzz Strength", &settings.FuzzStrength, 0.0f, 2.0f, "%.2f");

	ImGui::SliderFloat("Fuzz Roughness", &settings.FuzzRoughness, 0.1f, 1.0f, "%.2f");

	ImGui::SliderFloat("Fuzz F0", &settings.FuzzF0, 0.0f, 0.5f, "%.4f");

	ImGui::Spacing();

	ImGui::Checkbox("Enable SSS Transmission", &settings.UseSSS);

	ImGui::SliderFloat("Translucency", &settings.Translucency, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Translucency of the SSS Transmittance effect");
	}

	ImGui::SliderFloat("SSS Width", &settings.sssWidth, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Width of the SSS Transmittance effect");
	}

	ImGui::Spacing();

	// (44a) Skin wetness has its own page.
	ImGui::TextDisabled("Skin wetness (model, extra wetness, sheen, droplets, rivulets): see the Wet Skin page (Characters).");

	ImGui::SliderFloat("Wetness Fade Out Time", &settings.WetFadeTime, 0.0f, 50.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Time it takes for the wetness to reduce from 1.0 to 0.0 after leaving water");
	}

	if (isDynamicWetnessAvailable) {
		ImGui::Text("Dynamic Wetness detected.");
		ImGui::Checkbox("Use Dynamic Wetness", &settings.UseDynamicWetness);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Let the Dynamic Wetness mod decide how wet skin is, instead of the stamina-based sweat below.");
	} else {
		settings.UseDynamicWetness = false;
	}

	if (!settings.UseDynamicWetness)
	{
		ImGui::SliderFloat("Stamina Threshold for Sweat", &settings.StartSweat, 0.0f, 1.0f, "%.2f", 
			ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Characters start to look sweaty when stamina drops below this fraction (0.75 = 75 percent).");
		ImGui::SliderFloat("Full Sweat Threshold", &settings.FullSweat, 0.0f, 1.0f, "%.2f",
			ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Stamina fraction at which sweat reaches full strength.");
	}


	ImGui::Spacing();

	ImGui::Checkbox("Enable Skin Detail", &settings.EnableSkinDetail);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Enable skin detail texture");
	}

	ImGui::SliderFloat("Skin Detail Strength", &settings.SkinDetailStrength, -2.0f, 2.0f);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Strength of skin detail texture");
	}

	ImGui::SliderFloat("Skin Detail Tiling", &settings.SkinDetailTiling, 1.0f, 50.0f, "%1.f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("The more tiling, the more detailed the skin will be");
	}

	ImGui::SliderFloat("Body Tiling Multiplier", &settings.BodyTilingMultiplier, 0.5f, 5.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Multiply the tiling for the body to match the face");
	}

	if (ImGui::Button("Reload Skin Detail Texture")) {
		ReloadSkinDetail();
	}

	BUFFER_VIEWER_NODE(texSkinDetail, 1.0f)

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

void Skin::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Loading skin detail texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\Skin\\skin_detail_n.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texSkinDetail = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texSkinDetail->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 10 }
		};
		texSkinDetail->CreateSRV(srvDesc);
	}

	{
		PerGeometryCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PerGeometryData>());
	}

	// Check for Dynamic Wetness availability
	isDynamicWetnessAvailable = SWE::API::Init();
}

void Skin::ReloadSkinDetail()
{
	auto device = globals::d3d::device;

	logger::debug("Reloading skin detail texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\Skin\\skin_detail_n.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texSkinDetail = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texSkinDetail->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 10 }
		};
		texSkinDetail->CreateSRV(srvDesc);
	}
}

void Skin::Prepass()
{
	auto context = globals::d3d::context;

	if (texSkinDetail) {
		ID3D11ShaderResourceView* srv = texSkinDetail->srv.get();
		context->PSSetShaderResources(72, 1, &srv);
	}
}

void Skin::PostPostLoad()
{
	Hooks::Install();
}

Skin::SkinData Skin::GetCommonBufferData()
{
	SkinData data{};
	data.skinParams = float4(settings.SkinMainRoughness, settings.SkinSecondRoughness, settings.SkinSpecularTexMultiplier, float(settings.EnableSkin));
	// (44a) The wetness values come from the Wet Skin page.
	const auto& wet = globals::features::wetSkin.settings;
	data.skinParams2 = float4(settings.SecondarySpecularStrength, std::clamp(wet.ExtraWetness, 0.0f, 2.0f), settings.F0, settings.BaseColorMultiplier);
	data.skinDetailParams = float4(settings.SkinDetailTiling, settings.BodyTilingMultiplier, settings.SkinDetailStrength, float(settings.EnableSkinDetail && settings.EnableSkin));
	data.sssParams = float4(settings.Translucency, settings.sssWidth, 0.0f, float(settings.UseSSS));
	data.fuzzParams = float4(settings.FuzzStrength, settings.FuzzRoughness, settings.FuzzF0, settings.ExtraEdgeRoughness);
	data.physicalParams = float4(settings.PhysicalMainRoughnessMultiplier, settings.PhysicalSecondRoughnessMultiplier, settings.PhysicalSpecularStrength, std::clamp(wet.FilmStrength, 0.0f, 3.0f));
	data.wetParams = wet.WetParams;
	return data;
}

void Skin::LoadSettings(json& o_json)
{
	// (44a) The skin wetness options moved to Wet Skin, which takes them from here once.
	WetSkin::StashLegacySkinJson(o_json);
	settings = o_json;
}

void Skin::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skin::RestoreDefaultSettings()
{
	settings = {};
}

// By PO3
// https://github.com/powerof3/Splashes-of-Skyrim/blob/master/src/Manager.cpp
float Skin::GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos)
{
	float waterHeight = -RE::NI_INFINITY;

	if (const auto waterManager = RE::TESWaterSystem::GetSingleton()) {
		waterHeight = a_ref->GetWaterHeight();

		if (waterHeight != -RE::NI_INFINITY) {
			return waterHeight;
		}

		const auto get_nearest_water_object_height = [&]() {
			for (const auto& waterObject : waterManager->waterObjects) {
				if (waterObject) {
					for (const auto& bound : waterObject->multiBounds) {
						if (bound) {
							if (auto size{ bound->size }; size.z <= 10.0f) {  //avoid sloped water
								auto       center{ bound->center };
								const auto boundMin = center - size;
								const auto boundMax = center + size;
								if (!(a_pos.x < boundMin.x || a_pos.x > boundMax.x || a_pos.y < boundMin.y || a_pos.y > boundMax.y)) {
									return center.z;
								}
							}
						}
					}
				}
			}

			return -RE::NI_INFINITY;
		};

		waterHeight = get_nearest_water_object_height();
	}

	return waterHeight;
}

void Skin::PruneActorWetStates(uint32_t a_frame)
{
	// (44b) Drop the characters not drawn for a while first (their state is rebuilt from the game
	// when they are drawn again); if every entry is recent, start over (upstream 176a00831).
	std::erase_if(actorWetStates, [a_frame](const auto& a_entry) { return a_frame - a_entry.second.frame > kActorWetStaleFrames; });
	if (actorWetStates.size() >= kMaxActorWetStates)
		actorWetStates.clear();
}

float4 Skin::GetWetness(RE::BSGeometry* geometry, float4& o_wetSkin)
{
	o_wetSkin = float4(0.0f, 0.0f, 0.0f, 0.0f);
	auto userData = geometry->GetUserData();
	auto actor = userData ? userData->As<RE::Character>() : nullptr;
	if (!actor)
		return float4(0.0f, 0.0f, 0.0f, 0.0f);

	// (44b) One state per character (form ID), computed once per frame: skin, hair, every piece of
	// gear and anything equipped later all read the same wetness and the same drying timers.
	// Before, each geometry had its own timer (from "dry" for gear put on after leaving the water)
	// and the timers advanced once per draw, never pruned.
	const uint32_t frame = globals::state->frameCount;
	if (frame != wetClockFrame) {
		wetClockFrame = frame;
		wetClock += std::max(*globals::game::deltaTime, 0.0f);
	}
	const RE::FormID id = actor->GetFormID();
	auto it = actorWetStates.find(id);
	if (it != actorWetStates.end() && it->second.valid && it->second.frame == frame) {
		o_wetSkin = it->second.wetSkin;
		return it->second.skin;
	}
	if (it == actorWetStates.end()) {
		if (actorWetStates.size() >= kMaxActorWetStates)
			PruneActorWetStates(frame);
		it = actorWetStates.emplace(id, ActorWetState{}).first;
	}
	auto& state = it->second;
	// Game time since this character was last updated (paused menus add nothing). A character not
	// drawn for a while catches up at once.
	const float dt = state.valid ? std::clamp(static_cast<float>(wetClock - state.clock), 0.0f, 1e6f) : 0.0f;
	state.clock = wetClock;
	state.frame = frame;

	const bool inWater = actor->IsInWater();
	const float positionZ = actor->GetPositionZ();
	const float depth = inWater ? std::max(0.0f, GetWaterHeight(actor->AsReference(), actor->GetPosition()) - positionZ) : 0.0f;

	// (44a) Wet Skin New model: in the water = 1; out of it, held for HoldTime, then drying off over
	// DryTime (smooth). The deepest submersion is kept until the character is dry, so a character
	// who climbed out stays wet up to where the water reached.
	{
		const auto& wet = globals::features::wetSkin.settings;
		float swim = 0.0f;
		if (inWater) {
			state.timeOut = 0.0f;
			state.maxDepth = std::max(state.maxDepth, depth);
			swim = 1.0f;
		} else {
			state.timeOut = std::min(state.timeOut + dt, 1e6f);
			const float hold = std::max(wet.HoldTime, 0.0f);
			const float dry = std::max(wet.DryTime, 1.0f);
			const float t = std::clamp((state.timeOut - hold) / dry, 0.0f, 1.0f);
			swim = 1.0f - t * t * (3.0f - 2.0f * t);
			if (swim <= 0.0f)
				state.maxDepth = 0.0f;
		}
		state.wetSkin = float4(swim, state.maxDepth, inWater ? 1.0f : 0.0f, 0.0f);
	}

	// Sweat and the Original model's swim wetness (Advanced Skin's WetFadeTime fade).
	float4 wetness = float4(0.0f, 0.0f, positionZ, 0.0f);
	if (settings.UseDynamicWetness && isDynamicWetnessAvailable) {
		wetness.x = SWE::API::GetFinalWetness(actor);
	} else {
		const float stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		const float permanentStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		const float temporaryStamina = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);
		const float maxStamina = permanentStamina + temporaryStamina;
		const float staminaPercentage = actor->IsDead() ? 1.0f : (maxStamina > 0.0f ? stamina / maxStamina : 1.0f);
		wetness.x = (staminaPercentage >= settings.StartSweat) ? 0.0f :
		            (staminaPercentage <= settings.FullSweat)  ? 1.0f :
		                                                         (settings.StartSweat - staminaPercentage) / (settings.StartSweat - settings.FullSweat);
	}
	wetness.y = inWater ? 2.0f : 0.0f;
	wetness.w = depth;

	const float fade = dt * (1.0f / std::max(settings.WetFadeTime, 0.01f));
	if (!state.valid) {
		state.old = wetness;
	} else {
		auto& old = state.old;
		if (old.x < wetness.x) {
			old.x = wetness.x;
		} else if (old.x > wetness.x) {
			old.x = std::max(old.x - fade, 0.0f);
			wetness.x = old.x;
		}

		if (old.y < wetness.y) {
			old.y = wetness.y;
			if (old.w < wetness.w)
				old.w = wetness.w;
			else
				wetness.w = old.w;
		} else if (old.y > wetness.y) {
			old.y = std::max(old.y - fade, 0.0f);
			wetness.y = old.y;
			if (wetness.y == 0.0f) {
				wetness.w = 0.0f;
				old.w = 0.0f;
			} else if (old.w < wetness.w) {
				old.w = wetness.w;
			} else {
				wetness.w = old.w;
			}
		} else if (old.w < wetness.w) {
			old.w = wetness.w;
		} else {
			wetness.w = old.w;
		}
	}
	state.skin = wetness;
	state.valid = true;

	o_wetSkin = state.wetSkin;
	return state.skin;
}

struct SkinExtendedRendererState
{
	static constexpr uint32_t NumPSTextures = 1;
	static constexpr uint32_t FirstPSTexture = 71;

	uint32_t PSResourceModifiedBits = 0;
	std::array<ID3D11ShaderResourceView*, 2> PSTexture;

	void SetExtraSkinPSTexture(RE::BSGraphics::Texture* newTexture, RE::BSGraphics::Texture* newTexture2)
	{
		{
			PSTexture = {
				newTexture ? newTexture->resourceView : nullptr,
				newTexture2 ? newTexture2->resourceView : nullptr
			};
			PSResourceModifiedBits = 1;
		}
	}

	SkinExtendedRendererState()
	{
		PSTexture.fill(nullptr);
	}
} skinExtendedRendererState;

void Skin::SetupExtraTexture(RE::BSLightingShaderMaterialBase const* material, RE::BSTextureSet* inTextureSet, uint32_t i_hashKey)
{
	if (!inTextureSet || material->normalTexture == nullptr) {
		logger::error("[Advanced Skin] SetupExtraTexture : Texture set is null for material: {}", i_hashKey);
		return;
	}

	uint32_t hashKey = 0;
	hashKey = material->hashKey;
	if (hashKey == 0 || hashKey != i_hashKey) {
		logger::error("[Advanced Skin] SetupExtraTexture : Invalid hash key for material: {}", i_hashKey);
		return;
	}

	const char extraTextureName[] = "_rfaos.dds";
	const char wetnessTextureName[] = "_wet.dds";
	const char* workingNormalPath = nullptr;
	const char* workingSpecularPath = nullptr;
	auto workingMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
	auto hasSpecular = workingMaterial->specularBackLightingTexture != nullptr;

	auto graphicsState = globals::game::graphicsState;
	const auto& stateData = graphicsState->GetRuntimeData();

	if (hasSpecular) {
		if (auto specularPath = inTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kSpecular)) {
			workingSpecularPath = specularPath;
		}
	}
	if (auto normalPath = inTextureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal)) {
		workingNormalPath = normalPath;
	} else {
		logger::error("[Advanced Skin] SetupExtraTexture : No specular or normal texture found in texture set from material: {}", hashKey);
		auto& workingExtraPtr = skinExtraTextures.try_emplace(hashKey).first->second;
		workingExtraPtr.rfaosTexture = stateData.defaultTextureBlack;
		workingExtraPtr.wetnessTexture = stateData.defaultTextureBlack;
		workingExtraPtr.extraTexturePath = "";
		workingExtraPtr.wetnessTexturePath = "";
		workingExtraPtr.hasExtraTexture = false;
		workingExtraPtr.hasWetnessTexture = false;
		return;
	}

	const char* foundPath = nullptr;
	std::string extraTexturePath = "";
	std::string wetnessTexturePath = "";
	if (!workingSpecularPath && !workingNormalPath) {
		return;
	}

	auto findIgnoreCase = [](std::string_view str, std::string_view pattern) -> size_t {
		auto it = std::search(str.begin(), str.end(), pattern.begin(), pattern.end(),
			[](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); });
		return it == str.end() ? std::string_view::npos : std::distance(str.begin(), it);
	};

	if (hasSpecular && workingSpecularPath && findIgnoreCase(workingSpecularPath, "_s.dds") != std::string_view::npos) {
		auto pos = findIgnoreCase(workingSpecularPath, "_s.dds");
		if (pos != std::string_view::npos) {
			auto newPath = std::string(workingSpecularPath);
			auto newPath2 = std::string(workingSpecularPath);
			newPath.replace(pos, 6, extraTextureName);
			newPath2.replace(pos, 6, wetnessTextureName);
			extraTexturePath = newPath;
			wetnessTexturePath = newPath2;
			foundPath = workingSpecularPath;
		}
	} else {
		if (workingNormalPath && findIgnoreCase(workingNormalPath, "_n.dds") != std::string_view::npos) {
			auto pos = findIgnoreCase(workingNormalPath, "_n.dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				auto newPath2 = std::string(workingNormalPath);
				newPath.replace(pos, 6, extraTextureName);
				newPath2.replace(pos, 6, wetnessTextureName);
				extraTexturePath = newPath;
				wetnessTexturePath = newPath2;
				foundPath = workingNormalPath;
			}
		} else if (workingNormalPath && findIgnoreCase(workingNormalPath, "_msn.dds") != std::string_view::npos) {
			auto pos = findIgnoreCase(workingNormalPath, "_msn.dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				auto newPath2 = std::string(workingNormalPath);
				newPath.replace(pos, 8, extraTextureName);
				newPath2.replace(pos, 8, wetnessTextureName);
				extraTexturePath = newPath;
				wetnessTexturePath = newPath2;
				foundPath = workingNormalPath;
			}
		} else {
			auto pos = findIgnoreCase(std::string_view(workingNormalPath), ".dds");
			if (pos != std::string_view::npos) {
				auto newPath = std::string(workingNormalPath);
				auto newPath2 = std::string(workingNormalPath);
				newPath.replace(pos, 4, extraTextureName);
				newPath2.replace(pos, 4, wetnessTextureName);
				extraTexturePath = newPath;
				wetnessTexturePath = newPath2;
				foundPath = workingNormalPath;
			}
		}
	}

	logger::debug("[Advanced Skin] SetupExtraTexture : Extra texture path: {} for {}", extraTexturePath, foundPath);
	logger::debug("[Advanced Skin] SetupExtraTexture : Wetness texture path: {} for {}", wetnessTexturePath, foundPath);

	auto& workingExtraPtr = skinExtraTextures.try_emplace(hashKey).first->second;
	workingExtraPtr.rfaosTexture = stateData.defaultTextureWhite;
	workingExtraPtr.wetnessTexture = stateData.defaultTextureWhite;
	workingExtraPtr.extraTexturePath = extraTexturePath;
	workingExtraPtr.wetnessTexturePath = wetnessTexturePath;

	inTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kEnvironment, workingExtraPtr.extraTexturePath.c_str());
	inTextureSet->SetTexturePath(RE::BSTextureSet::Texture::kMultilayer, workingExtraPtr.wetnessTexturePath.c_str());
	inTextureSet->SetTexture(RE::BSTextureSet::Texture::kEnvironment, workingExtraPtr.rfaosTexture);
	inTextureSet->SetTexture(RE::BSTextureSet::Texture::kMultilayer, workingExtraPtr.wetnessTexture);

	workingExtraPtr.hasExtraTexture = workingExtraPtr.rfaosTexture != nullptr && !workingExtraPtr.extraTexturePath.empty() && workingExtraPtr.rfaosTexture != stateData.defaultTextureBlack;
	workingExtraPtr.hasWetnessTexture = workingExtraPtr.wetnessTexture != nullptr && !workingExtraPtr.wetnessTexturePath.empty() && workingExtraPtr.wetnessTexture != stateData.defaultTextureBlack;

	if (workingExtraPtr.hasExtraTexture || workingExtraPtr.hasWetnessTexture) {
		logger::debug("[Advanced Skin] SetupExtraTexture : Extra texture set with hash key: {}", hashKey);
	} else {
		logger::debug("[Advanced Skin] SetupExtraTexture : Failed to set extra texture for material: {}", hashKey);
	}

	// (batch 16, item 4) Bound the map here rather than on a timer: this is the only insertion
	// point, so it is the only place the size can grow, and pruning here means the check costs
	// nothing on the hot path (BSLightingShader_SetupMaterial, per draw call).
	PruneSkinExtraTextures();
}

// (batch 16, item 4) See Skin::maxSkinExtraTextures for why eviction is safe here.
void Skin::PruneSkinExtraTextures()
{
	if (skinExtraTextures.size() <= maxSkinExtraTextures)
		return;

	// Drop down to three quarters of the cap rather than to exactly the cap. Trimming to the
	// cap would make every subsequent insertion trigger another prune, i.e. an O(n) pass per
	// new facegen material for the rest of the session.
	const size_t target = maxSkinExtraTextures - (maxSkinExtraTextures / 4);

	std::vector<std::pair<std::int64_t, uint32_t>> byAge;
	byAge.reserve(skinExtraTextures.size());
	for (const auto& [key, entry] : skinExtraTextures)
		byAge.emplace_back(entry.lastUsed, key);

	const size_t toDrop = skinExtraTextures.size() - target;
	std::nth_element(byAge.begin(), byAge.begin() + toDrop, byAge.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });

	for (size_t i = 0; i < toDrop; ++i)
		skinExtraTextures.erase(byAge[i].second);

	logger::debug("[Advanced Skin] Pruned {} least recently drawn skin material entries, {} remain", toDrop, skinExtraTextures.size());
}

void Skin::BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material)
{
	auto materialFeature = material->GetFeature();
	if (materialFeature != RE::BSShaderMaterial::Feature::kFaceGen &&
		materialFeature != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
		return;
	}

	auto materialTextureSet = material->textureSet.get();

	uint32_t hashKey = 0;
	hashKey = material->hashKey;
	if (hashKey == 0) {
		logger::error("[Advanced Skin] BSLightingShader_SetupMaterial : Invalid hash key for material: {}", static_cast<int>(materialFeature));
		return;
	}

	if (!skinExtraTextures.contains(hashKey)) {
		// logger::debug("[Advanced Skin] BSLightingShader_SetupMaterial : Setting up extra texture for material: {}", static_cast<int>(materialFeature));
		globals::features::skin.SetupExtraTexture(material, materialTextureSet, hashKey);
	}

	auto graphicsState = globals::game::graphicsState;
	// (batch 16, item 4) Reference, not a copy. This runs once per facegen draw call and the
	// copy it replaced duplicated two NiPointer refcount round-trips and two std::string heap
	// allocations every time. Taking it after SetupExtraTexture (and therefore after any
	// rehash or prune the insertion above may have caused) is what makes the reference safe;
	// nothing below mutates the map.
	auto& workingExtraPtr = skinExtraTextures[hashKey];
	// Recency stamp for PruneSkinExtraTextures: an entry drawn this frame is the last thing
	// that should be evicted.
	workingExtraPtr.lastUsed = std::chrono::steady_clock::now().time_since_epoch().count();

	if (workingExtraPtr.hasExtraTexture || workingExtraPtr.hasWetnessTexture) {
		skinExtendedRendererState.SetExtraSkinPSTexture(workingExtraPtr.rfaosTexture->rendererTexture, workingExtraPtr.wetnessTexture->rendererTexture);
	} else {
		skinExtendedRendererState.SetExtraSkinPSTexture(graphicsState->GetRuntimeData().defaultTextureBlack->rendererTexture, graphicsState->GetRuntimeData().defaultTextureBlack->rendererTexture);
	}
}

void Skin::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	// auto deferred = globals::deferred;
	auto context = globals::d3d::context;

	if (settings.EnableSkin) {
		auto geometry = a_pass->geometry;
		PerGeometryData perGeometryData{};
		perGeometryData.skinPerGeometry = GetWetness(geometry, perGeometryData.wetSkinPerGeometry);
		// (44a) Wet Skin settings ride along (they change rarely; the buffer is only rewritten
		// when something in it changed).
		const auto params = globals::features::wetSkin.GetGPUParams();
		for (int i = 0; i < 6; ++i)
			perGeometryData.wetSkinParams[i] = params.p[i];

		if (!perGeometryValid || std::memcmp(currentPerGeometry.data(), &perGeometryData, sizeof(PerGeometryData)) != 0) {
			std::memcpy(currentPerGeometry.data(), &perGeometryData, sizeof(PerGeometryData));
			perGeometryValid = true;
			currentWetness = perGeometryData.skinPerGeometry;
			PerGeometryCB->Update(perGeometryData);
		}

		ID3D11Buffer* buffer = { PerGeometryCB->CB() };
		context->PSSetConstantBuffers(7, 1, &buffer);
	}
}

void Skin::SetShaderResouces(ID3D11DeviceContext* a_context)
{
	if (skinExtendedRendererState.PSResourceModifiedBits != 0) {
		a_context->PSSetShaderResources(71, 1, &skinExtendedRendererState.PSTexture.at(0));
		a_context->PSSetShaderResources(74, 1, &skinExtendedRendererState.PSTexture.at(1));
	}
	skinExtendedRendererState.PSResourceModifiedBits = 0;
}

void Skin::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& skin = globals::features::skin;
	skin.BSLightingShader_SetupGeometry(Pass);
	return func(This, Pass, RenderFlags);
}
