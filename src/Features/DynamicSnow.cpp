#include "DynamicSnow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

#include "Menu.h"
#include "State.h"
#include "Util.h"
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
	OverrideAmount,
	AmountOverride)

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
	return shaderType == RE::BSShader::Type::Lighting;
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
	}

	// Prints are drawn only once the map holds this window's content (cleared at least once
	// by a Prepass); the first frame after enabling simply has none.
	if (trailsWanted && trailContentValid && trailSRV) {
		data.Flags |= FlagTrails;
		if (settings.TrailsOnSnow)
			data.Flags |= FlagTrailsOnSnow;
		if (settings.TrailsOnAccumulated)
			data.Flags |= FlagTrailsOnAccumulated;
		if (settings.MudTrails)
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

void DynamicSnow::FindFeet(RE::Actor* a_actor, ActorFeet& a_feet)
{
	auto* root = a_actor->Get3D(false);
	a_feet.root.reset(root);
	a_feet.count = 0;
	a_feet.humanoid = false;
	for (auto& foot : a_feet.feet)
		foot.reset();
	a_feet.restHeight.fill(std::numeric_limits<float>::max());
	if (!root)
		return;

	static const RE::BSFixedString kLeftFoot("NPC L Foot [Lft ]");
	static const RE::BSFixedString kRightFoot("NPC R Foot [Rft ]");
	auto* left = root->GetObjectByName(kLeftFoot);
	auto* right = root->GetObjectByName(kRightFoot);
	if (left && right) {
		a_feet.feet[0].reset(left);
		a_feet.feet[1].reset(right);
		a_feet.count = 2;
		a_feet.humanoid = true;
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
}

void DynamicSnow::AddStamp(float a_x, float a_y, float a_z, float a_dirX, float a_dirY, float a_lengthUnits, float a_widthUnits)
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

	// Whole print inside the window, away from the faded edge.
	const float margin = std::max(s.Radii.x, s.Radii.y) + static_cast<float>(trailMapSize / 16);
	if (s.Center.x < margin || s.Center.y < margin || s.Center.x > trailMapSize - margin || s.Center.y > trailMapSize - margin)
		return;
	stamps.push_back(s);
}

void DynamicSnow::GatherStamps(const RE::NiPoint3& a_center)
{
	stamps.clear();
	status.actorsTracked = 0;

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	std::vector<RE::Actor*> actors;
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
		if (!actor || !actor->Is3DLoaded() || actor->IsDead())
			continue;
		const auto pos = actor->GetPosition();
		if (std::abs(pos.x - a_center.x) > reach || std::abs(pos.y - a_center.y) > reach)
			continue;
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

		const bool useFeet = feet.count > 0 && !(actor == player && playerFirstPerson);
		if (useFeet) {
			const float height = std::max(actor->GetHeight(), 32.0f);
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
				if (h - rest > 4.0f * scale)
					continue;
				if (feet.humanoid) {
					// Ankle node: the print's centre sits a little ahead of it.
					AddStamp(wp.x + dirX * 3.0f * scale, wp.y + dirY * 3.0f * scale, wp.z - rest,
						dirX, dirY, 9.0f * scale * size, 4.5f * scale * size);
				} else {
					const float r = std::clamp(height * 0.04f, 3.0f, 40.0f) * size;
					AddStamp(wp.x, wp.y, wp.z - rest, dirX, dirY, r * 1.2f, r);
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
			const float lengthUnits = (humanSized ? 9.0f * scale : std::clamp(height * 0.05f, 3.0f, 40.0f)) * size;
			const float widthUnits = (humanSized ? 4.5f * scale : std::clamp(height * 0.04f, 3.0f, 40.0f)) * size;
			AddStamp(pos.x + fy * side, pos.y - fx * side, pos.z, fx, fy, lengthUnits, widthUnits);
			feet.lastStride = pos;
			feet.strideLeft = !feet.strideLeft;
		}
	}

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
	//    up, i.e. a few times a second at the default refill time, not every frame.
	if (!globals::game::ui->GameIsPaused())
		decayAccumulator += RE::GetSecondsSinceLastFrame() / std::max(settings.TrailRefillSeconds, 1.0f) * 65535.0f;
	if (decayAccumulator >= 64.0f) {
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
			ID3D11ShaderResourceView* srvs[1] = { stampBuffer->srv.get() };
			context->CSSetShaderResources(0, 1, srvs);
			dispatch(stampCS, cb.StampCount, 1);
			srvs[0] = nullptr;
			context->CSSetShaderResources(0, 1, srvs);
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
		ImGui::SliderFloat("Normal Threshold", &settings.NormalThreshold, 0.0f, 0.95f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How flat a surface must be to hold snow. %.2f is about %.0f degrees from level;\n"
				"higher = only flatter surfaces get snow.",
				settings.NormalThreshold, std::acos(std::clamp(settings.NormalThreshold, 0.0f, 1.0f)) * 57.2958f);
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
			ImGui::Text("Actors tracked: %u   Prints this frame: %u   Map: %u x %u", status.actorsTracked, status.stamps, status.trailMapSize, status.trailMapSize);
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
		ImGui::TreePop();
	}
}

void DynamicSnow::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.Condition = std::clamp(settings.Condition, 0, static_cast<int>(SnowCondition::Count) - 1);
	settings.TrailResolution = std::clamp(settings.TrailResolution, 0, 2);
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
