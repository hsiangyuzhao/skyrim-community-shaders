#include "DynamicSnow.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <thread>

#include "Features/Skin.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/ActorUtils.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DynamicSnow::Settings,
	EnableAccumulation,
	Condition,
	ColdRegionSnowShare,
	AccumulationHours,
	MeltHours,
	MaxCoverage,
	NormalThreshold,
	SnowColor,
	SnowRoughness,
	EnableTrails,
	TrailsOnSnow,
	TrailsOnAccumulated,
	MudTrails,
	TrailsFromNPCs,
	TrailResolution,
	SnowOnCharacters,
	TrailSize,
	TrailDepth,
	TrailRefillSeconds,
	TrailDarken,
	MudStrength,
	SnowOnTrees,
	TreeCoverage,
	SnowOnGrass,
	GrassCoverage,
	ColdRegionsOnly,
	NoLodSnow,
	SnowLineHeight,
	SnowLineFade,
	BodyAndObjectTrails,
	TrailRim,
	OverrideAmount,
	ShowColdMap,
	AmountOverride)

namespace
{
	// (44h) The 44f "EvenCoverOn44f" migration (Even Cover switched on once) is gone with Even Cover
	// itself: coverage is always the NormalThreshold rule now, whatever the saved file says.

	constexpr const char* kConditionNames[] = {
		"Snow weather (any region)",
		"Snow weather in a cold region",
		"Snow weather, or any rain/snow in a cold region",
	};

	/// Weather fade like Wetness Effects' CalculateWeatherWetness: how far the current weather's
	/// precipitation has faded in, or the last weather's has faded out.
	float PrecipitationFade(RE::TESWeather* a_weather, float a_weatherPct, bool a_current)
	{
		if (!a_weather)
			return 0.0f;
		auto linearstep = [](float a_edge0, float a_edge1, float a_x) {
			return std::clamp((a_x - a_edge0) / std::max(a_edge1 - a_edge0, 1e-4f), 0.0f, 1.0f);
		};
		const float progress = a_weatherPct * 255.0f;
		if (a_current) {
			const float fadeIn = a_weather->data.precipitationBeginFadeIn / 255.0f;
			if (fadeIn <= 0.0f)
				return a_weatherPct > 0.1f ? 1.0f : 0.0f;
			return linearstep(255.0f * (1.0f - fadeIn), 255.0f, progress);
		}
		const float fadeOut = a_weather->data.precipitationEndFadeOut / 255.0f;
		return 1.0f - linearstep(255.0f * fadeOut, 255.0f, progress);
	}

	std::string ToLower(std::string_view a_text)
	{
		std::string out(a_text);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	/// (44i) Which test made a weather count as snow.
	enum class SnowTest
	{
		None,
		Flag,             ///< the weather's Snow classification (the only test before 44i)
		ParticleType,     ///< its precipitation particles are of the snow type
		ParticleTexture,  ///< its precipitation particle texture is named "snow"
	};

	const char* SnowTestName(SnowTest a_test)
	{
		switch (a_test) {
		case SnowTest::Flag:
			return "snow (classification)";
		case SnowTest::ParticleType:
			return "snow (particle type)";
		case SnowTest::ParticleTexture:
			return "snow (particle texture)";
		default:
			return "not snow";
		}
	}

	/// (44i) Snow by the weather's precipitation particles (BGSShaderParticleGeometryData DATA: particle
	/// type 1 = snow; or a particle texture whose path names snow).
	SnowTest ParticleSnow(const RE::TESWeather* a_weather)
	{
		auto* particles = a_weather ? a_weather->precipitationData : nullptr;
		if (!particles)
			return SnowTest::None;
		constexpr auto typeIndex = static_cast<uint32_t>(RE::BGSShaderParticleGeometryData::DataID::kParticleType);
		constexpr auto snowType = static_cast<uint32_t>(RE::BGSShaderParticleGeometryData::ParticleType::kSnow);
		const char* texture = nullptr;
		if (REL::Module::IsVR()) {
			auto& rd = particles->GetVRRuntimeData();
			if (rd.data.size() > typeIndex && rd.data[typeIndex].value.i == snowType)
				return SnowTest::ParticleType;
			texture = rd.particleTexture.textureName.c_str();
		} else {
			auto& rd = particles->GetRuntimeData();
			if (rd.data.size() > typeIndex && rd.data[typeIndex].i == snowType)
				return SnowTest::ParticleType;
			texture = rd.particleTexture.textureName.c_str();
		}
		if (texture && texture[0] && ToLower(texture).find("snow") != std::string::npos)
			return SnowTest::ParticleTexture;
		return SnowTest::None;
	}

	// (47q) The 44i "Count Snow by Precipitation Particles" switch (ParticleSnowTest) is gone: always the classification only. The
	// particle test stays for the region diagnostics' hints.
	constexpr bool kParticleSnowTest = false;

	SnowTest ClassifySnow(const RE::TESWeather* a_weather, bool a_particles)
	{
		if (!a_weather)
			return SnowTest::None;
		if (a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow))
			return SnowTest::Flag;
		return a_particles ? ParticleSnow(a_weather) : SnowTest::None;
	}

	bool IsSnowWeather(const RE::TESWeather* a_weather, bool a_particles)
	{
		return ClassifySnow(a_weather, a_particles) != SnowTest::None;
	}

	bool HasPrecipitation(const RE::TESWeather* a_weather, bool a_particles)
	{
		return a_weather && a_weather->precipitationData &&
		       (a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy, RE::TESWeather::WeatherDataFlag::kSnow) ||
				   (a_particles && ParticleSnow(a_weather) != SnowTest::None));
	}

	/// Share (by chance) of snow weathers in a weather list, or -1 if the list is empty.
	float SnowShare(RE::BSSimpleList<RE::WeatherType*>& a_list, bool a_particles)
	{
		float total = 0.0f;
		float snow = 0.0f;
		for (auto* type : a_list) {
			if (!type || !type->weather)
				continue;
			const float chance = static_cast<float>(type->chance);
			total += chance;
			if (IsSnowWeather(type->weather, a_particles))
				snow += chance;
		}
		return total > 0.0f ? snow / total : -1.0f;
	}

	/// (44i) "FormID plugin [EditorID]": the defining plugin, and the last one overriding it if different.
	std::string FormLabel(const RE::TESForm* a_form)
	{
		if (!a_form)
			return "-";
		std::string out = std::format("{:08X}", a_form->GetFormID());
		auto* first = a_form->GetFile(0);
		auto* last = a_form->GetFile(-1);
		if (first)
			out += std::format(" {}", first->GetFilename());
		if (last && last != first)
			out += std::format(" (last edit {})", last->GetFilename());
		const char* id = a_form->GetFormEditorID();
		if (id && id[0])
			out += std::format(" [{}]", id);
		return out;
	}
}

bool DynamicSnow::HasShaderDefine(RE::BSShader::Type shaderType)
{
	// (batch 39c) Grass and distant trees take snow too.
	return shaderType == RE::BSShader::Type::Lighting || shaderType == RE::BSShader::Type::Grass || shaderType == RE::BSShader::Type::DistantTree;
}

bool DynamicSnow::AccumulationActive() const
{
	return loaded && settings.EnableAccumulation;
}

bool DynamicSnow::CharacterSnowStateActive() const
{
	const auto& skin = globals::features::skin;
	return AccumulationActive() && settings.SnowOnCharacters && skin.loaded && skin.settings.EnableSkin;
}

bool DynamicSnow::TrailsActive() const
{
	return loaded && settings.EnableTrails &&
	       (settings.TrailsOnSnow || settings.TrailsOnAccumulated || settings.MudTrails);
}

// ---------------------------------------------------------------------------------------------
// Item 5: accumulation
// ---------------------------------------------------------------------------------------------

bool DynamicSnow::IsColdRegion(RE::TESObjectCELL* a_cell, RE::TESWorldSpace* a_worldSpace)
{
	if (a_cell == coldCacheCell)
		return coldCacheShare >= settings.ColdRegionSnowShare;
	coldCacheCell = a_cell;
	coldCacheShare = -1.0f;
	status.region.clear();
	if (!a_cell)
		return false;

	// The cell's regions first: the weather list they carry is what the game itself draws the
	// weather from here. The highest snow share decides.
	float best = -1.0f;
	if (auto* regions = a_cell->GetRegionList(false)) {
		for (auto* region : *regions) {
			if (!region || !region->dataList)
				continue;
			for (auto* data : region->dataList->regionDataList) {
				if (!data || data->GetType() != RE::TESRegionData::Type::kWeather)
					continue;
				const float share = SnowShare(static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes, kParticleSnowTest);
				if (share > best) {
					best = share;
					const char* id = region->GetFormEditorID();
					status.region = (id && id[0]) ? std::string(id) : std::format("{:08X}", region->GetFormID());
				}
			}
		}
	}
	// No region weather here: the worldspace's climate.
	if (best < 0.0f && a_worldSpace && a_worldSpace->climate) {
		best = SnowShare(a_worldSpace->climate->weatherList, kParticleSnowTest);
		const char* id = a_worldSpace->climate->GetFormEditorID();
		status.region = std::format("climate {}", (id && id[0]) ? std::string(id) : std::format("{:08X}", a_worldSpace->climate->GetFormID()));
	}
	coldCacheShare = best;
	return coldCacheShare >= settings.ColdRegionSnowShare;
}

void DynamicSnow::UpdateAccumulation()
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* sky = globals::game::sky;
	auto* calendar = RE::Calendar::GetSingleton();

	auto* cell = player ? player->GetParentCell() : nullptr;

	const double nowHours = calendar ? static_cast<double>(calendar->GetDaysPassed()) * 24.0 : 0.0;
	double deltaHours = lastGameHours < 0.0 ? 0.0 : nowHours - lastGameHours;
	lastGameHours = nowHours;

	status.snowWeather = false;
	status.precipitation = false;
	status.snowfall = 0.0f;
	status.snowing = false;

	if (!status.exterior) {
		// Interiors: nothing is drawn and the outdoor amount is kept as it was.
		status.amount = amount;
		return;
	}

	const float pct = sky->currentWeatherPct;
	const float fadeCurrent = PrecipitationFade(sky->currentWeather, pct, true);
	const float fadeLast = PrecipitationFade(sky->lastWeather, pct, false);

	const bool particles = kParticleSnowTest;
	const float snowFade = std::min(1.0f, (IsSnowWeather(sky->currentWeather, particles) ? fadeCurrent : 0.0f) + (IsSnowWeather(sky->lastWeather, particles) ? fadeLast : 0.0f));
	const float precipFade = std::min(1.0f, (HasPrecipitation(sky->currentWeather, particles) ? fadeCurrent : 0.0f) + (HasPrecipitation(sky->lastWeather, particles) ? fadeLast : 0.0f));
	status.snowWeather = snowFade > 0.0f;
	status.precipitation = precipFade > 0.0f;

	const auto condition = static_cast<SnowCondition>(std::clamp(settings.Condition, 0, static_cast<int>(SnowCondition::Count) - 1));
	const bool needCold = condition != SnowCondition::SnowWeather;
	// (44i) Above the snow line (at least half way into its fade) the player counts as cold too.
	status.aboveSnowLine = player && SnowLineFactor(player->GetPositionZ()) >= 0.5f;
	status.coldRegion = needCold ? (IsColdRegion(cell, player->GetWorldspace()) || status.aboveSnowLine) : false;

	float snowfall = 0.0f;
	switch (condition) {
	case SnowCondition::SnowWeather:
		snowfall = snowFade;
		break;
	case SnowCondition::SnowWeatherColdRegion:
		snowfall = status.coldRegion ? snowFade : 0.0f;
		break;
	default:
		snowfall = std::max(snowFade, status.coldRegion ? precipFade : 0.0f);
		break;
	}
	status.snowfall = snowfall;
	status.snowing = snowfall > 0.0f;

	if (!amountInitialised || deltaHours < 0.0) {
		// First frame after a load, or an earlier save loaded: assume the weather has been
		// what it is now for a while.
		amount = snowfall >= 0.5f ? 1.0f : 0.0f;
		amountInitialised = true;
	} else if (deltaHours > 0.0) {
		// Integrated over game time, so waiting, sleeping and fast travel count; a long skip is
		// just a long step.
		if (snowfall > 0.0f)
			amount += static_cast<float>(deltaHours) * snowfall / std::max(settings.AccumulationHours, 0.01f);
		else
			amount -= static_cast<float>(deltaHours) / std::max(settings.MeltHours, 0.01f);
		amount = std::clamp(amount, 0.0f, 1.0f);
	}
	status.amount = amount;
}

// ---------------------------------------------------------------------------------------------
// (44h) Cold-region map and the loaded area
// ---------------------------------------------------------------------------------------------
//
// Skyrim has one weather per worldspace, so a snowstorm at High Hrothgar used to whiten the
// whole visible map. The mask says, for every point of the worldspace, whether the region there
// is a cold one, by the same rule IsColdRegion applies to the player's cell: the highest snow
// share of the weather regions covering the point (a region's area is the polygons of its RPLD
// point lists), or the worldspace climate's share where no weather region lies, against Cold
// Region Threshold. Shaders multiply the global amount by it, so a warm region's surfaces stay
// bare while the cold ones build up, and the border thins out through the coverage noise.

namespace
{
	constexpr float kCellUnits = 4096.0f;

	/// Highest snow share of a region's weather lists, or -1 if it has none.
	float RegionSnowShare(RE::TESRegion* a_region, bool a_particles)
	{
		float best = -1.0f;
		if (!a_region || !a_region->dataList)
			return best;
		for (auto* data : a_region->dataList->regionDataList) {
			if (!data || data->GetType() != RE::TESRegionData::Type::kWeather)
				continue;
			best = std::max(best, SnowShare(static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes, a_particles));
		}
		return best;
	}

	std::string FormName(RE::TESForm* a_form)
	{
		if (!a_form)
			return "-";
		const char* id = a_form->GetFormEditorID();
		return (id && id[0]) ? std::string(id) : std::format("{:08X}", a_form->GetFormID());
	}
}

void DynamicSnow::UpdateLoadedArea()
{
	auto* tes = RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	// Loaded cells per side: the live grid (it takes its size from uGridsToLoad), else the INI.
	uint32_t grids = (tes && tes->gridCells) ? tes->gridCells->length : 0;
	if (grids == 0 || grids > 63) {
		auto* setting = RE::GetINISetting("uGridsToLoad:General");
		grids = setting ? setting->GetUInt() : 0;
	}
	if (grids == 0 || grids > 63)
		grids = 5;

	// The grid is centred on TES's current grid cell; the player's own cell if that looks wrong
	// (the player must always stand inside the loaded grid).
	const auto pos = player->GetPosition();
	const int px = static_cast<int>(std::floor(pos.x / kCellUnits));
	const int py = static_cast<int>(std::floor(pos.y / kCellUnits));
	int cx = px;
	int cy = py;
	if (tes) {
		const int half = static_cast<int>(grids / 2);
		if (std::abs(tes->currentGridX - px) <= half && std::abs(tes->currentGridY - py) <= half) {
			cx = tes->currentGridX;
			cy = tes->currentGridY;
		}
	}
	loadedCenter[0] = (static_cast<float>(cx) + 0.5f) * kCellUnits;
	loadedCenter[1] = (static_cast<float>(cy) + 0.5f) * kCellUnits;

	// Near snow is full up to one cell short of the loaded area's edge and gone just inside it,
	// where LOD terrain and LOD objects (no accumulation) take over.
	const float halfExtent = static_cast<float>(grids) * kCellUnits * 0.5f;
	loadedFadeEnd = std::max(halfExtent - 256.0f, 2048.0f);
	loadedFadeStart = std::max(loadedFadeEnd - kCellUnits, 0.0f);
	status.gridsToLoad = grids;
	status.fadeStart = loadedFadeStart;
	status.fadeEnd = loadedFadeEnd;
}

void DynamicSnow::ReleaseColdMap()
{
	if (coldMaskSRV && globals::d3d::context) {
		ID3D11ShaderResourceView* nullSrv = nullptr;
		globals::d3d::context->PSSetShaderResources(kColdMaskSlot, 1, &nullSrv);
	}
	coldMaskSRV = nullptr;
	coldMaskTexture = nullptr;
	status.coldMapValid = false;
}

void DynamicSnow::BuildColdShareGrid(RE::TESWorldSpace* a_worldSpace)
{
	const auto t0 = std::chrono::steady_clock::now();
	ReleaseColdMap();
	coldShare.clear();
	coldShareRegion.clear();
	coldMaskCPU.clear();
	coldMapW = coldMapH = 0;
	coldMapThreshold = -1.0f;
	coldClimateShare = -1.0f;
	status.coldMapRegions = 0;
	status.coldMapColdRegions = 0;
	status.coldMapColdPercent = 0.0f;
	status.coldMapWidth = status.coldMapHeight = 0;
	status.playerMask = 1.0f;

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!a_worldSpace || !dataHandler) {
		status.coldMapNote = "no worldspace data";
		return;
	}

	struct Polygon
	{
		std::vector<RE::NiPoint2> points;
		float minX, minY, maxX, maxY;
		float share;
	};
	std::vector<Polygon> polygons;

	auto gather = [&](RE::TESWorldSpace* a_ws) {
		constexpr auto skipFlags = RE::TESRegion::RecordFlags::kBorderRegion | RE::TESRegion::RecordFlags::kDeleted | RE::TESRegion::RecordFlags::kIgnored;
		for (auto* region : dataHandler->GetFormArray<RE::TESRegion>()) {
			if (!region || region->worldSpace != a_ws || !region->pointLists || (region->formFlags & skipFlags))
				continue;
			const float share = RegionSnowShare(region, kParticleSnowTest);
			if (share < 0.0f)
				continue;  // not a weather region: it says nothing about cold
			bool any = false;
			for (auto* list : *region->pointLists) {
				if (!list)
					continue;
				Polygon poly{};
				for (auto* point : *list) {
					if (point && std::isfinite(point->point.x) && std::isfinite(point->point.y))
						poly.points.push_back(point->point);
				}
				if (poly.points.size() < 3)
					continue;
				poly.minX = poly.maxX = poly.points[0].x;
				poly.minY = poly.maxY = poly.points[0].y;
				for (const auto& p : poly.points) {
					poly.minX = std::min(poly.minX, p.x);
					poly.maxX = std::max(poly.maxX, p.x);
					poly.minY = std::min(poly.minY, p.y);
					poly.maxY = std::max(poly.maxY, p.y);
				}
				poly.share = share;
				polygons.push_back(std::move(poly));
				any = true;
			}
			if (any)
				coldShareRegion.push_back(share);
		}
	};

	gather(a_worldSpace);
	std::string source = FormName(a_worldSpace);
	if (polygons.empty() && a_worldSpace->parentWorld) {
		// City worldspaces (Whiterun, Solitude...) share their parent's coordinates and weather.
		gather(a_worldSpace->parentWorld);
		source = std::format("{} (regions of {})", source, FormName(a_worldSpace->parentWorld));
	}
	if (polygons.empty()) {
		status.coldMapNote = "no weather regions with an area here: snow everywhere (old behaviour)";
		logger::info("[{}] Cold map for {}: no weather regions with an area; accumulation stays global", GetName(), source);
		return;
	}

	// Where no weather region lies: the climate, as IsColdRegion does for such a cell.
	RE::TESClimate* climate = a_worldSpace->climate;
	if (!climate && a_worldSpace->parentWorld)
		climate = a_worldSpace->parentWorld->climate;
	coldClimateShare = climate ? SnowShare(climate->weatherList, kParticleSnowTest) : -1.0f;

	// Bounds of all weather regions plus a two-cell margin, so the mask's edge texels (repeated
	// outside it by the shader) are "no region".
	float minX = polygons[0].minX, minY = polygons[0].minY, maxX = polygons[0].maxX, maxY = polygons[0].maxY;
	for (const auto& poly : polygons) {
		minX = std::min(minX, poly.minX);
		minY = std::min(minY, poly.minY);
		maxX = std::max(maxX, poly.maxX);
		maxY = std::max(maxY, poly.maxY);
	}
	constexpr float margin = 2.0f * kCellUnits;
	minX -= margin;
	minY -= margin;
	maxX += margin;
	maxY += margin;
	const float extent = std::max(maxX - minX, maxY - minY);
	coldMapTexel = std::max(kColdMapTexelUnits, extent / static_cast<float>(kColdMapMaxSize));
	coldMapW = std::clamp(static_cast<uint32_t>(std::ceil((maxX - minX) / coldMapTexel)), 4u, kColdMapMaxSize);
	coldMapH = std::clamp(static_cast<uint32_t>(std::ceil((maxY - minY) / coldMapTexel)), 4u, kColdMapMaxSize);
	coldMapOriginX = minX;
	coldMapOriginY = minY;
	coldShare.assign(static_cast<size_t>(coldMapW) * coldMapH, -1.0f);

	// Scanline fill per polygon (even-odd), keeping the highest share where regions overlap.
	std::vector<float> crossings;
	for (const auto& poly : polygons) {
		const int y0 = std::max(0, static_cast<int>(std::floor((poly.minY - coldMapOriginY) / coldMapTexel - 0.5f)));
		const int y1 = std::min(static_cast<int>(coldMapH) - 1, static_cast<int>(std::ceil((poly.maxY - coldMapOriginY) / coldMapTexel - 0.5f)));
		const size_t n = poly.points.size();
		for (int j = y0; j <= y1; ++j) {
			const float yc = coldMapOriginY + (static_cast<float>(j) + 0.5f) * coldMapTexel;
			crossings.clear();
			for (size_t k = 0; k < n; ++k) {
				const auto& a = poly.points[k];
				const auto& b = poly.points[(k + 1) % n];
				if ((a.y <= yc) != (b.y <= yc))
					crossings.push_back(a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y));
			}
			std::sort(crossings.begin(), crossings.end());
			for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
				// Texels whose centre lies in [x0, x1).
				const int i0 = std::max(0, static_cast<int>(std::ceil((crossings[k] - coldMapOriginX) / coldMapTexel - 0.5f)));
				const int i1 = std::min(static_cast<int>(coldMapW) - 1, static_cast<int>(std::ceil((crossings[k + 1] - coldMapOriginX) / coldMapTexel - 0.5f)) - 1);
				float* row = coldShare.data() + static_cast<size_t>(j) * coldMapW;
				for (int i = i0; i <= i1; ++i)
					row[i] = std::max(row[i], poly.share);
			}
		}
	}

	status.coldMapRegions = static_cast<uint32_t>(coldShareRegion.size());
	status.coldMapWidth = coldMapW;
	status.coldMapHeight = coldMapH;
	status.coldMapNote = source;
	coldShareBuildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	coldMapLogPending = true;
}

void DynamicSnow::BuildColdMask()
{
	const auto t0 = std::chrono::steady_clock::now();
	const float threshold = settings.ColdRegionSnowShare;
	coldMapThreshold = threshold;
	const size_t count = static_cast<size_t>(coldMapW) * coldMapH;
	if (count == 0 || coldShare.size() != count)
		return;

	std::vector<float> mask(count);
	size_t coldTexels = 0;
	for (size_t k = 0; k < count; ++k) {
		const float share = coldShare[k] >= 0.0f ? coldShare[k] : coldClimateShare;
		const bool cold = share >= 0.0f && share >= threshold;
		mask[k] = cold ? 1.0f : 0.0f;
		coldTexels += cold;
	}
	uint32_t coldRegions = 0;
	for (float share : coldShareRegion)
		coldRegions += share >= threshold;

	// Soft border: two passes of a separable 3x3 box (a tent about two texels, one cell, wide),
	// and the shader reads it bilinearly.
	std::vector<float> tmp(count);
	const int w = static_cast<int>(coldMapW);
	const int h = static_cast<int>(coldMapH);
	for (int pass = 0; pass < 2; ++pass) {
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				tmp[y * w + x] = (mask[y * w + std::max(x - 1, 0)] + mask[y * w + x] + mask[y * w + std::min(x + 1, w - 1)]) * (1.0f / 3.0f);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				mask[y * w + x] = (tmp[std::max(y - 1, 0) * w + x] + tmp[y * w + x] + tmp[std::min(y + 1, h - 1) * w + x]) * (1.0f / 3.0f);
	}

	std::vector<uint8_t> texels(count);
	for (size_t k = 0; k < count; ++k)
		texels[k] = static_cast<uint8_t>(std::lround(std::clamp(mask[k], 0.0f, 1.0f) * 255.0f));

	ReleaseColdMap();
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = coldMapW;
	desc.Height = coldMapH;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init{ texels.data(), coldMapW, 0 };
	auto device = globals::d3d::device;
	if (!device || FAILED(device->CreateTexture2D(&desc, &init, coldMaskTexture.put())) ||
		FAILED(device->CreateShaderResourceView(coldMaskTexture.get(), nullptr, coldMaskSRV.put()))) {
		logger::warn("[{}] Could not create the {}x{} cold-region map; accumulation stays global", GetName(), coldMapW, coldMapH);
		ReleaseColdMap();
		status.coldMapNote = "could not create the map texture: snow everywhere (old behaviour)";
		coldShare.clear();
		return;
	}
	Util::SetResourceName(coldMaskTexture.get(), "DynamicSnow::ColdRegionMask");

	coldMaskCPU = std::move(mask);
	status.coldMapValid = true;
	status.coldMapColdRegions = coldRegions;
	status.coldMapColdPercent = 100.0f * static_cast<float>(coldTexels) / static_cast<float>(count);
	const float maskMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	status.coldMapBuildMs = (coldMapLogPending ? coldShareBuildMs : 0.0f) + maskMs;
	if (coldMapLogPending) {
		logger::info("[{}] Cold map for {}: {} weather regions, {} cold at threshold {:.2f}, {:.1f}% of the {}x{} map cold ({:.0f} units per texel), no-region share {:.2f}, built in {:.1f} ms",
			GetName(), status.coldMapNote, status.coldMapRegions, coldRegions, threshold, status.coldMapColdPercent, coldMapW, coldMapH, coldMapTexel, coldClimateShare, status.coldMapBuildMs);
		coldMapLogPending = false;
	} else {
		logger::debug("[{}] Cold map rebuilt at threshold {:.2f}: {} cold regions, {:.1f}% cold, {:.2f} ms", GetName(), threshold, coldRegions, status.coldMapColdPercent, maskMs);
	}
}

float DynamicSnow::ColdnessAt(float a_x, float a_y, float a_z) const
{
	if (!loaded || coldMaskCPU.empty() || coldMapW == 0 || coldMapH == 0)
		return -1.0f;
	float cold = std::clamp(SampleColdMaskCPU(a_x, a_y), 0.0f, 1.0f);
	if (settings.SnowLineHeight > 0.0f) {
		const float start = settings.SnowLineHeight - std::max(settings.SnowLineFade, 1.0f);
		const float t = std::clamp((a_z - start) / std::max(settings.SnowLineHeight - start, 1.0f), 0.0f, 1.0f);
		cold = std::max(cold, t * t * (3.0f - 2.0f * t));
	}
	return cold;
}

float DynamicSnow::SampleColdMaskCPU(float a_x, float a_y) const
{
	if (coldMaskCPU.empty() || coldMapW == 0 || coldMapH == 0)
		return 1.0f;
	const float tx = (a_x - coldMapOriginX) / coldMapTexel - 0.5f;
	const float ty = (a_y - coldMapOriginY) / coldMapTexel - 0.5f;
	const float fx = std::floor(tx);
	const float fy = std::floor(ty);
	const int w = static_cast<int>(coldMapW);
	const int h = static_cast<int>(coldMapH);
	auto at = [&](int x, int y) {
		return coldMaskCPU[static_cast<size_t>(std::clamp(y, 0, h - 1)) * coldMapW + static_cast<size_t>(std::clamp(x, 0, w - 1))];
	};
	const int x0 = static_cast<int>(fx);
	const int y0 = static_cast<int>(fy);
	const float ax = tx - fx;
	const float ay = ty - fy;
	const float top = std::lerp(at(x0, y0), at(x0 + 1, y0), ax);
	const float bottom = std::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), ax);
	return std::lerp(top, bottom, ay);
}

void DynamicSnow::UpdateColdMap()
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* worldSpace = player ? player->GetWorldspace() : nullptr;
	if (!worldSpace)
		return;
	if (worldSpace != coldMapWorldSpace || !coldMapTried) {
		coldMapWorldSpace = worldSpace;
		coldMapTried = true;
		try {
			BuildColdShareGrid(worldSpace);
		} catch (const std::exception& e) {
			logger::warn("[{}] Cold map build failed ({}); accumulation stays global", GetName(), e.what());
			ReleaseColdMap();
			coldShare.clear();
			status.coldMapNote = "build failed: snow everywhere (old behaviour)";
		}
	}
	if (!coldShare.empty() && coldMapThreshold != settings.ColdRegionSnowShare) {
		try {
			BuildColdMask();
		} catch (const std::exception& e) {
			logger::warn("[{}] Cold mask build failed ({}); accumulation stays global", GetName(), e.what());
			ReleaseColdMap();
			coldShare.clear();
			status.coldMapNote = "build failed: snow everywhere (old behaviour)";
		}
	}
	if (status.coldMapValid) {
		const auto pos = player->GetPosition();
		// (44i) As the shader: the region mask or the snow line, whichever is higher.
		status.playerMask = std::max(SampleColdMaskCPU(pos.x, pos.y), SnowLineFactor(pos.z));
	}
}

namespace
{
	/// (44i) Even-odd point-in-polygon test on one RPLD point list.
	bool PointInList(RE::TESRegionPointList* a_list, float a_x, float a_y)
	{
		if (!a_list)
			return false;
		std::vector<RE::NiPoint2> pts;
		for (auto* point : *a_list) {
			if (point && std::isfinite(point->point.x) && std::isfinite(point->point.y))
				pts.push_back(point->point);
		}
		if (pts.size() < 3)
			return false;
		bool inside = false;
		for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
			const auto& a = pts[i];
			const auto& b = pts[j];
			if ((a.y > a_y) != (b.y > a_y) && a_x < (b.x - a.x) * (a_y - a.y) / (b.y - a.y) + a.x)
				inside = !inside;
		}
		return inside;
	}

	/// (44i) Number of usable polygons (3+ finite points) of a region.
	uint32_t UsablePolygons(RE::TESRegion* a_region)
	{
		uint32_t count = 0;
		if (!a_region || !a_region->pointLists)
			return 0;
		for (auto* list : *a_region->pointLists) {
			if (!list)
				continue;
			uint32_t n = 0;
			for (auto* point : *list)
				n += (point && std::isfinite(point->point.x) && std::isfinite(point->point.y)) ? 1u : 0u;
			count += n >= 3;
		}
		return count;
	}

	void AppendWeatherLines(std::vector<std::string>& a_lines, RE::BSSimpleList<RE::WeatherType*>& a_list, bool a_particles)
	{
		for (auto* type : a_list) {
			if (!type || !type->weather)
				continue;
			auto* w = type->weather;
			const auto test = ClassifySnow(w, a_particles);
			// Without the particle test, still say when the particles alone would have counted.
			std::string hint;
			if (test == SnowTest::None && !a_particles) {
				const auto p = ParticleSnow(w);
				if (p != SnowTest::None)
					hint = std::format(" (would count with Count Snow by Precipitation Particles: {})", SnowTestName(p));
			}
			std::string flags;
			using F = RE::TESWeather::WeatherDataFlag;
			if (w->data.flags.any(F::kPleasant))
				flags += "pleasant ";
			if (w->data.flags.any(F::kCloudy))
				flags += "cloudy ";
			if (w->data.flags.any(F::kRainy))
				flags += "rainy ";
			if (w->data.flags.any(F::kSnow))
				flags += "snow ";
			if (flags.empty())
				flags = "none ";
			flags.pop_back();
			a_lines.push_back(std::format("      weather {}  chance {}{}  class: {}  -> {}{}",
				FormLabel(w), type->chance, type->global ? " (global)" : "", flags, SnowTestName(test), hint));
		}
	}
}

void DynamicSnow::UpdateRegionDiagnostics()
{
	auto& lines = status.regionLines;
	lines.clear();
	diagTime = std::chrono::steady_clock::now();
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	auto* worldSpace = player ? player->GetWorldspace() : nullptr;
	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	diagCell = cell;
	if (!player || !cell || !cell->IsExteriorCell() || !worldSpace || !dataHandler) {
		lines.push_back("Not in an exterior worldspace.");
		return;
	}
	const auto pos = player->GetPosition();
	const bool particles = kParticleSnowTest;
	const float threshold = settings.ColdRegionSnowShare;

	lines.push_back(std::format("Worldspace {}, position {:.0f}, {:.0f}, height {:.0f} ({:.0f} m)",
		FormLabel(worldSpace), pos.x, pos.y, pos.z, pos.z * 0.01428f));
	if (settings.SnowLineHeight > 0.0f)
		lines.push_back(std::format("Snow line {:.0f} (fades in from {:.0f}): {:.0f}% here",
			settings.SnowLineHeight, settings.SnowLineHeight - std::max(settings.SnowLineFade, 1.0f), SnowLineFactor(pos.z) * 100.0f));
	else
		lines.push_back("Snow line: off");

	// Regions of the cell's own region list (what the game draws the weather from).
	std::vector<RE::TESRegion*> cellRegions;
	if (auto* list = cell->GetRegionList(false)) {
		for (auto* region : *list)
			if (region)
				cellRegions.push_back(region);
	}

	uint32_t found = 0;
	for (auto* region : dataHandler->GetFormArray<RE::TESRegion>()) {
		if (!region)
			continue;
		const bool parent = worldSpace->parentWorld && region->worldSpace == worldSpace->parentWorld;
		if (region->worldSpace != worldSpace && !parent)
			continue;
		bool inArea = false;
		if (region->pointLists) {
			for (auto* list : *region->pointLists) {
				if (PointInList(list, pos.x, pos.y)) {
					inArea = true;
					break;
				}
			}
		}
		const bool inCell = std::find(cellRegions.begin(), cellRegions.end(), region) != cellRegions.end();
		if (!inArea && !inCell)
			continue;
		++found;

		// Same order as BuildColdShareGrid's checks.
		bool hasWeatherData = false;
		if (region->dataList) {
			for (auto* data : region->dataList->regionDataList)
				hasWeatherData |= data && data->GetType() == RE::TESRegionData::Type::kWeather;
		}
		const float share = RegionSnowShare(region, particles);
		std::string state;
		if (region->formFlags & RE::TESRegion::RecordFlags::kBorderRegion)
			state = "skipped (border region)";
		else if (region->formFlags & RE::TESRegion::RecordFlags::kDeleted)
			state = "skipped (deleted)";
		else if (region->formFlags & RE::TESRegion::RecordFlags::kIgnored)
			state = "skipped (ignored)";
		else if (!hasWeatherData)
			state = "skipped (no weather data)";
		else if (share < 0.0f)
			state = "skipped (empty weather list)";
		else if (UsablePolygons(region) == 0)
			state = std::format("skipped (no area), snow share {:.2f}", share);
		else
			state = std::format("snow share {:.2f} -> {}", share, share >= threshold ? "COLD" : "not cold");
		lines.push_back(std::format("Region {}{}{}{}: {}", FormLabel(region), inArea ? " [area]" : "", inCell ? " [cell list]" : "",
			parent ? " [parent worldspace]" : "", state));
		if (region->dataList) {
			for (auto* data : region->dataList->regionDataList) {
				if (data && data->GetType() == RE::TESRegionData::Type::kWeather)
					AppendWeatherLines(lines, static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes, particles);
			}
		}
	}
	if (found == 0)
		lines.push_back("No region of this worldspace covers this point or is in this cell's region list.");

	// The climate (where no weather region lies).
	RE::TESClimate* climate = worldSpace->climate;
	if (!climate && worldSpace->parentWorld)
		climate = worldSpace->parentWorld->climate;
	if (climate) {
		const float share = SnowShare(climate->weatherList, particles);
		lines.push_back(std::format("Climate {}: snow share {:.2f} -> {} (used where no weather region lies)", FormLabel(climate), share,
			share >= 0.0f && share >= threshold ? "COLD" : "not cold"));
		AppendWeatherLines(lines, climate->weatherList, particles);
	} else {
		lines.push_back("Climate: none");
	}

	// What the map holds here.
	const float line = SnowLineFactor(pos.z);
	if (!settings.ColdRegionsOnly) {
		lines.push_back("Map here: Snow Only in Cold Regions is off (same amount everywhere)");
	} else if (!status.coldMapValid || coldShare.empty() || coldMapW == 0 || coldMapH == 0) {
		lines.push_back(std::format("Map here: no map ({})", status.coldMapNote.empty() ? "not built yet" : status.coldMapNote));
	} else {
		const int ix = std::clamp(static_cast<int>(std::floor((pos.x - coldMapOriginX) / coldMapTexel)), 0, static_cast<int>(coldMapW) - 1);
		const int iy = std::clamp(static_cast<int>(std::floor((pos.y - coldMapOriginY) / coldMapTexel)), 0, static_cast<int>(coldMapH) - 1);
		const float raw = coldShare[static_cast<size_t>(iy) * coldMapW + static_cast<size_t>(ix)];
		const float mask = SampleColdMaskCPU(pos.x, pos.y);
		const char* source = raw >= 0.0f ? "region" : "climate fallback";
		const float share = raw >= 0.0f ? raw : coldClimateShare;
		const char* decides = line > mask ? "snow line" : (raw >= 0.0f ? "region" : "climate fallback");
		lines.push_back(std::format("Map here: {} share {:.2f} -> region mask {:.0f}%, snow line {:.0f}% -> {:.0f}% snow (decided by {})",
			source, share, mask * 100.0f, line * 100.0f, std::max(mask, line) * 100.0f, decides));
	}
}

float DynamicSnow::SnowLineFactor(float a_z) const
{
	if (settings.SnowLineHeight <= 0.0f)
		return 0.0f;
	const float fade = std::max(settings.SnowLineFade, 1.0f);
	const float t = std::clamp((a_z - (settings.SnowLineHeight - fade)) / fade, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

DynamicSnow::MapCbData DynamicSnow::GetMapBufferData()
{
	UpdateFrameState();

	MapCbData data{};
	data.MaskOrigin = { coldMapOriginX, coldMapOriginY };
	data.MaskInvTexelSize = 1.0f / std::max(coldMapTexel, 1.0f);
	data.MaskSizeX = std::max(coldMapW, 1u);
	data.MaskSizeY = std::max(coldMapH, 1u);
	data.LoadedCenter = { loadedCenter[0], loadedCenter[1] };
	data.FadeStart = loadedFadeStart;
	data.FadeEnd = std::max(loadedFadeEnd, loadedFadeStart + 1.0f);
	// (44i) Snow line (read with the cold mask only). Off: far above any terrain, so it adds nothing.
	if (settings.SnowLineHeight > 0.0f) {
		data.SnowLineEnd = settings.SnowLineHeight;
		data.SnowLineStart = settings.SnowLineHeight - std::max(settings.SnowLineFade, 1.0f);
	} else {
		data.SnowLineStart = 1e9f;
		data.SnowLineEnd = 2e9f;
	}
	return data;
}

// ---------------------------------------------------------------------------------------------
// Per-frame state
// ---------------------------------------------------------------------------------------------

void DynamicSnow::UpdateFrameState()
{
	if (!frameChecker.IsNewFrame())
		return;
	++frameIndex;

	// (44e) The refill clock: game-running seconds, as the 44d refill pass counted them. Advanced here,
	// once per frame and before anything reads the tick, so the shaders and the stamps agree on "now".
	if (globals::game::ui && !globals::game::ui->GameIsPaused())
		trailClock += RE::GetSecondsSinceLastFrame();

	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* sky = globals::game::sky;
		status.exterior = cell && cell->IsExteriorCell() && sky && sky->mode.get() == RE::Sky::Mode::kFull;
	}

	if (AccumulationActive())
		UpdateAccumulation();

	// (44h) Cold-region map (built per worldspace) and the loaded area around the player.
	if (loaded && status.exterior && (AccumulationActive() || settings.ShowColdMap)) {
		UpdateLoadedArea();
		if (settings.ColdRegionsOnly)
			UpdateColdMap();
	}

	// (44i) With the debug map on, log the region diagnostics when the player's cell changes (at most
	// every two seconds; a cell entered within that time is logged once the two seconds are up).
	if (loaded && status.exterior && settings.ShowColdMap) {
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		const auto now = std::chrono::steady_clock::now();
		if (cell && cell != diagLogCell && now - diagLogTime >= std::chrono::seconds(2)) {
			diagLogCell = cell;
			diagLogTime = now;
			UpdateRegionDiagnostics();
			logger::info("[{}] Regions at the player (cell {:08X}):", GetName(), cell->GetFormID());
			for (const auto& line : status.regionLines)
				logger::info("[{}]   {}", GetName(), line);
		}
	}

	trailsWanted = TrailsActive() && status.exterior;
	if (!trailsWanted) {
		// Leaving the exterior, switching off: the map is stale next time it is used.
		trailContentValid = false;
		if (!TrailsActive())
			ReleaseTrailResources();
		return;
	}

	if (!EnsureTrailResources()) {
		trailsWanted = false;
		return;
	}

	// Window centred on the player, snapped to whole texels.
	auto* player = RE::PlayerCharacter::GetSingleton();
	const auto pos = player->GetPosition();
	const int64_t half = trailMapSize / 2;
	windowOrigin[0] = static_cast<int64_t>(std::floor(pos.x / trailTexelSize)) - half;
	windowOrigin[1] = static_cast<int64_t>(std::floor(pos.y / trailTexelSize)) - half;

	auto* worldSpace = player->GetWorldspace();
	if (worldSpace != trailWorldSpace) {
		trailWorldSpace = worldSpace;
		trailContentValid = false;
	}
}

DynamicSnow::CommonBufferData DynamicSnow::GetCommonBufferData()
{
	UpdateFrameState();

	CommonBufferData data{};
	data.MaxCoverage = std::clamp(settings.MaxCoverage, 0.0f, 1.0f);
	data.NormalThreshold = std::clamp(settings.NormalThreshold, 0.0f, 0.99f);
	data.SnowColor = settings.SnowColor;
	data.SnowRoughness = std::clamp(settings.SnowRoughness, 0.05f, 1.0f);

	const float drawnAmount = settings.OverrideAmount ? std::clamp(settings.AmountOverride, 0.0f, 1.0f) : amount;
	data.Amount = drawnAmount;
	if (AccumulationActive() && status.exterior && drawnAmount > 0.0f) {
		data.Flags |= FlagAccumulation;
		if (settings.SnowOnCharacters) {
			data.Flags |= FlagSnowOnCharacters;
			// (46a) Characters take their own amount from Advanced Skin's b7 (Skin::GetWetness).
			if (CharacterSnowStateActive()) {
				data.Flags |= FlagCharacterSnowState;
				// (46d) Roofs: the per-character state (no build-up under a roof) instead of the per-pixel sky test. (47m) Always; (47o) the
				// shader takes it from FlagCharacterSnowState (FlagCharacterRoofs is no longer written).
			}
		}
		// (batch 39c) (44h) FlagCoverageSlope (Even Cover) is never set: NormalThreshold decides coverage.
		if (settings.SnowOnTrees)
			data.Flags |= FlagTrees;
		if (settings.SnowOnGrass)
			data.Flags |= FlagGrass;
		// (44h) Distant trees only with "No Snow on Distant LOD" off (the old LOD-tree tint).
		if (!settings.NoLodSnow)
			data.Flags |= FlagLodTrees;
	}

	// (44h) Cold-region mask and the near/LOD split. Only alongside something that reads them, so a
	// frame with nothing to draw still has Flags == 0 (Lighting.hlsl skips its whole snow block).
	const bool showColdMap = settings.ShowColdMap && loaded && status.exterior;
	if ((data.Flags & FlagAccumulation) || showColdMap) {
		if (settings.ColdRegionsOnly && status.coldMapValid && coldMaskSRV)
			data.Flags |= FlagColdMap;
		if (settings.NoLodSnow)
			data.Flags |= FlagNoLodSnow;
		if (showColdMap)
			data.Flags |= FlagShowColdMap;
	}

	// Prints are drawn only once the map holds this window's content (cleared at least once
	// by a Prepass); the first frame after enabling simply has none.
	// (44h) No longer limited to built-up snow when the Footprints mod is loaded.
	if (trailsWanted && trailContentValid && trailSRV) {
		data.Flags |= FlagTrails;
		if (settings.TrailsOnSnow)
			data.Flags |= FlagTrailsOnSnow;
		if (settings.TrailsOnAccumulated)
			data.Flags |= FlagTrailsOnAccumulated;
		if (settings.MudTrails)
			data.Flags |= FlagMudTrails;
		// (44e) The map is always time-stamped ((47o) the refill-pass encoding is gone).
		data.Flags |= FlagTrailTimeStamps;
		data.pad0 = static_cast<float>(NowTick());
	}

	data.TrailMapSize = trailMapSize ? trailMapSize : 1;
	data.TrailTexelSize = trailTexelSize;
	data.TrailOrigin = { static_cast<float>(static_cast<double>(windowOrigin[0]) * trailTexelSize),
		static_cast<float>(static_cast<double>(windowOrigin[1]) * trailTexelSize) };
	if (trailMapSize) {
		data.TrailWrapX = static_cast<int>(windowOrigin[0] & static_cast<int64_t>(trailMapSize - 1));
		data.TrailWrapY = static_cast<int>(windowOrigin[1] & static_cast<int64_t>(trailMapSize - 1));
	}
	data.TrailDepth = std::max(settings.TrailDepth, 0.0f);
	data.TrailDarken = std::clamp(settings.TrailDarken, 0.0f, 1.0f);
	data.TrailZTolerance = 40.0f;
	data.TrailMudStrength = std::clamp(settings.MudStrength, 0.0f, 1.0f);
	data.TrailEdgeFade = static_cast<float>(trailMapSize / 16);

	// (batch 39c) (44h) SlopeStart / SlopeFull / SnowSmoothing / SteepCover are read only under
	// FlagCoverageSlope, which is never set now; their old defaults are written so the layout stays.
	data.SlopeStart = 0.3f;
	data.SlopeFull = 0.55f;
	data.TreeCoverage = std::clamp(settings.TreeCoverage, 0.0f, 1.0f);
	data.GrassCoverage = std::clamp(settings.GrassCoverage, 0.0f, 1.0f);
	data.LodTreeCoverage = 0.5f;  // (44h) the old slider's default, pinned
	data.TrailRim = std::clamp(settings.TrailRim, 0.0f, 1.0f);
	// (40e)
	data.SnowSmoothing = 0.2f;
	data.SteepCover = 0.8f;

	status.accumulationDrawn = (data.Flags & FlagAccumulation) != 0;
	status.trailsDrawn = (data.Flags & FlagTrails) != 0;
	return data;
}

// ---------------------------------------------------------------------------------------------
// Item 6: trails
// ---------------------------------------------------------------------------------------------

void DynamicSnow::SetupResources()
{
	trailCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TrailCB>());

	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DYNAMIC;
	sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	sbDesc.StructureByteStride = sizeof(Stamp);
	sbDesc.ByteWidth = sizeof(Stamp) * kMaxStamps;
	stampBuffer = eastl::make_unique<Buffer>(sbDesc);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = kMaxStamps;
	stampBuffer->CreateSRV(srvDesc);

	stamps.reserve(kMaxStamps);
	CompileShaders();
}

void DynamicSnow::CompileShaders()
{
	auto compile = [](const char* a_define) {
		return static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\DynamicSnow\\SnowTrailsCS.hlsl", { { a_define, "" } }, "cs_5_0"));
	};
	if (!clearCS)
		clearCS = compile("CLEAR");
	// (44e) Time-stamped trail map: stamps with a compare-exchange on the print as it is now, and the sweep. (47o) The 44d refill
	// pass (DECAY) and the untimed stamps are gone: if any of these fails to compile, there are no footprints.
	if (!stampTimeCS)
		stampTimeCS = compile("STAMP");
	if (!sweepCS)
		sweepCS = compile("SWEEP");
}

void DynamicSnow::ClearShaderCache()
{
	for (auto** cs : { &clearCS, &stampTimeCS, &sweepCS }) {
		if (*cs) {
			(*cs)->Release();
			*cs = nullptr;
		}
	}
	CompileShaders();
}

bool DynamicSnow::EnsureTrailResources()
{
	const uint32_t wantedSize = 1024u << std::clamp(settings.TrailResolution, 0, 2);
	if (trailTexture && trailMapSize == wantedSize)
		return true;
	ReleaseTrailResources();

	auto device = globals::d3d::device;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = wantedSize;
	desc.Height = wantedSize;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_UINT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (FAILED(device->CreateTexture2D(&desc, nullptr, trailTexture.put()))) {
		logger::warn("[{}] Could not create the {}x{} trail map; footprints are off", GetName(), wantedSize, wantedSize);
		ReleaseTrailResources();
		return false;
	}
	Util::SetResourceName(trailTexture.get(), "DynamicSnow::TrailMap");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = desc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	if (FAILED(device->CreateShaderResourceView(trailTexture.get(), &srvDesc, trailSRV.put())) ||
		FAILED(device->CreateUnorderedAccessView(trailTexture.get(), &uavDesc, trailUAV.put()))) {
		logger::warn("[{}] Could not create the trail map views; footprints are off", GetName());
		ReleaseTrailResources();
		return false;
	}

	trailMapSize = wantedSize;
	trailTexelSize = kTrailWindowUnits / static_cast<float>(wantedSize);
	trailContentValid = false;
	status.trailMapSize = trailMapSize;
	return true;
}

void DynamicSnow::ReleaseTrailResources()
{
	if (trailSRV && globals::d3d::context) {
		// Never leave a released view bound at t101.
		ID3D11ShaderResourceView* nullSrv = nullptr;
		globals::d3d::context->PSSetShaderResources(kTrailSlot, 1, &nullSrv);
	}
	trailUAV = nullptr;
	trailSRV = nullptr;
	trailTexture = nullptr;
	trailMapSize = 0;
	trailContentValid = false;
	status.trailMapSize = 0;
	actorFeet.clear();
}

// ---------------------------------------------------------------------------------------------
// (batch 39c) Print shapes from the installed footprint mods
// ---------------------------------------------------------------------------------------------

// Texture base names follow the Footprints mod ("footprints" + name): Realistic PBR Footprints
// ships "<name>_p.dds" height maps under textures\pbr\actors\footprints, Footprints itself
// "<name>_h.dds" under textures\actors\footprints (in Footprints.bsa). decalUnits = the size of
// the Footprints mod's decal for the animal (TXST DODT), so our prints match theirs in scale.
const DynamicSnow::PrintKindInfo DynamicSnow::kPrintKinds[static_cast<int>(PrintKind::Count)] = {
	{ "Human", { "humanl", "humanr", nullptr, nullptr }, 32.0f, "" },
	{ "Horse", { "horsel", "horser", nullptr, nullptr }, 24.0f, "horse" },
	{ "Canine", { "caninel", "caniner", nullptr, nullptr }, 32.0f, "wolf|dog|hound|fox" },
	{ "Bear", { "bearfl", "bearfr", "bearbl", "bearbr" }, 36.0f, "bear" },
	{ "Sabre cat", { "sabrecatfl", "sabrecatfr", "sabrecatbl", "sabrecatbr" }, 32.0f, "sabrecat" },
	{ "Deer", { "elk", nullptr, nullptr, nullptr }, 28.0f, "deer|elk" },
	{ "Cow, goat", { "cowl", "cowr", nullptr, nullptr }, 32.0f, "cow|goat|boar" },
	{ "Giant", { "giantl", "giantr", nullptr, nullptr }, 64.0f, "giant" },
	{ "Mammoth", { "mammothl", "mammothr", nullptr, nullptr }, 96.0f, "mammoth" },
	{ "Troll", { "trolll", "trollr", nullptr, nullptr }, 48.0f, "troll" },
	{ "Werewolf", { "werewolffl", "werewolffr", "werewolfbl", "werewolfbr" }, 56.0f, "werewolf" },
	{ "Skeever", { "skeeverfl", "skeeverfr", "skeeverbl", "skeeverbr" }, 20.0f, "skeever" },
};

namespace
{
	/// Race EditorID fragment -> print kind, checked in order ("werewolf" before "wolf"), with a
	/// size factor for the smaller animals the Footprints mod draws with a shared texture.
	struct RaceMatch
	{
		const char* key;
		int kind;
		float sizeFactor;
	};
	constexpr RaceMatch kRaceMatches[] = {
		{ "werewolf", 10, 1.0f },
		{ "werebear", 3, 1.55f },
		{ "fox", 2, 0.5f },
		{ "goat", 6, 0.5f },
		{ "boar", 6, 0.62f },
		{ "horse", 1, 1.0f },
		{ "wolf", 2, 1.0f },
		{ "dog", 2, 1.0f },
		{ "hound", 2, 0.75f },
		{ "sabrecat", 4, 1.0f },
		{ "bear", 3, 1.0f },
		{ "deer", 5, 1.0f },
		{ "elk", 5, 1.0f },
		{ "cow", 6, 1.0f },
		{ "giant", 7, 1.0f },
		{ "mammoth", 8, 1.0f },
		{ "troll", 9, 1.0f },
		{ "skeever", 11, 1.0f },
	};

	float RaceSizeFactor(RE::Actor* a_actor)
	{
		auto* race = a_actor ? a_actor->GetRace() : nullptr;
		const char* id = race ? race->GetFormEditorID() : nullptr;
		if (!id || !id[0])
			return 1.0f;
		const std::string lower = ToLower(id);
		for (const auto& m : kRaceMatches) {
			if (lower.find(m.key) != std::string::npos)
				return m.sizeFactor;
		}
		return 1.0f;
	}

	/// Reads a file from the game's data (loose files and archives alike).
	bool ReadGameFile(const std::string& a_path, std::vector<uint8_t>& a_out)
	{
		a_out.clear();
		RE::BSResourceNiBinaryStream stream(a_path);
		if (!stream.good())
			return false;
		const std::uint32_t size = stream.stream ? stream.stream->totalSize : 0;
		if (size == 0 || size > (16u << 20))
			return false;
		a_out.resize(size);
		if (!stream.read(a_out.data(), size)) {
			a_out.clear();
			return false;
		}
		return true;
	}

	constexpr size_t kShapeSize = 64;  ///< atlas slice side, texels (a print is at most ~100 trail texels)

	/// Decodes a footprint height map into a kShapeSize^2 signed height field: -1 = the bottom of
	/// the print, 0 = the untouched snow around it (the median of the border), > 0 = the rim.
	bool DecodeShape(const std::vector<uint8_t>& a_dds, std::vector<float>& a_out)
	{
		DirectX::TexMetadata md{};
		DirectX::ScratchImage image;
		if (FAILED(DirectX::LoadFromDDSMemory(a_dds.data(), a_dds.size(), DirectX::DDS_FLAGS_NONE, &md, image)))
			return false;
		const DirectX::Image* src = image.GetImage(0, 0, 0);
		if (!src)
			return false;
		DirectX::ScratchImage decompressed, converted;
		if (DirectX::IsCompressed(src->format)) {
			if (FAILED(DirectX::Decompress(*src, DXGI_FORMAT_UNKNOWN, decompressed)))
				return false;
			src = decompressed.GetImage(0, 0, 0);
		}
		if (src->format != DXGI_FORMAT_R32_FLOAT) {
			if (FAILED(DirectX::Convert(*src, DXGI_FORMAT_R32_FLOAT, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted)))
				return false;
			src = converted.GetImage(0, 0, 0);
		}
		// Area average down to kShapeSize^2 (no WIC, so no COM needed on this thread).
		const size_t w = src->width, hgt = src->height;
		if (w < kShapeSize / 2 || hgt < kShapeSize / 2)
			return false;
		std::vector<float> h(kShapeSize * kShapeSize, 0.0f);
		for (size_t y = 0; y < kShapeSize; ++y) {
			const size_t y0 = y * hgt / kShapeSize, y1 = std::max(y0 + 1, (y + 1) * hgt / kShapeSize);
			for (size_t x = 0; x < kShapeSize; ++x) {
				const size_t x0 = x * w / kShapeSize, x1 = std::max(x0 + 1, (x + 1) * w / kShapeSize);
				double sum = 0.0;
				for (size_t sy = y0; sy < y1; ++sy) {
					const float* row = reinterpret_cast<const float*>(src->pixels + sy * src->rowPitch);
					for (size_t sx = x0; sx < x1; ++sx)
						sum += row[sx];
				}
				h[y * kShapeSize + x] = static_cast<float>(sum / static_cast<double>((y1 - y0) * (x1 - x0)));
			}
		}

		// Untouched level: median of a 3-texel border ring. Depth: the 1st percentile below it.
		std::vector<float> border;
		for (size_t y = 0; y < kShapeSize; ++y)
			for (size_t x = 0; x < kShapeSize; ++x)
				if (x < 3 || y < 3 || x >= kShapeSize - 3 || y >= kShapeSize - 3)
					border.push_back(h[y * kShapeSize + x]);
		std::nth_element(border.begin(), border.begin() + border.size() / 2, border.end());
		const float level = border[border.size() / 2];
		std::vector<float> sorted = h;
		std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 100, sorted.end());
		const float bottom = sorted[sorted.size() / 100];
		const float depth = level - bottom;
		if (depth < 0.04f)
			return false;  // flat: not a height map we understand

		a_out.resize(kShapeSize * kShapeSize);
		for (size_t i = 0; i < h.size(); ++i) {
			const size_t x = i % kShapeSize, y = i / kShapeSize;
			// Fade the outermost texels to untouched so a print never ends in a hard square edge.
			const float edge = static_cast<float>(std::min({ x, y, kShapeSize - 1 - x, kShapeSize - 1 - y }));
			const float fade = std::clamp(edge / 3.0f, 0.0f, 1.0f);
			a_out[i] = std::clamp((h[i] - level) / depth, -1.0f, 1.0f) * fade;
		}
		return true;
	}
}

DynamicSnow::PrintKind DynamicSnow::ClassifyRace(RE::Actor* a_actor, bool a_humanoid)
{
	auto* race = a_actor ? a_actor->GetRace() : nullptr;
	const char* id = race ? race->GetFormEditorID() : nullptr;
	if (id && id[0]) {
		const std::string lower = ToLower(id);
		for (const auto& m : kRaceMatches) {
			if (lower.find(m.key) != std::string::npos)
				return static_cast<PrintKind>(m.kind);
		}
	}
	return a_humanoid ? PrintKind::Human : PrintKind::None;
}

std::unique_ptr<DynamicSnow::PrintShapeData> DynamicSnow::DecodePrintShapes()
{
	// (46q) Files, decoding and the mip chains: no D3D and no member state, so it may run on a background thread
	// (Background Shape Load). The engine's resource streams are read from its own loader threads as well.
	auto data = std::make_unique<PrintShapeData>();
	std::vector<std::vector<float>> slices;
	std::vector<uint8_t> file;
	std::vector<float> shape;
	for (int k = 0; k < static_cast<int>(PrintKind::Count); ++k) {
		for (int f = 0; f < 4; ++f) {
			const char* name = kPrintKinds[k].files[f];
			if (!name)
				continue;
			// Realistic PBR Footprints first (finer, with a rim), then the Footprints mod itself.
			const std::string pbr = std::format("textures\\pbr\\actors\\footprints\\footprints{}_p.dds", name);
			const std::string classic = std::format("textures\\actors\\footprints\\footprints{}_h.dds", name);
			bool ok = false;
			if (ReadGameFile(pbr, file) && DecodeShape(file, shape)) {
				ok = data->anyPbr = true;
			} else if (ReadGameFile(classic, file) && DecodeShape(file, shape)) {
				ok = data->anyClassic = true;
			}
			if (!ok)
				continue;
			data->slots[k].slot[f] = static_cast<int>(slices.size());
			slices.push_back(shape);
		}
	}
	data->slices = static_cast<uint32_t>(slices.size());
	if (slices.empty())
		return data;

	// Mip chain per slice, box filtered (the trail map is coarser than the shapes at 1024).
	uint32_t mips = 1;
	while ((kShapeSize >> mips) >= 4)
		++mips;
	data->mips = mips;
	data->levels.reserve(slices.size() * mips);
	for (auto& slice : slices) {
		size_t size = kShapeSize;
		data->levels.push_back(slice);
		for (uint32_t m = 1; m < mips; ++m) {
			const auto& prev = data->levels.back();
			const size_t half = size / 2;
			std::vector<float> next(half * half);
			for (size_t y = 0; y < half; ++y)
				for (size_t x = 0; x < half; ++x)
					next[y * half + x] = 0.25f * (prev[(2 * y) * size + 2 * x] + prev[(2 * y) * size + 2 * x + 1] + prev[(2 * y + 1) * size + 2 * x] + prev[(2 * y + 1) * size + 2 * x + 1]);
			data->levels.push_back(std::move(next));
			size = half;
		}
	}
	return data;
}

void DynamicSnow::ApplyPrintShapes(std::unique_ptr<PrintShapeData> a_data)
{
	shapesTried = true;
	shapeSRV = nullptr;
	shapeCount = 0;
	for (auto& s : shapeSlots)
		s = {};
	status.shapesLoaded = 0;
	status.shapeSource.clear();
	shapeState = 3;
	if (!a_data || a_data->slices == 0 || a_data->levels.size() != static_cast<size_t>(a_data->slices) * a_data->mips) {
		logger::info("[{}] No footprint textures found (Footprints / Realistic PBR Footprints); using the built-in oval prints", GetName());
		return;
	}
	const uint32_t mips = a_data->mips;
	std::vector<D3D11_SUBRESOURCE_DATA> init(a_data->levels.size());
	for (size_t s = 0; s < a_data->slices; ++s) {
		for (uint32_t m = 0; m < mips; ++m) {
			auto& d = init[s * mips + m];
			d.pSysMem = a_data->levels[s * mips + m].data();
			d.SysMemPitch = static_cast<UINT>((kShapeSize >> m) * sizeof(float));
		}
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(kShapeSize);
	desc.Height = static_cast<UINT>(kShapeSize);
	desc.MipLevels = mips;
	desc.ArraySize = a_data->slices;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	winrt::com_ptr<ID3D11Texture2D> texture;
	auto device = globals::d3d::device;
	if (!device || FAILED(device->CreateTexture2D(&desc, init.data(), texture.put())) ||
		FAILED(device->CreateShaderResourceView(texture.get(), nullptr, shapeSRV.put()))) {
		logger::warn("[{}] Could not create the footprint shape atlas; using the built-in oval prints", GetName());
		shapeSRV = nullptr;
		return;
	}
	Util::SetResourceName(texture.get(), "DynamicSnow::PrintShapes");
	if (!shapeSampler) {
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		device->CreateSamplerState(&sd, shapeSampler.put());
	}
	shapeSlots = a_data->slots;
	shapeCount = a_data->slices;
	shapeState = 2;
	status.shapesLoaded = shapeCount;
	status.shapeSource = a_data->anyPbr && a_data->anyClassic ? "Realistic PBR Footprints + Footprints" : a_data->anyPbr ? "Realistic PBR Footprints" : "Footprints";
	logger::info("[{}] Loaded {} footprint shapes from {}", GetName(), shapeCount, status.shapeSource);
}

void DynamicSnow::LoadPrintShapes()
{
	// 46p: everything on the render thread, on first use.
	std::unique_ptr<PrintShapeData> data;
	try {
		data = DecodePrintShapes();
	} catch (const std::exception& e) {
		logger::warn("[{}] Footprint shapes could not be read ({}); using the built-in oval prints", GetName(), e.what());
	}
	ApplyPrintShapes(std::move(data));
}

void DynamicSnow::PollPrintShapes()
{
	// (46q) Background Shape Load: the decode runs on its own thread; the render thread only creates the atlas from the result.
	if (shapesTried)
		return;
	if (!shapeFuture.valid()) {
		try {
			// A detached thread with a packaged_task, not std::async: std::async's future blocks in its destructor, and this feature is
			// a static destroyed under the loader lock at exit (the thread already killed): a game closed mid-load would hang.
			// COM on this thread: DirectX::Convert takes the WIC path for sRGB sources (same result as the render-thread load).
			std::packaged_task<std::unique_ptr<PrintShapeData>()> task([]() -> std::unique_ptr<PrintShapeData> {
				const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				std::unique_ptr<PrintShapeData> result;
				try {
					result = DecodePrintShapes();
				} catch (...) {
					result = nullptr;
				}
				if (SUCCEEDED(com))
					CoUninitialize();
				return result;
			});
			auto future = task.get_future();
			std::thread(std::move(task)).detach();
			shapeFuture = std::move(future);
			shapeState = 1;
		} catch (const std::exception& e) {
			shapeFuture = {};
			logger::warn("[{}] Background footprint shape load could not start ({}); loading now", GetName(), e.what());
			LoadPrintShapes();
		}
		return;
	}
	if (shapeFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	std::unique_ptr<PrintShapeData> data;
	try {
		data = shapeFuture.get();
	} catch (...) {
		data = nullptr;
	}
	ApplyPrintShapes(std::move(data));
}

// ---------------------------------------------------------------------------------------------
// Item 6: feet
// ---------------------------------------------------------------------------------------------

void DynamicSnow::FindFeet(RE::Actor* a_actor, ActorFeet& a_feet)
{
	auto* root = a_actor->Get3D(false);
	a_feet.root.reset(root);
	a_feet.count = 0;
	a_feet.humanoid = false;
	for (auto& foot : a_feet.feet)
		foot.reset();
	for (auto& toe : a_feet.toes)
		toe.reset();
	a_feet.restHeight.fill(std::numeric_limits<float>::max());
	a_feet.planted.fill(false);
	a_feet.kind = PrintKind::None;
	a_feet.sizeFactor = RaceSizeFactor(a_actor);
	if (!root)
		return;

	static const RE::BSFixedString kLeftFoot("NPC L Foot [Lft ]");
	static const RE::BSFixedString kRightFoot("NPC R Foot [Rft ]");
	static const RE::BSFixedString kLeftToe("NPC L Toe0 [LToe]");
	static const RE::BSFixedString kRightToe("NPC R Toe0 [RToe]");
	auto* left = root->GetObjectByName(kLeftFoot);
	auto* right = root->GetObjectByName(kRightFoot);
	if (left && right) {
		a_feet.feet[0].reset(left);
		a_feet.feet[1].reset(right);
		a_feet.toes[0].reset(root->GetObjectByName(kLeftToe));
		a_feet.toes[1].reset(root->GetObjectByName(kRightToe));
		a_feet.count = 2;
		a_feet.humanoid = true;
		a_feet.kind = ClassifyRace(a_actor, true);
		return;
	}

	// Creatures: any skeleton node called "...foot..." or "...hoof...", minus the helpers that
	// share the word. Up to four, lowest first.
	std::vector<RE::NiAVObject*> candidates;
	RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* a_object) -> RE::BSVisit::BSVisitControl {
		if (!a_object || !a_object->AsNode())
			return RE::BSVisit::BSVisitControl::kContinue;
		const std::string name = ToLower(a_object->name.c_str());
		if (name.find("foot") == std::string::npos && name.find("hoof") == std::string::npos)
			return RE::BSVisit::BSVisitControl::kContinue;
		for (const char* skip : { "toe", "target", "ik", "nub", "fx", "pivot", "ref", "lock", "ctrl", "helper" }) {
			if (name.find(skip) != std::string::npos)
				return RE::BSVisit::BSVisitControl::kContinue;
		}
		candidates.push_back(a_object);
		return RE::BSVisit::BSVisitControl::kContinue;
	});
	std::sort(candidates.begin(), candidates.end(), [](RE::NiAVObject* a, RE::NiAVObject* b) {
		return a->world.translate.z < b->world.translate.z;
	});
	for (auto* node : candidates) {
		if (a_feet.count >= a_feet.feet.size())
			break;
		a_feet.feet[a_feet.count++].reset(node);
	}
	a_feet.kind = ClassifyRace(a_actor, false);
}

void DynamicSnow::AddStamp(float a_x, float a_y, float a_z, float a_dirX, float a_dirY, float a_lengthUnits, float a_widthUnits, uint a_shape)
{
	if (stamps.size() >= kMaxStamps)
		return;
	const double originX = static_cast<double>(windowOrigin[0]) * trailTexelSize;
	const double originY = static_cast<double>(windowOrigin[1]) * trailTexelSize;
	Stamp s{};
	s.Center = { static_cast<float>((a_x - originX) / trailTexelSize), static_cast<float>((a_y - originY) / trailTexelSize) };
	const float n = std::sqrt(a_dirX * a_dirX + a_dirY * a_dirY);
	s.Axis = n > 1e-4f ? float2{ a_dirX / n, a_dirY / n } : float2{ 0.0f, 1.0f };
	s.Radii = { std::max(a_lengthUnits, 0.5f) / trailTexelSize, std::max(a_widthUnits, 0.5f) / trailTexelSize };
	float wrapped = std::fmod(a_z, kTrailZWrap);
	if (wrapped < 0.0f)
		wrapped += kTrailZWrap;
	s.Z16 = static_cast<uint>(std::clamp(wrapped / kTrailZWrap, 0.0f, 1.0f) * 65535.0f + 0.5f) & 0xFFFF;
	s.Strength = 1.0f;
	s.End = s.Center;
	s.Shape = a_shape;
	s.Rim = 0.5f;  // shape-level rim; TrailRim scales every rim when the map is read

	// Whole print inside the window, away from the faded edge.
	const float margin = std::max(s.Radii.x, s.Radii.y) * 1.5f + static_cast<float>(trailMapSize / 16);
	if (s.Center.x < margin || s.Center.y < margin || s.Center.x > trailMapSize - margin || s.Center.y > trailMapSize - margin)
		return;
	stamps.push_back(s);
}

void DynamicSnow::AddCapsule(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius)
{
	if (stamps.size() >= kMaxStamps)
		return;
	const double originX = static_cast<double>(windowOrigin[0]) * trailTexelSize;
	const double originY = static_cast<double>(windowOrigin[1]) * trailTexelSize;
	Stamp s{};
	s.Center = { static_cast<float>((a_from.x - originX) / trailTexelSize), static_cast<float>((a_from.y - originY) / trailTexelSize) };
	s.End = { static_cast<float>((a_to.x - originX) / trailTexelSize), static_cast<float>((a_to.y - originY) / trailTexelSize) };
	s.Axis = { 0.0f, 1.0f };
	const float r = std::max(a_radius, 0.5f) / trailTexelSize;
	s.Radii = { r, r };
	float wrapped = std::fmod(std::min(a_from.z, a_to.z), kTrailZWrap);
	if (wrapped < 0.0f)
		wrapped += kTrailZWrap;
	s.Z16 = static_cast<uint>(std::clamp(wrapped / kTrailZWrap, 0.0f, 1.0f) * 65535.0f + 0.5f) & 0xFFFF;
	s.Strength = 0.8f;
	s.Shape = kShapeCapsule;
	s.Rim = 0.5f;  // shape-level rim; TrailRim scales every rim when the map is read

	const float margin = r * 1.5f + static_cast<float>(trailMapSize / 16);
	auto inside = [&](const float2& p) {
		return p.x >= margin && p.y >= margin && p.x <= trailMapSize - margin && p.y <= trailMapSize - margin;
	};
	if (!inside(s.Center) || !inside(s.End))
		return;
	stamps.push_back(s);
}

void DynamicSnow::GatherBodyAndObjectStamps(const RE::NiPoint3& a_center, std::vector<RE::Actor*>& a_deadActors)
{
	// After community-shaders PR #2659 (PppPlyr1, "Snow Deformation"): collision shapes that move
	// near the ground carve a trench from where they were last frame. Living actors keep their
	// shaped foot prints; this covers what feet do not: ragdolls being dragged or sliding, and
	// loose objects (dropped weapons, baskets, anything Havok moves).
	constexpr float kSurfaceBand = 40.0f;   // shape bottom further than this above the ground: no trench
	constexpr float kMoveGate = 3.0f;       // per-frame movement below this: at rest (or ragdoll jitter)
	constexpr float kBreakDistance = 256.0f;  // teleport, cell load
	const float reach = kTrailWindowUnits * 0.5f * 0.85f;

	bodyCurPositions.clear();
	auto stampShapes = [&](RE::NiAVObject* a_root, uint32_t a_formID, float a_groundZ) {
		uint32_t index = 0;
		RE::BSVisit::TraverseScenegraphCollision(a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			RE::NiPoint3 centre;
			float radius = 0.0f;
			if (!Util::GetShapeBound(a_object, centre, radius))
				return RE::BSVisit::BSVisitControl::kContinue;
			const uint64_t key = (static_cast<uint64_t>(a_formID) << 16) | (index++ & 0xFFFF);
			if (stamps.size() >= kMaxStamps)
				return RE::BSVisit::BSVisitControl::kStop;
			if (radius < 2.0f || radius > 128.0f || centre.z - radius > a_groundZ + kSurfaceBand) {
				bodyCurPositions[key] = centre;
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			auto it = bodyPrevPositions.find(key);
			if (it == bodyPrevPositions.end()) {
				bodyCurPositions[key] = centre;  // first sight: baseline only
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			const float dx = centre.x - it->second.x, dy = centre.y - it->second.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 < kMoveGate * kMoveGate) {
				bodyCurPositions[key] = it->second;  // keep the anchor so slow drags accumulate
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			bodyCurPositions[key] = centre;
			if (d2 < kBreakDistance * kBreakDistance) {
				RE::NiPoint3 from = it->second, to = centre;
				from.z = to.z = a_groundZ;
				AddCapsule(from, to, std::clamp(radius * 0.8f, 3.0f, 40.0f));
				++status.bodyStamps;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	};

	for (auto* actor : a_deadActors) {
		if (auto* root = actor->Get3D(false))
			stampShapes(root, actor->GetFormID(), actor->GetPosition().z);
	}

	auto* tes = RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (tes && player) {
		tes->ForEachReferenceInRange(player, reach, [&](RE::TESObjectREFR* a_ref) {
			if (!a_ref || a_ref->IsDisabled() || a_ref->As<RE::Actor>())
				return RE::BSContainer::ForEachResult::kContinue;
			auto* base = a_ref->GetBaseObject();
			if (!base)
				return RE::BSContainer::ForEachResult::kContinue;
			// Objects Havok moves; never projectiles, which would carve under their flight path.
			switch (base->GetFormType()) {
			case RE::FormType::Misc:
			case RE::FormType::Weapon:
			case RE::FormType::Armor:
			case RE::FormType::Ammo:
			case RE::FormType::Book:
			case RE::FormType::Ingredient:
			case RE::FormType::AlchemyItem:
			case RE::FormType::SoulGem:
			case RE::FormType::KeyMaster:
			case RE::FormType::Light:
			case RE::FormType::MovableStatic:
				break;
			default:
				return RE::BSContainer::ForEachResult::kContinue;
			}
			auto* root = a_ref->Get3D(false);
			if (!root)
				return RE::BSContainer::ForEachResult::kContinue;
			// Cheap gate first: the 3D root has to have moved since last frame.
			const auto pos = root->world.translate;
			if (std::abs(pos.x - a_center.x) > reach || std::abs(pos.y - a_center.y) > reach)
				return RE::BSContainer::ForEachResult::kContinue;
			const uint64_t rootKey = (static_cast<uint64_t>(a_ref->GetFormID()) << 16) | 0xFFFF;
			auto it = bodyPrevPositions.find(rootKey);
			const bool moved = it != bodyPrevPositions.end() && pos.GetSquaredDistance(it->second) >= kMoveGate * kMoveGate;
			bodyCurPositions[rootKey] = (it == bodyPrevPositions.end() || moved) ? pos : it->second;
			if (!moved)
				return RE::BSContainer::ForEachResult::kContinue;
			float ground = pos.z;
			tes->GetLandHeight(pos, ground);
			stampShapes(root, a_ref->GetFormID(), ground);
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}
	bodyPrevPositions.swap(bodyCurPositions);
}

void DynamicSnow::GatherStamps(const RE::NiPoint3& a_center)
{
	stamps.clear();
	status.actorsTracked = 0;
	status.bodyStamps = 0;

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	{
		// (46q) Footprint shapes: on first use (Background Shape Load: started now, applied once ready; ovals meanwhile).
		const auto shapesStart = std::chrono::steady_clock::now();
		if (!shapesTried)
			PollPrintShapes();
		Util::FrameTrace::On46qSnow(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - shapesStart).count(), shapeState);
	}
	const bool useShapes = shapeCount > 0;

	std::vector<RE::Actor*> actors;
	std::vector<RE::Actor*> dead;
	actors.push_back(player);
	if (settings.TrailsFromNPCs) {
		if (auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles) {
				auto actorPtr = handle.get();
				if (actorPtr && actorPtr.get() && actorPtr.get() != player)
					actors.push_back(actorPtr.get());
			}
		}
	}

	const float reach = kTrailWindowUnits * 0.5f * 0.85f;
	std::sort(actors.begin() + 1, actors.end(), [&a_center](RE::Actor* a, RE::Actor* b) {
		return a_center.GetSquaredDistance(a->GetPosition()) < a_center.GetSquaredDistance(b->GetPosition());
	});

	auto* camera = RE::PlayerCamera::GetSingleton();
	const bool playerFirstPerson = camera && camera->IsInFirstPerson();
	const float dt = globals::game::ui->GameIsPaused() ? 0.0f : RE::GetSecondsSinceLastFrame();

	for (auto* actor : actors) {
		if (stamps.size() + 4 > kMaxStamps)
			break;
		if (!actor || !actor->Is3DLoaded())
			continue;
		const auto pos = actor->GetPosition();
		if (std::abs(pos.x - a_center.x) > reach || std::abs(pos.y - a_center.y) > reach)
			continue;
		if (actor->IsDead()) {
			dead.push_back(actor);
			continue;
		}
		if (actor->IsInMidair() || actor->IsOnMount())
			continue;
		if (auto* state = actor->AsActorState(); state && state->IsSwimming())
			continue;

		++status.actorsTracked;
		auto& feet = actorFeet[actor->GetFormID()];
		feet.lastSeenFrame = frameIndex;
		auto* root = actor->Get3D(false);
		if (feet.root.get() != root)
			FindFeet(actor, feet);

		const float scale = std::max(actor->GetScale(), 0.1f);
		const float heading = actor->GetAngleZ();
		const float dirX = std::sin(heading);
		const float dirY = std::cos(heading);
		const float size = std::max(settings.TrailSize, 0.1f);

		// (batch 39c) Shaped print for this actor, if its animal's textures were found.
		const int kindIndex = static_cast<int>(feet.kind);
		const bool shaped = useShapes && kindIndex >= 0 && shapeSlots[kindIndex].slot[0] >= 0;
		const float shapeHalf = shaped ? 0.5f * kPrintKinds[kindIndex].decalUnits * feet.sizeFactor * scale * size : 0.0f;
		// Slice for foot i: left/right, and front/back where the animal has both.
		auto sliceFor = [&](bool a_left, bool a_back) -> uint {
			const auto& slots = shapeSlots[kindIndex].slot;
			int f = (a_back && slots[2] >= 0) ? 2 : 0;
			int s = slots[f + (a_left ? 0 : 1)];
			if (s >= 0)
				return static_cast<uint>(s);
			// Only one side found (or one texture for both, like the deer): mirror it.
			return static_cast<uint>(slots[f]) | (a_left ? 0u : kShapeMirror);
		};

		const bool useFeet = feet.count > 0 && !(actor == player && playerFirstPerson);
		if (useFeet) {
			const float height = std::max(actor->GetHeight(), 32.0f);
			const float rightX = dirY, rightY = -dirX;
			for (uint32_t i = 0; i < feet.count; ++i) {
				auto& node = feet.feet[i];
				if (!node)
					continue;
				const auto& wp = node->world.translate;
				const float h = wp.z - pos.z;
				// Lowest height this foot has been seen at, creeping up slowly so a bad first
				// reading does not stick: a foot within a few units of it is planted.
				float& rest = feet.restHeight[i];
				rest = (rest == std::numeric_limits<float>::max()) ? h : std::min(rest + 2.0f * dt, h);
				if (h - rest > 4.0f * scale) {
					feet.planted[i] = false;
					continue;
				}

				// (batch 39c) The print points where the foot points (ankle -> toe for people),
				// latched when the foot comes down, so it does not turn as the foot rolls off.
				if (!feet.planted[i]) {
					float fx = dirX, fy = dirY;
					if (feet.humanoid && i < feet.toes.size() && feet.toes[i]) {
						const auto& tp = feet.toes[i]->world.translate;
						const float tx = tp.x - wp.x, ty = tp.y - wp.y;
						const float tl = std::sqrt(tx * tx + ty * ty);
						if (tl > 2.0f * scale) {
							fx = tx / tl;
							fy = ty / tl;
						}
					}
					feet.plantedDir[i] = { fx, fy };
					feet.planted[i] = true;
				}
				const float fx = feet.plantedDir[i].x, fy = feet.plantedDir[i].y;

				if (feet.humanoid) {
					if (shaped) {
						// Ankle node: the heel is a few units behind it, the print's centre about
						// two thirds of a foot ahead. Humanoid creatures (werewolves) stand on their
						// hind feet.
						const float ahead = 9.5f * scale * size;
						AddStamp(wp.x + fx * ahead, wp.y + fy * ahead, wp.z - rest, fx, fy, shapeHalf, shapeHalf, sliceFor(i == 0, true));
					} else {
						AddStamp(wp.x + fx * 3.0f * scale, wp.y + fy * 3.0f * scale, wp.z - rest,
							fx, fy, 9.0f * scale * size, 4.5f * scale * size);
					}
				} else if (shaped) {
					const float ox = wp.x - pos.x, oy = wp.y - pos.y;
					const bool left = ox * rightX + oy * rightY < 0.0f;
					const bool back = feet.count > 2 && ox * dirX + oy * dirY < 0.0f;
					AddStamp(wp.x, wp.y, wp.z - rest, fx, fy, shapeHalf, shapeHalf, sliceFor(left, back));
				} else {
					const float r = std::clamp(height * 0.04f, 3.0f, 40.0f) * size;
					AddStamp(wp.x, wp.y, wp.z - rest, fx, fy, r * 1.2f, r);
				}
			}
		} else {
			// No feet to follow (or the player in first person, whose third-person skeleton is
			// not drawn): lay prints at walking stride, alternating left and right.
			const float height = std::max(actor->GetHeight(), 32.0f);
			const float stride = std::max(height * 0.3f, 20.0f);
			if (!feet.strideValid) {
				feet.lastStride = pos;
				feet.strideValid = true;
				continue;
			}
			const float dx = pos.x - feet.lastStride.x;
			const float dy = pos.y - feet.lastStride.y;
			if (dx * dx + dy * dy < stride * stride)
				continue;
			const float side = (feet.strideLeft ? -1.0f : 1.0f) * height * 0.05f;
			const float len = std::sqrt(dx * dx + dy * dy);
			const float fx = dx / len;
			const float fy = dy / len;
			const bool humanSized = feet.humanoid || actor == player;
			if (shaped || (useShapes && actor == player && shapeSlots[0].slot[0] >= 0)) {
				const int k = shaped ? kindIndex : 0;
				const float half = shaped ? shapeHalf : 0.5f * kPrintKinds[0].decalUnits * scale * size;
				const auto& slots = shapeSlots[k].slot;
				const int s = slots[feet.strideLeft ? 0 : 1];
				const uint slice = s >= 0 ? static_cast<uint>(s) : (static_cast<uint>(slots[0]) | (feet.strideLeft ? 0u : kShapeMirror));
				AddStamp(pos.x + fy * side, pos.y - fx * side, pos.z, fx, fy, half, half, slice);
			} else {
				const float lengthUnits = (humanSized ? 9.0f * scale : std::clamp(height * 0.05f, 3.0f, 40.0f)) * size;
				const float widthUnits = (humanSized ? 4.5f * scale : std::clamp(height * 0.04f, 3.0f, 40.0f)) * size;
				AddStamp(pos.x + fy * side, pos.y - fx * side, pos.z, fx, fy, lengthUnits, widthUnits);
			}
			feet.lastStride = pos;
			feet.strideLeft = !feet.strideLeft;
		}
	}

	if (settings.BodyAndObjectTrails)
		GatherBodyAndObjectStamps(a_center, dead);
	else
		bodyPrevPositions.clear();

	// Forget actors not seen for a while (their 3D may be long gone).
	if ((frameIndex & 255) == 0) {
		std::erase_if(actorFeet, [this](const auto& a_entry) { return frameIndex - a_entry.second.lastSeenFrame > 600; });
	}
	status.stamps = static_cast<uint32_t>(stamps.size());
}

void DynamicSnow::UpdateTrailMap()
{
	auto context = globals::d3d::context;
	// (47o) Without the time-stamp shaders (compile failure) no footprints: the map is never valid, FlagTrails stays off.
	if (!trailUAV || !clearCS || !stampTimeCS || !sweepCS)
		return;

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	// (44e) Refill clock. A change makes the stored texels meaningless: start over.
	const float refill = std::max(settings.TrailRefillSeconds, 1.0f);
	if (refill != trailClockRefill)
		trailContentValid = false;
	trailClockRefill = refill;

	// (44e) Upkeep pauses while no snow is near. The window still scrolls (its strips are cleared), so
	// nothing stale can show up later; a pause longer than a full refill starts the map over.
	const bool upkeep = SnowNearby();  // (44h) pausing is always on
	if (!upkeep) {
		if (!trailMaintenancePaused) {
			trailMaintenancePaused = true;
			trailPausedAt = trailClock;
		}
	} else if (trailMaintenancePaused) {
		trailMaintenancePaused = false;
		if (trailClock - trailPausedAt > refill * 1.5)
			trailContentValid = false;
	}
	status.trailUpkeep = !upkeep ? "paused (no snow near)" : "time-stamped (no refill pass)";

	if (upkeep)
		GatherStamps(player->GetPosition());
	else
		stamps.clear();

	auto timers = Util::GpuPassTimers::GetSingleton();
	timers->Begin(Util::GpuBucket::SnowTrails);

	const int64_t mask = static_cast<int64_t>(trailMapSize - 1);
	TrailCB cb{};
	cb.Wrap[0] = static_cast<int>(windowOrigin[0] & mask);
	cb.Wrap[1] = static_cast<int>(windowOrigin[1] & mask);
	cb.MapSize = trailMapSize;
	cb.pad[0] = NowTick();  // SnowTrailCB.NowTick

	ID3D11UnorderedAccessView* uav = trailUAV.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto dispatch = [&](ID3D11ComputeShader* a_cs, uint32_t a_x, uint32_t a_y) {
		trailCB->Update(cb);
		ID3D11Buffer* cbs[1] = { trailCB->CB() };
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShader(a_cs, nullptr, 0);
		context->Dispatch(a_x, a_y, 1);
	};

	// 1. Scroll: clear what came into view (the whole map on first use, a worldspace change or
	//    a jump larger than the window).
	const int64_t dx = windowOrigin[0] - committedOrigin[0];
	const int64_t dy = windowOrigin[1] - committedOrigin[1];
	const int64_t n = trailMapSize;
	if (!trailContentValid || std::abs(dx) >= n || std::abs(dy) >= n) {
		const UINT zeros[4] = { 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(uav, zeros);
		trailContentValid = true;
	} else {
		if (dx != 0) {
			cb.RectMin[0] = dx > 0 ? static_cast<int>(n - dx) : 0;
			cb.RectMin[1] = 0;
			cb.RectSize[0] = static_cast<int>(std::abs(dx));
			cb.RectSize[1] = static_cast<int>(n);
			dispatch(clearCS, static_cast<uint32_t>((cb.RectSize[0] + 7) / 8), static_cast<uint32_t>((cb.RectSize[1] + 7) / 8));
		}
		if (dy != 0) {
			cb.RectMin[0] = 0;
			cb.RectMin[1] = dy > 0 ? static_cast<int>(n - dy) : 0;
			cb.RectSize[0] = static_cast<int>(n);
			cb.RectSize[1] = static_cast<int>(std::abs(dy));
			dispatch(clearCS, static_cast<uint32_t>((cb.RectSize[0] + 7) / 8), static_cast<uint32_t>((cb.RectSize[1] + 7) / 8));
		}
	}
	committedOrigin[0] = windowOrigin[0];
	committedOrigin[1] = windowOrigin[1];

	// 2. Refill: (44e) no refill pass, the shaders compute it from each texel's age. A narrow band of rows per frame clears texels
	//    that have fully refilled, so the whole map is visited at least every half refill time and a 10-bit stamp time can never
	//    wrap (1024 ticks = 4 refill times).
	if (upkeep) {
		const float dt = globals::game::ui->GameIsPaused() ? 0.0f : RE::GetSecondsSinceLastFrame();
		const uint32_t rows = std::clamp(static_cast<uint32_t>(std::ceil(static_cast<float>(trailMapSize) * 2.0f * dt / refill)), 16u, trailMapSize);
		cb.RectMin[0] = 0;
		cb.RectMin[1] = static_cast<int>(sweepRow);
		cb.RectSize[0] = static_cast<int>(trailMapSize);
		cb.RectSize[1] = static_cast<int>(rows);
		dispatch(sweepCS, (trailMapSize + 63) / 64, rows);
		sweepRow = (sweepRow + rows) % trailMapSize;
	}

	// 3. Prints.
	if (!stamps.empty()) {
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(context->Map(stampBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, stamps.data(), sizeof(Stamp) * stamps.size());
			context->Unmap(stampBuffer->resource.get(), 0);
			cb.StampCount = static_cast<uint>(stamps.size());
			// (batch 39c) Print shapes from the installed footprint mods, if any.
			cb.ShapeCount = shapeSRV ? shapeCount : 0;
			ID3D11ShaderResourceView* srvs[2] = { stampBuffer->srv.get(), shapeSRV.get() };
			context->CSSetShaderResources(0, 2, srvs);
			ID3D11SamplerState* sampler = shapeSampler.get();
			context->CSSetSamplers(0, 1, &sampler);
			dispatch(stampTimeCS, cb.StampCount, 1);
			srvs[0] = srvs[1] = nullptr;
			context->CSSetShaderResources(0, 2, srvs);
			sampler = nullptr;
			context->CSSetSamplers(0, 1, &sampler);
		}
	}

	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	timers->End(Util::GpuBucket::SnowTrails);
}

uint32_t DynamicSnow::NowTick() const
{
	const double tickSeconds = std::max(static_cast<double>(trailClockRefill), 1.0) / static_cast<double>(kTrailTicksPerRefill);
	return static_cast<uint32_t>(static_cast<uint64_t>(trailClock / tickSeconds) & 1023u);
}

bool DynamicSnow::SnowNearby()
{
	// Cheap signals first; the ground under and around the player is checked twice a second.
	if (settings.MudTrails || status.snowWeather || (settings.TrailsOnAccumulated && status.accumulationDrawn))
		return snowNearby = true;
	if (snowNearbyFrame != 0 && frameIndex - snowNearbyFrame < 30)
		return snowNearby;
	snowNearbyFrame = frameIndex;
	snowNearby = false;
	auto* tes = RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!tes || !player)
		return snowNearby = true;  // cannot tell: keep the upkeep running
	const auto pos = player->GetPosition();
	constexpr float r = 1500.0f;
	const RE::NiPoint3 points[] = { pos, { pos.x + r, pos.y, pos.z }, { pos.x - r, pos.y, pos.z }, { pos.x, pos.y + r, pos.z }, { pos.x, pos.y - r, pos.z } };
	for (const auto& p : points) {
		const auto material = tes->GetLandMaterialType(p);
		if (material == RE::MATERIAL_ID::kSnow || material == RE::MATERIAL_ID::kSnowStairs || material == RE::MATERIAL_ID::kIce)
			return snowNearby = true;
	}
	return snowNearby;
}

void DynamicSnow::Prepass()
{
	UpdateFrameState();

	auto context = globals::d3d::context;
	if (trailsWanted && trailTexture)
		UpdateTrailMap();

	ID3D11ShaderResourceView* srv = (trailsWanted && trailSRV) ? trailSRV.get() : nullptr;
	context->PSSetShaderResources(kTrailSlot, 1, &srv);

	// (44h) Cold-region mask, read under FlagColdMap only.
	ID3D11ShaderResourceView* maskSrv = (settings.ColdRegionsOnly && status.coldMapValid) ? coldMaskSRV.get() : nullptr;
	context->PSSetShaderResources(kColdMaskSlot, 1, &maskSrv);
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void DynamicSnow::DrawSettings()
{
	// (44k) Main view: the everyday switches. The rest is under Advanced, status lines under Diagnostics.
	if (ImGui::TreeNodeEx("Snow Accumulation", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Snow Accumulation", &settings.EnableAccumulation);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"While it snows, the tops of the ground, roofs, rocks and other upward-facing surfaces slowly turn white;\n"
				"after the snow stops it slowly melts away. Nothing builds up under roofs or indoors.\n"
				"Surfaces that are already snow (snowy ground textures, snow-covered rocks) are left as they are.");

		ImGui::BeginDisabled(!settings.EnableAccumulation);
		ImGui::SliderFloat("Accumulation Speed", &settings.AccumulationHours, 0.1f, 12.0f, "%.1f game hours to full", ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			auto* calendar = RE::Calendar::GetSingleton();
			const float timescale = calendar ? std::max(calendar->GetTimescale(), 1.0f) : 20.0f;
			ImGui::Text(
				"Game hours of steady snowfall from bare ground to full cover.\n"
				"At the current timescale (%.0f) that is about %.1f real minutes. Waiting and sleeping count.",
				timescale, settings.AccumulationHours * 60.0f / timescale);
		}
		ImGui::SliderFloat("Melt Speed", &settings.MeltHours, 0.1f, 48.0f, "%.1f game hours to bare", ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Game hours from full cover back to bare ground after the snow stops.");
		ImGui::SliderFloat("Max Coverage", &settings.MaxCoverage, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("How white a fully snowed-over surface gets. 1 = completely covered.");
		// (44h) "Even Cover on Roofs and Slopes" and its four sliders removed: always the Normal Threshold rule.
		ImGui::SliderFloat("Normal Threshold", &settings.NormalThreshold, 0.0f, 0.95f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How flat a surface must be to hold snow. %.2f is about %.0f degrees from level;\n"
				"higher = only flatter surfaces get snow.",
				settings.NormalThreshold, std::acos(std::clamp(settings.NormalThreshold, 0.0f, 1.0f)) * 57.2958f);

		ImGui::Checkbox("Snow on Trees and Bushes", &settings.SnowOnTrees);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Trees, bushes and ferns get snow on the tops of their branches and leaves.\n"
				"It stays put while the leaves sway in the wind.");
		ImGui::BeginDisabled(!settings.SnowOnTrees);
		ImGui::SliderFloat("Tree Snow Amount", &settings.TreeCoverage, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Checkbox("Snow on Grass", &settings.SnowOnGrass);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Grass turns white from the root up as snow builds (as if half buried), with a dusting on top.\n"
				"Not under roofs or dense trees, like the ground.");
		ImGui::BeginDisabled(!settings.SnowOnGrass);
		ImGui::SliderFloat("Grass Snow Amount", &settings.GrassCoverage, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		// (44h) "Snow on Distant Trees" and "Distant Tree Snow Amount" removed: see No Snow on Distant LOD.
		ImGui::Checkbox("Snow on Characters", &settings.SnowOnCharacters);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Off (default): people, creatures and everything they wear or carry stay free of built-up snow.\n"
				"On: their upward-facing parts (shoulders, hoods) get snow like the ground does. Each character keeps their own:\n"
				"none indoors, it builds up outdoors while it snows (never more than the ground), stops building up under a roof,\n"
				"and water washes it off. Needs Advanced Skin (without it, characters show the ground's snow).");
		// (46a) Per-character snow. (47m) Build-up per character, the water wash and roofs are permanent (their switches removed).
		if (settings.SnowOnCharacters && !(globals::features::skin.loaded && globals::features::skin.settings.EnableSkin))
			ImGui::TextDisabled("Advanced Skin is off: characters show the ground's snow.");
		ImGui::EndDisabled();
		ImGui::TreePop();
	}

	ImGui::Spacing();

	if (ImGui::TreeNodeEx("Footprints and Trails", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Footprints", &settings.EnableTrails);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"The player and nearby people and creatures leave footprints in snow, which slowly fill back in.\n"
				"Works within about 25 m of the player.");

		ImGui::BeginDisabled(!settings.EnableTrails);
		ImGui::Checkbox("In Snowy Ground and Snow Materials", &settings.TrailsOnSnow);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Snow textures on the ground, and snow-covered rocks and meshes.");
		ImGui::Checkbox("In Built-Up Snow", &settings.TrailsOnAccumulated);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("The snow from Snow Accumulation above.");
		ImGui::Checkbox("In Mud and Dirt (all other ground)", &settings.MudTrails);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Also on ground that is not snow. The game does not mark which ground is soft,\n"
				"so this applies to all non-snow ground, roads included. Weaker than in snow (Mud Strength).");
		ImGui::Checkbox("From NPCs and Creatures", &settings.TrailsFromNPCs);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Off = only the player leaves prints.");

		// Prints are shaped from installed footprint textures (Realistic PBR Footprints, or Footprints
		// itself); plain ovals when none are installed. (46r) The shape status: under Diagnostics ((47m) always loaded in the background).
		// (44h) "Leave Snowy Ground to the Footprints Mod" removed: off, our prints go everywhere set above.
		ImGui::Checkbox("Trenches from Bodies and Objects", &settings.BodyAndObjectTrails);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Bodies being dragged or sliding, and loose objects moving through snow, leave trenches\n"
				"(from their collision shapes).");
		ImGui::EndDisabled();
		ImGui::TreePop();
	}

	ImGui::Spacing();

	if (ImGui::TreeNodeEx("Advanced")) {
		ImGui::BeginDisabled(!settings.EnableAccumulation);
		ImGui::Combo("When It Builds Up", &settings.Condition, kConditionNames, static_cast<int>(SnowCondition::Count));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Which weather counts as snowing.\n"
				"- Snow weather (any region): any weather marked as snow, wherever it happens. Weather mods that bring snow\n"
				"  to the south (Whiterun, Riften) then whiten those regions too. Default.\n"
				"- Snow weather in a cold region: as above, but only where the region normally gets snow\n"
				"  (see Cold Region Threshold). Use this if snow in the south looks wrong to you.\n"
				"- Snow weather, or any rain/snow in a cold region: also treats rain in a cold region as snow,\n"
				"  for weather mods that rain in the north.");

		ImGui::Checkbox("Snow Only in Cold Regions", &settings.ColdRegionsOnly);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Skyrim has one weather for the whole map, so when it snows up north the snow used to build up\n"
				"everywhere you can see, Whiterun's plains included.\n"
				"On (default): snow only builds up in cold regions (see Cold Region Threshold), with a soft edge.\n"
				"Off: the same amount everywhere (the old behaviour).");
		ImGui::Checkbox("No Snow on Distant LOD", &settings.NoLodSnow);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"On (default): far-away terrain, buildings and trees (LOD) get no built-up snow; their own textures\n"
				"already show snow where there should be some. Nearby snow thins out over the last cell before\n"
				"the far-away terrain begins, so there is no line.\n"
				"Off: far-away terrain and buildings take the snow too, and distant trees get a white tint (the old behaviour).");

		ImGui::BeginDisabled(settings.Condition == static_cast<int>(SnowCondition::SnowWeather) && !settings.ColdRegionsOnly);
		ImGui::SliderFloat("Cold Region Threshold", &settings.ColdRegionSnowShare, 0.05f, 0.9f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"A region counts as cold when at least this share of the weathers it can have are snow weathers\n"
				"(read from the region's weather list, or the worldspace's climate where there is none).\n"
				"Used by Snow Only in Cold Regions and by the cold-region choices of When It Builds Up.");

		// (44i) Snow line and the looser snow test.
		{
			const std::string lineFormat = settings.SnowLineHeight > 0.0f ?
			                                   std::format("%.0f units ({:.0f} m)", settings.SnowLineHeight * 0.01428f) :
			                                   std::string("off");
			ImGui::SliderFloat("Snow Line Height", &settings.SnowLineHeight, 0.0f, 40000.0f, lineFormat.c_str());
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"Above this height everything counts as cold, whatever region it is in: mountain tops such as\n"
					"High Hrothgar and the Throat of the World get snow even when their region is not a cold one.\n"
					"0 = off. Your own height is shown under Diagnostics (\"Your height\"); Whiterun is about -3500, Riften about 9800.");
			ImGui::BeginDisabled(settings.SnowLineHeight <= 0.0f);
			const std::string fadeFormat = std::format("%.0f units ({:.0f} m)", settings.SnowLineFade * 0.01428f);
			ImGui::SliderFloat("Snow Line Fade", &settings.SnowLineFade, 500.0f, 16000.0f, fadeFormat.c_str());
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How far below the snow line the snow starts to appear (none there, full at the line).");
			ImGui::EndDisabled();
			settings.SnowLineHeight = std::max(settings.SnowLineHeight, 0.0f);
			settings.SnowLineFade = std::max(settings.SnowLineFade, 1.0f);
		}
		ImGui::EndDisabled();
		// (47q) "Count Snow by Precipitation Particles" removed (always off: the weather's Snow classification decides).

		if (ImGui::TreeNodeEx("Snow Look")) {
			ImGui::ColorEdit3("Snow Colour", &settings.SnowColor.x);
			ImGui::SliderFloat("Snow Roughness", &settings.SnowRoughness, 0.3f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Snow never makes a surface shinier than it was; this only roughens shiny surfaces under snow.");
			ImGui::TreePop();
		}
		ImGui::EndDisabled();

		ImGui::BeginDisabled(!settings.EnableTrails);
		if (ImGui::TreeNodeEx("Print Look")) {
			ImGui::SliderFloat("Print Rim", &settings.TrailRim, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Height of the snow pushed up around a print, relative to its depth. 0 = no rim.");
			ImGui::SliderFloat("Print Size", &settings.TrailSize, 0.5f, 2.0f, "%.2fx");
			ImGui::SliderFloat("Print Depth", &settings.TrailDepth, 0.0f, 15.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How deep a print looks (%.1f cm). Only shading: the ground itself does not move.",
					settings.TrailDepth * 1.428f);
			ImGui::SliderFloat("Refill Time", &settings.TrailRefillSeconds, 10.0f, 1200.0f, "%.0f s", ImGuiSliderFlags_Logarithmic);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Real seconds for a print to fill back in completely.");
			ImGui::SliderFloat("Darken", &settings.TrailDarken, 0.0f, 0.6f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How much darker packed snow in a print is.");
			ImGui::BeginDisabled(!settings.MudTrails);
			ImGui::SliderFloat("Mud Strength", &settings.MudStrength, 0.0f, 1.0f, "%.2f");
			ImGui::EndDisabled();
			ImGui::TreePop();
		}
		const char* resolutions[] = { "1024 (4 units per texel, 4 MB)", "2048 (2 units per texel, 16 MB)", "4096 (1 unit per texel, 64 MB)" };
		ImGui::Combo("Trail Map Detail", &settings.TrailResolution, resolutions, 3);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Detail of the footprint map around the player (always 58 m across). Changing it clears existing prints.\n"
				"4096 gives the sharpest prints; it costs 64 MB of video memory and its refill pass touches 4x more memory.");
		// (44h) "Refill Without a Map Pass (44e)" and "Pause When No Snow Is Near (44e)" removed: both always on.
		ImGui::EndDisabled();
		ImGui::TreePop();
	}

	ImGui::Spacing();

	// (47m) Diagnostics, closed by default: the player snow readout, the test overrides and the status lines.
	if (ImGui::TreeNodeEx("Diagnostics")) {
		// (46d) The player's water and snow state (47m: moved here from Snow on Characters).
		if (settings.SnowOnCharacters && globals::features::skin.loaded && ImGui::TreeNodeEx("Player snow readout")) {
			const auto& w = globals::features::skin.playerWater;
			const auto& ps = globals::features::skin.playerSnow;
			auto z = [](float a_z) { return a_z > -1e29f ? std::format("{:.0f}", a_z) : std::string("none"); };
			ImGui::TextDisabled("Player water: local %s (Havok %s, water object %s, cell default %s), feet %.0f, depth %.0f%s%s",
				z(w.surfaceZ).c_str(), z(w.relevant).c_str(), z(w.multibound).c_str(), z(w.cellDefault).c_str(), w.feetZ, w.depth,
				w.clamped ? std::format(" (clamped from {:.0f})", w.rawDepth).c_str() : "", w.swimming ? ", swimming" : "");
			ImGui::TextDisabled("Player snow: %.2f, washed part %.2f below %.0f units, %s", ps.amount, ps.washed, ps.washDepth,
				ps.sheltered ? "under a roof" : "open sky");
			ImGui::TreePop();
		}
		ImGui::Checkbox("Override Accumulated Amount", &settings.OverrideAmount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Test only: draw this much accumulated snow whatever the weather (outdoors only).");
		ImGui::BeginDisabled(!settings.OverrideAmount);
		ImGui::SliderFloat("Amount", &settings.AmountOverride, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Checkbox("Show Cold-Region Map", &settings.ShowColdMap);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Test only: tints the world blue where snow can build up fully and red where it cannot\n"
				"(warm regions, distant LOD, the edge of the loaded area). Outdoors only.");
		if (status.coldMapValid)
			ImGui::TextDisabled("Map: %u x %u texels, %s, built in %.1f ms", status.coldMapWidth, status.coldMapHeight, status.coldMapNote.c_str(), status.coldMapBuildMs);

		// (44k) Status lines, moved here from the main view.
		ImGui::SeparatorText("Snow Accumulation");
		ImGui::Text("Now: %s", !AccumulationActive() ? "off" :
							   !status.exterior      ? "indoors (kept as it was)" :
							   status.snowing        ? "snowing, building up" :
							   status.amount > 0.0f  ? "melting" :
													   "no snow");
		ImGui::Text("Accumulated: %.0f%%   Snowfall: %.0f%%", status.amount * 100.0f, status.snowfall * 100.0f);
		if (settings.Condition != static_cast<int>(SnowCondition::SnowWeather))
			ImGui::Text("Region: %s (%s)", status.region.empty() ? "-" : status.region.c_str(),
				!status.coldRegion ? "not cold" : status.aboveSnowLine ? "cold: above the snow line" : "cold");
		if (AccumulationActive() && status.exterior) {
			if (!settings.ColdRegionsOnly)
				ImGui::TextDisabled("Cold map: off (same amount everywhere)");
			else if (status.coldMapValid)
				ImGui::Text("Cold map: %u regions (%u cold), %.0f%% of the map cold, here %.0f%%",
					status.coldMapRegions, status.coldMapColdRegions, status.coldMapColdPercent, status.playerMask * 100.0f);
			else
				ImGui::Text("Cold map: %s", status.coldMapNote.empty() ? "not built yet" : status.coldMapNote.c_str());
			if (settings.NoLodSnow)
				ImGui::TextDisabled("Near snow: %u x %u loaded cells, thins out %.0f..%.0f units from their centre",
					status.gridsToLoad, status.gridsToLoad, status.fadeStart, status.fadeEnd);
		}
		// (44i) Height readout for tuning the snow line, and what decides "cold" here.
		if (auto* player = RE::PlayerCharacter::GetSingleton(); player && status.exterior) {
			const float z = player->GetPositionZ();
			if (settings.SnowLineHeight > 0.0f)
				ImGui::Text("Your height: %.0f (%.0f m), snow line: %.0f (snow here %.0f%%)", z, z * 0.01428f, settings.SnowLineHeight, SnowLineFactor(z) * 100.0f);
			else
				ImGui::Text("Your height: %.0f (%.0f m), snow line: off", z, z * 0.01428f);
			if (ImGui::TreeNodeEx("Regions Here (diagnostics)")) {
				auto* cell = player->GetParentCell();
				if (cell != diagCell || std::chrono::steady_clock::now() - diagTime >= std::chrono::seconds(1))
					UpdateRegionDiagnostics();
				for (const auto& line : status.regionLines)
					ImGui::TextUnformatted(line.c_str());
				ImGui::TextDisabled("With Show Cold-Region Map on, this is also written to the log whenever you enter a new cell.");
				ImGui::TreePop();
			}
		}

		ImGui::SeparatorText("Footprints");
		if (!shapesTried)
			ImGui::TextDisabled(shapeFuture.valid() ? "Print shapes: loading in the background" : "Print shapes: loaded the first time prints are made");
		else if (status.shapesLoaded)
			ImGui::TextDisabled("Print shapes: %u from %s", status.shapesLoaded, status.shapeSource.c_str());
		else
			ImGui::TextDisabled("Print shapes: no footprint textures found, oval prints");
		ImGui::Text("Now: %s", !TrailsActive() ? "off" : !status.exterior ? "indoors" :
													 status.trailsDrawn   ? "on" :
																			"starting");
		if (status.trailsDrawn)
			ImGui::Text("Actors tracked: %u   Prints this frame: %u (trenches %u)   Map: %u x %u", status.actorsTracked, status.stamps, status.bodyStamps, status.trailMapSize, status.trailMapSize);
		if (status.exterior && TrailsActive())
			ImGui::Text("Map upkeep: %s", status.trailUpkeep);
		// (47m) "Load Footprint Shapes in the Background" (46r A/B) is permanent: the shapes are always prepared on a helper thread.
		ImGui::TreePop();
	}
}

void DynamicSnow::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.Condition = std::clamp(settings.Condition, 0, static_cast<int>(SnowCondition::Count) - 1);
	settings.TrailResolution = std::clamp(settings.TrailResolution, 0, 2);
	if (!std::isfinite(settings.SnowLineHeight) || settings.SnowLineHeight < 0.0f)
		settings.SnowLineHeight = 0.0f;
	if (!std::isfinite(settings.SnowLineFade))
		settings.SnowLineFade = 4000.0f;
	settings.SnowLineFade = std::clamp(settings.SnowLineFade, 1.0f, 100000.0f);
	coldCacheCell = nullptr;
}

void DynamicSnow::SaveSettings(json& o_json)
{
	o_json = settings;
}

void DynamicSnow::RestoreDefaultSettings()
{
	settings = {};
	coldCacheCell = nullptr;
}
