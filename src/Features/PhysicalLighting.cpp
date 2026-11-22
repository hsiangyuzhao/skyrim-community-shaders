#include "PhysicalLighting.h"

#include "FeatureBuffer.h"
#include "Features/IBL.h"
#include "Features/InverseSquareLighting.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/PostProcessing/HistogramAutoExposure.h"
#include "Features/SkySync.h"
#include "Features/WindowLight.h"
#include "State.h"
#include "Util.h"
#include "Utils/FrameTrace.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalLighting::Settings,
	Enabled)

namespace
{
	constexpr double kWobblePeriodSeconds = 4.0;
	constexpr float kPRateEVps = 1.0f;     // pre-exposure: at most 1 EV per second
	constexpr float kPSmoothSeconds = 0.5f;
	constexpr float kPLogLimit = 8.0f;     // P within 2^-8 .. 2^8
	// Reference exposure: Classic's exposure at an adapted luminance of 0.125 (log2 = -3, its "0 EV") and 0 EV compensation:
	// 0.18 / 0.125. P = applied exposure / this, i.e. 0.125 x compensation / the eye's divisor.
	constexpr float kReferenceDivisor = 0.125f;
	constexpr float kPrecisionCeiling = 16384.0f;  // 2^14
	constexpr float kEmitterPeak = 64.0f;          // assumed peak buffer value of an emitter / sun quad / moon disc at f = 1

	float Lum(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
	float Lum(const float3& c) { return Lum(c.x, c.y, c.z); }

	bool SameTransform(const RE::NiTransform& a, const RE::NiTransform& b)
	{
		return std::memcmp(a.rotate.entry, b.rotate.entry, sizeof(a.rotate.entry)) == 0 && a.translate.x == b.translate.x &&
		       a.translate.y == b.translate.y && a.translate.z == b.translate.z;
	}

	bool SameColor(const RE::NiColor& a, const RE::NiColor& b)
	{
		return a.red == b.red && a.green == b.green && a.blue == b.blue;
	}

	float SafePow(float a_base, float a_exponent)
	{
		if (!(a_base > 0.0f) || !std::isfinite(a_base))
			return a_base == 0.0f ? 0.0f : 1.0f;
		return std::pow(a_base, a_exponent);
	}

	HistogramAutoExposure* AutoExposure()
	{
		auto& pp = globals::features::postProcessing;
		if (!pp.loaded)
			return nullptr;
		return static_cast<HistogramAutoExposure*>(pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::AutoExposure)].get());
	}

	RE::NiDirectionalLight* SunLight()
	{
		auto* sm = globals::game::smState;
		if (!sm)
			return nullptr;
		auto* ssn = sm->shadowSceneNode[0];
		if (!ssn || !ssn->GetRuntimeData().sunLight)
			return nullptr;
		return skyrim_cast<RE::NiDirectionalLight*>(ssn->GetRuntimeData().sunLight->light.get());
	}

	void WriteFloat(unsigned char* a_data, size_t a_size, size_t a_offset, float a_value)
	{
		if (a_offset + sizeof(float) <= a_size)
			std::memcpy(a_data + a_offset, &a_value, sizeof(float));
	}

	void ScaleFloat(unsigned char* a_data, size_t a_size, size_t a_offset, float a_factor)
	{
		if (a_offset + sizeof(float) > a_size)
			return;
		float v;
		std::memcpy(&v, a_data + a_offset, sizeof(float));
		v *= a_factor;
		std::memcpy(a_data + a_offset, &v, sizeof(float));
	}

	void ScaleFloat3(unsigned char* a_data, size_t a_size, size_t a_offset, float a_factor)
	{
		for (size_t i = 0; i < 3; ++i)
			ScaleFloat(a_data, a_size, a_offset + i * sizeof(float), a_factor);
	}

	/// The real-world reference column of the audit (A4), for the meter's panel.
	struct ReferenceValue
	{
		const char* what;
		const char* value;
	};
	constexpr ReferenceValue kReferences[] = {
		{ "Clear noon, ground in the sun", "~100000 lx (sun at the top of the atmosphere 127000 lx)" },
		{ "Clear sky (luminance)", "3000 - 8000 cd/m2" },
		{ "Overcast day", "1000 - 20000 lx" },
		{ "Sunset / sunrise", "~400 lx" },
		{ "Civil twilight end (sun -6 deg)", "~3 lx" },
		{ "Full moon at the zenith", "0.1 - 0.3 lx (0.26 lx)" },
		{ "Quarter moon", "0.02 - 0.03 lx" },
		{ "Moonless clear night", "~0.001 lx (starlight 0.0002 - 0.001 lx)" },
		{ "Natural dark night sky (luminance)", "0.00017 - 0.00027 cd/m2" },
		{ "Candle (1 cd) at 1 m", "~1 lx; flame ~5000 cd/m2" },
		{ "Torch (~25 cd) at 1 m / 3 m", "~25 lx / ~3 lx" },
		{ "Campfire / brazier (150 - 500 cd) at 2 m", "40 - 125 lx" },
		{ "Tavern near the hearth / in a corner", "20 - 100 lx / 1 - 5 lx" },
		{ "Indoors near a window (daylight factor 2-5 %)", "~300 lx overcast, up to ~3000 lx sunny" },
		{ "Lit window at night seen from outside", "10 - 50 cd/m2" },
		{ "Sunlit snow (luminance)", "~30000 cd/m2" },
	};
}

const char* PhysicalLighting::ClassName(Class a_class)
{
	switch (a_class) {
	case Class::Sun:
		return "Sun";
	case Class::Moon:
		return "Moon";
	case Class::InteriorDirectional:
		return "Interior Directional";
	case Class::Ambient:
		return "Ambient";
	case Class::Sky:
		return "Sky";
	case Class::NightGlow:
		return "Night Sky Glow";
	case Class::SunDisc:
		return "Sun Disc";
	case Class::MoonDisc:
		return "Moon Discs";
	case Class::Stars:
		return "Stars";
	case Class::Lamps:
		return "Lamps";
	case Class::LampsISL:
		return "Lamps (Linear Flag)";
	case Class::Window:
		return "Window Light";
	case Class::Emissive:
		return "Emissive";
	case Class::Glow:
		return "Glow Maps";
	case Class::FireMagic:
		return "Fire and Magic";
	case Class::EffectSelfLit:
		return "Effects (Self-Lit)";
	case Class::Precipitation:
		return "Rain, Snow, Unlit Smoke";
	case Class::Fog:
		return "Fog";
	case Class::Water:
		return "Water Body";
	case Class::CharacterLight:
		return "Character Light";
	case Class::SnowRim:
		return "Snow Rim Light";
	case Class::SnowSparkle:
		return "Snow Sparkles";
	default:
		return "?";
	}
}

// ------------------------------------------------------------------------------------------------
// Settings
// ------------------------------------------------------------------------------------------------

void PhysicalLighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PhysicalLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PhysicalLighting::RestoreDefaultSettings()
{
	settings = {};
}

// ------------------------------------------------------------------------------------------------
// Per frame
// ------------------------------------------------------------------------------------------------

std::string PhysicalLighting::PrerequisiteReason() const
{
	auto& ll = globals::features::linearLighting;
	if (!ll.loaded || !ll.settings.enableLinearLighting)
		return "Linear Lighting is off (Lighting > Linear Lighting)";
	auto& pp = globals::features::postProcessing;
	if (!pp.loaded || pp.bypass)
		return "Post Processing is off";
	auto* ae = AutoExposure();
	if (!ae || !ae->enabled || !ae->adaptationSB)
		return "CS auto exposure is off (Post Processing > Histogram Auto Exposure)";
	if (!pp.settings.DisableVanillaTonemapping)
		return "the game's own tone mapping is on (Post Processing > Disable Vanilla Tonemapping)";
	return {};
}

bool PhysicalLighting::MenuExcludes() const
{
	auto* ui = globals::game::ui;
	if (!ui)
		return true;
	// Item previews, the map and every pausing menu: the frame is drawn at S = 1 (the classic values).
	static const std::string_view kMenus[] = { RE::InventoryMenu::MENU_NAME, RE::MagicMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME,
		RE::BarterMenu::MENU_NAME, RE::GiftMenu::MENU_NAME, RE::MapMenu::MENU_NAME, RE::CraftingMenu::MENU_NAME, RE::MainMenu::MENU_NAME,
		RE::LoadingMenu::MENU_NAME, RE::BookMenu::MENU_NAME, RE::LockpickingMenu::MENU_NAME, RE::RaceSexMenu::MENU_NAME };
	for (const auto& m : kMenus)
		if (ui->IsMenuOpen(m))
			return true;
	return ui->GameIsPaused();
}

void PhysicalLighting::UpdatePreExposure(float a_dt)
{
	// Readback ring of the adaptation buffer (4 floats): copy this frame's (the AE of the frame that just ended), read the oldest one.
	auto* ae = AutoExposure();
	auto context = globals::d3d::context;
	if (!ae || !ae->adaptationSB || !context)
		return;
	if (!aeStaging[0]) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& b : aeStaging)
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, b.put())))
				return;
	}
	winrt::com_ptr<ID3D11Resource> source;
	if (auto* srv = ae->adaptationSB->SRV())
		srv->GetResource(source.put());
	if (!source)
		return;

	// Read the oldest slot first (two frames old).
	const int readIndex = (aeRingIndex + 1) % kRing;
	if (aeStagingPending[readIndex]) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(aeStaging[readIndex].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			std::memcpy(aeLastRead, mapped.pData, sizeof(aeLastRead));
			context->Unmap(aeStaging[readIndex].get(), 0);
			aeStagingPending[readIndex] = false;
			aeLastEye = ae->EyeActive();
		}
	}
	context->CopyResource(aeStaging[aeRingIndex].get(), source.get());
	aeStagingPending[aeRingIndex] = true;
	aeRingIndex = (aeRingIndex + 1) % kRing;

	// The exposure the eye applied (S-free: the AE meters buffer / S).
	float compensation = 0.f;
	float2 range{};
	ae->GetUserExposureParameters(compensation, range);
	const float comp = std::exp2(compensation);
	float divisor = aeLastRead[0];
	if (!aeLastEye) {
		const float lo = std::exp2(range.x) * 0.125f, hi = std::exp2(std::max(range.y, range.x)) * 0.125f;
		divisor = std::clamp(divisor, lo, hi);
	}
	if (std::isfinite(divisor) && divisor > 0.0f)
		frame.pTarget = std::clamp(kReferenceDivisor * comp / divisor, std::exp2(-kPLogLimit), std::exp2(kPLogLimit));

	// Snap on loading screens, time skips and when the master comes on; otherwise smoothed and rate limited.
	bool snap = !pInitialised;
	if (auto* ui = globals::game::ui; ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME))
		snap = true;
	float hours = -1.0f;
	if (auto* calendar = RE::Calendar::GetSingleton())
		hours = calendar->GetCurrentGameTime() * 24.0f;
	if (lastGameHours >= 0.0f && hours >= 0.0f && std::abs(hours - lastGameHours) > 0.25f)
		snap = true;
	lastGameHours = hours;
	const float target = std::log2(frame.pTarget);
	if (snap) {
		logP = target;
		pInitialised = true;
	} else {
		float step = (target - logP) * (1.0f - std::exp(-a_dt / kPSmoothSeconds));
		step = std::clamp(step, -kPRateEVps * a_dt, kPRateEVps * a_dt);
		logP += step;
	}
	logP = std::clamp(logP, -kPLogLimit, kPLogLimit);
	frame.snapped = snap;
}

void PhysicalLighting::BeginFrame()
{
	++frame.frame;
	const float dt = std::clamp(RE::BSTimer::GetSingleton()->realTimeDelta, 0.0f, 0.25f);

	if (meterOn)
		ReadMeterResults();

	const std::string reason = PrerequisiteReason();
	frame.prerequisites = reason.empty();
	frame.reason = reason;
	frame.menuExcluded = MenuExcludes();

	if ((settings.Enabled || meterOn) && frame.prerequisites)
		UpdatePreExposure(dt);
	if (settings.Enabled && frame.prerequisites && !wasActive)
		pInitialised = false;  // the master came on: snap P to the eye

	const bool active = settings.Enabled && frame.prerequisites && !frame.menuExcluded && loaded;
	if (active && !wasActive)
		wobbleStart = static_cast<double>(GetTickCount64()) * 1e-3;
	wasActive = active;
	frame.active = active;
	frame.P = std::exp2(logP);

	const double t = static_cast<double>(GetTickCount64()) * 1e-3 - wobbleStart;
	const float pulse = static_cast<float>(std::exp2(std::sin(2.0 * 3.14159265358979 * t / kWobblePeriodSeconds)));
	frame.wobble = (active && sceneWobble) ? pulse : 1.0f;
	frame.classWobble = (active && classWobbleOn) ? pulse : 1.0f;
	if (active) {
		frame.S = frame.P * frame.wobble;
		frame.invS = 1.0f / frame.S;
	} else {
		frame.S = 1.0f;
		frame.invS = 1.0f;
	}
}

float PhysicalLighting::Scale(Class a_class)
{
	const auto index = static_cast<uint32_t>(a_class);
	if (index >= kClassCount)
		return 1.0f;
	float f = 1.0f;
	if (frame.active) {
		f = frame.S;  // cal(class) = 1 in 52a
		if (classWobbleOn && static_cast<uint32_t>(classWobbleIndex) == index)
			f *= frame.classWobble;
		// Precision headroom only (never a look clamp): emitter, sun-quad and moon-disc classes stay below 2^14 / their peak buffer value at
		// f = 1 (half-float / R11G11B10 headroom). With 52a's values (P within 2^-8 .. 2^8, peaks <= 64) this never acts.
		switch (a_class) {
		case Class::SunDisc:
		case Class::MoonDisc:
		case Class::Emissive:
		case Class::Glow:
		case Class::FireMagic:
		case Class::EffectSelfLit:
			f = std::min(f, kPrecisionCeiling / kEmitterPeak);
			break;
		default:
			break;
		}
	}
	registry[index].lastFrame = frame.frame;
	registry[index].lastFactor = f;
	return f;
}

// ------------------------------------------------------------------------------------------------
// Engine light / ambient (written over the engine's values; the engine's kept and put back)
// ------------------------------------------------------------------------------------------------

void PhysicalLighting::ApplyEngineLights()
{
	auto* sm = globals::game::smState;
	if (!sm)
		return;
	const auto ll = globals::features::linearLighting.GetCommonBufferData();
	const bool linearOn = ll.enableLinearLighting != 0;

	// DALC: Color::Ambient raises it to ambientGamma, so the engine's (gamma-space) transform is scaled by f^(1 / ambientGamma).
	{
		const float f = Scale(Class::Ambient);
		const float gamma = linearOn && ll.ambientGamma > 1e-3f ? ll.ambientGamma : 1.0f;
		const float k = f == 1.0f ? 1.0f : SafePow(f, 1.0f / gamma);
		RE::NiTransform& engineTransform = sm->directionalAmbientTransform;
		const bool holdsOurs = dalcWritten && SameTransform(engineTransform, writtenDalc);
		if (!holdsOurs) {
			engineDalc = engineTransform;
			dalcWritten = false;
		}
		if (k != 1.0f) {
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					engineTransform.rotate.entry[r][c] = engineDalc.rotate.entry[r][c] * k;
			engineTransform.translate = engineDalc.translate * k;
			writtenDalc = engineTransform;
			dalcWritten = true;
		} else if (holdsOurs) {
			std::memcpy(engineTransform.rotate.entry, engineDalc.rotate.entry, sizeof(engineTransform.rotate.entry));
			engineTransform.translate = engineDalc.translate;
			dalcWritten = false;
		}
	}

	// Directional light: linear (Physical Sky's override) or gamma (Color::DirectionalLight raises it to lightGamma).
	if (auto* light = SunLight()) {
		Class cls = Class::InteriorDirectional;
		if (!Util::IsInterior()) {
			const int dominant = globals::features::skySync.loaded ? globals::features::skySync.DominantCaster() : 0;
			cls = (dominant == 1 || dominant == 2) ? Class::Moon : Class::Sun;
		}
		const float f = Scale(cls);
		const bool linearLight = ll.isDirLightLinear != 0;
		const float gamma = (!linearLight && linearOn && ll.lightGamma > 1e-3f) ? ll.lightGamma : 1.0f;
		const float k = f == 1.0f ? 1.0f : SafePow(f, 1.0f / gamma);
		auto& diffuse = light->GetLightRuntimeData().diffuse;
		const bool holdsOurs = diffuseWritten && dirLightObject == light && SameColor(diffuse, writtenDiffuse);
		if (!holdsOurs) {
			if (diffuseWritten && dirLightObject && dirLightObject != light) {
				// A different light object now: the old one is not ours to restore any more (it may be gone).
			}
			engineDiffuse = diffuse;
			dirLightObject = light;
			diffuseWritten = false;
		}
		if (k != 1.0f) {
			diffuse = RE::NiColor(engineDiffuse.red * k, engineDiffuse.green * k, engineDiffuse.blue * k);
			writtenDiffuse = diffuse;
			diffuseWritten = true;
		} else if (holdsOurs) {
			diffuse = engineDiffuse;
			diffuseWritten = false;
		}
	}
}

RE::NiColor PhysicalLighting::EngineDirLightDiffuse(const RE::NiDirectionalLight* a_light) const
{
	if (!a_light)
		return {};
	auto* light = const_cast<RE::NiDirectionalLight*>(a_light);
	const auto& diffuse = light->GetLightRuntimeData().diffuse;
	if (diffuseWritten && dirLightObject == a_light && SameColor(diffuse, writtenDiffuse))
		return engineDiffuse;
	return diffuse;
}

const RE::NiTransform& PhysicalLighting::EngineDalc() const
{
	static RE::NiTransform empty{};
	auto* sm = globals::game::smState;
	if (!sm)
		return empty;
	const RE::NiTransform& engineTransform = sm->directionalAmbientTransform;
	if (dalcWritten && SameTransform(engineTransform, writtenDalc))
		return engineDalc;
	return engineTransform;
}

RE::NiPoint3 PhysicalLighting::EngineDalcTranslate() const
{
	return EngineDalc().translate;
}

// ------------------------------------------------------------------------------------------------
// FeatureData
// ------------------------------------------------------------------------------------------------

void PhysicalLighting::PatchFeatureData(unsigned char* a_data, size_t a_size)
{
	const auto& o = GetFeatureDataUnitOffsets();
	if (!a_data || a_size < o.total)
		return;

	// Light values carried in FeatureData.
	if (frame.active) {
		const float sky = Scale(Class::Sky);
		ScaleFloat3(a_data, a_size, o.sunlightColor, sky);
		ScaleFloat3(a_data, a_size, o.masserColor, sky);
		ScaleFloat3(a_data, a_size, o.secundaColor, sky);
		const float glow = Scale(Class::NightGlow);
		ScaleFloat3(a_data, a_size, o.nightBaseUpper, glow);
		ScaleFloat3(a_data, a_size, o.nightBaseHorizon, glow);
		ScaleFloat3(a_data, a_size, o.nightBaseLower, glow);
		const float moonDisc = Scale(Class::MoonDisc);  // after Physical Sky's [1, cap] clamp of the disc scale (precision only)
		ScaleFloat(a_data, a_size, o.masserDiskScale, moonDisc);
		ScaleFloat(a_data, a_size, o.secundaDiskScale, moonDisc);
		ScaleFloat3(a_data, a_size, o.cloudSunColor, Scale(Class::Sun));
		const float moon = Scale(Class::Moon);
		ScaleFloat3(a_data, a_size, o.cloudMasserColor, moon);
		ScaleFloat3(a_data, a_size, o.cloudSecundaColor, moon);
		ScaleFloat(a_data, a_size, o.fogSkyLight, Scale(Class::Fog));
		ScaleFloat(a_data, a_size, o.pointLightMult, Scale(Class::Lamps));
		ScaleFloat(a_data, a_size, o.emitColorMult, Scale(Class::Emissive));
		ScaleFloat(a_data, a_size, o.glowmapMult, Scale(Class::Glow));
	}

	// Shader scalars (exactly 1.0f while inactive).
	uint32_t physSkyEnabled = 0;
	std::memcpy(&physSkyEnabled, a_data + o.physSkyEnabled, sizeof(physSkyEnabled));
	const auto ll = globals::features::linearLighting.GetCommonBufferData();
	const bool linearOn = ll.enableLinearLighting != 0;
	WriteFloat(a_data, a_size, o.padCharacterLight, Scale(Class::CharacterLight));
	WriteFloat(a_data, a_size, o.padSnowRim, Scale(Class::SnowRim));
	WriteFloat(a_data, a_size, o.padFog, Scale(Class::Fog));
	WriteFloat(a_data, a_size, o.padSkyDome, Scale(Class::Sky));
	WriteFloat(a_data, a_size, o.padSkyStars, Scale(Class::Stars));
	WriteFloat(a_data, a_size, o.padSkySunDisc, Scale(Class::SunDisc));
	// Moon discs: with Physical Sky the disc scale above carries the class; without it the shader scales the disc itself.
	{
		const float moonDisc = Scale(Class::MoonDisc);
		WriteFloat(a_data, a_size, o.padSkyMoonBase, physSkyEnabled ? 1.0f : moonDisc);
	}
	WriteFloat(a_data, a_size, o.padSkyCloud, Scale(Class::Sky));
	WriteFloat(a_data, a_size, o.padWaterGlint, frame.active ? 1.0f : 0.0f);
	WriteFloat(a_data, a_size, o.padWaterBody, Scale(Class::Water));
	WriteFloat(a_data, a_size, o.padWaterFallback, Scale(Class::Sky));
	WriteFloat(a_data, a_size, o.padPrecipitation, Scale(Class::Precipitation));
	WriteFloat(a_data, a_size, o.padSparkle, Scale(Class::SnowSparkle));
	WriteFloat(a_data, a_size, o.padFrameS, frame.S);
	WriteFloat(a_data, a_size, o.padFrameInvS, frame.invS);
	// The vanilla lens flare is composited through Color::GammaToLinear (pow 1.6) with Linear Lighting: its output carries S^(1 / 1.6).
	WriteFloat(a_data, a_size, o.padFlareOut, frame.S == 1.0f ? 1.0f : (linearOn ? SafePow(frame.S, 1.0f / 1.6f) : frame.S));
}

// ------------------------------------------------------------------------------------------------
// Other carriers
// ------------------------------------------------------------------------------------------------

float PhysicalLighting::LightFadeScale(RE::NiLight* a_light, bool a_linear)
{
	if (!frame.active)
		return 1.0f;
	if (a_light && WindowLight::IsWindowLight(a_light)) {
		const float window = Scale(Class::Window);
		if (a_linear)
			return window;
		const float lamps = Scale(Class::Lamps);  // already in pointLightMult
		return lamps > 0.0f ? window / lamps : 1.0f;
	}
	if (a_linear)
		return Scale(Class::LampsISL);
	return 1.0f;
}

float PhysicalLighting::GlassColorScale()
{
	if (!frame.active)
		return 1.0f;
	const float window = Scale(Class::Window);
	const float emissive = Scale(Class::Emissive);  // already in emitColorMult
	const float ratio = emissive > 0.0f ? window / emissive : 1.0f;
	if (ratio == 1.0f)
		return 1.0f;
	const auto ll = globals::features::linearLighting.GetCommonBufferData();
	const float gamma = ll.enableLinearLighting && ll.emitColorGamma > 1e-3f ? ll.emitColorGamma : 1.0f;
	return SafePow(ratio, 1.0f / gamma);
}

float PhysicalLighting::CubemapHistoryScale(int a_which)
{
	const int i = std::clamp(a_which, 0, 1);
	const float ratio = frame.S / cubemapLastS[i];
	cubemapLastS[i] = frame.S;
	cubemapLastFrame[i] = frame.frame;
	return std::isfinite(ratio) && ratio > 0.0f ? ratio : 1.0f;
}

PhysicalLighting::EffectScalars PhysicalLighting::GetEffectScalars()
{
	EffectScalars s{};
	s.selfLit = Scale(Class::EffectSelfLit);
	s.fire = Scale(Class::FireMagic);
	s.unlit = Scale(Class::Precipitation);
	s.dLight = Scale(Class::Ambient);
	s.fog = Scale(Class::Fog);
	s.normDiv = frame.invS;  // multiplied in the shader (x 1.0f exactly off)
	if (frame.active) {
		const auto ll = globals::features::linearLighting.GetCommonBufferData();
		const float gamma = ll.enableLinearLighting && ll.ambientGamma > 1e-3f ? ll.ambientGamma : 1.0f;
		const float f = s.dLight;
		s.ambientRaw = f == 1.0f ? 1.0f : f / SafePow(f, 1.0f / gamma);  // raw DALC readers get f^(1/gamma) from the engine transform
	}
	return s;
}

// ------------------------------------------------------------------------------------------------
// Light meter
// ------------------------------------------------------------------------------------------------

void PhysicalLighting::SetupResources()
{
	meterOut = std::make_unique<StructuredBuffer>(StructuredBufferDesc<float4>(kMeterSlots, false), kMeterSlots);
	meterOut->CreateUAV();
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = sizeof(float4) * kMeterSlots;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (auto& b : meterStaging)
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, b.put()));
}

void PhysicalLighting::ClearShaderCache()
{
	meterCS = nullptr;
	meterCompileFailed = false;
}

void PhysicalLighting::DispatchMeter(ID3D11ShaderResourceView* a_main, ID3D11ShaderResourceView* a_albedo, ID3D11ShaderResourceView* a_normalRoughness,
	ID3D11ShaderResourceView* a_masks, ID3D11ShaderResourceView* a_specular, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_shadowMask)
{
	if (!meterOn || !meterOut || frame.menuExcluded || !globals::state->inWorld)
		return;
	if (!meterCS && !meterCompileFailed) {
		meterCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalLighting\\MeterCS.hlsl", {}, "cs_5_0")));
		if (!meterCS) {
			meterCompileFailed = true;
			logger::warn("[Physical Lighting] MeterCS failed to compile; the light meter is unavailable");
			return;
		}
	}
	if (!meterCS)
		return;
	auto context = globals::d3d::context;
	ID3D11Buffer* perFrame = *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &perFrame);
	ID3D11ShaderResourceView* srvs[7] = { a_main, a_albedo, a_normalRoughness, a_masks, a_specular, a_depth, a_shadowMask };
	context->CSSetShaderResources(0, 7, srvs);
	ID3D11UnorderedAccessView* uav = meterOut->UAV();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(meterCS.get(), nullptr, 0);
	context->Dispatch(1, 1, 1);
	ID3D11ShaderResourceView* nullSrvs[7] = {};
	context->CSSetShaderResources(0, 7, nullSrvs);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(12, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	winrt::com_ptr<ID3D11Resource> source;
	uav->GetResource(source.put());
	context->CopyResource(meterStaging[meterRingIndex].get(), source.get());
	meterPending[meterRingIndex] = true;
	meterFrameOf[meterRingIndex] = frame.frame;
	meterSOf[meterRingIndex] = frame.S;
	meterPOf[meterRingIndex] = frame.P;
	meterRingIndex = (meterRingIndex + 1) % kRing;
}

void PhysicalLighting::ReadMeterResults()
{
	auto context = globals::d3d::context;
	// Newest finished slot, without waiting.
	for (int age = 1; age <= kRing; ++age) {
		const int index = (meterRingIndex - age + kRing * 2) % kRing;
		if (!meterPending[index])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(meterStaging[index].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			continue;
		float4 raw[kMeterSlots];
		std::memcpy(raw, mapped.pData, sizeof(raw));
		context->Unmap(meterStaging[index].get(), 0);
		// Older pending slots are superseded.
		for (int k = 0; k < kRing; ++k)
			if (meterFrameOf[k] <= meterFrameOf[index])
				meterPending[k] = false;
		current.S = meterSOf[index];
		current.P = meterPOf[index];
		current.frame = meterFrameOf[index];
		ComputeReading(raw);
		return;
	}
}

void PhysicalLighting::FillContext(Reading& r) const
{
	const auto location = Util::FrameTrace::CurrentLocation();
	r.location = location.world.empty() ? (location.interior ? "interior" : "-") : location.world;
	r.cell = location.cell.empty() ? "-" : location.cell;
	if (auto* sky = globals::game::sky) {
		r.weather = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
		if (sky->masser)
			r.moonPhase = static_cast<int>(std::lround(SkySync::PhaseFactorFromTexture(sky->masser) * 100.0f));
	}
	if (auto* calendar = RE::Calendar::GetSingleton()) {
		const float hours = calendar->GetHour();
		const int h = static_cast<int>(hours);
		r.time = std::format("day {:.0f}, {:02}:{:02}", calendar->GetDaysPassed(), h, static_cast<int>((hours - h) * 60.0f));
	}
	auto& skySync = globals::features::skySync;
	if (skySync.loaded) {
		auto elevation = [&](int i) { return std::asin(std::clamp(skySync.RawDirection(i).z, -1.f, 1.f)) * 57.29578f; };
		r.sunElevation = elevation(0);
		r.masserElevation = elevation(1);
		r.secundaElevation = elevation(2);
	}
}

void PhysicalLighting::ComputeReading(const float4* a_raw)
{
	Reading r;
	r.valid = true;
	r.frame = current.frame;
	r.S = current.S > 0.f ? current.S : 1.f;
	r.P = current.P;
	r.rawLit = { a_raw[0].x, a_raw[0].y, a_raw[0].z };
	r.rawAlbedo = { a_raw[1].x, a_raw[1].y, a_raw[1].z };
	r.normalWS = { a_raw[2].x, a_raw[2].y, a_raw[2].z };
	r.rawMasksAmbientY = a_raw[3].z;
	r.rawSpecular = { a_raw[4].x, a_raw[4].y, a_raw[4].z };
	const float3 rel = { a_raw[5].x, a_raw[5].y, a_raw[5].z };
	r.depth = a_raw[5].w;
	r.rawShadow = std::clamp(a_raw[6].x, 0.f, 1.f);
	r.distance = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);

	const auto ll = globals::features::linearLighting.GetCommonBufferData();
	const float vdm = ll.enableLinearLighting ? ll.vanillaDiffuseMult : 1.0f;
	const float invS = 1.0f / r.S;
	constexpr float kPi = 3.14159265f;
	const float albedoY = std::max(Lum(r.rawAlbedo), 0.04f);

	// Measured: lit diffuse / albedo (the direct specular taken out; reflections and fog-free composite).
	const float3 diffuse = { std::max(r.rawLit.x - r.rawSpecular.x, 0.f), std::max(r.rawLit.y - r.rawSpecular.y, 0.f), std::max(r.rawLit.z - r.rawSpecular.z, 0.f) };
	r.eMeasured = kPi * Lum(diffuse) / albedoY * kR0 * invS;
	r.luminance = Lum(r.rawLit) * kR0 * invS;

	// Directional (engine colour, unscaled) x N.L x shadow visibility; before the atmosphere's transmittance (Physical Sky applies it in
	// the shader).
	float3 n = r.normalWS;
	if (auto* light = SunLight()) {
		const RE::NiColor c0 = EngineDirLightDiffuse(light);
		const float fade = light->GetLightRuntimeData().fade;
		float sunlightScale = 1.f;
		if (auto* ism = RE::ImageSpaceManager::GetSingleton())
			sunlightScale = !globals::game::isVR ? ism->GetRuntimeData().data.baseData.hdr.sunlightScale : ism->GetVRRuntimeData().data.baseData.hdr.sunlightScale;
		float3 c = { c0.red * fade * sunlightScale, c0.green * fade * sunlightScale, c0.blue * fade * sunlightScale };
		if (ll.enableLinearLighting && !ll.isDirLightLinear) {
			const float llm = (!Util::IsInterior() && ll.dirLightMult > 1e-5f) ? ll.dirLightMult : 1.f;
			auto conv = [&](float v) { return SafePow(std::max(v / llm, 0.f), ll.lightGamma) * ll.lightMult * ll.directionalLightMult * llm; };
			c = { conv(c.x), conv(c.y), conv(c.z) };
		}
		const auto& d = light->GetWorldDirection();
		float3 toLight = { -d.x, -d.y, -d.z };
		const float len = std::sqrt(toLight.x * toLight.x + toLight.y * toLight.y + toLight.z * toLight.z);
		if (len > 1e-6f)
			toLight = toLight / len;
		const float nl = std::max(n.x * toLight.x + n.y * toLight.y + n.z * toLight.z, 0.f);
		r.eDirectional = kPi * vdm * Lum(c) * nl * r.rawShadow * kR0;
		const int dominant = globals::features::skySync.loaded ? globals::features::skySync.DominantCaster() : 0;
		r.directionalIsSun = !(dominant == 1 || dominant == 2);
	}

	// Ambient: Masks.z (DALC x amount + IBL, x albedo, after skylighting) / albedo; DALC on its own at the normal (engine, unscaled).
	r.eAmbient = kPi * std::max(r.rawMasksAmbientY, 0.f) / albedoY * kR0 * invS;
	{
		const auto& t = EngineDalc();
		float3 a = {
			t.rotate.entry[0][0] * n.x + t.rotate.entry[0][1] * n.y + t.rotate.entry[0][2] * n.z + t.translate.x,
			t.rotate.entry[1][0] * n.x + t.rotate.entry[1][1] * n.y + t.rotate.entry[1][2] * n.z + t.translate.y,
			t.rotate.entry[2][0] * n.x + t.rotate.entry[2][1] * n.y + t.rotate.entry[2][2] * n.z + t.translate.z,
		};
		const float g = ll.enableLinearLighting ? ll.ambientGamma : 1.f;
		a = { SafePow(std::max(a.x, 0.f), g), SafePow(std::max(a.y, 0.f), g), SafePow(std::max(a.z, 0.f), g) };
		auto& ibl = globals::features::ibl;
		const float amount = (ibl.loaded && ibl.settings.EnableDiffuseIBL && (!Util::IsInterior() || ibl.settings.EnableInterior)) ? ibl.settings.DALCAmount : 1.f;
		r.eDalc = kPi * Lum(a) * amount * kR0;
		r.eIbl = std::max(r.eAmbient - r.eDalc, 0.f);
	}

	// Lamps: the scene's point lights as Light Limit Fix fills them (Inverse Square Lighting included), unshadowed, x N.L.
	{
		float sum = 0.f;
		int count = 0;
		RE::NiPoint3 cam{};
		if (auto* pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot)
			cam = pc->cameraRoot->world.translate;
		const RE::NiPoint3 pos = { cam.x + rel.x, cam.y + rel.y, cam.z + rel.z };
		auto visit = [&](const RE::NiPointer<RE::BSLight>& e) {
			auto* bsLight = e.get();
			auto* niLight = bsLight ? bsLight->light.get() : nullptr;
			if (!niLight)
				return;
			LightLimitFix::LightData d{};
			LightLimitFix::FillLightData(bsLight, niLight, d);
			const RE::NiPoint3 lp = niLight->world.translate;
			float3 toL = { lp.x - pos.x, lp.y - pos.y, lp.z - pos.z };
			const float dist = std::sqrt(toL.x * toL.x + toL.y * toL.y + toL.z * toL.z);
			if (dist >= d.radius || d.radius <= 0.f)
				return;
			float att;
			if (globals::features::inverseSquareLighting.loaded) {
				const bool enabled = !d.lightFlags.any(LightLimitFix::LightFlags::Disabled);
				const bool invSqFlag = d.lightFlags.any(LightLimitFix::LightFlags::InverseSquare);
				constexpr float kScaledUnitsSq = 0.8f * 70.f * 70.f;
				float invSq = kScaledUnitsSq / (dist * dist + d.sizeBias);
				const float tt = std::clamp((d.radius - dist) * d.fadeZone, 0.f, 1.f);
				invSq *= tt * tt * (3.f - 2.f * tt);
				const float fi = std::clamp(dist * d.invRadius, 0.f, 1.f);
				const float reg = 1.f - fi * fi;
				const DirectX::PackedVector::HALF hx = static_cast<DirectX::PackedVector::HALF>(d.falloffBlend >> 16);
				const DirectX::PackedVector::HALF hy = static_cast<DirectX::PackedVector::HALF>(d.falloffBlend & 0xFFFF);
				const float fx = DirectX::PackedVector::XMConvertHalfToFloat(hx), fy = DirectX::PackedVector::XMConvertHalfToFloat(hy);
				att = enabled ? ((invSqFlag ? invSq : reg) + (fy * invSq - fx * reg)) : 0.f;
			} else {
				const float fi = std::clamp(dist / d.radius, 0.f, 1.f);
				att = 1.f - fi * fi;
			}
			if (att <= 1e-5f)
				return;
			const bool linear = d.lightFlags.any(LightLimitFix::LightFlags::Linear);
			float3 c = d.color;
			if (ll.enableLinearLighting && !linear) {
				auto conv = [&](float v) { return SafePow(std::max(v, 0.f), ll.lightGamma) * ll.lightMult * ll.pointLightMult; };
				c = { conv(c.x), conv(c.y), conv(c.z) };
			}
			const float nl = dist > 1e-3f ? std::max((n.x * toL.x + n.y * toL.y + n.z * toL.z) / dist, 0.f) : 1.f;
			sum += Lum(c) * att * d.fade * nl;
			++count;
		};
		if (auto* ssn = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr) {
			for (auto& e : ssn->GetRuntimeData().activeLights)
				visit(e);
			for (auto& e : ssn->GetRuntimeData().activeShadowLights)
				visit(e);
		}
		r.eLamps = kPi * vdm * sum * kR0;
		r.lampCount = count;
	}

	r.eSplit = r.eDirectional + r.eAmbient + r.eLamps;
	r.residual = r.eMeasured - r.eSplit;
	r.emissive = std::max(r.luminance - Lum(r.rawAlbedo) * r.eSplit / kPi, 0.f);
	r.ev100Reflected = r.luminance > 0.f ? std::log2(r.luminance * 100.f / 12.5f) : -99.f;
	r.ev100Incident = r.eMeasured > 0.f ? std::log2(r.eMeasured / 2.5f) : -99.f;

	// The eye's levels (adaptation buffer, S-free) in cd/m2.
	if (aeLastEye) {
		r.aeDivisor = aeLastRead[0] * kR0;
		r.aeAdapted = aeLastRead[1] * kR0;
		r.aeTarget = aeLastRead[2] * kR0;
	} else {
		r.aeAdapted = aeLastRead[0] * kR0;
		r.aeDivisor = r.aeAdapted;
		r.aeTarget = 0.f;
	}
	if (r.aeTarget > 0.f && r.aeAdapted > 0.f) {
		const float d = std::log2(r.aeTarget / r.aeAdapted);
		r.adaptation = d > 0.1f ? "adapting brighter" : (d < -0.1f ? "adapting darker" : "steady");
	} else {
		r.adaptation = "steady (Classic: no target stored)";
	}
	FillContext(r);
	current = std::move(r);
}

nlohmann::json PhysicalLighting::ReadingJson(const Reading& r)
{
	auto v3 = [](const float3& v) { return nlohmann::json::array({ v.x, v.y, v.z }); };
	return {
		{ "valid", r.valid },
		{ "frame", r.frame },
		{ "location", r.location },
		{ "cell", r.cell },
		{ "weather_form_id", std::format("{:08X}", r.weather) },
		{ "game_time", r.time },
		{ "sun_elevation_deg", r.sunElevation },
		{ "masser_elevation_deg", r.masserElevation },
		{ "secunda_elevation_deg", r.secundaElevation },
		{ "masser_phase_lit_pct", r.moonPhase },
		{ "S", r.S },
		{ "P", r.P },
		{ "R0_cd_per_unit", kR0 },
		{ "lux", { { "measured", r.eMeasured }, { "split_sum", r.eSplit }, { "directional", r.eDirectional }, { "directional_is_sun", r.directionalIsSun },
					 { "ambient", r.eAmbient }, { "dalc", r.eDalc }, { "ibl_and_rest", r.eIbl }, { "lamps", r.eLamps }, { "lamp_count", r.lampCount },
					 { "residual", r.residual }, { "residual_pct", r.eMeasured > 0.f ? 100.f * r.residual / r.eMeasured : 0.f } } },
		{ "luminance_cd_m2", r.luminance },
		{ "emissive_estimate_cd_m2", r.emissive },
		{ "ev100_reflected", r.ev100Reflected },
		{ "ev100_incident", r.ev100Incident },
		{ "eye", { { "adapted_cd_m2", r.aeAdapted }, { "target_cd_m2", r.aeTarget }, { "divisor_cd_m2", r.aeDivisor }, { "state", r.adaptation } } },
		{ "raw", { { "lit", v3(r.rawLit) }, { "albedo", v3(r.rawAlbedo) }, { "specular", v3(r.rawSpecular) }, { "normal_ws", v3(r.normalWS) },
					 { "masks_ambient_y", r.rawMasksAmbientY }, { "shadow", r.rawShadow }, { "depth", r.depth }, { "distance_units", r.distance } } },
	};
}

nlohmann::json PhysicalLighting::MeterJson() const
{
	nlohmann::json j;
	j["master"] = settings.Enabled;
	j["active"] = frame.active;
	j["reason"] = frame.reason;
	j["S"] = frame.S;
	j["P"] = frame.P;
	j["P_target"] = frame.pTarget;
	j["scene_wobble"] = sceneWobble;
	j["class_wobble"] = classWobbleOn ? ClassName(static_cast<Class>(classWobbleIndex)) : "off";
	j["meter_on"] = meterOn;
	j["current"] = ReadingJson(current);
	auto arr = nlohmann::json::array();
	for (const auto& r : captured)
		arr.push_back(ReadingJson(r));
	j["captured"] = std::move(arr);
	auto reg = nlohmann::json::object();
	for (uint32_t i = 0; i < kClassCount; ++i)
		reg[ClassName(static_cast<Class>(i))] = { { "factor", registry[i].lastFactor }, { "applied_recently", registry[i].lastFrame != UINT32_MAX && registry[i].lastFrame + 2 >= frame.frame } };
	j["registry"] = std::move(reg);
	return j;
}

std::string PhysicalLighting::DescribeForTrace() const
{
	std::string s = std::format("physical lighting {} (S {:.4g}, P {:.4g}{})", settings.Enabled ? (frame.active ? "on" : "on, inactive") : "off", frame.S, frame.P,
		frame.reason.empty() ? "" : ", " + frame.reason);
	if (current.valid)
		s += std::format("; meter {:.4g} lx measured (sun/moon {:.4g}, ambient {:.4g}, lamps {:.4g}), {:.4g} cd/m2, EV100 {:.2f}, at {} / {} {}", current.eMeasured,
			current.eDirectional, current.eAmbient, current.eLamps, current.luminance, current.ev100Reflected, current.location, current.cell, current.time);
	return s;
}

// ------------------------------------------------------------------------------------------------
// UI
// ------------------------------------------------------------------------------------------------

void PhysicalLighting::DrawSettings()
{
	const std::string reason = PrerequisiteReason();
	ImGui::BeginDisabled(!reason.empty());
	ImGui::Checkbox("Physical Lighting", &settings.Enabled);
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Master switch (default off). Package A (52a): every light goes through one conversion factor per light class and the frame\n"
			"buffer is pre-exposed (multiplied by about the previous frame's exposure, at most 1 EV per second); the eye divides it back out.\n"
			"The picture should look exactly the same as with the switch off. Needs Linear Lighting and CS auto exposure; never changes them.");
	}
	if (!reason.empty())
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Unavailable: %s", reason.c_str());
	{
		const char* state = !settings.Enabled ? "off" : (frame.active ? "active" : (frame.menuExcluded ? "on (paused in this menu: S = 1)" : "on, inactive"));
		const float ev = frame.pTarget > 0.f ? std::log2(frame.pTarget) : 0.f;
		ImGui::Text("Status: %s | pre-exposure P %.3f (%+.2f EV) | applied S %.3f | eye exposure %+.2f EV from the reference | %s", state, frame.P,
			std::log2(std::max(frame.P, 1e-6f)), frame.S, ev, current.valid ? current.adaptation.c_str() : "meter off");
		// The eye's adapted level as EV100 (provisional R0; the adaptation buffer is read back while the master or the meter is on).
		const float adapted = (aeLastEye ? aeLastRead[1] : aeLastRead[0]) * kR0;
		if ((settings.Enabled || meterOn) && adapted > 0.f)
			ImGui::Text("Eye: adapted to EV100 %.2f (%.4g cd/m2, provisional units)", std::log2(adapted * 100.f / 12.5f), adapted);
		if (current.valid)
			ImGui::Text("Meter: EV100 %.2f at the crosshair, %.4g cd/m2", current.ev100Reflected, current.luminance);
	}

	ImGui::Spacing();
	if (ImGui::TreeNodeEx("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::TreeNodeEx("Unit Registry", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Every light class, its calibration constant (1 = the 50e value in 52a), its factor this frame and whether it went\n"
							"through the conversion this frame (a class that never shows a tick has no carrier in this scene).");
			}
			if (ImGui::BeginTable("##registry", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("Class");
				ImGui::TableSetupColumn("Constant");
				ImGui::TableSetupColumn("Factor");
				ImGui::TableSetupColumn("Applied");
				ImGui::TableHeadersRow();
				for (uint32_t i = 0; i < kClassCount; ++i) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(ClassName(static_cast<Class>(i)));
					ImGui::TableNextColumn();
					ImGui::TextUnformatted("1 (identity)");
					ImGui::TableNextColumn();
					ImGui::Text("%.4f", registry[i].lastFactor);
					ImGui::TableNextColumn();
					const bool applied = registry[i].lastFrame + 2 >= frame.frame && registry[i].lastFrame != UINT32_MAX;
					ImGui::TextUnformatted(applied ? "yes" : "no");
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Debug", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Light Meter", &meterOn);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("A crosshair and a reading of the spot it points at: how much light reaches it (lux, split by source), how bright it\n"
							"looks (cd/m2, EV100) and the eye's state. Works with the master on or off. Values use one provisional constant\n"
							"(%.0f cd/m2 per buffer unit), so read them relative to each other. Not saved.",
					kR0);
			}
			if (meterOn) {
				ImGui::SameLine();
				if (ImGui::Button("Capture Reading") && current.valid) {
					captured.push_back(current);
					while (captured.size() > kMaxCaptured)
						captured.pop_front();
				}
				ImGui::SameLine();
				ImGui::Text("%d captured (written into every F12 save)", static_cast<int>(captured.size()));
				if (ImGui::Button("Clear Captured"))
					captured.clear();
				if (ImGui::TreeNode("Reference Values")) {
					if (ImGui::BeginTable("##refs", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
						for (const auto& ref : kReferences) {
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(ref.what);
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(ref.value);
						}
						ImGui::EndTable();
					}
					ImGui::TreePop();
				}
			}

			ImGui::BeginDisabled(!settings.Enabled || !frame.prerequisites);
			ImGui::Checkbox("Scene Scale Wobble", &sceneWobble);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Self-test (needs the master on): every light gets x 2^sin(2 pi t / 4 s) (+-1 EV over 4 seconds) and the eye divides\n"
							"exactly the same factor out. The picture must stay perfectly still. Anything that pulses missed its conversion;\n"
							"trails mean a history (reflection probe, temporal filter) was not rescaled. Not saved.");
			}
			ImGui::Checkbox("Class Wobble", &classWobbleOn);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Self-test (needs the master on): only the light class chosen below pulses (+-1 EV over 4 seconds); the eye does not\n"
							"divide it out. Exactly what that class lights should pulse, nothing else. Not saved.");
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(220.f);
			if (ImGui::BeginCombo("##classWobble", ClassName(static_cast<Class>(classWobbleIndex)))) {
				for (uint32_t i = 0; i < kClassCount; ++i) {
					const bool selected = static_cast<uint32_t>(classWobbleIndex) == i;
					if (ImGui::Selectable(ClassName(static_cast<Class>(i)), selected))
						classWobbleIndex = static_cast<int>(i);
				}
				ImGui::EndCombo();
			}
			ImGui::EndDisabled();
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
}

void PhysicalLighting::DrawOverlay()
{
	if (!meterOn || !loaded)
		return;
	auto* ui = globals::game::ui;
	if (ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)))
		return;

	// Crosshair mark at the screen centre.
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const ImVec2 c = { size.x * 0.5f, size.y * 0.5f };
	auto* fg = ImGui::GetForegroundDrawList();
	const ImU32 col = IM_COL32(255, 220, 64, 230);
	fg->AddLine({ c.x - 14.f, c.y }, { c.x - 4.f, c.y }, col, 2.f);
	fg->AddLine({ c.x + 4.f, c.y }, { c.x + 14.f, c.y }, col, 2.f);
	fg->AddLine({ c.x, c.y - 14.f }, { c.x, c.y - 4.f }, col, 2.f);
	fg->AddLine({ c.x, c.y + 4.f }, { c.x, c.y + 14.f }, col, 2.f);

	ImGui::SetNextWindowPos({ c.x + 40.f, c.y + 40.f }, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowBgAlpha(0.75f);
	if (ImGui::Begin("Physical Lighting: Light Meter", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
		if (!current.valid) {
			ImGui::TextUnformatted(frame.menuExcluded ? "Paused in this menu" : "Waiting for a reading...");
		} else {
			const auto& r = current;
			const float pct = r.eMeasured > 0.f ? 100.f * r.residual / r.eMeasured : 0.f;
			ImGui::Text("Illuminance (measured) %.4g lx   EV100 incident %.2f", r.eMeasured, r.ev100Incident);
			ImGui::Text("  %s %.4g lx | ambient %.4g lx (DALC %.4g, IBL and rest %.4g) | lamps %.4g lx (%d)", r.directionalIsSun ? "sun" : "moon", r.eDirectional,
				r.eAmbient, r.eDalc, r.eIbl, r.eLamps, r.lampCount);
			if (std::abs(pct) > 10.f)
				ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "  residual %.4g lx (%+.0f %%): an unlisted light source here", r.residual, pct);
			else
				ImGui::Text("  residual %.4g lx (%+.0f %%)", r.residual, pct);
			ImGui::Text("Luminance %.4g cd/m2   EV100 %.2f   emissive estimate %.4g cd/m2", r.luminance, r.ev100Reflected, r.emissive);
			ImGui::Text("Eye: adapted %.4g cd/m2, target %.4g cd/m2, %s", r.aeAdapted, r.aeTarget, r.adaptation.c_str());
			ImGui::Text("P %.3f  S %.3f  distance %.0f units  shadow %.2f", r.P, r.S, r.distance, r.rawShadow);
			ImGui::Text("%s / %s  %s  weather %08X", r.location.c_str(), r.cell.c_str(), r.time.c_str(), r.weather);
			ImGui::TextDisabled("Raw: lit %.4g  albedo %.3f  ambient Y %.4g  (x S, provisional R0 %.0f cd/m2 per unit)", Lum(r.rawLit), Lum(r.rawAlbedo),
				r.rawMasksAmbientY, kR0);
			ImGui::TextDisabled("Captured %d (F11 > Sky > Physical Lighting > Physics > Debug > Capture Reading)", static_cast<int>(captured.size()));
		}
	}
	ImGui::End();
}
