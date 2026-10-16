#include "SkySync.h"

#include <numbers>

#include "PhysicalSky.h"
#include "VolumetricLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkySync::Settings,
	Enabled,
	UseAlternateSunPath,
	MoonLightSource,
	SunPath,
	CustomAngle,
	DisabledWorldspaces)  // (47a2)
// (44h) removed MoonOrbit, MasserNightPosition, SecundaNightPosition, NightArc, KeepMoonPosition,
// LightSourceModel, BothMoonsLight: pinned in Effective() (Stable orbit, discs kept, Combined, both
// moons); the Night Sky sliders only applied to the Night Sky orbit. Old keys are ignored on load.
// Code paths kept for a possible rollback (specs/physsky-rollback-candidates.md).

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
		// (47a2) Read-only: the latitude whose equinox sun follows this path (noon height = 90 - |latitude|).
		{
			const float latitude = 90.0f - sunAngle;
			ImGui::SameLine();
			ImGui::TextDisabled("~ latitude %.0f deg %s (equinox)", std::abs(latitude), latitude >= 0.0f ? "N" : "S");
		}

		if (settings.SunPath == static_cast<int32_t>(SunPath::Custom)) {
			if (ImGui::SliderFloat("Custom angle", &settings.CustomAngle, -90.0f, 90.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
				SetSunAngle();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Tilt of the sun's path in degrees. 0 = straight overhead; -35 matches Southern Sky, +35 Northern Sky.");
		}
	}

	ImGui::SliderInt("Moon light source", &settings.MoonLightSource, 0, static_cast<uint8_t>(MoonLightSource::Count) - 1, MoonLightSourceNames[settings.MoonLightSource], ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Which moon casts the shadows at night (both moons still add their light). If the chosen moon is not up, the other one takes over.");

	// (44h) removed "Light Source Model", "Both Moons Light the Scene", "Moon Orbit" (with the Night Sky
	// sliders) and "Keep Moons Where the Game Puts Them": pinned in Effective(). Code paths kept for a
	// possible rollback (specs/physsky-rollback-candidates.md).
	if (globals::features::physicalSky.IsLegacy())
		ImGui::TextColored({ 1.f, 0.8f, 0.3f, 1.f }, "Physical Sky's Sky Model is Legacy (36f): moons and moonlight work as in 36f right now.");

	// (47a2) Read-only status.
	if (ImGui::TreeNode("Status")) {
		auto clock = [](double a_hour) {
			double h = std::fmod(a_hour, 24.0);
			if (h < 0.0)
				h += 24.0;
			const int minutes = static_cast<int>(std::lround(h * 60.0)) % (24 * 60);
			return fmt::format("{:02}:{:02}", minutes / 60, minutes % 60);
		};
		const auto& sunDir = rawDirections[static_cast<int>(Caster::Sun)];
		ImGui::Text("Sun true elevation: %.1f deg", DirectX::XMConvertToDegrees(std::asin(std::clamp(sunDir.z, -1.0f, 1.0f))));
		if (!SkyFixesActive() || !settings.UseAlternateSunPath)
			ImGui::Text("Timing: 46y (Sky Fixes off or alternate path off)");
		else if (!sunWarp.valid)
			ImGui::Text("Timing: 46y (this climate's windows cannot be warped)");
		else
			ImGui::Text("Timing: -12 deg %s, 0 deg %s, noon %s, 0 deg %s, -12 deg %s", clock(sunWarp.csb).c_str(), clock(sunWarp.midR).c_str(),
				clock((sunWarp.midR + sunWarp.midS) * 0.5).c_str(), clock(sunWarp.midS).c_str(), clock(sunWarp.cse).c_str());
		static constexpr const char* bodyNames[4] = { "Sun", "Masser", "Secunda", "None" };
		ImGui::Text("Light: %s (sun %.2f, Masser %.2f, Secunda %.2f)", bodyNames[std::clamp(lastDominant, 0, 3)], lastStrength[0], lastStrength[1], lastStrength[2]);
		if (const auto* sky = RE::Sky::GetSingleton())
			ImGui::Text("Moon phase brightness: Masser %.2f, Secunda %.2f", PhaseFactorFromTexture(sky->masser), PhaseFactorFromTexture(sky->secunda));
		ImGui::TreePop();
	}
}

SkySync::Settings SkySync::Effective() const
{
	Settings s = settings;
	// (44h) No longer user settings: pinned here so every reader gets them (Combined light model, both
	// moons in the sum, Stable orbit, discs left where the game puts them). Legacy (36f) below still
	// sets its 36f values.
	s.LightSourceModel = static_cast<int32_t>(LightSourceModel::Combined);
	s.BothMoonsLight = true;
	s.MoonOrbit = static_cast<int32_t>(MoonOrbit::Stable);
	s.KeepMoonPosition = true;
	if (globals::features::physicalSky.IsLegacy()) {
		// 36f: the engine's own moon stepping, discs lowered by altitude (40d/40e options off).
		s.MoonOrbit = static_cast<int32_t>(MoonOrbit::Vanilla);
		s.KeepMoonPosition = false;
		s.LightSourceModel = static_cast<int32_t>(LightSourceModel::Clock);  // (42b)
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
	settings.LightSourceModel = std::clamp(settings.LightSourceModel, 0, static_cast<int32_t>(LightSourceModel::Count) - 1);
	// (47a2) A broken or out-of-range angle from the JSON: back to the default / into range.
	if (!std::isfinite(settings.CustomAngle))
		settings.CustomAngle = Settings{}.CustomAngle;
	settings.CustomAngle = std::clamp(settings.CustomAngle, -90.0f, 90.0f);
	disabledCacheValid = false;
	SetSunAngle();
}

void SkySync::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkySync::RestoreDefaultSettings()
{
	settings = {};
	disabledCacheValid = false;
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

	// (47a2) For the vanilla arc's night half (alternate path off).
	if (const auto collection = RE::GameSettingCollection::GetSingleton())
		gSunAlphaTransTime = collection->GetSetting("fSunAlphaTransTime");
}

void SkySync::OnGameLoaded()
{
	// (47a2) The cell pointers of the last game may be gone: start the cell and sky rotation caches afresh.
	currentCell = nullptr;
	currentCellValid = false;
	currentCellInterior = false;
	currentCellWorldspace = nullptr;
	currentSkyRotation = D3D11_FLOAT32_MAX;
	disabledCacheValid = false;
	shadowFader.Reset();
	combinedHandover.Reset();
}

bool SkySync::SkipReason(const RE::PlayerCharacter* a_player)
{
	const auto* cell = a_player ? a_player->GetParentCell() : nullptr;
	if (!cell)
		return true;
	if (cell->IsInteriorCell())
		return !cell->cellFlags.all(static_cast<RE::TESObjectCELL::Flag>(1u << 15));  // "sunlight shadows", as Interior Sun
	const RE::TESWorldSpace* worldspace = cell->GetRuntimeData().worldSpace;
	if (!worldspace)
		return false;
	if (!disabledCacheValid || worldspace != disabledCacheWorldspace) {
		disabledCacheValid = true;
		disabledCacheWorldspace = worldspace;
		const char* editorID = worldspace->GetFormEditorID();
		disabledCacheResult = editorID && *editorID && std::ranges::any_of(settings.DisabledWorldspaces, [editorID](const std::string& a_listed) { return _stricmp(a_listed.c_str(), editorID) == 0; });
	}
	return disabledCacheResult;
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
		// (47a2, upstream a59c212f7) Never dereference a null cell or the previous (possibly unloaded) one:
		// the cell's kind and worldspace are cached instead.
		const bool hadCell = currentCellValid;
		const bool wasInterior = currentCellInterior;
		const auto* oldWorldspace = currentCellWorldspace;
		if (cell) {
			SetSkyRotation(sky, cell);
			if (hadCell && (cell->IsInteriorCell() != wasInterior || cell->GetRuntimeData().worldSpace != oldWorldspace)) {
				shadowFader.Reset();
				combinedHandover.Reset();
			}
		} else {
			currentCell = nullptr;
			currentCellValid = false;
		}
	}

	// (47a2, upstream a59c212f7 + DisabledWorldspaces) Leave the light to the game when there is no cell,
	// in an interior without the sunlight-shadows flag, or in a worldspace listed as disabled.
	if (const auto skip = SkipReason(player); skip) {
		if (!lightReleased) {
			if (sun->light)
				sun->light->GetLightRuntimeData().fade = 1.0f;
			volumetricLightingIntensityFactor = 1.0f;
			volumetricLightingIsMoon = false;
			lightReleased = true;
		}
		return;
	}
	lightReleased = false;

	const float time = sky->currentGameHour;
	const bool isDayTime = time > timings.sunriseFadeOutMoonEnd && time < timings.sunsetFadeInMoonStart;

	const auto worldSpace = player->GetWorldspace();
	const float altitude = worldSpace ? player->GetPositionZ() - worldSpace->GetDefaultWaterHeight() : 0.0f;

	ProcessSun(sky, sun, time, altitude, isDayTime);
	ProcessMoon(sky->masser, time, Caster::Masser, altitude, isDayTime);
	ProcessMoon(sky->secunda, time, Caster::Secunda, altitude, isDayTime);

	// (42b) A model change (menu, A/B Compare) starts the new model from scratch.
	const int32_t model = Effective().LightSourceModel;
	if (model != lastLightSourceModel) {
		shadowFader.Reset();
		combinedHandover.Reset();
		lastLightSourceModel = model;
	}

	if (model == static_cast<int32_t>(LightSourceModel::Combined))
		UpdateCombined(sky);
	else
		shadowFader.Update(sun, directions, intensities, lightColors, isDayTime);
}

float SkySync::MoonDayFade(const float sunElevationDeg)
{
	// (47a) Sky Fixes: upstream's moonlight retreat. Its moonlight cap before sunrise is
	// (effective horizon - sun height) / 7 degrees with the effective horizon at -1 degree: full
	// at -8, gone at -1 (by then the real sky is already far brighter than any full moon).
	if (SkyFixesActive())
		return std::clamp((-1.0f - sunElevationDeg) / 7.0f, 0.0f, 1.0f);
	return 1.0f - SmoothStep(MoonDayFadeStart, MoonDayFadeEnd, sunElevationDeg);
}

bool SkySync::SkyFixesActive()
{
	return globals::features::physicalSky.SkyFixesActive();
}

float SkySync::GetDaytimeColorExtension()
{
	if (!gDaytimeColorExtension) {
		if (const auto collection = RE::GameSettingCollection::GetSingleton())
			gDaytimeColorExtension = collection->GetSetting("fDaytimeColorExtension");
	}
	const float value = gDaytimeColorExtension ? gDaytimeColorExtension->GetFloat() : 0.5f;
	return std::isfinite(value) ? std::clamp(value, 0.0f, 3.0f) : 0.5f;
}

void SkySync::SunWarp::Build(const RE::TESClimate::Timing& a_timing, const float a_colorExtension, const float a_sunAngleDeg)
{
	const uint8_t newKey[4] = { a_timing.sunrise.begin, a_timing.sunrise.end, a_timing.sunset.begin, a_timing.sunset.end };
	if (built && std::equal(std::begin(newKey), std::end(newKey), std::begin(key)) && keyExtension == a_colorExtension && keySunAngle == a_sunAngleDeg)
		return;
	built = true;
	std::copy(std::begin(newKey), std::end(newKey), std::begin(key));
	keyExtension = a_colorExtension;
	keySunAngle = a_sunAngleDeg;
	++generation;
	valid = false;

	// All zero: no timing at all (the 46y formula, as the game's own colours then never change either).
	if (newKey[0] == 0 && newKey[1] == 0 && newKey[2] == 0 && newKey[3] == 0)
		return;

	// TNAM is in 10-minute units. Windows shorter than half an hour are widened about their middle.
	double srb = newKey[0] / 6.0, sre = newKey[1] / 6.0, ssb = newKey[2] / 6.0, sse = newKey[3] / 6.0;
	auto widen = [](double& a_begin, double& a_end) {
		if (a_end - a_begin < 0.5) {
			const double centre = (a_begin + a_end) * 0.5;
			a_begin = centre - 0.25;
			a_end = centre + 0.25;
		}
	};
	widen(srb, sre);
	widen(ssb, sse);

	// The engine's colour keys: night until sunrise begin - fDaytimeColorExtension (clamped at 0), sunrise
	// colours peak half way to sunrise end, day from sunrise end to sunset begin, sunset colours peak half
	// way to sunset end + extension (clamped at 23.99), night after that. Unclamped, midR and midS are the
	// 46y path's own horizon crossings ((begin + end) / 2 -+ a quarter hour).
	const double ext = a_colorExtension;
	const double keyCsb = std::max(0.0, srb - ext);
	const double keyCse = std::min(23.99, sse + ext);
	const double keyMidR = keyCsb + (sre - keyCsb) * 0.5;
	const double keyMidS = ssb + (keyCse - ssb) * 0.5;

	// Overlapping, inverted or crowded windows (polar days and nights, broken records): no warp.
	constexpr double gap = 0.25;
	if (!(keyCsb + gap <= keyMidR && keyMidR + gap <= sre && sre <= ssb && ssb + gap <= keyMidS && keyMidS + gap <= keyCse && keyCse + gap <= keyCsb + 24.0))
		return;

	// Elevation -> phase on the tilted path: sin(elevation) = sin(phase) sin(tilt). The night target is kept
	// within 0.8 of the path's own maximum, so a low path still sets smoothly.
	const double sinTilt = std::abs(std::sin(a_sunAngleDeg * (std::numbers::pi / 180.0)));
	if (sinTilt < std::sin(10.0 * std::numbers::pi / 180.0))
		return;
	const double maxElevationDeg = std::asin(std::min(sinTilt, 1.0)) * (180.0 / std::numbers::pi);
	const double el = std::clamp(NightElevation, -0.8 * maxElevationDeg, 0.8 * maxElevationDeg);
	const double phaseNight = std::asin(std::clamp(std::sin(el * (std::numbers::pi / 180.0)) / sinTilt, -1.0, 1.0));
	if (!(phaseNight < 0.0) || !(std::numbers::pi + 2.0 * phaseNight > 0.0))
		return;

	// Uniform motion above the horizon (the path's natural rate), and the pieces below it: each ends at the
	// horizon with that rate (beta x its own mean rate). Their shape: a cubic whose relative slope at -12 deg
	// (alpha) is the largest monotone one up to 3 (Fritsch-Carlson), so the sun leaves -12 deg briskly and
	// spends the twilight nearer the horizon; t^beta when beta > 4 (no monotone cubic exists).
	const double rateUniform = std::numbers::pi / (keyMidS - keyMidR);
	const double riseMean = -phaseNight / (keyMidR - keyCsb);
	const double setMean = -phaseNight / (keyCse - keyMidS);
	const double betaRise = rateUniform / riseMean;
	const double betaSet = rateUniform / setMean;
	const bool powerRise = betaRise > 4.0;
	const bool powerSet = betaSet > 4.0;
	// Phase rate at -12 deg (radians per hour) = relative slope there x the mean rate.
	auto monotoneCubic = [](double a_alpha, double a_beta) {
		return (a_alpha + a_beta - 2.0 <= 0.0) || (2.0 * a_alpha + a_beta - 3.0 <= 0.0) || (a_alpha + 2.0 * a_beta - 3.0 <= 0.0) ||
		       (a_alpha - (2.0 * a_alpha + a_beta - 3.0) * (2.0 * a_alpha + a_beta - 3.0) / (3.0 * (a_alpha + a_beta - 2.0)) >= 0.0);
	};
	auto pickAlpha = [&](double a_beta) {
		for (int i = 60; i >= 1; --i) {
			const double alpha = i * 0.05;  // 3.00, 2.95, ... 0.05
			if (monotoneCubic(alpha, a_beta))
				return alpha;
		}
		return 0.05;
	};
	const double alphaRise = powerRise ? 0.0 : pickAlpha(betaRise);
	const double alphaSet = powerSet ? 0.0 : pickAlpha(betaSet);
	const double m1 = alphaRise * riseMean;
	const double m0 = alphaSet * setMean;

	// The night piece (cse -> csb + 24): a cubic from pi - pn to 2 pi + pn with those end rates, monotone.
	const double nightMean = (std::numbers::pi + 2.0 * phaseNight) / (keyCsb + 24.0 - keyCse);
	const double a = m0 / nightMean, b = m1 / nightMean;
	const bool monotone = (a + b - 2.0 <= 0.0) || (2.0 * a + b - 3.0 <= 0.0) || (a + 2.0 * b - 3.0 <= 0.0) ||
	                      (a - (2.0 * a + b - 3.0) * (2.0 * a + b - 3.0) / (3.0 * (a + b - 2.0)) >= 0.0);
	if (!monotone || !std::isfinite(rateUniform) || !std::isfinite(betaRise) || !std::isfinite(betaSet))
		return;

	csb = keyCsb;
	midR = keyMidR;
	midS = keyMidS;
	cse = keyCse;
	pn = phaseNight;
	rate = rateUniform;
	riseBeta = betaRise;
	riseAlpha = alphaRise;
	setAlpha = alphaSet;
	setBeta = betaSet;
	risePower = powerRise;
	setPower = powerSet;
	nightM0 = m0;
	nightM1 = m1;
	valid = true;
}

namespace
{
	// Normalised monotone piece on [0, 1]: s(0) = 0, s(1) = 1, s'(1) = a_beta; s'(0) = a_alpha (cubic) or 0 (power).
	double WarpPiece(double a_t, double a_alpha, double a_beta, bool a_power)
	{
		const double t = std::clamp(a_t, 0.0, 1.0);
		if (a_power)
			return std::pow(t, a_beta);
		const double t2 = t * t, t3 = t2 * t;
		return (t3 - 2.0 * t2 + t) * a_alpha + (-2.0 * t3 + 3.0 * t2) + (t3 - t2) * a_beta;
	}
}

double SkySync::SunWarp::Phase(double a_hour) const
{
	if (!std::isfinite(a_hour))
		a_hour = 12.0;
	// Into [csb, csb + 24).
	double h = std::fmod(a_hour - csb, 24.0);
	if (h < 0.0)
		h += 24.0;
	h += csb;
	constexpr double pi = std::numbers::pi;
	if (h <= midR)  // -12 deg -> horizon
		return pn * (1.0 - WarpPiece((h - csb) / (midR - csb), riseAlpha, riseBeta, risePower));
	if (h <= midS)  // the 46y uniform path
		return rate * (h - midR);
	if (h <= cse)  // horizon -> -12 deg
		return pi - pn + pn * WarpPiece((cse - h) / (cse - midS), setAlpha, setBeta, setPower);
	// Night: Hermite from (cse, pi - pn, nightM0) to (csb + 24, 2 pi + pn, nightM1).
	const double span = csb + 24.0 - cse;
	const double t = std::clamp((h - cse) / span, 0.0, 1.0);
	const double t2 = t * t, t3 = t2 * t;
	return (2.0 * t3 - 3.0 * t2 + 1.0) * (pi - pn) + (t3 - 2.0 * t2 + t) * span * nightM0 + (-2.0 * t3 + 3.0 * t2) * (2.0 * pi + pn) + (t3 - t2) * span * nightM1;
}

void SkySync::PutVanillaNightBelowHorizon(RE::NiPoint3& dir, const float hour, const RE::TESClimate::Timing& timing)
{
	// Upstream CalculateVanillaSunDirection: the game's sun touches the horizon where its alpha fade ends, the
	// middle of each interval -+ half fSunAlphaTransTime; between those hours the arc is the day half, outside
	// them the night half, whose height the engine writes mirrored (above the horizon).
	constexpr float hoursPerUnit = 1.0f / 6.0f;
	auto middle = [](const RE::TESClimate::Timing::Interval& a_interval) { return (a_interval.end * hoursPerUnit + a_interval.begin * hoursPerUnit) * 0.5f; };
	const float transition = gSunAlphaTransTime ? gSunAlphaTransTime->GetFloat() : 2.0f;
	const float half = std::isfinite(transition) ? std::clamp(transition, 0.0f, 6.0f) * 0.5f : 1.0f;
	const float sunrise = middle(timing.sunrise) - half;
	const float sunset = middle(timing.sunset) + half;
	const float dayLength = sunset - sunrise;
	if (!(dayLength > 0.1f && dayLength < 23.9f) || !std::isfinite(hour))
		return;
	const float sinceSunrise = hour >= sunrise ? hour - sunrise : hour + 24.0f - sunrise;
	if (sinceSunrise >= dayLength)
		dir.z = -std::abs(dir.z);
}

void SkySync::GetSunDirectionAtHour(const RE::Sky* sky, const float hour, RE::NiPoint3& outDir, float& outDist)
{
	double phase;
	if (sky && sky->currentClimate) {
		sunWarp.Build(sky->currentClimate->timing, GetDaytimeColorExtension(), sunAngle);
	}
	if (sky && sky->currentClimate && sunWarp.valid) {
		phase = sunWarp.Phase(hour);
	} else {
		// The 46y formula, guarded against a window that would divide by ~0.
		float sunrise = timings.sunrise, sunset = timings.sunset;
		if (!(sunset - sunrise > 0.1f) || !std::isfinite(sunrise) || !std::isfinite(sunset)) {
			sunrise = 6.0f;
			sunset = 18.0f;
		}
		phase = std::numbers::pi * ((hour - sunrise) / (sunset - sunrise));
	}
	const double tilt = sunAngle * (std::numbers::pi / 180.0);
	const double sinPhi = std::sin(phase), cosPhi = std::cos(phase);
	outDir = { static_cast<float>(cosPhi), static_cast<float>(-sinPhi * std::cos(tilt)), static_cast<float>(sinPhi * std::sin(tilt)) };
	if (outDir.Unitize() < FLT_EPSILON)
		outDir = { 0.0f, 0.0f, 1.0f };
	outDist = std::lerp(SunHorizonDistance, SunPeakDistance, std::max(static_cast<float>(sinPhi), 0.0f));
}

void SkySync::FadeSunAtHorizon(const RE::Sky* sky, const float sunElevationDeg)
{
	if (!sky || !sky->sun)
		return;
	const float el = std::isfinite(sunElevationDeg) ? sunElevationDeg : -90.0f;
	const float discFade = std::clamp((el + 3.0f) / 2.0f, 0.0f, 1.0f);
	const float glareFade = std::clamp(el / 1.0f, 0.0f, 1.0f);

	// The weathers' own glare, blended as Sun::Update blends it (its alpha also carries the clock fade,
	// which is exactly what this replaces).
	const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) : 1.0f;
	float glare = 0.0f;
	if (sky->currentWeather) {
		const float current = sky->currentWeather->data.sunGlare * (1.0f / 255.0f);
		if (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.0f)
			glare = std::lerp(sky->lastWeather->data.sunGlare * (1.0f / 255.0f), current, pct);
		else
			glare = current;
	}

	const std::pair<RE::BSTriShape*, float> alphas[] = { { sky->sun->sunBase.get(), discFade }, { sky->sun->sunGlare.get(), glare * glareFade } };
	for (const auto& [geometry, alpha] : alphas) {
		if (!geometry)
			continue;
		if (const auto property = skyrim_cast<RE::BSSkyShaderProperty*>(geometry->GetGeometryRuntimeData().properties[1].get()))
			property->kBlendColor.alpha = alpha;
	}
}

void SkySync::UpdateCombined(const RE::Sky* sky)
{
	constexpr int sunIdx = static_cast<int>(Caster::Sun);
	constexpr int masserIdx = static_cast<int>(Caster::Masser);
	constexpr int secundaIdx = static_cast<int>(Caster::Secunda);
	auto elevationDeg = [](const RE::NiPoint3& d) {
		return DirectX::XMConvertToDegrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)));
	};

	// Strength of each body from its true elevation (rawDirections: no altitude dip, no clamp).
	const float sunElevation = elevationDeg(rawDirections[sunIdx]);
	float twilight = 1.0f;
	if (const auto& physicalSky = globals::features::physicalSky; physicalSky.loaded)
		twilight = std::clamp(physicalSky.Effective().twilightLength, 1.0f, 3.0f);
	float strength[3] = {};
	strength[sunIdx] = SunResidual * SmoothStep(SunResidualEndElevation * twilight, 0.0f, sunElevation) +
	                   (1.0f - SunResidual) * SmoothStep(0.0f, SunFullElevation, sunElevation);

	// The moons: phase, height, and gone by day. The weather does not switch them off (as in the
	// Clock model: overcast nights keep their moonlight).
	const float dayFade = MoonDayFade(sunElevation);
	const RE::Moon* moons[3] = { nullptr, sky->masser, sky->secunda };
	for (const int i : { masserIdx, secundaIdx }) {
		if (!moons[i] || !moons[i]->moonMesh)
			continue;
		float s = SmoothStep(MoonStartElevation, MoonFullElevation, elevationDeg(rawDirections[i]));
		s *= PhaseFactorFromTexture(moons[i]) * dayFade;
		if (i == secundaIdx)
			s *= SecundaIntensityFactor;
		strength[i] = s;
	}

	// Brightness of each, for picking the direction and blending the moons.
	const bool hasColors = lightColors.has_value();
	auto luminance = [](const RE::NiColor& c) { return 0.2126f * c.red + 0.7152f * c.green + 0.0722f * c.blue; };
	float weight[3];
	for (int i = 0; i < 3; ++i)
		weight[i] = strength[i] * (hasColors ? std::max(luminance((*lightColors)[i]), 0.0f) : 1.0f);

	// Which moons are in the sum.
	const auto src = static_cast<MoonLightSource>(settings.MoonLightSource);
	float share[3] = { 1.0f, 0.0f, 0.0f };
	if (Effective().BothMoonsLight) {
		share[masserIdx] = 1.0f;
		share[secundaIdx] = 1.0f;
	} else if (src == MoonLightSource::Masser) {
		share[masserIdx] = 1.0f;
	} else if (src == MoonLightSource::Secunda) {
		share[secundaIdx] = 1.0f;
	} else if (const float moonSum = weight[masserIdx] + weight[secundaIdx]; moonSum > 0.0f) {
		// Brightest, without a jump when the two cross: each by its share of the moonlight. As bright as
		// the brighter moon when one dominates, and at the crossing.
		share[masserIdx] = weight[masserIdx] / moonSum;
		share[secundaIdx] = weight[secundaIdx] / moonSum;
	}

	// Direction: the brightest body, among the moons only those "Moon light source" allows.
	// (44h) With Masser or Secunda picked, the other moon may cast the shadows while the picked one has
	// no light (below the horizon, gone, faded by day) and the other one is in the sum; as soon as the
	// picked moon has light again it is the only moon allowed, and the hand-over below (turn, or fade
	// out and back in) moves the light back to it smoothly.
	auto moonAllowed = [&](const int i) {
		if (src == MoonLightSource::Brightest)
			return true;
		const int picked = src == MoonLightSource::Masser ? masserIdx : secundaIdx;
		if (i == picked)
			return true;
		return !(weight[picked] > 0.0f) && share[i] > 0.0f;
	};
	auto best = Caster::None;
	float bestWeight = 0.0f;
	for (int i = 0; i < 3; ++i) {
		if (i != sunIdx && !moonAllowed(i))
			continue;
		if (weight[i] > bestWeight) {
			bestWeight = weight[i];
			best = static_cast<Caster>(i);
		}
	}
	// Hysteresis (D1): the body the light is heading to keeps it until another is clearly brighter.
	if (const auto held = combinedHandover.target; held != Caster::None && best != held) {
		const float heldWeight = weight[static_cast<int>(held)];
		const bool heldAllowed = held == Caster::Sun || moonAllowed(static_cast<int>(held));
		if (heldAllowed && heldWeight > 0.0f && bestWeight < heldWeight * CombinedHandover::Hysteresis)
			best = held;
	}

	// Time skips (waiting, fast travel, console) and a stopped clock jump straight to the result.
	const auto calendar = RE::Calendar::GetSingleton();
	const float hoursPassed = calendar ? calendar->GetHoursPassed() : 0.0f;
	const float timeScale = calendar ? calendar->GetTimescale() : 0.0f;
	const bool snap = combinedHandover.previousHoursPassed < 0.0f || timeScale <= 0.0f || std::abs(hoursPassed - combinedHandover.previousHoursPassed) >= 0.01f;
	combinedHandover.previousHoursPassed = hoursPassed;
	const float deltaTime = globals::game::deltaTime ? *globals::game::deltaTime : 0.0f;
	const float advance = std::max(deltaTime * timeScale, 0.0f);

	const RE::NiPoint3 dir = combinedHandover.Step(best, directions, advance, snap);
	const float handover = combinedHandover.weight;
	const auto dominant = combinedHandover.dominant;

	// The sum. The twilight sun gets a warm tint while the light points at a moon (see SunTwilightTint).
	std::optional<RE::NiColor> color = std::nullopt;
	float total = 0.0f;
	RE::NiColor sum{ 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < 3; ++i) {
		const float f = share[i] * strength[i];
		if (!(f > 0.0f))
			continue;
		total += f;
		if (hasColors) {
			RE::NiColor c = f * (*lightColors)[i];
			if (i == sunIdx && dominant != Caster::Sun) {
				c.red *= SunTwilightTint[0];
				c.green *= SunTwilightTint[1];
				c.blue *= SunTwilightTint[2];
			}
			sum += c;
		}
	}
	if (hasColors)
		color = handover * sum;

	// (47a2) For the menu'"'"'s Status block.
	for (int i = 0; i < 3; ++i)
		lastStrength[i] = strength[i];
	lastDominant = static_cast<int>(dominant);
	const float dominantStrength = dominant == Caster::None ? 0.0f : strength[static_cast<int>(dominant)];
	volumetricLightingIsMoon = dominant == Caster::Masser || dominant == Caster::Secunda;
	SetLightingCombined(sky->sun, dir, color, std::min(total, 1.0f) * handover, std::min(dominantStrength, 1.0f) * handover);
}

void SkySync::SetLightingCombined(const RE::Sun* sun, RE::NiPoint3 dir, std::optional<RE::NiColor> color, const float vanillaFade, const float vlFactor)
{
	ShadowFader::ClampDirection(dir);

	RE::NiMatrix3& m = sun->light->local.rotate;
	m.entry[0][0] = -dir.x;
	m.entry[1][0] = -dir.y;
	m.entry[2][0] = -dir.z;

	auto& light = sun->light->GetLightRuntimeData();
	if (color.has_value()) {
		// Upstream 56784ccd1: fade = 0 blacked out effect meshes while the light changed over. The
		// brightness is in the colour instead.
		light.diffuse = *color;
		light.fade = 1.0f;
	} else {
		// No Physical Sky colours: the colour is the weather's, written by the game, so fade stays the
		// dimmer here (as in the Clock model).
		light.fade = std::clamp(vanillaFade, 0.0f, 1.0f);
	}

	RE::NiUpdateData updateData;
	sun->light->Update(updateData);

	volumetricLightingIntensityFactor = std::clamp(vlFactor, 0.0f, 1.0f);
}

void SkySync::CombinedHandover::Reset()
{
	phase = Phase::Steady;
	dominant = Caster::None;
	target = Caster::None;
	weight = 1.0f;
	turnTimer = 0.0f;
	turnFrom = { 0.0f, 0.0f, 1.0f };
	lastDir = { 0.0f, 0.0f, 1.0f };
	previousHoursPassed = -1.0f;
}

RE::NiPoint3 SkySync::CombinedHandover::Step(const Caster a_best, const RE::NiPoint3 a_dirs[3], const float a_advance, const bool a_snap)
{
	// A usable direction for a body (None: keep the last one; the light is ~0 then anyway).
	auto dirOf = [&](const Caster c) {
		if (c == Caster::None)
			return lastDir;
		RE::NiPoint3 d = a_dirs[static_cast<int>(c)];
		if (d.Unitize() <= FLT_EPSILON)  // D6: never a zero vector
			return lastDir;
		return d;
	};
	auto angleDeg = [](const RE::NiPoint3& a, const RE::NiPoint3& b) {
		return DirectX::XMConvertToDegrees(std::acos(std::clamp(a.Dot(b), -1.0f, 1.0f)));
	};

	if (a_snap) {
		dominant = target = a_best;
		phase = Phase::Steady;
		weight = 1.0f;
		lastDir = dirOf(dominant);
		return lastDir;
	}

	const float half = FadeTime * 0.5f;
	if (a_best != target) {
		target = a_best;
		if (dominant == Caster::None || target == Caster::None) {
			// Coming from or going to "no light": the sum is ~0 at that moment, so the direction can
			// change at once; the body's own horizon ramp does the fading.
			if (phase == Phase::FadeOut || phase == Phase::FadeIn)
				phase = Phase::FadeIn;  // finish bringing the weight back up
			else
				phase = Phase::Steady;
			if (target != Caster::None)
				dominant = target;
			else
				dominant = Caster::None;
		} else if (phase != Phase::FadeOut && phase != Phase::FadeIn && angleDeg(lastDir, dirOf(target)) <= TurnMaxAngle) {
			phase = Phase::Turn;
			turnFrom = lastDir;
			turnTimer = 0.0f;
			dominant = target;
		} else if (phase == Phase::FadeOut && target == dominant) {
			phase = Phase::FadeIn;  // the old body is back on top: fade back in, same direction
		} else if (phase != Phase::FadeOut) {
			phase = Phase::FadeOut;  // from the current weight (re-planned mid fade-in, D2)
		}
	}

	RE::NiPoint3 dir = dirOf(dominant);
	switch (phase) {
	case Phase::Turn:
		{
			turnTimer = std::min(turnTimer + a_advance, FadeTime);
			const float t = SmoothStep(0.0f, 1.0f, turnTimer / FadeTime);
			RE::NiPoint3 blended = turnFrom * (1.0f - t) + dir * t;  // < 25 degrees apart: no cancellation
			if (blended.Unitize() > FLT_EPSILON)
				dir = blended;
			weight = 1.0f;
			if (turnTimer >= FadeTime)
				phase = Phase::Steady;
			break;
		}
	case Phase::FadeOut:
		weight = std::max(weight - a_advance / half, 0.0f);
		dir = lastDir;  // the old direction, held while the light fades (also mid-turn)
		if (weight <= 0.0f) {
			// The direction changes only here, at zero light (D2).
			dominant = target;
			dir = dirOf(dominant);
			phase = target == Caster::None ? Phase::Steady : Phase::FadeIn;
			if (target == Caster::None)
				weight = 1.0f;
		}
		break;
	case Phase::FadeIn:
		weight = std::min(weight + a_advance / half, 1.0f);
		if (weight >= 1.0f)
			phase = Phase::Steady;
		break;
	case Phase::Steady:
	default:
		weight = 1.0f;
		break;
	}

	lastDir = dir;
	return dir;
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
	// (47a2) What it was, for later frames (the pointer itself is never dereferenced again).
	currentCellValid = true;
	currentCellInterior = cell->IsInteriorCell();
	currentCellWorldspace = cell->GetRuntimeData().worldSpace;
	const float rotation = cell->GetNorthRotation();
	if (rotation == currentSkyRotation)
		return;

	currentSkyRotation = rotation;
	sky->root->local.rotate = RE::NiMatrix3{ RE::NiPoint3{ 0.0f, 0.0f, -rotation } };
	RE::NiUpdateData updateData;
	sky->root->Update(updateData);
}

void SkySync::ProcessSun(const RE::Sky* sky, const RE::Sun* sun, const float time, const float altitude, const bool isDayTime)
{
	RE::NiPoint3 dir;
	float dist;

	// (47a) Sky Fixes: the alternate path's timing follows the climate's colour keys (SunWarp). Without
	// the alternate path the game's own sun position is kept (no warp), as in 46y.
	const bool skyFixes = SkyFixesActive();
	if (settings.UseAlternateSunPath) {
		if (skyFixes)
			GetSunDirectionAtHour(sky, time, dir, dist);
		else
			CalculateAlternateSunDirectionAndDistance(dir, dist, time, timings.sunrise, timings.sunset, sunAngle);
	} else {
		CalculateSunDirectionAndDistance(sun, dir, dist);
		// (47a2, upstream) The game mirrors its night arc above the horizon: put it below.
		if (sky && sky->currentClimate)
			PutVanillaNightBelowHorizon(dir, time, sky->currentClimate->timing);
	}

	rawDirections[static_cast<int>(Caster::Sun)] = dir;

	const RE::NiPoint3 apparentDir = GetApparentDirection(dir, altitude);
	SetSunPosition(sun, apparentDir, dist);

	directions[static_cast<int>(Caster::Sun)] = apparentDir;

	// (47a) Sky Fixes: disc and glare by the sun's true elevation instead of on/off by the clock
	// (06:30-19:30 with NAT's climate, which showed a red disc 15 degrees below the horizon).
	if (skyFixes)
		FadeSunAtHorizon(sky, DirectX::XMConvertToDegrees(std::asin(std::clamp(dir.z, -1.0f, 1.0f))));
	else
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
	// (47a2) Sky Fixes: no altitude dip (upstream removed it in 54962ef3b). The sun disc, the light and the
	// moons' light use the true direction, which is what Physical Sky's planet model already assumes; the LUTs
	// used the true height since 41b, so the procedural disc (sunAlignToVanilla) and its glow still sit on
	// the vanilla quad.
	if (SkyFixesActive())
		return dir;
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
	ShadowFader::ClampDirection(dir);

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
