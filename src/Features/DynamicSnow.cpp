#include "DynamicSnow.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/ActorUtils.h"
#include "Utils/Batch39.h"
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
	SmoothTrails,
	DetectAuthoredSnow,
	AlbedoSnowGuess,
	SnowOnCharacters,
	TrailSize,
	TrailDepth,
	TrailRefillSeconds,
	TrailDarken,
	MudStrength,
	SlopeCoverage,
	SlopeStart,
	SlopeFull,
	SnowOnTrees,
	TreeCoverage,
	SnowOnGrass,
	GrassCoverage,
	SnowOnLodTrees,
	LodTreeCoverage,
	UseModFootprintShapes,
	YieldToFootprintsMod,
	BodyAndObjectTrails,
	TrailRim,
	OverrideAmount,
	AmountOverride,
	FlipPrintShapes)

namespace
{
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

	bool IsSnowWeather(const RE::TESWeather* a_weather)
	{
		return a_weather && a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
	}

	bool HasPrecipitation(const RE::TESWeather* a_weather)
	{
		return a_weather && a_weather->precipitationData &&
		       a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy, RE::TESWeather::WeatherDataFlag::kSnow);
	}

	/// Share (by chance) of snow weathers in a weather list, or -1 if the list is empty.
	float SnowShare(RE::BSSimpleList<RE::WeatherType*>& a_list)
	{
		float total = 0.0f;
		float snow = 0.0f;
		for (auto* type : a_list) {
			if (!type || !type->weather)
				continue;
			const float chance = static_cast<float>(type->chance);
			total += chance;
			if (IsSnowWeather(type->weather))
				snow += chance;
		}
		return total > 0.0f ? snow / total : -1.0f;
	}

	std::string ToLower(std::string_view a_text)
	{
		std::string out(a_text);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
	return loaded && Batch39::IsOn() && settings.EnableAccumulation;
}

bool DynamicSnow::TrailsActive() const
{
	return loaded && Batch39::IsOn() && settings.EnableTrails &&
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
				const float share = SnowShare(static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes);
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
		best = SnowShare(a_worldSpace->climate->weatherList);
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

	const float snowFade = std::min(1.0f, (IsSnowWeather(sky->currentWeather) ? fadeCurrent : 0.0f) + (IsSnowWeather(sky->lastWeather) ? fadeLast : 0.0f));
	const float precipFade = std::min(1.0f, (HasPrecipitation(sky->currentWeather) ? fadeCurrent : 0.0f) + (HasPrecipitation(sky->lastWeather) ? fadeLast : 0.0f));
	status.snowWeather = snowFade > 0.0f;
	status.precipitation = precipFade > 0.0f;

	const auto condition = static_cast<SnowCondition>(std::clamp(settings.Condition, 0, static_cast<int>(SnowCondition::Count) - 1));
	const bool needCold = condition != SnowCondition::SnowWeather;
	status.coldRegion = needCold ? IsColdRegion(cell, player->GetWorldspace()) : false;

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
// Per-frame state
// ---------------------------------------------------------------------------------------------

void DynamicSnow::UpdateFrameState()
{
	if (!frameChecker.IsNewFrame())
		return;
	++frameIndex;

	UpdateFootprintsModState();
	status.yieldingToMod = settings.YieldToFootprintsMod && footprintsModLoaded;

	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* sky = globals::game::sky;
		status.exterior = cell && cell->IsExteriorCell() && sky && sky->mode.get() == RE::Sky::Mode::kFull;
	}

	if (AccumulationActive())
		UpdateAccumulation();

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
		if (settings.SnowOnCharacters)
			data.Flags |= FlagSnowOnCharacters;
		// (batch 39c)
		if (settings.SlopeCoverage)
			data.Flags |= FlagCoverageSlope;
		if (settings.SnowOnTrees)
			data.Flags |= FlagTrees;
		if (settings.SnowOnGrass)
			data.Flags |= FlagGrass;
		if (settings.SnowOnLodTrees)
			data.Flags |= FlagLodTrees;
	}

	// Prints are drawn only once the map holds this window's content (cleared at least once
	// by a Prepass); the first frame after enabling simply has none.
	// (batch 39c) With the Footprints mod loaded, its decals own snowy ground and mud (it picks
	// them from the ground's material); ours stay in built-up snow, so the two never double up.
	const bool yielding = status.yieldingToMod;
	if (trailsWanted && trailContentValid && trailSRV) {
		data.Flags |= FlagTrails;
		if (settings.TrailsOnSnow && !yielding)
			data.Flags |= FlagTrailsOnSnow;
		if (settings.TrailsOnAccumulated)
			data.Flags |= FlagTrailsOnAccumulated;
		if (settings.MudTrails && !yielding)
			data.Flags |= FlagMudTrails;
		if (settings.SmoothTrails)
			data.Flags |= FlagSmoothTrails;
	}
	// Authored-snow recognition matters to both halves: accumulation leaves authored snow as it
	// is, and prints there are snow prints, not mud.
	if (data.Flags != 0 && settings.DetectAuthoredSnow) {
		data.Flags |= FlagLandSnowDetect;
		if (settings.AlbedoSnowGuess)
			data.Flags |= FlagAlbedoSnowGuess;
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

	// (batch 39c)
	data.SlopeStart = std::clamp(settings.SlopeStart, 0.0f, 0.95f);
	data.SlopeFull = std::clamp(settings.SlopeFull, data.SlopeStart + 0.02f, 1.0f);
	data.TreeCoverage = std::clamp(settings.TreeCoverage, 0.0f, 1.0f);
	data.GrassCoverage = std::clamp(settings.GrassCoverage, 0.0f, 1.0f);
	data.LodTreeCoverage = std::clamp(settings.LodTreeCoverage, 0.0f, 1.0f);
	data.TrailRim = std::clamp(settings.TrailRim, 0.0f, 1.0f);

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
	if (!decayCS)
		decayCS = compile("DECAY");
	if (!stampCS)
		stampCS = compile("STAMP");
}

void DynamicSnow::ClearShaderCache()
{
	for (auto** cs : { &clearCS, &decayCS, &stampCS }) {
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

void DynamicSnow::UpdateFootprintsModState()
{
	if (footprintsModChecked)
		return;
	auto* data = RE::TESDataHandler::GetSingleton();
	if (!data)
		return;
	footprintsModChecked = true;
	footprintsModLoaded = data->LookupLoadedModByName("Footprints.esp"sv) != nullptr || data->LookupLoadedLightModByName("Footprints.esp"sv) != nullptr;
	status.footprintsMod = footprintsModLoaded;
	logger::info("[{}] Footprints mod (Footprints.esp) {}", GetName(), footprintsModLoaded ? "loaded" : "not loaded");
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

void DynamicSnow::LoadPrintShapes()
{
	shapesTried = true;
	shapeSRV = nullptr;
	shapeCount = 0;
	for (auto& s : shapeSlots)
		s = {};
	status.shapesLoaded = 0;
	status.shapeSource.clear();

	std::vector<std::vector<float>> slices;
	std::vector<uint8_t> file;
	std::vector<float> shape;
	bool anyPbr = false, anyClassic = false;
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
				ok = anyPbr = true;
			} else if (ReadGameFile(classic, file) && DecodeShape(file, shape)) {
				ok = anyClassic = true;
			}
			if (!ok)
				continue;
			shapeSlots[k].slot[f] = static_cast<int>(slices.size());
			slices.push_back(shape);
		}
	}
	if (slices.empty()) {
		logger::info("[{}] No footprint textures found (Footprints / Realistic PBR Footprints); using the built-in oval prints", GetName());
		return;
	}

	// Mip chain per slice, box filtered (the trail map is coarser than the shapes at 1024).
	uint32_t mips = 1;
	while ((kShapeSize >> mips) >= 4)
		++mips;
	std::vector<std::vector<float>> levels;
	std::vector<D3D11_SUBRESOURCE_DATA> init;
	levels.reserve(slices.size() * mips);
	for (auto& slice : slices) {
		size_t size = kShapeSize;
		levels.push_back(slice);
		for (uint32_t m = 1; m < mips; ++m) {
			const auto& prev = levels.back();
			const size_t half = size / 2;
			std::vector<float> next(half * half);
			for (size_t y = 0; y < half; ++y)
				for (size_t x = 0; x < half; ++x)
					next[y * half + x] = 0.25f * (prev[(2 * y) * size + 2 * x] + prev[(2 * y) * size + 2 * x + 1] + prev[(2 * y + 1) * size + 2 * x] + prev[(2 * y + 1) * size + 2 * x + 1]);
			levels.push_back(std::move(next));
			size = half;
		}
	}
	init.resize(levels.size());
	for (size_t s = 0; s < slices.size(); ++s) {
		for (uint32_t m = 0; m < mips; ++m) {
			auto& d = init[s * mips + m];
			d.pSysMem = levels[s * mips + m].data();
			d.SysMemPitch = static_cast<UINT>((kShapeSize >> m) * sizeof(float));
		}
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(kShapeSize);
	desc.Height = static_cast<UINT>(kShapeSize);
	desc.MipLevels = mips;
	desc.ArraySize = static_cast<UINT>(slices.size());
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	winrt::com_ptr<ID3D11Texture2D> texture;
	auto device = globals::d3d::device;
	if (FAILED(device->CreateTexture2D(&desc, init.data(), texture.put())) ||
		FAILED(device->CreateShaderResourceView(texture.get(), nullptr, shapeSRV.put()))) {
		logger::warn("[{}] Could not create the footprint shape atlas; using the built-in oval prints", GetName());
		shapeSRV = nullptr;
		for (auto& s : shapeSlots)
			s = {};
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
	shapeCount = static_cast<uint32_t>(slices.size());
	status.shapesLoaded = shapeCount;
	status.shapeSource = anyPbr && anyClassic ? "Realistic PBR Footprints + Footprints" : anyPbr ? "Realistic PBR Footprints" : "Footprints";
	logger::info("[{}] Loaded {} footprint shapes from {}", GetName(), shapeCount, status.shapeSource);
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
	if ((a_shape & 0xFFFF) < kShapeCapsule && settings.FlipPrintShapes) {
		// Debug: heel and toe of the installed textures the other way round.
		s.Axis = { -s.Axis.x, -s.Axis.y };
		s.Shape ^= kShapeMirror;
	}
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

	if (settings.UseModFootprintShapes && !shapesTried)
		LoadPrintShapes();
	const bool useShapes = settings.UseModFootprintShapes && shapeCount > 0;

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
	if (!trailUAV || !clearCS || !decayCS || !stampCS)
		return;

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	GatherStamps(player->GetPosition());

	auto timers = Util::GpuPassTimers::GetSingleton();
	timers->Begin(Util::GpuBucket::SnowTrails);

	const int64_t mask = static_cast<int64_t>(trailMapSize - 1);
	TrailCB cb{};
	cb.Wrap[0] = static_cast<int>(windowOrigin[0] & mask);
	cb.Wrap[1] = static_cast<int>(windowOrigin[1] & mask);
	cb.MapSize = trailMapSize;

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
		decayAccumulator = 0.0f;
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

	// 2. Refill. Batched: a whole-map pass only once enough strength (about 0.1%) has built
	//    up, i.e. a few times a second at the default refill time, not every frame. (39c) Depth
	//    and rim are 15-bit codes now (SnowTrailsCS.hlsl).
	if (!globals::game::ui->GameIsPaused())
		decayAccumulator += RE::GetSecondsSinceLastFrame() / std::max(settings.TrailRefillSeconds, 1.0f) * 32767.0f;
	if (decayAccumulator >= 32.0f) {
		cb.DecayStep = static_cast<uint>(decayAccumulator);
		decayAccumulator -= static_cast<float>(cb.DecayStep);
		dispatch(decayCS, (trailMapSize + 7) / 8, (trailMapSize + 7) / 8);
		cb.DecayStep = 0;
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
			dispatch(stampCS, cb.StampCount, 1);
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

void DynamicSnow::Prepass()
{
	UpdateFrameState();

	auto context = globals::d3d::context;
	if (trailsWanted && trailTexture)
		UpdateTrailMap();

	ID3D11ShaderResourceView* srv = (trailsWanted && trailSRV) ? trailSRV.get() : nullptr;
	context->PSSetShaderResources(kTrailSlot, 1, &srv);
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void DynamicSnow::DrawSettings()
{
	if (ImGui::TreeNodeEx("Snow Accumulation (Batch 39)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Snow Accumulation", &settings.EnableAccumulation);
		Batch39::MasterNote();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"While it snows, the tops of the ground, roofs, rocks and other upward-facing surfaces slowly turn white;\n"
				"after the snow stops it slowly melts away. Nothing builds up under roofs or indoors.\n"
				"Surfaces that are already snow (snowy ground textures, snow-covered rocks) are left as they are.");

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

		ImGui::BeginDisabled(settings.Condition == static_cast<int>(SnowCondition::SnowWeather));
		ImGui::SliderFloat("Cold Region Threshold", &settings.ColdRegionSnowShare, 0.05f, 0.9f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"A region counts as cold when at least this share of the weathers it can have are snow weathers\n"
				"(read from the region's weather list, or the worldspace's climate where there is none).");
		ImGui::EndDisabled();

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
		ImGui::Checkbox("Even Cover on Roofs and Slopes (39c)", &settings.SlopeCoverage);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"On (default): how much snow a surface holds depends on its overall slope only, with a gentle\n"
				"fade between the two angles below, so a roof gets the same cover whatever its texture. The\n"
				"texture's bumps only decide where thin snow sits first, and thick snow hides them.\n"
				"Off: the 39b rule (a hard cut at Normal Threshold, half decided by the texture's bumps):\n"
				"steep thatch roofs came out thin and streaky.");
		if (settings.SlopeCoverage) {
			auto degrees = [](float a_z) { return std::acos(std::clamp(a_z, 0.0f, 1.0f)) * 57.2958f; };
			ImGui::SliderFloat("Snow Starts Holding At", &settings.SlopeStart, 0.0f, 0.9f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Steepest surface that holds any snow: %.2f is about %.0f degrees from level.", settings.SlopeStart, degrees(settings.SlopeStart));
			ImGui::SliderFloat("Full Cover From", &settings.SlopeFull, 0.05f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Surfaces at least this flat get full cover: %.2f is about %.0f degrees from level.\n"
					"Most roofs are 35-55 degrees.",
					settings.SlopeFull, degrees(settings.SlopeFull));
			settings.SlopeFull = std::max(settings.SlopeFull, settings.SlopeStart + 0.02f);
		} else {
			ImGui::SliderFloat("Normal Threshold", &settings.NormalThreshold, 0.0f, 0.95f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How flat a surface must be to hold snow. %.2f is about %.0f degrees from level;\n"
					"higher = only flatter surfaces get snow.",
					settings.NormalThreshold, std::acos(std::clamp(settings.NormalThreshold, 0.0f, 1.0f)) * 57.2958f);
		}

		ImGui::Checkbox("Snow on Trees and Bushes (39c)", &settings.SnowOnTrees);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Trees, bushes and ferns get snow on the tops of their branches and leaves.\n"
				"It stays put while the leaves sway in the wind.");
		ImGui::BeginDisabled(!settings.SnowOnTrees);
		ImGui::SliderFloat("Tree Snow Amount", &settings.TreeCoverage, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Checkbox("Snow on Grass (39c)", &settings.SnowOnGrass);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Grass turns white from the root up as snow builds (as if half buried), with a dusting on top.\n"
				"Not under roofs or dense trees, like the ground.");
		ImGui::BeginDisabled(!settings.SnowOnGrass);
		ImGui::SliderFloat("Grass Snow Amount", &settings.GrassCoverage, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Checkbox("Snow on Distant Trees (39c)", &settings.SnowOnLodTrees);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Far-away (LOD) trees get a matching white tint, whiter towards the top,\n"
				"so near and far forests look alike.");
		ImGui::BeginDisabled(!settings.SnowOnLodTrees);
		ImGui::SliderFloat("Distant Tree Snow Amount", &settings.LodTreeCoverage, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Checkbox("Snow on Characters", &settings.SnowOnCharacters);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Off (default): people, creatures and everything they wear or carry stay free of built-up snow.\n"
				"On: their upward-facing parts (shoulders, hoods) get snow like the ground does.");

		if (ImGui::TreeNodeEx("Snow Look")) {
			ImGui::ColorEdit3("Snow Colour", &settings.SnowColor.x);
			ImGui::SliderFloat("Snow Roughness", &settings.SnowRoughness, 0.3f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Snow never makes a surface shinier than it was; this only roughens shiny surfaces under snow.");
			ImGui::TreePop();
		}
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::Text("Now: %s", !AccumulationActive() ? "off" :
							   !status.exterior      ? "indoors (kept as it was)" :
							   status.snowing        ? "snowing, building up" :
							   status.amount > 0.0f  ? "melting" :
													   "no snow");
		ImGui::Text("Accumulated: %.0f%%   Snowfall: %.0f%%", status.amount * 100.0f, status.snowfall * 100.0f);
		if (settings.Condition != static_cast<int>(SnowCondition::SnowWeather))
			ImGui::Text("Region: %s (%s)", status.region.empty() ? "-" : status.region.c_str(), status.coldRegion ? "cold" : "not cold");
		ImGui::TreePop();
	}

	ImGui::Spacing();

	if (ImGui::TreeNodeEx("Footprints and Trails (Batch 39)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Footprints", &settings.EnableTrails);
		Batch39::MasterNote();
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

		ImGui::Checkbox("Use Installed Footprint Textures (39c)", &settings.UseModFootprintShapes);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Shapes the prints like real boots and paws, read from the footprint textures of mods you have\n"
				"installed (Realistic PBR Footprints, or Footprints itself): human, horse, wolf, bear, sabre cat,\n"
				"deer, cow, giant, mammoth, troll, werewolf, skeever. Each print points the way the foot points\n"
				"and has a small raised rim. Off, or no such mod installed: plain oval prints.");
		if (settings.UseModFootprintShapes) {
			if (!shapesTried)
				ImGui::TextDisabled("  (loaded the first time prints are made)");
			else if (status.shapesLoaded)
				ImGui::TextDisabled("  %u shapes from %s", status.shapesLoaded, status.shapeSource.c_str());
			else
				ImGui::TextDisabled("  none found: oval prints");
		}
		ImGui::Checkbox("Leave Snowy Ground to the Footprints Mod (39c)", &settings.YieldToFootprintsMod);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Only matters when the Footprints mod (Footprints.esp) is loaded. It already puts its own prints on\n"
				"snowy ground and mud, so on (default) ours are made only in built-up snow and the two never double up.\n"
				"Off: ours everywhere set above as well - pick this if you switched the Footprints mod's prints off\n"
				"in its MCM and want ours instead. Theirs are sharper close up; ours are real dents that fill back\n"
				"in, work on built-up snow and for every nearby actor.");
		ImGui::SameLine();
		ImGui::TextDisabled(status.footprintsMod ? "(Footprints mod: loaded)" : "(Footprints mod: not loaded)");
		ImGui::Checkbox("Trenches from Bodies and Objects (39c)", &settings.BodyAndObjectTrails);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Bodies being dragged or sliding, and loose objects moving through snow, leave trenches\n"
				"(from their collision shapes).");
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
		ImGui::Checkbox("Smooth Footprints", &settings.SmoothTrails);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Reads the footprint map with a smooth (bicubic) filter, so the print's edges and slopes are soft\n"
				"instead of showing the map's square grid (the 'mosaic' look). Off = the 39a reading, for comparison.");
		const char* resolutions[] = { "1024 (4 units per texel, 4 MB)", "2048 (2 units per texel, 16 MB)", "4096 (1 unit per texel, 64 MB)" };
		ImGui::Combo("Trail Map Detail", &settings.TrailResolution, resolutions, 3);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Detail of the footprint map around the player (always 58 m across). Changing it clears existing prints.\n"
				"4096 gives the sharpest prints; it costs 64 MB of video memory and its refill pass touches 4x more memory.");
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::Checkbox("Recognise Snowy Ground", &settings.DetectAuthoredSnow);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Finds the snow that is already part of the map (e.g. High Hrothgar): ground whose land texture is marked as snow,\n"
				"has a snow material, or has 'snow' in its file name, also with PBR terrain mods; and white directional snow on rocks.\n"
				"Prints there are snow prints, and built-up snow leaves it as it is. Off = 39a, where PBR terrain never counted as snow.");
		ImGui::BeginDisabled(!settings.DetectAuthoredSnow);
		ImGui::Checkbox("Also Guess From Colour (bright white ground)", &settings.AlbedoSnowGuess);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Last resort for land textures that carry no snow marking at all: bright, grey-white ground counts as snow.\n"
				"Can also catch pale rock or sand.");
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::Text("Now: %s", !TrailsActive() ? "off" : !status.exterior ? "indoors" :
													 status.trailsDrawn   ? "on" :
																			"starting");
		if (status.trailsDrawn)
			ImGui::Text("Actors tracked: %u   Prints this frame: %u (trenches %u)   Map: %u x %u", status.actorsTracked, status.stamps, status.bodyStamps, status.trailMapSize, status.trailMapSize);
		ImGui::TreePop();
	}

	ImGui::Spacing();

	if (ImGui::TreeNodeEx("Debug")) {
		ImGui::Checkbox("Override Accumulated Amount", &settings.OverrideAmount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Test only: draw this much accumulated snow whatever the weather (outdoors only).");
		ImGui::BeginDisabled(!settings.OverrideAmount);
		ImGui::SliderFloat("Amount", &settings.AmountOverride, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Checkbox("Turn Footprint Shapes Around", &settings.FlipPrintShapes);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Check only: if the shaped prints point backwards (toe where the heel should be), tick this.\n"
				"The installed textures carry no direction marker; 39c assumes the heel is at the image's top.");
		ImGui::TreePop();
	}
}

void DynamicSnow::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.Condition = std::clamp(settings.Condition, 0, static_cast<int>(SnowCondition::Count) - 1);
	settings.TrailResolution = std::clamp(settings.TrailResolution, 0, 2);
	settings.SlopeStart = std::clamp(settings.SlopeStart, 0.0f, 0.9f);
	settings.SlopeFull = std::clamp(std::max(settings.SlopeFull, settings.SlopeStart + 0.02f), 0.05f, 1.0f);
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
