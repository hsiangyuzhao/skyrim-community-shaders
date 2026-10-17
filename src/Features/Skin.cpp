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

	// (44a) Skin wetness has its own page.
	ImGui::TextDisabled("Skin wetness (model, extra wetness, sheen, droplets, rivulets): see the Character Wetness page (Characters).");

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
		in.inWater = inWater;
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
		wetSkin.UpdatePhysical(state.phys, in);
		state.look = wetSkin.GetPhysicalLook(state.phys);
		const float allDry = (state.look.evapTime + 6.0f * state.look.runoffTime) * wetSkin.MaxDryMultiplier();
		if (!inWater && state.look.swimClock >= allDry)
			state.maxDepth = 0.0f;
		state.skin.x = state.look.sweat;
		state.wetSkin = float4(state.look.swim, state.maxDepth, inWater ? 1.0f : 0.0f, state.look.beads);
		state.wetChar = float4(inWater ? 0.0f : std::min(state.look.swimClock, allDry + 1.0f), 0.0f, inWater ? 1.0f : 0.0f, state.look.trails);
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
		// (47f) Physical: this character's levels and look ride in the same parameters (no extra GPU data).
		if (lastWetState && wetSkin.IsPhysical() && lastWetState->phys.valid) {
			const auto& look = lastWetState->look;
			auto& p = perGeometryData.wetSkinParams;
			auto& c = perGeometryData.wetCharParams;
			p[0].y = std::clamp(look.manualWater, 0.0f, 1.0f);
			p[0].w = std::clamp(look.rain, 0.0f, 1.0f);
			p[1].w = std::clamp(p[1].w * look.film, 0.0f, 2.5f);
			p[2].w = std::clamp(p[2].w * look.density, 0.0f, 1.0f);
			p[3].x = std::clamp(p[3].x * look.size, 0.25f, 4.0f);
			p[3].y = std::clamp(p[3].y * look.sizeRandom, 0.0f, 1.0f);
			p[4].x = std::clamp(p[4].x * look.beadTrails, 0.0f, 1.0f);
			p[5].z = std::clamp(p[5].z * look.dome, 0.0f, 2.0f);
			c[0].x = std::clamp(look.runoffTime, 0.1f, 600.0f);
			c[0].y = std::clamp(look.evapTime, 1.0f, 1e6f);
			c[11].z = std::clamp(look.peak, 0.0f, 1.0f);
			c[11].w = std::clamp(look.oil, 0.0f, 1.0f);
			// (47g) Gear and hair: the droplet / rivulet drivers without sweat, packed in the unused c[3].w (Skin.hlsli
			// WetGearDrivers): |v| = 1 + round(beads x 1023) + 0.998 x trails, negative while the manual look is Sweat.
			const float packed = 1.0f + std::round(std::clamp(look.gearBeads, 0.0f, 1.0f) * 1023.0f) + 0.998f * std::clamp(look.gearTrails, 0.0f, 1.0f);
			c[3].w = look.manualSweat ? -packed : packed;
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
