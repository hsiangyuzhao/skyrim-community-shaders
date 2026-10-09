#include "SkySync.h"

#include "PhysicalSky.h"
#include "VolumetricLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkySync::Settings,
	Enabled,
	UseAlternateSunPath,
	MoonLightSource,
	SunPath,
	CustomAngle,
	MoonOrbit,
	MasserNightPosition,
	SecundaNightPosition,
	NightArc,
	KeepMoonPosition)

void SkySync::DrawSettings()
{
	ImGui::Checkbox("Enabled", &settings.Enabled);

	ImGui::Checkbox("Use alternate sun path", &settings.UseAlternateSunPath);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Changes the tilt of the sun's daily path across the sky (picked below) for different shadow angles.");

	if (settings.UseAlternateSunPath) {
		if (ImGui::SliderInt("Sun path", &settings.SunPath, 0, static_cast<uint8_t>(SunPath::Count) - 1, SunPathNames[settings.SunPath], ImGuiSliderFlags_AlwaysClamp))
			SetSunAngle();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Southern/Northern Sky: the sun path tilts 35 degrees to that side for longer, more angled shadows. Vanilla: Skyrim's near-overhead path.");

		if (settings.SunPath == static_cast<int32_t>(SunPath::Custom)) {
			if (ImGui::SliderFloat("Custom angle", &settings.CustomAngle, -90.0f, 90.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
				SetSunAngle();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Tilt of the sun's path in degrees. 0 = straight overhead; -35 matches Southern Sky, +35 Northern Sky.");
		}
	}

	ImGui::SliderInt("Moon light source", &settings.MoonLightSource, 0, static_cast<uint8_t>(MoonLightSource::Count) - 1, MoonLightSourceNames[settings.MoonLightSource], ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Which moon lights the night. Brightest = whichever moon is brighter.");

	if (globals::features::physicalSky.IsLegacy())
		ImGui::TextColored({ 1.f, 0.8f, 0.3f, 1.f }, "Physical Sky's Sky Model is Legacy (36f): Moon Orbit is Vanilla and the discs\nare lowered by altitude right now, whatever is set below.");
	ImGui::Combo("Moon Orbit", &settings.MoonOrbit, MoonOrbitNames, static_cast<int>(MoonOrbit::Count));
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(
			"Where Masser and Secunda are in the sky.\n"
			"Night Sky (default): both moons are up every night, Secunda a little ahead of Masser on the same path across the sky.\n"
			"  They are up at dusk, cross the sky slowly through the night and set after dawn.\n"
			"Stable: each moon follows the date and time at its own speed (Masser overhead at midnight).\n"
			"  Secunda is faster and drifts a fifth of a turn a day, so it is up at night only about 2 nights in 5.\n"
			"Vanilla: the game's own stepping. Changing the time throws Secunda to a new place each time.");
	if (settings.MoonOrbit == static_cast<int32_t>(MoonOrbit::NightSky)) {
		ImGui::SliderFloat("Masser Position", &settings.MasserNightPosition, -60.0f, 60.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Where Masser is along its path at the middle of the night, in degrees from its highest point.\n"
				"Negative = towards the side it rises on. Default -15.");
		ImGui::SliderFloat("Secunda Position", &settings.SecundaNightPosition, -60.0f, 60.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Same for Secunda. Default +15: about 25 degrees from Masser in the sky, close but not touching.\n"
				"Both at the same number puts Secunda against Masser's lower edge, as the game often does.");
		ImGui::SliderFloat("Movement Through the Night", &settings.NightArc, 0.0f, 140.0f, "%.0f degrees", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"How far the moons travel from dusk to dawn. 0 = they stand still all night.\n"
				"Above about 80 with the default positions, a moon starts the night or ends it low and faint.");
	}
	if (!REL::Module::IsSE())
		ImGui::TextDisabled("(Stable and Night Sky only work on Skyrim SE 1.5.97; Vanilla is used here)");

	ImGui::Checkbox("Keep Moons Where the Game Puts Them", &settings.KeepMoonPosition);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(
			"On (default): Sky Sync no longer lowers the moon discs by your altitude (1.5-4 degrees), as in current\n"
			"upstream; Physical Sky's moon glow follows the disc's real position on screen. The moonlight direction is\n"
			"unchanged. Off: the discs are lowered as before and the glow uses the un-lowered direction.");
}

SkySync::Settings SkySync::Effective() const
{
	Settings s = settings;
	if (globals::features::physicalSky.IsLegacy()) {
		// 36f: the engine's own moon stepping, discs lowered by altitude (40d/40e options off).
		s.MoonOrbit = static_cast<int32_t>(MoonOrbit::Vanilla);
		s.KeepMoonPosition = false;
	}
	return s;
}

void SkySync::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MoonLightSource = std::clamp(settings.MoonLightSource, static_cast<int32_t>(MoonLightSource::Brightest), static_cast<int32_t>(MoonLightSource::Secunda));
	settings.SunPath = std::clamp(settings.SunPath, static_cast<int32_t>(SunPath::Southern), static_cast<int32_t>(SunPath::Custom));
	settings.MoonOrbit = std::clamp(settings.MoonOrbit, static_cast<int32_t>(MoonOrbit::Vanilla), static_cast<int32_t>(MoonOrbit::NightSky));
	settings.MasserNightPosition = std::clamp(settings.MasserNightPosition, -60.0f, 60.0f);
	settings.SecundaNightPosition = std::clamp(settings.SecundaNightPosition, -60.0f, 60.0f);
	settings.NightArc = std::clamp(settings.NightArc, 0.0f, 140.0f);
	SetSunAngle();
}

void SkySync::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkySync::RestoreDefaultSettings()
{
	settings = {};
	SetSunAngle();
}

void SkySync::PostPostLoad()
{
	moonAndStarsLoaded = GetModuleHandle(L"po3_MoonMod.dll");
	if (moonAndStarsLoaded)
		logger::info("[Sky Sync] Moon and Stars detected, compatibility enabled");

	if (GetModuleHandle(L"EVLaS.dll")) {
		DisableOnConflict("EVLaS");
		return;
	}

	stl::detour_thunk<Moon_Update>(REL::RelocationID(25626, 26169));
	stl::detour_thunk<Sky_Update>(REL::RelocationID(25682, 26229));
	stl::detour_thunk<Sky_OnNewClimate>(REL::RelocationID(25695, 26242));
	stl::write_thunk_call<ApplyVolumetricLighting_VolumetricLightingDescriptor_Get>(REL::RelocationID(100475, 107193).address() + 0x354);

	gSunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());
	gSunGlareSize = reinterpret_cast<float*>(REL::RelocationID(502611, 370235).address());
	gMasserSize = reinterpret_cast<uint32_t*>(REL::RelocationID(502558, 370155).address());
	gSecundaSize = reinterpret_cast<uint32_t*>(REL::RelocationID(502570, 370173).address());

	logger::info("[Sky Sync] Installed hooks");
}

void SkySync::DataLoaded()
{
	const auto data = RE::TESDataHandler::GetSingleton();
	if (data && (data->LookupLoadedModByName("DVLaSS.esp"sv) || data->LookupLoadedLightModByName("DVLaSS.esp"sv)))
		DisableOnConflict("DVLaSS");
}

void SkySync::DisableOnConflict(std::string_view conflictName)
{
	failedLoadedMessage = fmt::format("Disabled as {} has been detected, both cannot be used together", conflictName);
	loaded = false;
	settings.Enabled = false;
	logger::warn("[Sky Sync] {}", failedLoadedMessage);
}

void SkySync::Sky_Update::thunk(RE::Sky* sky)
{
	func(sky);
	globals::features::skySync.Update(sky);
}

void SkySync::Update(const RE::Sky* sky)
{
	if (!settings.Enabled)
		return;

	const auto sun = sky->sun;
	const auto climate = sky->currentClimate;
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!sun || !climate || !player)
		return;

	if (const auto cell = player->GetParentCell(); cell != currentCell) {
		SetSkyRotation(sky, cell);
		if (currentCell && (cell->IsInteriorCell() != currentCell->IsInteriorCell() || cell->GetRuntimeData().worldSpace != currentCell->GetRuntimeData().worldSpace))
			shadowFader.Reset();
	}

	const float time = sky->currentGameHour;
	const bool isDayTime = time > timings.sunriseFadeOutMoonEnd && time < timings.sunsetFadeInMoonStart;

	const auto worldSpace = player->GetWorldspace();
	const float altitude = worldSpace ? player->GetPositionZ() - worldSpace->GetDefaultWaterHeight() : 0.0f;

	ProcessSun(sun, time, altitude, isDayTime);
	ProcessMoon(sky->masser, time, Caster::Masser, altitude, isDayTime);
	ProcessMoon(sky->secunda, time, Caster::Secunda, altitude, isDayTime);

	shadowFader.Update(sun, directions, intensities, lightColors, isDayTime);
}
void SkySync::SetSunAngle()
{
	switch (static_cast<SunPath>(settings.SunPath)) {
	case SunPath::Southern:
		sunAngle = SouthernSunAngle;
		break;
	case SunPath::Northern:
		sunAngle = NorthernSunAngle;
		break;
	case SunPath::Vanilla:
		sunAngle = VanillaSunAngle;
		break;
	case SunPath::Custom:
		sunAngle = 90.0f + settings.CustomAngle;
		break;
	default:;
	}
}

void SkySync::SetSkyRotation(const RE::Sky* sky, RE::TESObjectCELL* cell)
{
	// If the interior cell isn't initialised it won't have the north rotation extra data ready, skip for a frame
	if (cell->IsInteriorCell() && cell->cellState == static_cast<RE::TESObjectCELL::CellState>(0))
		return;

	currentCell = cell;
	const float rotation = cell->GetNorthRotation();
	if (rotation == currentSkyRotation)
		return;

	currentSkyRotation = rotation;
	sky->root->local.rotate = RE::NiMatrix3{ RE::NiPoint3{ 0.0f, 0.0f, -rotation } };
	RE::NiUpdateData updateData;
	sky->root->Update(updateData);
}

void SkySync::ProcessSun(const RE::Sun* sun, const float time, const float altitude, const bool isDayTime)
{
	RE::NiPoint3 dir;
	float dist;

	if (settings.UseAlternateSunPath) {
		CalculateAlternateSunDirectionAndDistance(dir, dist, time, timings.sunrise, timings.sunset, sunAngle);
	} else
		CalculateSunDirectionAndDistance(sun, dir, dist);

	rawDirections[static_cast<int>(Caster::Sun)] = dir;

	const RE::NiPoint3 apparentDir = GetApparentDirection(dir, altitude);
	SetSunPosition(sun, apparentDir, dist);

	directions[static_cast<int>(Caster::Sun)] = apparentDir;

	SetSunBaseVisibility(sun, isDayTime ? 1.0f : 0.0f);

	intensities[static_cast<int>(Caster::Sun)] = isDayTime ? CalculateVisibility(dir, dist, *gSunGlareSize * SunScaleFactor) : 0.0f;
}

void SkySync::ProcessMoon(const RE::Moon* moon, const float time, const Caster type, const float altitude, const bool isDayTime)
{
	intensities[static_cast<int>(type)] = 0.0f;
	directions[static_cast<int>(type)] = { 0.0f, 0.0f, 1.0f };
	rawDirections[static_cast<int>(type)] = { 0.0f, 0.0f, -1.0f };

	if (!moon)
		return;

	const auto dir = moon->root->local.rotate.GetVectorY();

	rawDirections[static_cast<int>(type)] = dir;

	auto apparentDir = GetApparentDirection(dir, altitude);
	// (40d) Off = the old behaviour: the altitude dip is written into the moon's rotation, which
	// moves the disc. On: the disc stays where the game put it; only the light uses the dip.
	if (!Effective().KeepMoonPosition)
		SetMoonDirection(moon, apparentDir);

	// Moon and Stars adjusts some intermediary rotation matrices for the moon
	// Directly changing the directions here avoids 3 matrix multiplications and a vector rotation
	if (moonAndStarsLoaded)
		apparentDir = { apparentDir.y, -apparentDir.x, apparentDir.z };

	directions[static_cast<int>(type)] = apparentDir;

	if (isDayTime)
		return;

	const auto src = static_cast<MoonLightSource>(settings.MoonLightSource);
	const bool isValidSource = src == MoonLightSource::Brightest || (src == MoonLightSource::Masser && type == Caster::Masser) || (src == MoonLightSource::Secunda && type == Caster::Secunda);
	if (!isValidSource)
		return;

	const float moonRadius = type == Caster::Masser ? static_cast<float>(*gMasserSize) : static_cast<float>(*gSecundaSize);
	float intensity = CalculateVisibility(dir, moon->moonMesh->local.translate.y, moonRadius);

	if (type == Caster::Masser)
		intensity *= masserPhaseIntensityFactor;
	else if (type == Caster::Secunda)
		intensity *= secundaPhaseIntensityFactor * SecundaIntensityFactor;

	if (time >= timings.sunriseFadeOutMoonStart && time <= timings.sunriseFadeOutMoonEnd)
		intensity *= SmoothStep(timings.sunriseFadeOutMoonEnd, timings.sunriseFadeOutMoonStart, time);
	else if (time >= timings.sunsetFadeInMoonStart && time <= timings.sunsetFadeInMoonEnd)
		intensity *= SmoothStep(timings.sunsetFadeInMoonStart, timings.sunsetFadeInMoonEnd, time);

	intensities[static_cast<int>(type)] = intensity;
}

inline void SkySync::CalculateSunDirectionAndDistance(const RE::Sun* sun, RE::NiPoint3& outDir, float& outDistance)
{
	outDir = sun->root->local.translate;
	if (outDistance = outDir.Unitize(); outDistance < FLT_EPSILON) {
		outDir = { 0.0f, 0.0f, 1.0f };
		outDistance = SunPeakDistance;
	}
}

inline void SkySync::CalculateAlternateSunDirectionAndDistance(RE::NiPoint3& outDir, float& outDist, const float time, const float sunrise, const float sunset, const float sunAngle)
{
	const float phi = DirectX::XM_PI * ((time - sunrise) / (sunset - sunrise));
	float sinPhi, cosPhi;
	DirectX::XMScalarSinCosEst(&sinPhi, &cosPhi, phi);

	float tiltRadians = DirectX::XMConvertToRadians(sunAngle);
	float cosTilt, sinTilt;
	DirectX::XMScalarSinCosEst(&sinTilt, &cosTilt, tiltRadians);

	outDir = { cosPhi, -sinPhi * cosTilt, sinPhi * sinTilt };

	if (const float length = outDir.Unitize(); length < FLT_EPSILON)
		outDir = { 0.0f, 0.0f, 1.0f };

	const float elevationRatio = std::max(sinPhi, 0.0f);
	outDist = std::lerp(SunHorizonDistance, SunPeakDistance, elevationRatio);
}

RE::NiPoint3 SkySync::GetApparentDirection(const RE::NiPoint3& dir, const float altitude)
{
	const float dipAngle = -std::atan(altitude / RenderDistance);
	float sinPhi, cosPhi;
	DirectX::XMScalarSinCosEst(&sinPhi, &cosPhi, dipAngle);

	const auto rotationAxis = dir.UnitCross({ 0.0f, 0.0f, 1.0f });
	const float axisDotDir = rotationAxis.Dot(dir);
	const auto axisCrossDir = rotationAxis.Cross(dir);
	const float oneMinusCosPhi = 1.0f - cosPhi;

	const float x = dir.x * cosPhi + axisCrossDir.x * sinPhi + rotationAxis.x * (axisDotDir * oneMinusCosPhi);
	const float y = dir.y * cosPhi + axisCrossDir.y * sinPhi + rotationAxis.y * (axisDotDir * oneMinusCosPhi);
	const float z = dir.z * cosPhi + axisCrossDir.z * sinPhi + rotationAxis.z * (axisDotDir * oneMinusCosPhi);

	RE::NiPoint3 rotated = { x, y, z };
	rotated.Unitize();
	return rotated;
}

inline void SkySync::SetSunPosition(const RE::Sun* sun, const RE::NiPoint3& dir, const float distance)
{
	const auto position = dir * distance;
	sun->root->local.translate = position;
	sun->sunGlareNode->local.translate = position;
	*gSunPosition = position;
}

inline void SkySync::SetMoonDirection(const RE::Moon* moon, const RE::NiPoint3& dir)
{
	auto& m = moon->root->local.rotate;
	m.entry[0][1] = dir.x;
	m.entry[1][1] = dir.y;
	m.entry[2][1] = dir.z;
}

inline float SkySync::CalculateVisibility(const RE::NiPoint3& dir, const float dist, const float radius)
{
	const float height = dir.Dot({ 0.0f, 0.0f, 1.0f }) * dist;
	return SmoothStep(-radius, radius, height);
}

inline void SkySync::SetSunBaseVisibility(const RE::Sun* sun, const float visibility)
{
	if (const auto property = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().properties[1].get()))
		property->kBlendColor.alpha = visibility;
}

void SkySync::ShadowFader::Reset()
{
	fadePhase = Phase::None;
	current = Caster::None;
	target = Caster::None;
	fadeTimer = 0.0f;
}

void SkySync::ShadowFader::Update(const RE::Sun* sun, RE::NiPoint3 dirs[3], float intensities[3], std::optional<std::array<RE::NiColor, 3>> colors, const bool isDayTime)
{
	const float masserIntensity = intensities[static_cast<int>(Caster::Masser)];
	const float secundaIntensity = intensities[static_cast<int>(Caster::Secunda)];

	auto desired = Caster::None;
	if (isDayTime)
		desired = Caster::Sun;
	else if (masserIntensity > 0.0f && masserIntensity >= secundaIntensity)
		desired = Caster::Masser;
	else if (secundaIntensity > 0.0f)
		desired = Caster::Secunda;

	if (desired != target) {
		target = desired;
		fadeTimer = 0.0f;

		if (current == Caster::None) {
			fadePhase = Phase::FadeIn;
			current = target;
		} else
			fadePhase = Phase::FadeOut;
	}

	const auto calendar = RE::Calendar::GetSingleton();
	const float currentHoursPassed = calendar->GetHoursPassed();
	const float timeScale = calendar->GetTimescale();
	const float hoursPassedDiff = abs(currentHoursPassed - previousHoursPassed);
	previousHoursPassed = currentHoursPassed;
	if (timeScale <= 0.0f || hoursPassedDiff >= 0.01f) {
		fadePhase = Phase::None;
		current = target;
	}

	std::optional<RE::NiColor> color = std::nullopt;
	if (colors.has_value())
		color = { 0.f, 0.f, 0.f };

	volumetricLightingIsMoon = current == Caster::Masser || current == Caster::Secunda;

	if (current == Caster::None) {
		fadePhase = Phase::None;
		SetLighting(sun, { 0.0f, 0.0f, 1.0f }, 0.0f, color);
		return;
	}

	const auto& dir = dirs[static_cast<int>(current)];
	const auto intensity = intensities[static_cast<int>(current)];
	if (colors.has_value())
		color = (*colors)[static_cast<int>(current)];

	if (fadePhase == Phase::None) {
		SetLighting(sun, dir, intensity, color);
		return;
	}

	fadeTimer = std::min(fadeTimer + *globals::game::deltaTime * timeScale, FadeTime);

	const float t = fadeTimer / FadeTime;
	const float fade = fadePhase == Phase::FadeIn ? t : 1.0f - t;
	SetLighting(sun, dir, intensity * fade, color);

	if (fadePhase == Phase::FadeOut) {
		if (t >= 1.0f || intensity <= 0.0f) {
			current = target;
			fadePhase = Phase::FadeIn;
			fadeTimer = 0.0f;
		}
	} else if (fadePhase == Phase::FadeIn) {
		if (t >= 1.0f)
			fadePhase = Phase::None;
	}
}

void SkySync::ShadowFader::SetLighting(const RE::Sun* sun, RE::NiPoint3 dir, float intensity, std::optional<RE::NiColor> color)
{
	ClampDirection(dir);

	RE::NiMatrix3& m = sun->light->local.rotate;
	m.entry[0][0] = -dir.x;
	m.entry[1][0] = -dir.y;
	m.entry[2][0] = -dir.z;

	if (color.has_value())
		sun->light->GetLightRuntimeData().diffuse = *color;

	RE::NiUpdateData updateData;
	sun->light->Update(updateData);

	intensity = std::clamp(intensity, 0.0f, 1.0f);
	sun->light->GetLightRuntimeData().fade = intensity;
	volumetricLightingIntensityFactor = intensity;
}

inline void SkySync::ShadowFader::ClampDirection(RE::NiPoint3& dir)
{
	constexpr float minElev = DirectX::XMConvertToRadians(MinElevation);
	const float elev = DirectX::XMScalarASinEst(dir.z);
	if (elev >= minElev)
		return;

	const float heading = std::atan2(dir.y, dir.x);
	float sinElev, cosElev, sinHeading, cosHeading;
	DirectX::XMScalarSinCosEst(&sinElev, &cosElev, minElev);
	DirectX::XMScalarSinCosEst(&sinHeading, &cosHeading, heading);

	dir.x = cosElev * cosHeading;
	dir.y = cosElev * sinHeading;
	dir.z = sinElev;
}

SkySync::VolumetricLightingDescriptor* SkySync::ApplyVolumetricLighting_VolumetricLightingDescriptor_Get::thunk()
{
	const auto volumetricLightingDescriptor = func();
	const bool skySyncEnabled = globals::features::skySync.settings.Enabled;
	if (skySyncEnabled)
		volumetricLightingDescriptor->lightingIntensity *= volumetricLightingIntensityFactor;
	// (batch 37b, A2/A3) Night strength and "gamma on density only". Identity when 37b is off.
	volumetricLightingDescriptor->lightingIntensity = globals::features::volumetricLighting.AdjustIntensity(
		volumetricLightingDescriptor->lightingIntensity, skySyncEnabled && volumetricLightingIsMoon);
	return volumetricLightingDescriptor;
}

void SkySync::ClimateTimings::Update(const RE::TESClimate* climate)
{
	sunriseBegin = climate->timing.sunrise.begin / 6.0f;
	sunriseEnd = climate->timing.sunrise.end / 6.0f;
	sunsetBegin = climate->timing.sunset.begin / 6.0f;
	sunsetEnd = climate->timing.sunset.end / 6.0f;
	sunrise = (sunriseBegin + sunriseEnd) * 0.5f - 0.25f;
	sunset = (sunsetBegin + sunsetEnd) * 0.5f + 0.25f;
	sunriseFadeOutMoonStart = sunriseBegin - 0.5f;
	sunriseFadeOutMoonEnd = sunriseBegin + 1.0f;
	sunsetFadeInMoonStart = sunsetEnd - 1.0f;
	sunsetFadeInMoonEnd = sunsetEnd + 0.5f;
}

void SkySync::Sky_OnNewClimate::thunk(RE::Sky* sky)
{
	if (auto& singleton = globals::features::skySync; singleton.settings.Enabled && sky && sky->currentClimate)
		singleton.timings.Update(sky->currentClimate);
	func(sky);
}

void SkySync::Moon_Update::thunk(RE::Moon* moon, RE::Sky* sky)
{
	const auto updateMoonTexture = moon->updateMoonTexture;

	if (const auto& singleton = globals::features::skySync; singleton.settings.Enabled && singleton.Effective().MoonOrbit != static_cast<int32_t>(MoonOrbit::Vanilla))
		SetMoonAngle(moon, sky);

	func(moon, sky);

	if (auto& singleton = globals::features::skySync; singleton.settings.Enabled && updateMoonTexture != moon->updateMoonTexture) {
		// Gets the texture name of the current moon phase when it changes rather than reading direct global variables
		// Allows for compatability with other mods that don't directly update the in-game phase values
		const auto moonShaderProperty = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().properties[1].get());

		const auto name = moonShaderProperty->GetBaseTexture()->name.c_str();
		const size_t len = std::strlen(name);
		std::string lower;
		lower.reserve(len);
		for (size_t i = 0; i < len; ++i) {
			lower.push_back(static_cast<char>(std::tolower(name[i])));
		}

		static constexpr std::array<std::pair<std::string_view, RE::Moon::Phases::Phase>, 8> Lookup{
			{ { "full", RE::Moon::Phases::Phase::kFull },
				{ "three_wan", RE::Moon::Phases::Phase::kWaningGibbous },
				{ "half_wan", RE::Moon::Phases::Phase::kWaningQuarter },
				{ "one_wan", RE::Moon::Phases::Phase::kWaningCrescent },
				{ "new", RE::Moon::Phases::Phase::kNewMoon },
				{ "one_wax", RE::Moon::Phases::Phase::kWaxingCrescent },
				{ "half_wax", RE::Moon::Phases::Phase::kWaxingQuarter },
				{ "three_wax", RE::Moon::Phases::Phase::kWaxingGibbous } }
		};

		RE::Moon::Phases::Phase phase = RE::Moon::Phases::Phase::kFull;
		for (auto& [suffix, id] : Lookup) {
			if (lower.find(suffix) != std::string::npos) {
				phase = id;
				break;
			}
		}

		float* intensityFactor = moon == sky->masser ? &singleton.masserPhaseIntensityFactor : &singleton.secundaPhaseIntensityFactor;
		if (phase == RE::Moon::Phases::Phase::kNewMoon) {
			*intensityFactor = NewMoonIntensityFactor;
		} else {
			const float t = (abs(static_cast<float>(phase) - static_cast<float>(RE::Moon::Phases::Phase::kNewMoon)) - 1.0f) / 3.0f;
			*intensityFactor = std::lerp(CrescentMoonIntensityFactor, FullMoonIntensityFactor, t);
		}
	}
}

inline float SkySync::SmoothStep(const float start, const float end, const float x)
{
	const float t = std::clamp((x - start) / (end - start), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

void SkySync::SetMoonAngle(RE::Moon* moon, const RE::Sky* sky)
{
	// (40d) SkyrimSE.exe 1.5.97, Moon::Update (ID 25626 = 0x1403ADF90), read with dumpbin for
	// this change:
	//   if (moon+0xD0 == FLT_MAX) { moon+0xCC = 90.0f; moon+0xD0 = 0 }     first update
	//   dHour = sky+0x1B0 (currentGameHour) - moon+0xD0;  if (dHour < 0) dHour += 24
	//   moon+0xCC += moon+0xBC (speed) * 60.0f * dHour, wrapped into [0, 360)
	//   moon+0xD0 = sky+0x1B0
	// Writing the angle for the total game time and moon+0xD0 = the current hour makes dHour 0,
	// so the engine keeps the angle (and does everything else as before). With no time skip the
	// result equals the engine's own stepping. The offsets are not verified on other runtimes:
	// there this does nothing. Moon and Stars drives the moons itself: left alone.
	//
	// (40e) What the angle means, from the same function and the routines it calls (dumpbin on
	// 1.5.97; sin/cos identified from the import table):
	//   root rotation = Rx(-angle) * Rz(+moon+0xC0), so the disc direction (rotation column Y) is
	//   (sin c0, cos(angle) cos c0, sin(angle) cos c0): angle 0 and 180 are the two horizons and 90
	//   is the moon's highest point (55 degrees up for Masser, c0 = 35; 40 for Secunda, c0 = 50).
	//   The disc's alpha (0x1403AD260) is 1 for angle in [fadeStart, 180 - fadeStart] = [35, 145],
	//   fades linearly to 0 at fadeEnd = 20 and 160, and is 0 outside (below the horizon).
	//   The phase texture is swapped only while both alphas are 0 (moon+0xC8 == 1).
	// 40d set angle = 90 + speed * 60 * (hours passed). Hours passed is GameDaysPassed * 24, and a
	// new game starts at GameDaysPassed 1.0 with GameHour 8 (Skyrim.esm), so the total ran 8 hours
	// behind the clock: Masser was overhead at 8 am and up at night only from about 3 am; Secunda
	// (18 degrees an hour) drifted 72 degrees a day and was up at night about 2 nights in 5.
	static const bool verified = !REL::Module::IsVR() && REL::Module::get().version() == SKSE::RUNTIME_SSE_1_5_97;
	const auto& singleton = globals::features::skySync;
	if (!verified || !moon || !sky || singleton.moonAndStarsLoaded)
		return;
	const double hour = static_cast<double>(sky->currentGameHour);
	if (!std::isfinite(hour) || hour < 0.0 || hour > 24.0)
		return;

	double angle;
	const auto effective = singleton.Effective();
	if (effective.MoonOrbit == static_cast<int32_t>(MoonOrbit::Stable)) {
		const auto calendar = RE::Calendar::GetSingleton();
		if (!calendar)
			return;
		const double hours = static_cast<double>(calendar->GetHoursPassed());
		const double speed = static_cast<double>(moon->speed);
		if (!std::isfinite(hours) || hours < 0.0 || !std::isfinite(speed))
			return;
		// Whole days from the calendar, the time of day from the clock: Masser (15 degrees an hour,
		// exactly a turn a day) is overhead at midnight whatever offset the save has between them.
		const double days = std::floor((hours - hour) / 24.0 + 0.5);
		angle = 90.0 + speed * 60.0 * (days * 24.0 + hour);
	} else if (effective.MoonOrbit == static_cast<int32_t>(MoonOrbit::NightSky)) {
		// Night = from an hour before the end of sunset to an hour after the start of sunrise, the
		// span in which Sky Sync lets the moons light the scene (NAT's Skyrim climate: 19:30-06:30).
		// Through the night the angle moves evenly from centre - arc/2 to centre + arc/2; over the day
		// it carries on round the rest of the circle (below the horizon) to the next dusk.
		double dusk = 19.5;
		double night = 11.0;
		if (const auto climate = sky->currentClimate) {
			const double d = climate->timing.sunset.end / 6.0 - 1.0;
			const double n = std::fmod(climate->timing.sunrise.begin / 6.0 + 1.0 - d + 48.0, 24.0);
			if (n >= 4.0 && n <= 20.0) {
				dusk = d;
				night = n;
			}
		}
		const auto& s = effective;
		const double arc = std::clamp(static_cast<double>(s.NightArc), 0.0, 140.0);
		const double centre = 90.0 + std::clamp(static_cast<double>(moon == sky->masser ? s.MasserNightPosition : s.SecundaNightPosition), -60.0, 60.0);
		const double t = std::fmod(hour - dusk + 48.0, 24.0);
		if (t < night)
			angle = centre + arc * (t / night - 0.5);
		else
			angle = centre + arc * 0.5 + (360.0 - arc) * ((t - night) / (24.0 - night));
	} else
		return;

	angle = std::fmod(angle, 360.0);
	if (angle < 0.0)
		angle += 360.0;
	moon->unkCC = static_cast<float>(angle);
	moon->unkD0 = sky->currentGameHour;
}

float SkySync::PhaseFactorFromTexture(const RE::Moon* moon)
{
	if (!moon || !moon->moonMesh)
		return 1.0f;
	const auto property = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().properties[1].get());
	const auto texture = property ? property->GetBaseTexture() : nullptr;
	const char* name = texture ? texture->name.c_str() : nullptr;
	if (!name)
		return 1.0f;
	std::string lower(name);
	for (auto& c : lower)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	// Same table and factors as Moon_Update's phase tracking above.
	static constexpr std::array<std::pair<std::string_view, RE::Moon::Phases::Phase>, 8> Lookup{
		{ { "full", RE::Moon::Phases::Phase::kFull },
			{ "three_wan", RE::Moon::Phases::Phase::kWaningGibbous },
			{ "half_wan", RE::Moon::Phases::Phase::kWaningQuarter },
			{ "one_wan", RE::Moon::Phases::Phase::kWaningCrescent },
			{ "new", RE::Moon::Phases::Phase::kNewMoon },
			{ "one_wax", RE::Moon::Phases::Phase::kWaxingCrescent },
			{ "half_wax", RE::Moon::Phases::Phase::kWaxingQuarter },
			{ "three_wax", RE::Moon::Phases::Phase::kWaxingGibbous } }
	};
	RE::Moon::Phases::Phase phase = RE::Moon::Phases::Phase::kFull;
	for (auto& [suffix, id] : Lookup) {
		if (lower.find(suffix) != std::string::npos) {
			phase = id;
			break;
		}
	}
	if (phase == RE::Moon::Phases::Phase::kNewMoon)
		return NewMoonIntensityFactor;
	const float t = (abs(static_cast<float>(phase) - static_cast<float>(RE::Moon::Phases::Phase::kNewMoon)) - 1.0f) / 3.0f;
	return std::lerp(CrescentMoonIntensityFactor, FullMoonIntensityFactor, t);
}

float SkySync::VanillaVisibility(const RE::Moon* moon)
{
	if (!moon || !moon->root || !moon->moonMesh)
		return 0.0f;
	if (moon->root->GetFlags().any(RE::NiAVObject::Flag::kHidden) ||
		(moon->moonNode && moon->moonNode->GetFlags().any(RE::NiAVObject::Flag::kHidden)) ||
		moon->moonMesh->GetFlags().any(RE::NiAVObject::Flag::kHidden))
		return 0.0f;
	const auto property = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().properties[1].get());
	return property ? std::clamp(property->kBlendColor.alpha, 0.0f, 1.0f) : 1.0f;
}

bool SkySync::OnScreenDirection(const RE::Moon* moon, const RE::Sky* sky, RE::NiPoint3& outDir)
{
	if (!moon || !moon->moonMesh || !sky || !sky->root)
		return false;
	RE::NiPoint3 dir = moon->moonMesh->world.translate - sky->root->world.translate;
	if (dir.Unitize() <= FLT_EPSILON)
		return false;
	outDir = dir;
	return true;
}
