#include "Skin.h"
#include <DirectXTex.h>

#include "Deferred.h"
#include "Hooks.h"
#include "JiayeStatement.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Features/DynamicSnow.h"
#include "Features/WetSkin.h"
#include "Utils/WorldTiming.h"

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

	// (44a) Skin wetness has its own page. (47i) So do the sweat thresholds, Dynamic Wetness and the Legacy fade-out time.
	ImGui::TextDisabled("Skin wetness (model, sweat thresholds, Dynamic Wetness, sheen, droplets, rivulets): see the Character Wetness page (Characters).");


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

	// (46a) The player's snow is advanced every frame, drawn or not (first person, interiors), so
	// going indoors always clears it and stepping out starts it from nothing.
	if (globals::features::dynamicSnow.CharacterSnowStateActive()) {
		if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded()) {
			playerWater = SampleWater(player);
			playerSnowGPU = UpdateActorSnow(playerSnow, player, playerWater);
		}
	} else {
		playerSnow = {};
		playerSnowGPU = float4(-1.0f, 0.0f, 0.0f, 0.0f);
	}
}

void Skin::OnGameLoaded()
{
	// (46d) A loaded save: nobody keeps the snow (or the water line) of the game played before it.
	playerSnow = {};
	playerSnowGPU = float4(-1.0f, 0.0f, 0.0f, 0.0f);
	playerWater = {};
	for (auto& [id, state] : actorWetStates) {
		state.snow = {};
		state.snowGPU = float4(-1.0f, 0.0f, 0.0f, 0.0f);
	}
}

namespace
{
	std::int64_t SkinQpc()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	double SkinQpcMs()
	{
		static const double toMs = [] {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
		}();
		return toMs;
	}

	bool FiniteHeight(float a_z)
	{
		return std::isfinite(a_z) && a_z > -1e7f && a_z < 1e7f;
	}

	/// (46e) The pick under the world's read lock, released by a termination handler: a fault inside the pick
	/// (caught by PickUpSafe) must not leave the lock held (a C++ guard is not unwound by SEH under /EHsc, and a
	/// read lock never released would hang the next physics write for good).
	__declspec(noinline) void PickLocked(RE::bhkWorld* a_world, RE::bhkPickData* a_pick)
	{
		a_world->worldLock.LockForRead();
		__try {
			a_world->PickObject(*a_pick);
		} __finally {
			a_world->worldLock.UnlockForRead();
		}
	}

	/// The upward line-of-sight pick (SEH: the Havok world is engine memory).
	__declspec(noinline) void PickUpUnsafe(RE::bhkWorld* a_world, const RE::NiPoint3* a_from, const RE::NiPoint3* a_to, bool* o_hit)
	{
		RE::bhkPickData pick;
		const float scale = RE::bhkWorld::GetWorldScale();
		pick.rayInput.from = RE::hkVector4(a_from->x * scale, a_from->y * scale, a_from->z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_to->x * scale, a_to->y * scale, a_to->z * scale, 0.0f);
		pick.rayInput.filterInfo.filter = static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
		PickLocked(a_world, &pick);
		*o_hit = pick.rayOutput.HasHit();
	}

	bool PickUpSafe(RE::bhkWorld* a_world, const RE::NiPoint3* a_from, const RE::NiPoint3* a_to, bool* o_hit)
	{
		__try {
			// No waiting on the render thread: while the physics holds the world for writing, the test is skipped
			// (the last answer stays; asked again next frame). BSReadWriteLock: writer thread, then the lock word.
			const auto* lockWords = reinterpret_cast<const volatile std::uint32_t*>(&a_world->worldLock);
			if (lockWords[1] & RE::BSReadWriteLock::kLockWrite)
				return false;
			PickUpUnsafe(a_world, a_from, a_to, o_hit);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
}

float Skin::GetWaterObjectHeight(const RE::NiPoint3& a_pos, float a_maxDepth)
{
	// By PO3 (Splashes of Skyrim): the flat water object whose bounds hold the position.
	// https://github.com/powerof3/Splashes-of-Skyrim/blob/master/src/Manager.cpp
	// (46f) Only a surface above the feet and at most a_maxDepth above them (the nearest such one): standing under a
	// bridge's / a waterfall basin's water, or on a ledge above a pool, the XY bounds alone matched.
	float best = -1e30f;
	if (const auto waterManager = RE::TESWaterSystem::GetSingleton()) {
		for (const auto& waterObject : waterManager->waterObjects) {
			if (!waterObject)
				continue;
			for (const auto& bound : waterObject->multiBounds) {
				if (!bound)
					continue;
				if (auto size{ bound->size }; size.z <= 10.0f) {  // avoid sloped water
					const auto center{ bound->center };
					const auto boundMin = center - size;
					const auto boundMax = center + size;
					if (a_pos.x < boundMin.x || a_pos.x > boundMax.x || a_pos.y < boundMin.y || a_pos.y > boundMax.y)
						continue;
					const float depth = center.z - a_pos.z;
					if (depth <= 0.0f || depth > a_maxDepth)
						continue;
					if (best <= -1e29f || center.z < best)
						best = center.z;
				}
			}
		}
	}
	return best;
}

namespace
{
	/// (46f) Per character: the last flat-water-object answer (the search walks every water object of the loaded
	/// area, and SampleWater runs per character and per alpha-blended geometry each frame). Asked again only once
	/// the character moved or after a while; the Havok height (relevantWaterHeight) is never cached.
	struct WaterObjectCacheEntry
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float height = -1e30f;
		std::int64_t qpc = 0;
	};
	constexpr float kWaterCacheMove = 16.0f;        // units moved (any axis) before the search runs again
	constexpr double kWaterCacheMaxAgeMs = 500.0;   // and at least this often while standing
	constexpr double kWaterCachePruneMs = 10000.0;  // entries unused this long are dropped
	constexpr std::size_t kWaterCachePruneSize = 256;
	std::mutex g_waterCacheMutex;
	std::unordered_map<RE::FormID, WaterObjectCacheEntry> g_waterCache;
}

Skin::WaterSample Skin::SampleWater(RE::Actor* a_actor)
{
	WaterSample w;
	if (!a_actor)
		return w;
	const auto position = a_actor->GetPosition();
	w.feetZ = position.z;
	if (auto* loaded = a_actor->loadedData; loaded && FiniteHeight(loaded->relevantWaterHeight))
		w.relevant = loaded->relevantWaterHeight;
	if (auto* cell = a_actor->GetParentCell()) {
		const float h = cell->GetExteriorWaterHeight();
		if (FiniteHeight(h))
			w.cellDefault = h;
	}
	if (w.relevant <= -1e29f) {
		// (46f) Deeper than the character is tall (plus a margin) is not "standing in it"; cached per character.
		const float maxDepth = std::max(a_actor->GetHeight(), 32.0f) + 64.0f;
		const std::int64_t now = SkinQpc();
		float h = -1e30f;
		{
			std::scoped_lock lock(g_waterCacheMutex);
			auto& entry = g_waterCache[a_actor->GetFormID()];
			const bool moved = std::abs(position.x - entry.x) > kWaterCacheMove || std::abs(position.y - entry.y) > kWaterCacheMove ||
			                   std::abs(position.z - entry.z) > kWaterCacheMove;
			if (entry.qpc == 0 || moved || static_cast<double>(now - entry.qpc) * SkinQpcMs() > kWaterCacheMaxAgeMs) {
				const bool timed = Util::WorldTiming::On();  // (46l) measurement only
				const uint64_t searchStart = timed ? Util::WorldTiming::Stamp() : 0;
				entry.height = GetWaterObjectHeight(position, maxDepth);
				if (timed)
					Util::WorldTiming::AddWaterSearch(searchStart);
				entry.x = position.x;
				entry.y = position.y;
				entry.z = position.z;
				entry.qpc = now;
			}
			h = entry.height;
			// Within the move tolerance the cached surface may be just past the depth limit / below the feet: checked again below.
			if (g_waterCache.size() > kWaterCachePruneSize) {
				std::erase_if(g_waterCache, [&](const auto& a_pair) {
					return static_cast<double>(now - a_pair.second.qpc) * SkinQpcMs() > kWaterCachePruneMs;
				});
			}
		}
		if (FiniteHeight(h) && h - position.z <= maxDepth)
			w.multibound = h;
	}
	w.surfaceZ = w.relevant > -1e29f ? w.relevant : w.multibound;
	if (w.surfaceZ <= -1e29f)
		return w;
	w.rawDepth = w.surfaceZ - w.feetZ;
	if (w.rawDepth <= 0.0f)
		return w;
	w.inWater = true;
	if (auto* state = a_actor->AsActorState())
		w.swimming = state->IsSwimming();
	// Plausibility: a wading character is at most 0.6 of its height deep, a swimming one about its height.
	const float height = std::max(a_actor->GetHeight(), 32.0f);
	const float limit = w.swimming ? height : 0.6f * height;
	w.clamped = w.rawDepth > limit;
	w.depth = std::min(w.rawDepth, limit);
	return w;
}

bool Skin::IsSheltered(RE::Actor* a_actor, ActorSnow& a_snow, double a_intervalMs)
{
	const std::int64_t now = SkinQpc();
	if (a_snow.shelterQpc != 0 && static_cast<double>(now - a_snow.shelterQpc) * SkinQpcMs() < a_intervalMs)
		return a_snow.sheltered;
	a_snow.shelterQpc = now;
	auto* cell = a_actor ? a_actor->GetParentCell() : nullptr;
	auto* world = cell ? cell->GetbhkWorld() : nullptr;
	if (!world) {
		a_snow.sheltered = false;
		return false;
	}
	// From just above the head straight up: a roof, eaves, an overhang or a bridge.
	const auto position = a_actor->GetPosition();
	const float top = position.z + std::max(a_actor->GetHeight(), 32.0f) + 16.0f;
	const RE::NiPoint3 from{ position.x, position.y, top };
	const RE::NiPoint3 to{ position.x, position.y, top + 3000.0f };
	bool hit = false;
	const bool timed = Util::WorldTiming::On();  // (46l) measurement only
	const uint64_t pickStart = timed ? Util::WorldTiming::Stamp() : 0;
	const bool picked = PickUpSafe(world, &from, &to, &hit);
	if (timed)
		Util::WorldTiming::AddShelterPick(pickStart);
	if (picked)
		a_snow.sheltered = hit;
	else
		a_snow.shelterQpc = 0;  // not tested (physics busy, or a fault): the last answer, asked again next frame
	return a_snow.sheltered;
}

float4 Skin::UpdateActorSnow(ActorSnow& a_snow, RE::Actor* a_actor, const WaterSample& a_water)
{
	const auto& snow = globals::features::dynamicSnow;
	auto* calendar = RE::Calendar::GetSingleton();
	const double now = calendar ? static_cast<double>(calendar->GetDaysPassed()) * 24.0 : 0.0;
	auto* cell = a_actor->GetParentCell();
	const bool exterior = cell && cell->IsExteriorCell() && snow.status.exterior;
	const float cap = snow.DrawnAmount();
	const float feetZ = a_water.feetZ;

	if (!exterior) {
		// Indoors: no snow, and back outside it starts from nothing.
		a_snow.amount = 0.0f;
		a_snow.washed = 0.0f;
		a_snow.washDepth = 0.0f;
		a_snow.sheltered = false;
		a_snow.shelterQpc = 0;
		a_snow.hours = now;
		return float4(0.0f, 0.0f, 0.0f, feetZ);
	}
	const auto& s = snow.settings;
	// (46d) Under a roof: no build-up (the snow already there stays, melting as usual when it does not snow).
	const bool sheltered = s.CharacterSnowRoofs && IsSheltered(a_actor, a_snow, a_actor->IsPlayerRef() ? 250.0 : 1000.0 + static_cast<double>(a_actor->GetFormID() % 97));
	if (a_snow.hours < 0.0) {
		// First seen outdoors (a load, a character walking into view, the switch turned on): assume they
		// have been out as long as the ground has (none under a roof).
		a_snow.amount = sheltered ? 0.0f : cap;
		a_snow.washed = a_snow.amount;
		a_snow.washDepth = 0.0f;
	} else {
		// Same rates as the global amount; a step back in time (an earlier save) is no step.
		const float hours = static_cast<float>(std::clamp(now - a_snow.hours, 0.0, 1e4));
		const float buildRate = 1.0f / std::max(s.AccumulationHours, 0.01f);
		float delta = snow.status.snowing ? hours * snow.status.snowfall * buildRate : -hours / std::max(s.MeltHours, 0.01f);
		if (sheltered)
			delta = std::min(delta, 0.0f);
		a_snow.amount = std::clamp(a_snow.amount + delta, 0.0f, cap);
		// The debug override shows its amount at once.
		if (s.OverrideAmount)
			a_snow.amount = cap;
		// (46d) The washed part builds up again at the build-up rate whenever snow builds up (or the override is
		// on: it shows the build-up), else it melts with the rest; never above the body's amount.
		float washedDelta = delta;
		if (!sheltered && s.OverrideAmount)
			washedDelta = hours * buildRate;
		a_snow.washed = std::clamp(a_snow.washed + washedDelta, 0.0f, a_snow.amount);
	}
	a_snow.hours = now;

	// (46d) In the water: the part below the current local surface has no snow (the line follows the surface);
	// the deepest validated line is kept for after leaving, until the washed part has built up again.
	float washLine = 0.0f;
	if (s.CharacterSnowWaterWash) {
		if (a_water.inWater && a_water.depth > 0.0f) {
			a_snow.washDepth = std::max(a_snow.washDepth, a_water.depth);
			a_snow.washed = 0.0f;
			washLine = a_water.depth;
		} else {
			if (a_snow.washDepth > 0.0f && a_snow.washed >= a_snow.amount - 1e-3f)
				a_snow.washDepth = 0.0f;  // built back up to the rest of the body: one amount again
			washLine = a_snow.washDepth;
		}
	} else {
		a_snow.washDepth = 0.0f;
	}
	if (washLine <= 0.0f && a_snow.washDepth <= 0.0f)
		a_snow.washed = a_snow.amount;
	return float4(a_snow.amount, a_snow.washed, washLine, feetZ);
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

void Skin::PruneActorWetStates(uint32_t a_frame)
{
	// (44b) Drop the characters not drawn for a while first (their state is rebuilt from the game
	// when they are drawn again); if every entry is recent, start over (upstream 176a00831).
	std::erase_if(actorWetStates, [a_frame](const auto& a_entry) { return a_frame - a_entry.second.frame > kActorWetStaleFrames; });
	if (actorWetStates.size() >= kMaxActorWetStates)
		actorWetStates.clear();
}

float4 Skin::GetWetness(RE::BSGeometry* geometry, float4& o_wetSkin, float4& o_wetChar)
{
	o_wetSkin = float4(0.0f, 0.0f, 0.0f, 0.0f);
	o_wetChar = float4(1e6f, 0.0f, 0.0f, 0.0f);
	lastWetState = nullptr;
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
		lastWetState = &it->second;
		o_wetSkin = it->second.wetSkin;
		o_wetChar = it->second.wetChar;
		return it->second.skin;
	}
	// (46l) World-render split: one character's wetness / snow computed this frame (measurement only).
	struct ActorTimer
	{
		bool on = Util::WorldTiming::On();
		uint64_t start = on ? Util::WorldTiming::Stamp() : 0;
		~ActorTimer()
		{
			if (on)
				Util::WorldTiming::AddSkinActor(start);
		}
	} actorTimer;
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

	// (46d) The local water surface (never the cell's default water plane), shared with the snow.
	const WaterSample water = SampleWater(actor);
	const bool inWater = water.inWater;
	if (inWater)
		actorInWaterFrame = frame;  // (47c) the see-through-clothes depth copy is needed
	const float positionZ = water.feetZ;
	const float depth = water.depth;

	// (46a) Per-character snow (Dynamic Snow). The player's is advanced in Prepass every frame.
	if (globals::features::dynamicSnow.CharacterSnowStateActive()) {
		state.snowGPU = actor->IsPlayerRef() ? playerSnowGPU : UpdateActorSnow(state.snow, actor, water);
	} else {
		state.snow = {};
		state.snowGPU = float4(-1.0f, 0.0f, 0.0f, 0.0f);
	}

	// (44a) Wet Skin New model: in the water = 1; out of it, held for HoldTime, then drying off over
	// DryTime (smooth). The deepest submersion is kept until the character is dry, so a character
	// who climbed out stays wet up to where the water reached.
	{
		const auto& wet = globals::features::wetSkin.settings;
		const float hold = std::max(wet.HoldTime, 0.0f);
		const float dry = std::max(wet.DryTime, 1.0f);
		// (44c) Everything (hair, the slowest gear class) is dry after this.
		const float allDry = (hold + dry) * globals::features::wetSkin.MaxDryMultiplier();
		float swim = 0.0f;
		if (inWater) {
			state.timeOut = 0.0f;
			state.maxDepth = std::max(state.maxDepth, depth);
			swim = 1.0f;
		} else {
			state.timeOut = std::min(state.timeOut + dt, 1e6f);
			const float t = std::clamp((state.timeOut - hold) / dry, 0.0f, 1.0f);
			swim = 1.0f - t * t * (3.0f - 2.0f * t);
			// (44c) The water line is kept until the slowest class (hair, cloth ...) is dry too. (47f) Physical: below.
			if (state.timeOut >= allDry && !globals::features::wetSkin.IsPhysical())
				state.maxDepth = 0.0f;
		}
		state.wetSkin = float4(swim, state.maxDepth, inWater ? 1.0f : 0.0f, 0.0f);
		// (44c) Hair and gear dry at their own pace: the shader gets the time since leaving the water
		// (held at "all dry" afterwards, so the constant buffer stops changing).
		state.wetChar = float4(inWater ? 0.0f : std::min(state.timeOut, allDry + 1.0f), 0.0f, inWater ? 1.0f : 0.0f, 0.0f);
	}

	// Sweat and the Original model's swim wetness (Advanced Skin's WetFadeTime fade). (47i) The sweat thresholds and Dynamic
	// Wetness live on the Character Wetness page now (same meaning, every model).
	float4 wetness = float4(0.0f, 0.0f, positionZ, 0.0f);
	const auto& sweatSettings = globals::features::wetSkin.settings;
	if (sweatSettings.UseDynamicWetness && isDynamicWetnessAvailable) {
		wetness.x = SWE::API::GetFinalWetness(actor);
	} else {
		const float stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		const float permanentStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		const float temporaryStamina = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);
		const float maxStamina = permanentStamina + temporaryStamina;
		const float staminaPercentage = actor->IsDead() ? 1.0f : (maxStamina > 0.0f ? stamina / maxStamina : 1.0f);
		wetness.x = (staminaPercentage >= sweatSettings.StartSweat) ? 0.0f :
		            (staminaPercentage <= sweatSettings.FullSweat)  ? 1.0f :
		                                                              (sweatSettings.StartSweat - staminaPercentage) / (sweatSettings.StartSweat - sweatSettings.FullSweat);
	}
	const float exertion = std::isfinite(wetness.x) ? std::clamp(wetness.x, 0.0f, 1.0f) : 0.0f;  // (47f) Physical: the sweat source
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

	// (47f) Character Wetness Physical: sweat, rain and swim water advanced here, once per character per frame, and
	// turned into the per-source look the shader gets (BSLightingShader_SetupGeometry).
	if (auto& wetSkin = globals::features::wetSkin; wetSkin.IsPhysical()) {
		const auto& env = wetSkin.GetPhysicalFrame();
		const auto* cell = actor->GetParentCell();
		const bool outdoors = cell && !cell->IsInteriorCell();
		WetSkin::PhysicalInput in;
		in.dt = dt;
		in.exertion = exertion;
		// (47i) The local surface's depth above the feet (each height band of the body keeps its own swim water).
		in.depth = inWater ? std::max(water.depth, 0.0f) : 0.0f;
		in.height = std::max(actor->GetHeight(), 32.0f);
		in.outdoors = outdoors;
		// Open sky: a Havok ray up from the head (player every 0.25 s, others every 1 s), only while it rains.
		in.exposure = (outdoors && env.rainIntensity > 0.0f && !IsSheltered(actor, state.shelter, actor->IsPlayerRef() ? 250.0 : 1000.0)) ? 1.0f : 0.0f;
		float cold = outdoors ? env.snowing : 0.0f;
		if (outdoors && globals::features::dynamicSnow.loaded) {
			const auto pos = actor->GetPosition();
			const float mapCold = globals::features::dynamicSnow.ColdnessAt(pos.x, pos.y, pos.z);
			if (mapCold >= 0.0f)
				cold = std::max(cold, mapCold);
		}
		in.cold = cold;
		// (47l) Water Line per Limb: how far up each limb is under the water, from its bones (a humanoid with usable limb chains; else
		// the one line by the world height).
		if (const auto& ws = wetSkin.settings; ws.PhysWaterLine && ws.PhysBodyLine && ws.PhysLimbLines)
			in.limbs = UpdateBodyLimbs(actor, state, water, in);
		wetSkin.UpdatePhysical(state.phys, in);
		state.look = wetSkin.GetPhysicalLook(state.phys);
		state.maxDepth = 0.0f;  // (47i) the swim water is in the height bands
		state.skin.x = state.look.sweatLevel;
		state.wetSkin = float4(0.0f, 0.0f, inWater ? 1.0f : 0.0f, 0.0f);
		state.wetChar = float4(1e6f, 0.0f, inWater ? 1.0f : 0.0f, 0.0f);
	} else {
		state.phys.valid = false;  // a later switch to Physical starts afresh
	}

	lastWetState = &state;
	o_wetSkin = state.wetSkin;
	o_wetChar = state.wetChar;
	return state.skin;
}

namespace
{
	/// (44f) Item class from an item's keywords: 0 unknown, 1 cloth, 2 leather, 3 metal, 4 fur.
	/// Material keywords first (leather words before metal words: "ImperialStudded" is studded
	/// leather), then clothing / jewelry, then light / heavy armour as a weak fallback.
	uint8_t ClassifyItemKeywords(RE::TESForm* a_item)
	{
		if (!a_item)
			return 0;
		if (auto weapon = a_item->As<RE::TESObjectWEAP>())
			return (weapon->IsBow() || weapon->IsCrossbow() || weapon->IsStaff()) ? 2 : 3;  // wood / leather grips; blades
		auto keywordForm = a_item->As<RE::BGSKeywordForm>();
		if (!keywordForm)
			return 0;
		static constexpr std::array<std::string_view, 12> leatherWords = { "Leather", "Hide", "Studded", "Scaled", "Stormcloak", "Thieves", "Dragonscale", "Forsworn", "Chitin", "Fur", "DarkBrotherhood", "Nightingale" };
		static constexpr std::array<std::string_view, 16> metalWords = { "Iron", "Steel", "Dwarven", "Orcish", "Ebony", "Daedric", "Dragonplate", "Nordic", "Stalhrim", "Elven", "Glass", "Imperial", "Blades", "Dawnguard", "Falmer", "Bonemold" };
		uint8_t material = 0;
		bool clothing = false, jewelry = false, light = false, heavy = false;
		keywordForm->ForEachKeyword([&](RE::BGSKeyword* a_keyword) {
			if (!a_keyword)
				return RE::BSContainer::ForEachResult::kContinue;
			const std::string_view id = a_keyword->GetFormEditorID() ? a_keyword->GetFormEditorID() : "";
			if (id.empty())
				return RE::BSContainer::ForEachResult::kContinue;
			if (material == 0 && id.find("ArmorMaterial") != std::string_view::npos) {
				for (const auto word : leatherWords) {
					if (id.find(word) != std::string_view::npos) {
						material = 2;
						break;
					}
				}
				if (material == 0) {
					for (const auto word : metalWords) {
						if (id.find(word) != std::string_view::npos) {
							material = 3;
							break;
						}
					}
				}
			} else if (id.find("Clothing") != std::string_view::npos) {
				clothing = true;
			} else if (id.find("Jewelry") != std::string_view::npos) {
				jewelry = true;
			} else if (id == "ArmorLight") {
				light = true;
			} else if (id == "ArmorHeavy") {
				heavy = true;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		if (material != 0)
			return material;
		if (clothing)
			return 1;
		if (jewelry)
			return 3;
		if (heavy)
			return 3;
		if (light)
			return 2;
		// A wig or other hair-slot-only item without any armour keyword: hair-like (fur parameters).
		if (auto biped = a_item->As<RE::BGSBipedObjectForm>()) {
			const bool hairSlot = biped->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kHair) || biped->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kLongHair);
			if (hairSlot && !biped->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody))
				return 4;
		}
		return 0;
	}
}

// ---------------------------------------------------------------------------------------------
// (47k) "Physical: Water Line Follows the Body"
// ---------------------------------------------------------------------------------------------

namespace
{
	/// The skin instance's bone list and bind data (SEH: a mod can leave them dangling; the RayTracing Characters.cpp pattern).
	struct BodySkinRefs
	{
		RE::NiAVObject** bones = nullptr;
		RE::NiSkinData::BoneData* boneData = nullptr;
		std::uint32_t boneCount = 0;
	};

	bool ReadBodySkinRefs(RE::NiSkinInstance* a_skin, BodySkinRefs& o_refs)
	{
		__try {
			RE::NiSkinData* data = a_skin->skinData.get();
			o_refs.bones = a_skin->bones;
			if (!data || !o_refs.bones)
				return false;
			o_refs.boneCount = data->bones;
			o_refs.boneData = data->boneData;
			return o_refs.boneData != nullptr && o_refs.boneCount > 0 && o_refs.boneCount <= 1024;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	/// Index of the bone called a_name in the skin's list (-1 = none, also on a bad read).
	int32_t FindBodyBone(const BodySkinRefs& a_refs, const char* a_name)
	{
		__try {
			for (std::uint32_t i = 0; i < a_refs.boneCount; ++i) {
				const RE::NiAVObject* node = a_refs.bones[i];
				if (!node)
					continue;
				const char* name = node->name.c_str();
				if (name && std::strcmp(name, a_name) == 0)
					return static_cast<int32_t>(i);
			}
			return -1;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	/// A bone's bind-pose joint in the skin's (model) space: the origin of its bone space through skin-to-bone inverted
	/// (x = -R^T t / s), and that scale.
	bool ReadBindJoint(const BodySkinRefs& a_refs, int32_t a_index, float o_joint[3], float& o_scale)
	{
		if (a_index < 0 || static_cast<std::uint32_t>(a_index) >= a_refs.boneCount)
			return false;
		__try {
			const RE::NiTransform& s = a_refs.boneData[a_index].skinToBone;
			if (!(s.scale > 1e-4f) || !(s.scale < 1e4f))
				return false;
			const float t[3] = { s.translate.x, s.translate.y, s.translate.z };
			for (int c = 0; c < 3; ++c)
				o_joint[c] = -(s.rotate.entry[0][c] * t[0] + s.rotate.entry[1][c] * t[1] + s.rotate.entry[2][c] * t[2]) / s.scale;
			o_scale = s.scale;
			for (int c = 0; c < 3; ++c)
				if (!(std::fabs(o_joint[c]) < 1e5f))
					return false;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	/// A bone's world scale and Z this frame.
	bool ReadBoneWorld(const BodySkinRefs& a_refs, int32_t a_index, float& o_scale, float& o_z)
	{
		if (a_index < 0 || static_cast<std::uint32_t>(a_index) >= a_refs.boneCount)
			return false;
		__try {
			const RE::NiAVObject* node = a_refs.bones[a_index];
			if (!node)
				return false;
			o_scale = node->world.scale;
			o_z = node->world.translate.z;
			return std::isfinite(o_scale) && std::isfinite(o_z) && o_scale > 1e-4f && o_scale < 1e4f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	/// Whether a_object hangs below a_root (SEH: parents of a detached part may be gone).
	bool BodyIsUnder(const RE::NiAVObject* a_object, const RE::NiAVObject* a_root)
	{
		if (!a_object || !a_root)
			return false;
		__try {
			int depth = 0;
			for (const RE::NiAVObject* node = a_object; node && depth < 96; node = node->parent, ++depth)
				if (node == a_root)
					return true;
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool ReadNodeWorldZ(const RE::NiAVObject* a_node, float& o_z)
	{
		if (!a_node)
			return false;
		__try {
			o_z = a_node->world.translate.z;
			return std::isfinite(o_z);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// The central bones, in order of preference (an individually scaled head late).
	constexpr const char* kBodyCentralBones[] = { "NPC Pelvis [Pelv]", "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
		"NPC L Thigh [LThg]", "NPC R Thigh [RThg]", "NPC L Calf [LClf]", "NPC R Calf [RClf]", "NPC Neck [Neck]", "NPC Head [Head]",
		"NPC L UpperArm [LUar]", "NPC R UpperArm [RUar]", "NPC L Forearm [LLar]", "NPC R Forearm [RLar]", "NPC L Foot [Lft ]",
		"NPC R Foot [Rft ]" };
	constexpr const char* kBodyShoulder = "NPC L UpperArm [LUar]";
	constexpr int32_t kBodyOwnScaleRanks = 8;  // pelvis, spines, thighs, calves: their own world scale; the rest: the pelvis's

	bool ReadNodeWorldScale(const RE::NiAVObject* a_node, float& o_scale)
	{
		if (!a_node)
			return false;
		__try {
			o_scale = a_node->world.scale;
			return std::isfinite(o_scale) && o_scale > 1e-4f && o_scale < 1e4f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
	constexpr std::size_t kMaxBodyGeometries = 256;
}

void Skin::GetBodyFrame(RE::BSGeometry* a_geometry, RE::Actor* a_actor, ActorWetState& a_state, float4& o_body, float4& o_body2)
{
	o_body = float4(0.0f, 0.0f, 0.0f, 0.0f);
	o_body2 = float4(0.0f, 0.0f, 0.0f, 0.0f);
	const uint32_t frame = globals::state->frameCount;
	if (frame != bodyLineStatsFrame) {
		bodyLineStats = bodyLineCounting;
		bodyLineCounting = {};
		bodyLineStatsFrame = frame;
	}
	struct Count
	{
		Skin* self;
		const float4& body;
		~Count()
		{
			if (body.x > 0.0f)
				++self->bodyLineCounting.body;
			else
				++self->bodyLineCounting.world;
		}
	} count{ this, o_body };
	if (!a_geometry || !a_actor)
		return;
	// Rigid gear (helmets, shields, weapons): its model space is not the body's.
	RE::NiSkinInstance* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
	if (!skin)
		return;

	// Per character, once per frame: NPC Root (high heels), the humanoid check (pelvis, calf, foot; re-checked when its 3D changes
	// and every 300 frames), the feet and the height.
	if (!RefreshBodyActor(a_actor, a_state, frame))
		return;
	auto& body = a_state.body;
	// (47l) Water Line per Limb: a character without usable limb chains keeps the world height everywhere (never mixed); the
	// body / world decision of a geometry is sticky (BodyGeometry::failCount), and no root offset (the lines are in body units).
	const auto& wetSettings = globals::features::wetSkin.settings;
	const bool limbs = wetSettings.PhysWaterLine && wetSettings.PhysLimbLines;
	if (limbs && !body.limbs.valid)
		return;

	// Per geometry: its central bone, its bind height and skin-to-bone scale, the shoulder (rebuilt for another skin instance).
	if (a_state.bodyGeometry.size() >= kMaxBodyGeometries)
		a_state.bodyGeometry.clear();
	auto& geo = a_state.bodyGeometry[a_geometry];
	geo.usedFrame = frame;
	BodySkinRefs refs;
	if (!ReadBodySkinRefs(skin, refs)) {
		geo = {};
		return;
	}
	if (geo.skin != skin) {
		geo = {};
		geo.skin = skin;
		geo.usedFrame = frame;
		// First person (the player's first-person 3D): its arms are not where the third-person body's line is.
		const bool firstPerson = a_actor->IsPlayerRef() && BodyIsUnder(a_geometry, a_actor->Get3D(true));
		if (!firstPerson) {
			for (int32_t rank = 0; rank < static_cast<int32_t>(std::size(kBodyCentralBones)); ++rank) {
				const int32_t index = FindBodyBone(refs, kBodyCentralBones[rank]);
				float joint[3], scale;
				if (index >= 0 && ReadBindJoint(refs, index, joint, scale) && joint[2] > 5.0f) {
					geo.refBone = index;
					geo.refRank = rank;
					geo.refBindZ = joint[2];
					geo.stbScale = scale;
					geo.usable = true;
					break;
				}
			}
			const int32_t shoulder = FindBodyBone(refs, kBodyShoulder);
			float joint[3], scale;
			if (shoulder >= 0 && ReadBindJoint(refs, shoulder, joint, scale) && joint[2] > 10.0f && std::fabs(joint[0]) > 1.0f) {
				geo.shoulderKnown = true;
				geo.shoulderX = std::fabs(joint[0]);
				geo.shoulderZ = joint[2];
				body.shoulderKnown = true;  // the geometries without that bone (gloves, bracers) use the body's
				body.shoulderX = geo.shoulderX;
				body.shoulderZ = geo.shoulderZ;
			}
		}
	}
	if (!geo.usable || geo.stuckWorld)
		return;

	// s = the central bone's bone-matrix scale (its world scale x skin-to-bone scale: NPC / race scale included).
	float s = 0.0f;
	bool ok = false;
	float worldScale = 1.0f, boneZ = 0.0f;
	if (ReadBoneWorld(refs, geo.refBone, worldScale, boneZ)) {
		// A geometry with only the neck / head / arms / feet (hair, head parts, gloves, boots) takes the pelvis's world scale: a head or
		// limb bone can be scaled on its own (RaceMenu), the body is not.
		if (geo.refRank >= kBodyOwnScaleRanks && body.pelvisScale > 0.0f)
			worldScale = body.pelvisScale;
		s = worldScale * geo.stbScale;
		// The model space must be the body's: the central bone's bind height there (as a body height) roughly where the bone is now
		// (crouching, sitting, swimming still pass; another space, e.g. one around the head, does not).
		const float predicted = s * geo.refBindZ + body.rootOffset;
		const float actual = boneZ - body.feetZ;
		ok = s > 0.05f && s < 20.0f && std::fabs(predicted - actual) < 0.5f * body.height + 20.0f;
	}
	if (!limbs) {
		if (!ok)
			return;
	} else if (ok) {
		geo.failCount = 0;
		geo.lastScale = s;
	} else {
		// (47l) A failed check keeps the body height (last good scale) for up to kBodyFailLimit draws in a row; then the world height
		// until the character's next lookup. A geometry that never passed has no scale: the world height until it does.
		if (++geo.failCount >= kBodyFailLimit)
			geo.stuckWorld = true;
		if (geo.stuckWorld || !(geo.lastScale > 0.0f))
			return;
		s = geo.lastScale;
	}
	const bool own = geo.shoulderKnown;
	const bool shoulderKnown = own || body.shoulderKnown;
	o_body = float4(s, limbs ? 0.0f : body.rootOffset, shoulderKnown ? (own ? geo.shoulderX : body.shoulderX) : 0.0f, shoulderKnown ? (own ? geo.shoulderZ : body.shoulderZ) : 0.0f);
	o_body2 = float4(body.height, 0.0f, 0.0f, 0.0f);
	if (limbs) {
		const auto& l = body.limbs;
		o_body2 = float4(body.height, l.crotchZ, l.legSign, l.armSign);
	}
}

// ---------------------------------------------------------------------------------------------
// (47l) "Physical: Water Line per Limb (bones)"
// ---------------------------------------------------------------------------------------------

namespace
{
	enum LimbJoint : uint32_t
	{
		kLFoot,
		kLCalf,
		kLThigh,
		kRFoot,
		kRCalf,
		kRThigh,
		kPelvis,
		kSpine0,
		kSpine1,
		kSpine2,
		kNeck,
		kHead,
		kLUpper,
		kLFore,
		kLHand,
		kRUpper,
		kRFore,
		kRHand
	};
	constexpr const char* kLimbBones[Skin::BodyLimbs::kJoints] = { "NPC L Foot [Lft ]", "NPC L Calf [LClf]", "NPC L Thigh [LThg]",
		"NPC R Foot [Rft ]", "NPC R Calf [RClf]", "NPC R Thigh [RThg]", "NPC Pelvis [Pelv]", "NPC Spine [Spn0]", "NPC Spine1 [Spn1]",
		"NPC Spine2 [Spn2]", "NPC Neck [Neck]", "NPC Head [Head]", "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
		"NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]" };

	/// The nodes' world positions (SEH: a node of a 3D being torn down). A null node: false.
	bool ReadLimbPositions(const std::array<RE::NiPointer<RE::NiAVObject>, Skin::BodyLimbs::kJoints>& a_nodes, bool a_neck, bool a_head,
		std::array<RE::NiPoint3, Skin::BodyLimbs::kJoints>& o_pos)
	{
		__try {
			for (uint32_t j = 0; j < Skin::BodyLimbs::kJoints; ++j) {
				if ((j == kNeck && !a_neck) || (j == kHead && !a_head))
					continue;
				const RE::NiAVObject* node = a_nodes[j].get();
				if (!node)
					return false;
				o_pos[j] = node->world.translate;
				if (!std::isfinite(o_pos[j].x) || !std::isfinite(o_pos[j].y) || !std::isfinite(o_pos[j].z))
					return false;
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	/// The highest body height under the water surface along a chain of points (world z, body height), feet up: the joints under it
	/// and where a segment crosses it (0 = none under).
	float LimbChainDepth(const float* a_z, const float* a_body, uint32_t a_count, float a_surface)
	{
		float d = 0.0f;
		for (uint32_t k = 0; k < a_count; ++k) {
			if (a_z[k] < a_surface)
				d = std::max(d, a_body[k]);
			if (k + 1 < a_count && (a_z[k] < a_surface) != (a_z[k + 1] < a_surface)) {
				const float dz = a_z[k + 1] - a_z[k];
				const float t = std::fabs(dz) > 1e-4f ? std::clamp((a_surface - a_z[k]) / dz, 0.0f, 1.0f) : 0.5f;
				d = std::max(d, a_body[k] + (a_body[k + 1] - a_body[k]) * t);
			}
		}
		return d;
	}

	RE::NiPoint3 LimbDir(const RE::NiPoint3& a_to, const RE::NiPoint3& a_from)
	{
		RE::NiPoint3 d = a_to - a_from;
		const float len = d.Length();
		return len > 1e-3f ? d / len : RE::NiPoint3(0.0f, 0.0f, 1.0f);
	}

	/// The character's limb chains: the bones' bind joints from the skin data of its geometries (the first that has each bone, in the
	/// body's model space by the 47k check), their nodes, and the plausibility of every chain. Leaves a_limbs.valid false otherwise.
	void LookupBodyLimbs(RE::Actor* a_actor, RE::NiAVObject* a_root, const Skin::BodyActor& a_body, Skin::BodyLimbs& o_limbs)
	{
		o_limbs = {};
		std::array<std::array<float, 3>, Skin::BodyLimbs::kJoints> bind{};
		std::array<bool, Skin::BodyLimbs::kJoints> found{};
		float pelvisStb = 0.0f;
		uint32_t missing = Skin::BodyLimbs::kJoints;
		RE::NiAVObject* firstPerson = a_actor->IsPlayerRef() ? a_actor->Get3D(true) : nullptr;
		RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geom) {
			if (!a_geom || (firstPerson && BodyIsUnder(a_geom, firstPerson)))
				return RE::BSVisit::BSVisitControl::kContinue;
			RE::NiSkinInstance* skin = a_geom->GetGeometryRuntimeData().skinInstance.get();
			BodySkinRefs refs;
			if (!skin || !ReadBodySkinRefs(skin, refs))
				return RE::BSVisit::BSVisitControl::kContinue;
			for (uint32_t j = 0; j < Skin::BodyLimbs::kJoints; ++j) {
				if (found[j])
					continue;
				const int32_t index = FindBodyBone(refs, kLimbBones[j]);
				float joint[3], scale;
				if (index < 0 || !ReadBindJoint(refs, index, joint, scale))
					continue;
				// The model space must be the body's: the bind height (as a body height) roughly where the bone is now (47k check).
				float worldScale = 0.0f, boneZ = 0.0f;
				if (!ReadBoneWorld(refs, index, worldScale, boneZ))
					continue;
				const float s = (a_body.pelvisScale > 0.0f ? a_body.pelvisScale : worldScale) * scale;
				if (!(s > 0.05f && s < 20.0f) || !(std::fabs(s * joint[2] + a_body.rootOffset - (boneZ - a_body.feetZ)) < 0.5f * a_body.height + 20.0f))
					continue;
				bind[j] = { joint[0], joint[1], joint[2] };
				found[j] = true;
				if (j == kPelvis)
					pelvisStb = scale;
				--missing;
			}
			return missing ? RE::BSVisit::BSVisitControl::kContinue : RE::BSVisit::BSVisitControl::kStop;
		});
		for (uint32_t j = 0; j < Skin::BodyLimbs::kJoints; ++j) {
			if (j != kNeck && j != kHead && !found[j])
				return;
		}
		const auto z = [&](uint32_t j) { return bind[j][2]; };
		const auto x = [&](uint32_t j) { return bind[j][0]; };
		// Legs: foot < calf < thigh, the thighs on opposite sides; the torso rising from the pelvis.
		for (const uint32_t f : { kLFoot, kRFoot }) {
			if (!(z(f) > 0.0f && z(f + 1) > z(f) + 5.0f && z(f + 2) > z(f + 1) + 5.0f))
				return;
		}
		if (!(x(kLThigh) * x(kRThigh) < 0.0f && std::fabs(x(kLThigh)) > 1.0f && std::fabs(x(kRThigh)) > 1.0f))
			return;
		const float thighZ = 0.5f * (z(kLThigh) + z(kRThigh)), calfZ = 0.5f * (z(kLCalf) + z(kRCalf));
		if (!(z(kPelvis) > thighZ - 10.0f && z(kSpine0) > z(kPelvis) - 2.0f && z(kSpine1) > z(kSpine0) && z(kSpine2) > z(kSpine1)))
			return;
		o_limbs.hasNeck = found[kNeck] && z(kNeck) > z(kSpine2);
		o_limbs.hasHead = found[kHead] && z(kHead) > (o_limbs.hasNeck ? z(kNeck) : z(kSpine2));
		// Arms: the left shoulder (|x|, z) as the 47k remap; both upper arms there, elbow and wrist lower when hanging.
		const float shoulderX = std::fabs(x(kLUpper)), shoulderZ = z(kLUpper);
		if (!(shoulderZ > z(kSpine0) && shoulderX > 1.0f && x(kLUpper) * x(kRUpper) < 0.0f))
			return;
		const auto remap = [&](uint32_t j) {
			const float ax = std::max(std::fabs(x(j)) - shoulderX, 0.0f), dz = z(j) - shoulderZ;
			return shoulderZ - std::sqrt(ax * ax + dz * dz);
		};
		for (const uint32_t u : { kLUpper, kRUpper }) {
			if (!(remap(u) > remap(u + 1) + 3.0f && remap(u + 1) > remap(u + 2) + 3.0f))
				return;
		}
		if (!(pelvisStb > 1e-4f))
			return;
		// The nodes (world positions per frame).
		for (uint32_t j = 0; j < Skin::BodyLimbs::kJoints; ++j) {
			if ((j == kNeck && !o_limbs.hasNeck) || (j == kHead && !o_limbs.hasHead))
				continue;
			const RE::BSFixedString name(kLimbBones[j]);
			o_limbs.nodes[j].reset(a_root->GetObjectByName(name));
			if (!o_limbs.nodes[j]) {
				if (j == kNeck || j == kHead) {
					(j == kNeck ? o_limbs.hasNeck : o_limbs.hasHead) = false;
					continue;
				}
				o_limbs = {};
				return;
			}
		}
		for (uint32_t j = 0; j < Skin::BodyLimbs::kJoints; ++j)
			o_limbs.bindH[j] = (j >= kLUpper) ? remap(j) : z(j);
		o_limbs.pelvisStb = pelvisStb;
		o_limbs.crotchZ = thighZ - 0.25f * (thighZ - calfZ);
		const auto dist = [&](uint32_t a, uint32_t b) {
			const float dx = x(a) - x(b), dy = bind[a][1] - bind[b][1], dz = z(a) - z(b);
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};
		o_limbs.fingerLen = 0.7f * 0.5f * (dist(kLFore, kLHand) + dist(kRFore, kRHand));
		o_limbs.legSign = x(kLThigh) > 0.0f ? 1.0f : -1.0f;
		o_limbs.armSign = x(kLUpper) > 0.0f ? 1.0f : -1.0f;
		o_limbs.shoulderX = shoulderX;
		o_limbs.shoulderZ = shoulderZ;
		o_limbs.valid = true;
	}
}

bool Skin::RefreshBodyActor(RE::Actor* a_actor, ActorWetState& a_state, uint32_t a_frame)
{
	auto& body = a_state.body;
	RE::NiAVObject* root = a_actor ? a_actor->Get3D(false) : nullptr;
	if (!root)
		return false;
	const auto& wetSettings = globals::features::wetSkin.settings;
	const bool limbsWanted = wetSettings.PhysWaterLine && wetSettings.PhysBodyLine && wetSettings.PhysLimbLines;
	bool lookedUp = false;
	if (body.root != root || body.lookupFrame == UINT32_MAX || a_frame - body.lookupFrame > 300u) {
		static const RE::BSFixedString kRoot("NPC Root [Root]");
		static const RE::BSFixedString kPelvis("NPC Pelvis [Pelv]");
		static const RE::BSFixedString kCalf("NPC L Calf [LClf]");
		static const RE::BSFixedString kFoot("NPC L Foot [Lft ]");
		if (body.root != root) {
			body.shoulderKnown = false;
			a_state.bodyGeometry.clear();
		}
		body.root = root;
		body.lookupFrame = a_frame;
		body.npcRoot.reset(root->GetObjectByName(kRoot));
		body.pelvis.reset(root->GetObjectByName(kPelvis));
		body.humanoid = body.npcRoot && body.pelvis && root->GetObjectByName(kCalf) && root->GetObjectByName(kFoot);
		// (47l) Every geometry decides body / world afresh (BodyGeometry::failCount), the limb chains are looked up again.
		for (auto& [geometry, geo] : a_state.bodyGeometry) {
			geo.failCount = 0;
			geo.stuckWorld = false;
		}
		body.limbs = {};
		body.frame = UINT32_MAX;
		lookedUp = true;
	}
	if (!body.humanoid)
		return false;
	if (body.frame != a_frame) {
		body.frame = a_frame;
		body.feetZ = a_actor->GetPositionZ();
		body.height = std::max(a_actor->GetHeight(), 32.0f);
		float rootZ = body.feetZ;
		body.rootOffset = ReadNodeWorldZ(body.npcRoot.get(), rootZ) ? std::clamp(rootZ - body.feetZ, -64.0f, 64.0f) : 0.0f;
		body.pelvisScale = ReadNodeWorldScale(body.pelvis.get(), body.pelvisScale) ? body.pelvisScale : 0.0f;
	}
	// (47l) The limb chains: at the lookup (or once the switch is turned on).
	if (limbsWanted && (lookedUp || !body.limbsLooked)) {
		body.limbsLooked = true;
		LookupBodyLimbs(a_actor, root, body, body.limbs);
	} else if (!limbsWanted) {
		body.limbsLooked = false;
	}
	return true;
}

bool Skin::UpdateBodyLimbs(RE::Actor* a_actor, ActorWetState& a_state, const WaterSample& a_water, WetSkin::PhysicalInput& o_in)
{
	const uint32_t frame = globals::state->frameCount;
	if (!RefreshBodyActor(a_actor, a_state, frame))
		return false;
	auto& body = a_state.body;
	auto& l = body.limbs;
	if (!l.valid)
		return false;
	// s: the pelvis's bone-matrix scale (the body geometry's s in the shader); a frame it cannot be read: the last one.
	const float sNow = body.pelvisScale * l.pelvisStb;
	if (sNow > 0.05f && sNow < 20.0f)
		l.lastScale = sNow;
	const float s = l.lastScale;
	if (!(s > 0.0f))
		return false;
	using Limb = uint32_t;
	constexpr Limb kLegL = 0, kLegR = 1, kTorso = 2, kArmL = 3, kArmR = 4;
	const float topH = std::max(body.height, s * (l.bindH[l.hasHead ? kHead : (l.hasNeck ? kNeck : kSpine2)] + 8.0f));
	const float tipL = l.bindH[kLHand] - l.fingerLen, tipR = l.bindH[kRHand] - l.fingerLen;
	o_in.limbLo = { 0.0f, 0.0f, s * l.crotchZ, s * tipL, s * tipR };
	o_in.limbHi = { s * l.bindH[kLThigh], s * l.bindH[kRThigh], topH, s * l.bindH[kLUpper], s * l.bindH[kRUpper] };
	o_in.limbDepth = {};
	// The surface: where the feet are in the water, the one the depth is measured to (clamped as the depth); else a local surface
	// below the feet (standing on the bank or a jetty) still wets a hand or a knee that reaches into it.
	float surface = -1e30f;
	if (a_water.inWater && a_water.depth > 0.0f)
		surface = a_water.feetZ + a_water.depth;
	else if (a_water.surfaceZ > -1e29f && a_water.surfaceZ < a_water.feetZ && a_water.surfaceZ > a_water.feetZ - body.height)
		surface = a_water.surfaceZ;
	if (surface <= -1e29f)
		return true;
	std::array<RE::NiPoint3, BodyLimbs::kJoints> p{};
	if (!ReadLimbPositions(l.nodes, l.hasNeck, l.hasHead, p)) {
		// Unreadable this frame: every limb by the world depth.
		const float depth = a_water.inWater ? std::max(a_water.depth, 0.0f) : 0.0f;
		for (Limb i = 0; i < WetSkin::Phys::kLimbs; ++i)
			o_in.limbDepth[i] = depth > o_in.limbLo[i] ? std::min(depth, o_in.limbHi[i]) : 0.0f;
		return true;
	}
	const auto b = [&](uint32_t j) { return s * l.bindH[j]; };
	// Legs: the sole (straight below the ankle by its bind height), ankle, knee, hip.
	const auto leg = [&](uint32_t a_foot) {
		const float zs[4] = { p[a_foot].z - b(a_foot), p[a_foot].z, p[a_foot + 1].z, p[a_foot + 2].z };
		const float hs[4] = { 0.0f, b(a_foot), b(a_foot + 1), b(a_foot + 2) };
		return LimbChainDepth(zs, hs, 4, surface);
	};
	o_in.limbDepth[kLegL] = leg(kLFoot);
	o_in.limbDepth[kLegR] = leg(kRFoot);
	// Torso / head: the crotch (between the thighs at its bind height), pelvis, spine, neck, head, the top of the head.
	{
		const auto atCrotch = [&](uint32_t a_thigh) {
			const float span = l.bindH[a_thigh] - l.bindH[a_thigh - 1];
			const float t = span > 1e-3f ? std::clamp((l.bindH[a_thigh] - l.crotchZ) / span, 0.0f, 1.0f) : 0.0f;
			return p[a_thigh] + (p[a_thigh - 1] - p[a_thigh]) * t;
		};
		const RE::NiPoint3 crotch = (atCrotch(kLThigh) + atCrotch(kRThigh)) * 0.5f;
		float zs[8], hs[8];
		uint32_t n = 0;
		const auto push = [&](float a_z, float a_h) {
			zs[n] = a_z;
			hs[n++] = a_h;
		};
		push(crotch.z, s * l.crotchZ);
		for (const uint32_t j : { kPelvis, kSpine0, kSpine1, kSpine2 })
			push(p[j].z, b(j));
		uint32_t lastJoint = kSpine2, prevJoint = kSpine1;
		for (const uint32_t j : { kNeck, kHead }) {
			if ((j == kNeck && !l.hasNeck) || (j == kHead && !l.hasHead))
				continue;
			push(p[j].z, b(j));
			prevJoint = lastJoint;
			lastJoint = j;
		}
		const RE::NiPoint3 top = p[lastJoint] + LimbDir(p[lastJoint], p[prevJoint]) * std::max(topH - b(lastJoint), 0.0f);
		push(top.z, topH);
		o_in.limbDepth[kTorso] = LimbChainDepth(zs, hs, n, surface);
	}
	// Arms: shoulder, elbow, wrist, the fingertips (the hand's length on along the forearm), by their hanging heights.
	const auto arm = [&](uint32_t a_upper, float a_tip) {
		const RE::NiPoint3 tip = p[a_upper + 2] + LimbDir(p[a_upper + 2], p[a_upper + 1]) * (s * l.fingerLen);
		const float zs[4] = { p[a_upper].z, p[a_upper + 1].z, p[a_upper + 2].z, tip.z };
		const float hs[4] = { b(a_upper), b(a_upper + 1), b(a_upper + 2), s * a_tip };
		return LimbChainDepth(zs, hs, 4, surface);
	};
	o_in.limbDepth[kArmL] = arm(kLUpper, tipL);
	o_in.limbDepth[kArmR] = arm(kRUpper, tipR);
	for (Limb i = 0; i < WetSkin::Phys::kLimbs; ++i)
		o_in.limbDepth[i] = std::isfinite(o_in.limbDepth[i]) ? std::clamp(o_in.limbDepth[i], 0.0f, o_in.limbHi[i]) : 0.0f;
	return true;
}

uint8_t Skin::GetItemClassPrior(RE::Actor* a_actor, ActorWetState& a_state, const RE::BSGeometry* a_geometry, uint32_t a_frame)
{
	if (a_state.itemClassValid && a_frame - a_state.itemClassFrame > 600)
		a_state.itemClassValid = false;
	if (a_state.itemClassValid) {
		if (auto it = a_state.itemClass.find(a_geometry); it != a_state.itemClass.end())
			return it->second;
	}
	// A geometry not seen yet (new equipment, a rebuilt 3D, or one that belongs to no worn item):
	// rebuild the map, at most every 15 frames per character, and start over every ~10 s so
	// entries of geometry that no longer exists do not pile up.
	const uint8_t fallback = a_state.animal ? 4 : 0;
	if (a_state.itemClassValid && a_frame - a_state.itemClassFrame < 15) {
		a_state.itemClass.emplace(a_geometry, fallback);
		return fallback;
	}
	a_state.itemClass.clear();
	a_state.itemClassFrame = a_frame;
	a_state.itemClassValid = true;
	const auto race = a_actor->GetRace();
	a_state.animal = race && race->HasKeywordString("ActorTypeAnimal");
	if (const auto& biped = a_actor->GetBiped(false)) {
		for (const auto& object : biped->objects) {
			if (!object.item || !object.partClone)
				continue;
			const uint8_t itemClass = ClassifyItemKeywords(object.item);
			if (itemClass == 0)
				continue;
			RE::BSVisit::TraverseScenegraphGeometries(object.partClone.get(), [&](RE::BSGeometry* a_geom) {
				a_state.itemClass.emplace(a_geom, itemClass);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}
	return a_state.itemClass.emplace(a_geometry, static_cast<uint8_t>(a_state.animal ? 4 : 0)).first->second;
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
		perGeometryData.skinPerGeometry = GetWetness(geometry, perGeometryData.wetSkinPerGeometry, perGeometryData.wetCharPerGeometry);
		// (44a) Wet Skin settings ride along (they change rarely; the buffer is only rewritten
		// when something in it changed).
		const auto& wetSkin = globals::features::wetSkin;
		// (47c) Skip Invisible Work: the settings are turned into GPU values once per frame (a change
		// in the menu shows from the next frame on); off: on every draw, as before.
		const uint32_t paramsFrame = globals::state->frameCount;
		if (!wetSkin.settings.SkipInvisibleWork || wetParamsCacheFrame != paramsFrame) {
			const auto params = wetSkin.GetGPUParams();
			for (int i = 0; i < 6; ++i)
				wetSkinParamsCache[i] = params.p[i];
			for (int i = 0; i < 12; ++i)
				wetCharParamsCache[i] = params.c[i];
			wetParamsCacheFrame = wetSkin.settings.SkipInvisibleWork ? paramsFrame : UINT32_MAX;
		}
		for (int i = 0; i < 6; ++i)
			perGeometryData.wetSkinParams[i] = wetSkinParamsCache[i];
		for (int i = 0; i < 12; ++i)
			perGeometryData.wetCharParams[i] = wetCharParamsCache[i];
		// (47f) Physical: this character's look. (47i) Its water amounts, bead parameters and swim height profile in
		// wetPhysParams / wetPhysBands; a few look values ride in the frame's parameters (Skin.hlsli).
		if (lastWetState && wetSkin.IsPhysical() && lastWetState->phys.valid) {
			const auto& look = lastWetState->look;
			auto& p = perGeometryData.wetSkinParams;
			auto& c = perGeometryData.wetCharParams;
			p[0].y = 0.0f;  // manual water is in wetPhysParams
			p[0].w = 0.0f;
			p[1].w = std::clamp(p[1].w * look.filmMult, 0.0f, 2.5f);
			p[3].y = std::clamp(p[3].y * 0.35f, 0.0f, 1.0f);  // narrow size spread
			p[4].x = std::clamp(p[4].x * look.beadTrails, 0.0f, 1.0f);
			p[5].z = std::clamp(p[5].z * look.dome, 0.0f, 2.0f);
			c[11].w = std::clamp(look.oil, 0.0f, 1.0f);
			for (size_t i = 0; i < look.params.size(); ++i)
				perGeometryData.wetPhysParams[i] = look.params[i];
			for (size_t i = 0; i < look.bands.size(); ++i)
				perGeometryData.wetPhysBands[i] = look.bands[i];
			// (47l) Water Line per Limb (zero otherwise).
			if (look.limbMode) {
				for (size_t i = 0; i < look.limbs.size(); ++i)
					perGeometryData.wetPhysLimbs[i] = look.limbs[i];
			}
			// (47k) Water Line Follows the Body: the bind-pose height of this geometry (zero: the world height, as 47j).
			if (wetSkin.settings.PhysBodyLine) {
				auto userData = geometry->GetUserData();
				if (auto actor = userData ? userData->As<RE::Character>() : nullptr)
					GetBodyFrame(geometry, actor, *lastWetState, perGeometryData.bodyPerGeometry, perGeometryData.bodyPerGeometry2);
			}
		}
		// (44f) Item class prior from the keywords of the worn item this geometry belongs to.
		if (lastWetState && !wetSkin.IsOriginal() && wetSkin.settings.GearEnabled && wetSkin.settings.GearModel == 0 && wetSkin.settings.GearKeywords) {
			auto userData = geometry->GetUserData();
			if (auto actor = userData ? userData->As<RE::Character>() : nullptr)
				perGeometryData.wetCharPerGeometry.y = static_cast<float>(GetItemClassPrior(actor, *lastWetState, geometry, globals::state->frameCount));
		}
		// (46a) This character's own snow (x < 0: none kept, the shader uses the global amount).
		perGeometryData.snowPerGeometry = lastWetState ? lastWetState->snowGPU : float4(-1.0f, 0.0f, 0.0f, 0.0f);
		// (46a) See-through clothes of a character in water, drawn after the water this frame.
		perGeometryData.waterPerGeometry = float4(0.0f, 0.0f, 0.0f, 0.0f);
		if (waterDepthSwapped)
			RestoreWaterDepth(context);  // a RestoreGeometry went missing: never carry our depth into another pass
		waterDepthPending = false;
		if (lastWetState && wetSkin.settings.TranslucentUnderWater && preWaterDSV && preWaterDepthFrame == globals::state->frameCount)
			perGeometryData.waterPerGeometry = GetWaterDepthParams(geometry);

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

// ---------------------------------------------------------------------------------------------
// (46a) See-through clothes in water
// ---------------------------------------------------------------------------------------------

void Skin::OnEndDeferred()
{
	endDeferredFrame = globals::state->frameCount;
}

float4 Skin::GetWaterDepthParams(RE::BSGeometry* a_geometry)
{
	auto userData = a_geometry->GetUserData();
	auto actor = userData ? userData->As<RE::Character>() : nullptr;
	if (!actor)
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	// Alpha-blended geometry only (as Extended Translucency tells it: a blending alpha property, or a
	// material alpha below 1, which the engine draws blended).
	const auto& runtime = a_geometry->GetGeometryRuntimeData();
	auto& property0 = runtime.properties[RE::BSGeometry::States::kProperty];
	auto& property1 = runtime.properties[RE::BSGeometry::States::kEffect];
	auto alphaProperty = property0 && property0->GetRTTI() == globals::rtti::NiAlphaPropertyRTTI.get() ? static_cast<RE::NiAlphaProperty*>(property0.get()) : nullptr;
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;
	if (!(lightProperty && lightProperty->alpha < 0.999f) && (!alphaProperty || !alphaProperty->GetAlphaBlending()))
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	// (46d) The local water surface (as the wetness and the snow), not the cell's default water plane.
	const WaterSample water = SampleWater(actor);
	if (!water.inWater)
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	const float waterZ = water.surfaceZ;
	// Seen from below the surface the water is not in front of the clothes: nothing to do.
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust(0);
	if (eye.z <= waterZ + 1.0f)
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	waterDepthPending = true;
	return float4(waterZ, 1.0f, std::clamp(globals::features::wetSkin.settings.UnderWaterTint, 0.0f, 1.0f), 0.0f);
}

void Skin::CopyPreWaterDepth(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_boundDSV)
{
	auto& main = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!main.texture || !a_boundDSV)
		return;
	{
		winrt::com_ptr<ID3D11Resource> bound;
		a_boundDSV->GetResource(bound.put());
		if (bound.get() != main.texture) {
			waterStatusCounting.note = "the water is not drawn into the main depth";
			return;
		}
	}
	D3D11_TEXTURE2D_DESC desc{};
	main.texture->GetDesc(&desc);
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	a_boundDSV->GetDesc(&dsvDesc);
	if (desc.SampleDesc.Count != 1 || dsvDesc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D) {
		waterStatusCounting.note = "multisampled or unusual depth";
		return;
	}
	if (!preWaterDepth || desc.Width != preWaterDesc.Width || desc.Height != preWaterDesc.Height || desc.Format != preWaterDesc.Format ||
		desc.BindFlags != preWaterDesc.BindFlags || dsvDesc.Format != preWaterDSVFormat) {
		preWaterDepth = nullptr;
		preWaterDSV = nullptr;
		D3D11_TEXTURE2D_DESC copyDesc = desc;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		copyDesc.CPUAccessFlags = 0;
		copyDesc.MiscFlags = 0;
		D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
		viewDesc.Format = dsvDesc.Format;
		viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		viewDesc.Flags = 0;
		viewDesc.Texture2D.MipSlice = 0;
		auto device = globals::d3d::device;
		if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, preWaterDepth.put())) ||
			FAILED(device->CreateDepthStencilView(preWaterDepth.get(), &viewDesc, preWaterDSV.put()))) {
			preWaterDepth = nullptr;
			preWaterDSV = nullptr;
			waterStatusCounting.note = "could not create the depth copy";
			logger::warn("[Advanced Skin] See-through clothes in water: could not create the depth copy ({}x{}, format {})", desc.Width, desc.Height, static_cast<int>(desc.Format));
			return;
		}
		preWaterDesc = desc;
		preWaterDSVFormat = dsvDesc.Format;
	}
	// GPU copy in command order: the depth as it is right before the first water draw.
	a_context->CopyResource(preWaterDepth.get(), main.texture);
	preWaterDepthFrame = globals::state->frameCount;
}

void Skin::ReleaseSavedTargets()
{
	for (auto& rtv : savedRTVs) {
		if (rtv)
			rtv->Release();
		rtv = nullptr;
	}
	if (savedDSV)
		savedDSV->Release();
	savedDSV = nullptr;
	savedRTVCount = 0;
}

void Skin::OnDrawStatesApplied(ID3D11DeviceContext* a_context)
{
	if (!settings.EnableSkin)
		return;
	const bool wanted = globals::features::wetSkin.settings.TranslucentUnderWater;
	if (!wanted && !waterDepthSwapped) {
		if (preWaterDepth) {
			preWaterDepth = nullptr;
			preWaterDSV = nullptr;
			preWaterDepthFrame = UINT32_MAX;
		}
		return;
	}
	auto* shader = globals::state->currentShader;
	const uint32_t frame = globals::state->frameCount;
	if (waterStatusFrame != frame) {
		waterDepthStatus = waterStatusCounting;
		waterStatusCounting = {};
		waterStatusFrame = frame;
	}
	const bool lighting = shader && shader->shaderType.get() == RE::BSShader::Type::Lighting;
	if (waterDepthSwapped && !(lighting && waterDepthPending)) {
		// Our depth outlived its pass (no RestoreGeometry): put the engine's back before this draw.
		RestoreWaterDepth(a_context);
	}
	if (!wanted || !shader)
		return;

	if (shader->shaderType.get() == RE::BSShader::Type::Water) {
		// The first water draw after this frame's opaque pass: keep the depth from before it.
		// (47c) Skip Invisible Work: only when a character was in water this frame or the last one
		// (the last frame covers one whose draws all come after the water); nobody in water = no
		// draw is ever swapped (GetWaterDepthParams), so the full-screen copy was unused.
		const bool someoneInWater = actorInWaterFrame != UINT32_MAX && frame - actorInWaterFrame <= 1;
		if (preWaterDepthFrame != frame && endDeferredFrame == frame && (someoneInWater || !globals::features::wetSkin.settings.SkipInvisibleWork)) {
			ID3D11DepthStencilView* dsv = nullptr;
			a_context->OMGetRenderTargets(0, nullptr, &dsv);
			CopyPreWaterDepth(a_context, dsv);
			if (dsv)
				dsv->Release();
		}
		return;
	}
	if (!lighting || !waterDepthPending)
		return;

	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
	ID3D11DepthStencilView* dsv = nullptr;
	a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs.data(), &dsv);
	const auto releaseQueried = [&] {
		for (auto* rtv : rtvs)
			if (rtv)
				rtv->Release();
		if (dsv)
			dsv->Release();
	};
	if (dsv && dsv == preWaterDSV.get()) {
		releaseQueried();  // already ours (another draw of the same pass)
		return;
	}
	// Only the main scene's depth, at the size of our copy.
	bool usable = false;
	if (dsv && preWaterDSV) {
		winrt::com_ptr<ID3D11Resource> bound;
		dsv->GetResource(bound.put());
		usable = bound.get() == globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
	}
	if (!usable) {
		releaseQueried();
		++waterStatusCounting.skipped;
		waterStatusCounting.note = "drawn into another target";
		waterDepthPending = false;
		return;
	}
	if (waterDepthSwapped)
		ReleaseSavedTargets();  // the engine re-applied its targets inside the pass; keep the newest
	UINT count = 0;
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
		if (rtvs[i])
			count = i + 1;
	savedRTVs = rtvs;  // the references from OMGetRenderTargets are kept until the restore
	savedDSV = dsv;
	savedRTVCount = count;
	a_context->OMSetRenderTargetsAndUnorderedAccessViews(count, rtvs.data(), preWaterDSV.get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	waterDepthSwapped = true;
	++waterStatusCounting.swapped;
}

void Skin::RestoreWaterDepth(ID3D11DeviceContext* a_context)
{
	waterDepthPending = false;
	if (!waterDepthSwapped)
		return;
	waterDepthSwapped = false;
	ID3D11DepthStencilView* dsv = nullptr;
	a_context->OMGetRenderTargets(0, nullptr, &dsv);
	// Only if ours is still bound: anything else is the engine's own newer binding.
	if (dsv == preWaterDSV.get())
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(savedRTVCount, savedRTVs.data(), savedDSV, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	if (dsv)
		dsv->Release();
	ReleaseSavedTargets();
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

void Skin::Hooks::BSLightingShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	// (46a) Right after the draw: the engine's own render targets back before it issues anything else.
	auto& skin = globals::features::skin;
	if (skin.waterDepthSwapped || skin.waterDepthPending)
		skin.RestoreWaterDepth(globals::d3d::context);
	func(This, Pass, RenderFlags);
}
