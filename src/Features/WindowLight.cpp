#include "WindowLight.h"

#include "Features/LinearLighting.h"
#include "Features/SkySync.h"
#include "Features/WindowLightMath.h"
#include "State.h"

namespace WindowLight
{
	namespace
	{
		using WindowLightMath::Float3;
		using WindowLightMath::Weights;

		struct LightInfo
		{
			RE::TESObjectREFR* ref = nullptr;  // the owner when classified (a different owner = reclassify)
			RE::TESRegion* region = nullptr;   // null = not window light
			float ligh[3] = { 1.0f, 1.0f, 1.0f };
		};

		struct GlassInfo
		{
			RE::BSGeometry* geometry = nullptr;
			RE::TESRegion* region = nullptr;
		};

		struct RegionState
		{
			uint32_t frame = UINT32_MAX;
			bool hasKeys = false;
			Float3 keys[4];
			bool recompute = false;
			float encLight[3] = { 0.0f, 0.0f, 0.0f };  // recompute: the colour per unit LIGH colour, gamma-space (pow 1 / light gamma)
			float scaleLight = 1.0f;                   // weather only: diffuse x this
			RE::NiColor glass{};                       // glass: what the shape's emissive colour points at this frame
			float engineCheck = -1.0f;                 // max |engine emittance - clock reconstruction| (< 0: not available)
			float brightness = 0.0f;                   // light luminance relative to the day key (status)
		};

		struct FrameLight
		{
			RE::NiLight* light = nullptr;
			RegionState* region = nullptr;
			float ligh[3] = { 1.0f, 1.0f, 1.0f };
		};

		struct State
		{
			uint32_t frame = UINT32_MAX;
			bool active = false;  // this frame: a switch on, an interior, a sky
			const RE::TESObjectCELL* cell = nullptr;
			uint32_t framesInCell = 0;
			bool logged = false;
			std::unordered_map<RE::NiLight*, LightInfo> lightCache;
			std::unordered_map<RE::BSLightingShaderProperty*, GlassInfo> glassCache;
			std::unordered_map<RE::TESRegion*, RegionState> regions;
			std::vector<FrameLight> frameLights;
			uint32_t glassTied = 0;
			bool glassPointerLogged = false;

			// This frame's sky.
			float hour = 12.0f;
			float sunElevation = 90.0f;
			bool rising = true;
			float dayRise = 30.0f, daySet = 30.0f;
			Weights clock, altitude, weights;
			float fraction = 1.0f;
			float gammaLight = 1.8f, gammaEmit = 2.2f;
			bool noSat = false, followSun = false, smooth = false, weather = false;

			// Follow the Outdoor Weather.
			float weatherFactor = 1.0f;  // smoothed
			float weatherRatio = 1.0f, moonFactor = 1.0f;
			bool weatherSnap = true;
			float lastHour = -1.0f;
			const RE::TESClimate* refClimate = nullptr;
			const RE::TESWeather* refWeather = nullptr;
			const RE::TESWeather* outdoorWeather = nullptr;

			// Glass pointer swap in flight.
			RE::BSLightingShaderProperty* swapProperty = nullptr;
			RE::NiColor* swapOriginal = nullptr;
		};

		State g;

		Float3 ToFloat3(const RE::Color& c) { return { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f }; }

		/// The region's weather: the one the engine assigned (TESRegion::currentWeather), else the first of its weather list (RDWT).
		const RE::TESWeather* RegionWeather(const RE::TESRegion* a_region)
		{
			if (!a_region)
				return nullptr;
			if (a_region->currentWeather)
				return a_region->currentWeather;
			if (!a_region->dataList)
				return nullptr;
			for (auto* data : a_region->dataList->regionDataList) {
				if (!data || data->GetType() != RE::TESRegionData::Type::kWeather)
					continue;
				for (auto* type : static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes) {
					if (type && type->weather)
						return type->weather;
				}
			}
			return nullptr;
		}

		/// The reference's emittance source when it is a region (SEH-guarded: forms and extra data of a reference being unloaded).
		RE::TESRegion* EmittanceRegionUnsafe(RE::TESObjectREFR* a_ref)
		{
			const auto* extra = a_ref->extraList.GetByType<RE::ExtraEmittanceSource>();
			if (!extra || !extra->source)
				return nullptr;
			return extra->source->As<RE::TESRegion>();
		}
		RE::TESRegion* EmittanceRegion(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref)
				return nullptr;
			__try {
				return EmittanceRegionUnsafe(a_ref);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		bool LighColorUnsafe(RE::TESObjectREFR* a_ref, float o_rgb[3])
		{
			auto* base = a_ref->GetBaseObject();
			auto* ligh = base ? base->As<RE::TESObjectLIGH>() : nullptr;
			if (!ligh)
				return false;
			o_rgb[0] = ligh->data.color.red / 255.0f;
			o_rgb[1] = ligh->data.color.green / 255.0f;
			o_rgb[2] = ligh->data.color.blue / 255.0f;
			return true;
		}
		bool LighColor(RE::TESObjectREFR* a_ref, float o_rgb[3])
		{
			__try {
				return LighColorUnsafe(a_ref, o_rgb);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		RE::TESObjectREFR* OwnerOf(RE::NiAVObject* a_object)
		{
			if (!a_object)
				return nullptr;
			__try {
				return a_object->GetUserData();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		/// Sunlight luminance of a weather under the given key weights, in linear light (Linear Lighting's light gamma).
		float SunlightLuminance(const RE::TESWeather* a_weather, const Weights& a_w, float a_gamma)
		{
			if (!a_weather)
				return 0.0f;
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k) {
				const Float3 c = ToFloat3(a_weather->colorData[RE::TESWeather::ColorTypes::kSunlight][k]);
				sum += a_w.w[k] * WindowLightMath::Luminance(WindowLightMath::PowPerChannel(c, a_gamma));
			}
			return sum;
		}

		bool IsPleasant(const RE::TESWeather* a_weather)
		{
			return a_weather && a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kPleasant);
		}

		/// The clear-sky reference of a climate: its most likely pleasant weather (else its brightest-sunlight weather).
		const RE::TESWeather* ClearReference(const RE::TESClimate* a_climate, float a_gamma)
		{
			if (!a_climate)
				return nullptr;
			if (a_climate == g.refClimate && g.refWeather)
				return g.refWeather;
			const RE::TESWeather* best = nullptr;
			uint32_t bestChance = 0;
			const RE::TESWeather* brightest = nullptr;
			float bright = -1.0f;
			Weights day;
			day.w[WindowLightMath::kNight] = 0.0f;
			day.w[WindowLightMath::kDay] = 1.0f;
			for (auto* type : const_cast<RE::TESClimate*>(a_climate)->weatherList) {
				if (!type || !type->weather)
					continue;
				if (IsPleasant(type->weather) && (!best || type->chance > bestChance)) {
					best = type->weather;
					bestChance = type->chance;
				}
				const float l = SunlightLuminance(type->weather, day, a_gamma);
				if (l > bright) {
					bright = l;
					brightest = type->weather;
				}
			}
			g.refClimate = a_climate;
			g.refWeather = best ? best : brightest;
			return g.refWeather;
		}

		/// One weather's direct-light ratio against the clear reference (pleasant weathers: 1), at the given key weights.
		float WeatherRatio(const RE::TESWeather* a_weather, const RE::TESWeather* a_ref, const Weights& a_w, float a_gamma)
		{
			if (!a_weather || IsPleasant(a_weather) || !a_ref || a_weather == a_ref)
				return 1.0f;
			const float ref = SunlightLuminance(a_ref, a_w, a_gamma);
			if (!(ref > 1e-6f))
				return 1.0f;
			return WindowLightMath::Clamp01(SunlightLuminance(a_weather, a_w, a_gamma) / ref);
		}

		/// Both moons' strength now: height (0 at -1 degree, full at 3) x phase (Secunda x 0.67), clamped to 1 (a full Masser up).
		float MoonStrength(const RE::Sky* a_sky)
		{
			auto& skySync = globals::features::skySync;
			float sum = 0.0f;
			const std::pair<const RE::Moon*, float> moons[] = { { a_sky->masser, 1.0f }, { a_sky->secunda, 0.67f } };
			for (const auto& [moon, factor] : moons) {
				if (!moon || !moon->moonMesh)
					continue;
				const float height = SkySync::SmoothStep(-1.0f, 3.0f, skySync.MoonElevationDeg(moon, a_sky));
				sum += height * SkySync::PhaseFactorFromTexture(moon) * factor;
			}
			return WindowLightMath::Clamp01(sum);
		}

		void UpdateWeather(const RE::Sky* a_sky)
		{
			const Weights& w = g.weights;
			float target = 1.0f;
			g.weatherRatio = 1.0f;
			g.moonFactor = 1.0f;
			g.outdoorWeather = a_sky->currentWeather;
			if (g.weather && a_sky->currentWeather) {
				// The outdoor weather the engine keeps while indoors (Sky::currentWeather, with lastWeather during a transition).
				const auto* ref = ClearReference(a_sky->currentClimate, g.gammaLight);
				const float pct = std::isfinite(a_sky->currentWeatherPct) ? std::clamp(a_sky->currentWeatherPct, 0.0f, 1.0f) : 1.0f;
				const float now = WeatherRatio(a_sky->currentWeather, ref, w, g.gammaLight);
				const bool blending = a_sky->lastWeather && a_sky->lastWeather != a_sky->currentWeather && pct < 1.0f;
				g.weatherRatio = blending ? std::lerp(WeatherRatio(a_sky->lastWeather, ref, w, g.gammaLight), now, pct) : now;
				g.moonFactor = std::lerp(1.0f, WindowLightMath::MoonFactor(MoonStrength(a_sky)), w.w[WindowLightMath::kNight]);
				target = g.weatherRatio * g.moonFactor;
			}
			const float dt = globals::game::deltaTime ? std::clamp(*globals::game::deltaTime, 0.0f, 1.0f) : 0.0f;
			const bool skipped = g.lastHour >= 0.0f && std::abs(std::remainder(g.hour - g.lastHour, 24.0f)) > 0.1f;  // wait / sleep / load
			g.lastHour = g.hour;
			if (g.weatherSnap || skipped || !g.weather) {
				g.weatherFactor = target;
				g.weatherSnap = false;
			} else {
				constexpr float tau = 2.0f;  // seconds
				g.weatherFactor += (target - g.weatherFactor) * (1.0f - std::exp(-dt / tau));
			}
		}

		void ResetCell(const RE::TESObjectCELL* a_cell)
		{
			g.cell = a_cell;
			g.framesInCell = 0;
			g.logged = false;
			g.lightCache.clear();
			g.glassCache.clear();
			g.regions.clear();
			g.glassTied = 0;
			g.weatherSnap = true;
		}

		RegionState* Region(RE::TESRegion* a_region)
		{
			if (!a_region)
				return nullptr;
			auto& r = g.regions[a_region];
			if (r.frame == g.frame)
				return &r;
			r.frame = g.frame;
			const auto* weather = RegionWeather(a_region);
			r.hasKeys = weather != nullptr;
			if (r.hasKeys) {
				for (int k = 0; k < 4; ++k)
					r.keys[k] = ToFloat3(weather->colorData[RE::TESWeather::ColorTypes::kEffectLighting][k]);
				const Float3 rebuilt = WindowLightMath::EngineBlend(r.keys, g.clock);
				const auto& e = a_region->emittanceColor;
				r.engineCheck = std::max({ std::abs(e.red - rebuilt.r), std::abs(e.green - rebuilt.g), std::abs(e.blue - rebuilt.b) });
			} else {
				r.engineCheck = -1.0f;
			}
			r.recompute = r.hasKeys && (g.noSat || g.followSun || g.smooth);
			const float d = std::max(g.weatherFactor, 0.0f);
			if (r.recompute) {
				Float3 lin = WindowLightMath::Compose(r.keys, g.gammaLight, g.weights, g.noSat, g.smooth, g.fraction);
				lin = WindowLightMath::Scale(lin, d);
				const Float3 enc = WindowLightMath::PowPerChannel(lin, 1.0f / g.gammaLight);
				r.encLight[0] = enc.r;
				r.encLight[1] = enc.g;
				r.encLight[2] = enc.b;
				const float dayY = WindowLightMath::Luminance(WindowLightMath::KeyLinear(r.keys[WindowLightMath::kDay], g.gammaLight, false));
				r.brightness = dayY > 1e-6f ? WindowLightMath::Luminance(lin) / dayY : 0.0f;
				Float3 glin = WindowLightMath::Compose(r.keys, g.gammaEmit, g.weights, g.noSat, g.smooth, g.fraction);
				glin = WindowLightMath::Scale(glin, d);
				const Float3 genc = WindowLightMath::PowPerChannel(glin, 1.0f / g.gammaEmit);
				r.glass = { genc.r, genc.g, genc.b };
			} else {
				// Weather only (or no keys found): the engine's own colour x the factor, in linear light.
				r.scaleLight = std::pow(d, 1.0f / g.gammaLight);
				const float gs = std::pow(d, 1.0f / g.gammaEmit);
				const auto& e = a_region->emittanceColor;
				r.glass = { e.red * gs, e.green * gs, e.blue * gs };
				if (r.hasKeys) {
					const Float3 engine = WindowLightMath::PowPerChannel({ e.red, e.green, e.blue }, g.gammaLight);
					const float dayY = WindowLightMath::Luminance(WindowLightMath::KeyLinear(r.keys[WindowLightMath::kDay], g.gammaLight, false));
					r.brightness = dayY > 1e-6f ? WindowLightMath::Luminance(engine) * d / dayY : 0.0f;
				}
			}
			return &r;
		}

		const char* CellName(const RE::TESObjectCELL* a_cell)
		{
			const char* name = a_cell ? a_cell->GetFormEditorID() : nullptr;
			return (name && *name) ? name : "?";
		}

		void LogInterior()
		{
			std::unordered_map<RE::TESRegion*, int> perRegion;
			for (const auto& fl : g.frameLights)
				for (const auto& [region, state] : g.regions)
					if (&state == fl.region)
						++perRegion[region];
			std::string regions;
			for (const auto& [region, state] : g.regions) {
				const auto* weather = RegionWeather(region);
				regions += fmt::format(" [region {:08X} weather {:08X}: {} lights, engine-clock check {:.4f}, brightness {:.1f}% of the day key]", region->GetFormID(),
					weather ? weather->GetFormID() : 0u, perRegion[region], state.engineCheck, state.brightness * 100.0f);
			}
			logger::info(
				"[WindowLight] {} ({:08X}): {} region-tied lights, {} region-tied glass shapes; hour {:.2f}, sun {:.1f} deg ({}), weights sunrise {:.2f} day {:.2f} "
				"sunset {:.2f} night {:.2f}, smooth fraction {:.2f}, outdoor weather {:08X} ratio {:.2f} x moon {:.2f} = factor {:.2f}; switches: no extra saturation {}, "
				"follow the real sun {}, brightness {}, outdoor weather {}.{}",
				CellName(g.cell), g.cell ? g.cell->GetFormID() : 0u, g.frameLights.size(), g.glassTied, g.hour, g.sunElevation, g.rising ? "rising" : "setting",
				g.weights.w[0], g.weights.w[1], g.weights.w[2], g.weights.w[3], g.fraction, g.outdoorWeather ? g.outdoorWeather->GetFormID() : 0u, g.weatherRatio,
				g.moonFactor, g.weatherFactor, g.noSat, g.followSun, g.smooth ? "Smooth" : "Mod's Keys", g.weather, regions);
		}
	}

	void BeginFrame()
	{
		const uint32_t frame = globals::state ? globals::state->frameCount : 0u;
		if (frame == g.frame)
			return;
		g.frame = frame;
		g.frameLights.clear();
		g.active = false;

		const auto& ll = globals::features::linearLighting.settings;
		g.noSat = ll.windowNoExtraSaturation;
		g.followSun = ll.windowFollowSun;
		g.smooth = ll.windowBrightness == 0;
		g.weather = ll.windowFollowWeather;
		const bool any = g.noSat || g.followSun || g.smooth || g.weather;

		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		if (cell != g.cell)
			ResetCell(cell);
		const RE::Sky* sky = globals::game::sky;
		if (!any || !cell || !cell->IsInteriorCell() || !sky || !sky->currentClimate || !globals::game::smState)
			return;
		++g.framesInCell;

		// Linear Lighting's conversions (no conversion while it is off: the shaders then take the colour as it is).
		const bool linear = ll.enableLinearLighting != 0;
		g.gammaLight = linear && std::isfinite(ll.lightGamma) ? std::clamp(ll.lightGamma, 0.1f, 5.0f) : 1.0f;
		g.gammaEmit = linear && std::isfinite(ll.emitColorGamma) ? std::clamp(ll.emitColorGamma, 0.1f, 5.0f) : 1.0f;

		// The sky: the clock weights (the engine's rule), the sun's true elevation and its trend, the day-key elevations.
		auto& skySync = globals::features::skySync;
		g.hour = std::isfinite(sky->currentGameHour) ? sky->currentGameHour : 12.0f;
		const auto& tn = sky->currentClimate->timing;
		const auto timing = WindowLightMath::MakeTiming(tn.sunrise.begin / 6.0f, tn.sunrise.end / 6.0f, tn.sunset.begin / 6.0f, tn.sunset.end / 6.0f,
			SkySync::GetDaytimeColorExtension());
		g.clock = WindowLightMath::ClockWeights(timing, g.hour);
		g.sunElevation = skySync.SunElevationDegAtHour(sky, g.hour);
		g.rising = skySync.SunElevationDegAtHour(sky, std::fmod(g.hour + 0.05f, 24.0f)) >= g.sunElevation;
		g.dayRise = skySync.SunElevationDegAtHour(sky, timing.sre);
		g.daySet = skySync.SunElevationDegAtHour(sky, timing.ssb);
		g.altitude = WindowLightMath::AltitudeWeights(g.sunElevation, g.rising, g.dayRise, g.daySet);
		g.weights = g.followSun ? g.altitude : g.clock;
		g.fraction = WindowLightMath::SmoothBrightnessFraction(g.sunElevation, g.rising, g.dayRise, g.daySet);
		UpdateWeather(sky);
		g.active = true;

		// This frame's region-tied lights (classified once per light and owner).
		auto* ssn = globals::game::smState->shadowSceneNode[0];
		if (!ssn)
			return;
		auto visit = [](const RE::NiPointer<RE::BSLight>& a_light) {
			auto* bsLight = a_light.get();
			auto* niLight = bsLight ? bsLight->light.get() : nullptr;
			if (!niLight)
				return;
			auto* owner = OwnerOf(niLight);
			auto [it, inserted] = g.lightCache.try_emplace(niLight);
			LightInfo& info = it->second;
			if (inserted || info.ref != owner) {
				info = {};
				info.ref = owner;
				info.region = EmittanceRegion(owner);
				if (info.region && !LighColor(owner, info.ligh))
					info.region = nullptr;
			}
			if (!info.region)
				return;
			auto* region = Region(info.region);
			if (!region)
				return;
			FrameLight fl;
			fl.light = niLight;
			fl.region = region;
			std::copy_n(info.ligh, 3, fl.ligh);
			g.frameLights.push_back(fl);
		};
		for (auto& e : ssn->GetRuntimeData().activeLights)
			visit(e);
		for (auto& e : ssn->GetRuntimeData().activeShadowLights)
			visit(e);

		if (!g.logged && g.framesInCell >= 120) {
			g.logged = true;
			LogInterior();
		}
	}

	void ApplyToLight(RE::NiLight* a_niLight, float3& io_color)
	{
		if (!g.active || g.frameLights.empty())
			return;
		for (const auto& fl : g.frameLights) {
			if (fl.light != a_niLight)
				continue;
			if (fl.region->recompute) {
				io_color.x = fl.region->encLight[0] * fl.ligh[0];
				io_color.y = fl.region->encLight[1] * fl.ligh[1];
				io_color.z = fl.region->encLight[2] * fl.ligh[2];
			} else {
				io_color.x *= fl.region->scaleLight;
				io_color.y *= fl.region->scaleLight;
				io_color.z *= fl.region->scaleLight;
			}
			return;
		}
	}

	bool IsWindowLight(RE::NiLight* a_niLight)
	{
		if (!g.active)
			return false;
		for (const auto& fl : g.frameLights)
			if (fl.light == a_niLight)
				return true;
		return false;
	}

	void SetupGlass(RE::BSRenderPass* a_pass, RE::BSLightingShaderProperty* a_property)
	{
		g.swapProperty = nullptr;
		g.swapOriginal = nullptr;
		if (!g.active || !a_pass || !a_property || !a_property->emissiveColor ||
			a_property->flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance))
			return;
		auto* geometry = a_pass->geometry;
		auto [it, inserted] = g.glassCache.try_emplace(a_property);
		GlassInfo& info = it->second;
		if (inserted || info.geometry != geometry) {
			const bool wasTied = !inserted && info.region;
			info = {};
			info.geometry = geometry;
			info.region = EmittanceRegion(OwnerOf(geometry));
			if (info.region && !wasTied)
				++g.glassTied;
			else if (!info.region && wasTied && g.glassTied)
				--g.glassTied;
			if (info.region && !g.glassPointerLogged) {
				g.glassPointerLogged = true;
				logger::info("[WindowLight] Glass: the first region-tied shape's emissive colour pointer {} the region's emittance colour (region {:08X})",
					static_cast<const void*>(a_property->emissiveColor) == static_cast<const void*>(&info.region->emittanceColor) ? "points at" : "does not point at",
					info.region->GetFormID());
			}
		}
		if (!info.region)
			return;
		auto* region = Region(info.region);
		if (!region)
			return;
		g.swapProperty = a_property;
		g.swapOriginal = a_property->emissiveColor;
		a_property->emissiveColor = &region->glass;
	}

	void RestoreGlass()
	{
		if (g.swapProperty) {
			g.swapProperty->emissiveColor = g.swapOriginal;
			g.swapProperty = nullptr;
			g.swapOriginal = nullptr;
		}
	}

	void DrawStatus()
	{
		if (!g.active) {
			ImGui::TextDisabled("Not active here (interiors only, with at least one switch on).");
			return;
		}
		ImGui::TextDisabled("Here: %zu window lights, %u window glass shapes seen. Sun %.1f deg (%s).", g.frameLights.size(), g.glassTied, g.sunElevation,
			g.rising ? "rising" : "setting");
		ImGui::TextDisabled("Colour mix: sunrise %.2f, day %.2f, sunset %.2f, night %.2f. Outdoor weather factor %.2f (weather %.2f x moon %.2f).", g.weights.w[0],
			g.weights.w[1], g.weights.w[2], g.weights.w[3], g.weatherFactor, g.weatherRatio, g.moonFactor);
		for (const auto& [region, state] : g.regions) {
			if (state.frame != g.frame)
				continue;
			ImGui::TextDisabled("Region %08X: brightness %.0f%% of the mod's daytime level; game colour vs clock rebuild %.3f.", region->GetFormID(),
				state.brightness * 100.0f, state.engineCheck);
		}
	}
}
