#include "HistogramAutoExposure.h"

#include "State.h"
#include "Util.h"
#include "Utils/GpuTimers.h"

#include "Features/PostProcessing.h"
#include "Features/PhysicalSky.h"  // (51a) the natural light's perceptual factor and the mesopic conversion

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HistogramAutoExposure::Settings,
	ExposureCompensation,
	EnableToD,
	ToDExposure,
	EnableInterior,
	InteriorExposure,
	AdaptationRange,
	ToDAdaptationRange,
	InteriorAdaptationRange,
	AdaptArea,
	AdaptSpeed,
	PurkinjeStartEV,
	PurkinjeMaxEV,
	PurkinjeStrength,
	EyeModel,
	EyeNightDarkness,
	EyeMinVisibility,
	EyeMesopic,
	EyeDarkAdaptSlower,
	EyeMaxChangeEVps,
	EyeIgnoreSmallEV,
	EyeKeepAdaptingAfterLoad)

void HistogramAutoExposure::DrawSettings()
{
	// (50a) Eye Model (physical lighting redo, stage 1). Its own section first; the Classic controls below stay in effect (the eye reads
	// Exposure Compensation, Adaptation Range, Adaptation Speed, Focus Area and the Purkinje settings).
	ImGui::SeparatorText("Eye Model (new)");
	ImGui::Checkbox("Eye Model", &settings.EyeModel);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("A new eye for the auto exposure (first step of the physical lighting redo: only the eye and the screen change, no light).");
		ImGui::Text("  On with the settings below at their defaults: the picture should look the same as with it off.");
		ImGui::Text("  It measures the scene more robustly (ignores the darkest 5%% and very bright spots), can lift dark scenes (Night");
		ImGui::Text("  Darkness), keep a minimum visibility, show mesopic (night) colour vision, and adapts at once after a loading screen.");
		ImGui::Text("  It uses your Exposure Compensation, Adaptation Range, Adaptation Speed, Focus Area and Purkinje settings below.");
		ImGui::Text("Off (default): the auto exposure exactly as before.");
	}
	if (settings.EyeModel && !EyeActive())
		ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "The Eye Model's shaders did not compile: the classic auto exposure is in effect (see the log).");
	ImGui::BeginDisabled(!settings.EyeModel);
	ImGui::Indent();
	{
		float pct = settings.EyeNightDarkness * 100.f;
		if (ImGui::SliderFloat("Night Darkness", &pct, 0.f, 100.f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
			settings.EyeNightDarkness = pct / 100.f;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How much of a dark scene's own darkness is kept, below the darkest level the auto exposure adapts to (Adaptation");
			ImGui::Text("  Range minimum). 100 %% (default): all of it, as before (nights as dark as now). 60 %%: 40 %% of the missing brightness is");
			ImGui::Text("  made up (a night 5 stops below the minimum is lifted by 2 stops). 0 %%: fully adapted, nights like day.");
		}
		pct = settings.EyeMinVisibility * 100.f;
		if (ImGui::SliderFloat("Minimum Visibility", &pct, 0.f, 100.f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
			settings.EyeMinVisibility = pct / 100.f;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("A soft floor for dark areas, as a share of middle grey's brightness: 25 %% lifts an area that would be 6 stops below");
			ImGui::Text("  middle grey to about 2 stops below; brighter areas barely change. With Local Exposure on it acts on each area");
			ImGui::Text("  (Local Exposure's base), otherwise on the whole picture. 0 %% (default): off. May lift a black night sky a little.");
		}
		pct = settings.EyeMesopic * 100.f;
		if (ImGui::SliderFloat("Mesopic Vision", &pct, 0.f, 100.f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
			settings.EyeMesopic = pct / 100.f;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Night colour vision per pixel: very dark pixels lose colour and turn slightly blue-grey, by how dark they really are.");
			ImGui::Text("  Above 0 it replaces the Purkinje effect below. 0 %% (default): off, the Purkinje settings apply as before.");
			ImGui::Text("  (This step has no real-world brightness yet: it mostly reaches the darkest shadows. Later steps make it follow the night.)");
		}
	}
	if (ImGui::TreeNodeEx("Eye Adaptation", ImGuiTreeNodeFlags_None)) {
		ImGui::SliderFloat("Dark Adaptation Slower By", &settings.EyeDarkAdaptSlower, 1.f, 20.f, "x %.1f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Adapting to darkness takes this many times longer than to brightness (Adaptation Speed sets the latter). x 1 (default): the same.");
		ImGui::SliderFloat("Max Exposure Change", &settings.EyeMaxChangeEVps, 0.f, 8.f, settings.EyeMaxChangeEVps > 0.f ? "%.1f stops / s" : "no limit",
			ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("The adapted level never changes faster than this. 0 (default): no limit.");
		ImGui::SliderFloat("Ignore Small Changes", &settings.EyeIgnoreSmallEV, 0.f, 0.5f, settings.EyeIgnoreSmallEV > 0.f ? "%.2f stops" : "off", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Brightness changes smaller than this do not move the exposure (no breathing when a torch flickers). 0 (default): off.");
		ImGui::Checkbox("Keep Adapting After Load", &settings.EyeKeepAdaptingAfterLoad);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Off (default): after a loading screen, waiting or sleeping, the eye is adapted to the new place at once.");
			ImGui::Text("  On: it adapts gradually from where it was (as the classic auto exposure always did).");
		}
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("Developer", ImGuiTreeNodeFlags_None)) {
		if (ImGui::Checkbox("Scene Scale Wobble", &eyeWobble))
			eyeWobbleStart = static_cast<double>(GetTickCount64()) * 1e-3;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Self-test (not saved): the measured frame is scaled by 2^sin(2 pi t / 4 s) (up to 2x brighter / darker, every 4 s) before");
			ImGui::Text("  the eye measures it, with adaptation instant. The picture must stay perfectly still: anything that pulses means the");
			ImGui::Text("  eye's measurement is not scale-invariant. Needs Eye Model on.");
		}
		ImGui::TreePop();
	}
	ImGui::Unindent();
	ImGui::EndDisabled();
	ImGui::SeparatorText("Auto Exposure");

	if (globals::game::isVR) {
		settings.EnableToD = false;
	} else {
		ImGui::Checkbox("ToD Exposure Compensation", &settings.EnableToD);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Applying exposure adjustments based on the time of day.");
	}
	if (settings.EnableToD) {
		if (ImGui::TreeNodeEx("ToD Exposure Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Dawn", &settings.ToDExposure[0], -5.f, 5.f, "%+.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra brightness at this time of day, in EV (+1 = twice as bright, -1 = half).");
			ImGui::SliderFloat("Sunrise", &settings.ToDExposure[1], -5.f, 5.f, "%+.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra brightness at this time of day, in EV (+1 = twice as bright, -1 = half).");
			ImGui::SliderFloat("Day", &settings.ToDExposure[2], -5.f, 5.f, "%+.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra brightness at this time of day, in EV (+1 = twice as bright, -1 = half).");
			ImGui::SliderFloat("Sunset", &settings.ToDExposure[3], -5.f, 5.f, "%+.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra brightness at this time of day, in EV (+1 = twice as bright, -1 = half).");
			ImGui::SliderFloat("Dusk", &settings.ToDExposure[4], -5.f, 5.f, "%+.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra brightness at this time of day, in EV (+1 = twice as bright, -1 = half).");
			ImGui::SliderFloat("Night", &settings.ToDExposure[5], -5.f, 5.f, "%+.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra brightness at this time of day, in EV (+1 = twice as bright, -1 = half).");
			ImGui::TreePop();
		}
	} else {
		ImGui::SliderFloat("Exposure Compensation", &settings.ExposureCompensation, -5.f, 5.f, "%+.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Applying additional exposure adjustment to the image.");
	}
	ImGui::Checkbox("Interior Exposure Compensation", &settings.EnableInterior);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Applying exposure adjustments separately to interiors.");
	if (settings.EnableInterior) {
		ImGui::SliderFloat("Interior", &settings.InteriorExposure, -5.f, 5.f, "%+.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Extra brightness indoors, in EV (+1 = twice as bright, -1 = half).");
	}

	ImGui::SliderFloat("Adaptation Speed", &settings.AdaptSpeed, 0.1f, 5.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How fast the eye adjusts when moving between bright and dark areas. Higher = faster.");
	ImGui::SliderFloat2("Focus Area", &settings.AdaptArea.x, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Specifies the proportion of the area [width, height] that auto exposure will adapt to.");
	if (settings.EnableToD) {
		if (ImGui::TreeNodeEx("ToD Adaptation Range Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat2("Dawn Adaptation Range", &settings.ToDAdaptationRange[0].x, -6.f, 21.f, "%.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("[Min, Max] brightness range auto exposure adapts within at this time of day. See Adaptation Range.");
			ImGui::SliderFloat2("Sunrise Adaptation Range", &settings.ToDAdaptationRange[1].x, -6.f, 21.f, "%.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("[Min, Max] brightness range auto exposure adapts within at this time of day. See Adaptation Range.");
			ImGui::SliderFloat2("Day Adaptation Range", &settings.ToDAdaptationRange[2].x, -6.f, 21.f, "%.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("[Min, Max] brightness range auto exposure adapts within at this time of day. See Adaptation Range.");
			ImGui::SliderFloat2("Sunset Adaptation Range", &settings.ToDAdaptationRange[3].x, -6.f, 21.f, "%.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("[Min, Max] brightness range auto exposure adapts within at this time of day. See Adaptation Range.");
			ImGui::SliderFloat2("Dusk Adaptation Range", &settings.ToDAdaptationRange[4].x, -6.f, 21.f, "%.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("[Min, Max] brightness range auto exposure adapts within at this time of day. See Adaptation Range.");
			ImGui::SliderFloat2("Night Adaptation Range", &settings.ToDAdaptationRange[5].x, -6.f, 21.f, "%.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("[Min, Max] brightness range auto exposure adapts within at this time of day. See Adaptation Range.");
			ImGui::TreePop();
		}
	} else {
		ImGui::SliderFloat2("Adaptation Range", &settings.AdaptationRange.x, -6.f, 21.f, "%.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"[Min, Max] The average scene luminance will be clamped between them when doing auto exposure."
				"Turning up the minimum, for example, makes it adapt less to darkness and therefore prevents over-brightening of dark scenes.");
	}
	if (settings.EnableInterior) {
		ImGui::SliderFloat2("Interior Adaptation Range", &settings.InteriorAdaptationRange.x, -6.f, 21.f, "%.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"[Min, Max] The average scene luminance will be clamped between them when doing auto exposure for interiors."
				"Turning up the minimum, for example, makes it adapt less to darkness and therefore prevents over-brightening of dark scenes.");
	}
	if (ImGui::TreeNodeEx("Purkinje Effect", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped(
			"The Purkinje effect simulates the blue shift of human vision under low light.\n"
			"If you don't like the effect, you can set the strength to zero.");

		ImGui::SliderFloat("Max Strength", &settings.PurkinjeStrength, 0.f, 5.f, "%.2f");
		ImGui::SliderFloat("Fade In EV", &settings.PurkinjeStartEV, -6.f, 21.f, "%.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("The Purkinje effect will start to take place when the average scene luminance falls lower than this.");
		ImGui::SliderFloat("Max Effect EV", &settings.PurkinjeMaxEV, -6.f, 21.f, "%.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("From this point onward, the Purkinje effect remains the greatest.");

		ImGui::TreePop();
	}
}

void HistogramAutoExposure::RestoreDefaultSettings()
{
	settings = {};
}

void HistogramAutoExposure::LoadSettings(json& o_json)
{
	settings = o_json;
}

void HistogramAutoExposure::SaveSettings(json& o_json)
{
	o_json = settings;
}

void HistogramAutoExposure::SetupResources()
{
	auto renderer = globals::game::renderer;

	logger::debug("Creating buffers...");
	{
		autoExposureCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<AutoExposureCB>());

		histogramSB = std::make_unique<StructuredBuffer>(StructuredBufferDesc<uint>(256u, false), 256);
		histogramSB->CreateUAV();

		// (50a) 4 floats: [0] as before (the divisor); [1] - [3] the Eye Model's adapted level, measured level, applied lift.
		adaptationSB = std::make_unique<StructuredBuffer>(StructuredBufferDesc<float>(4u, false), 4);
		adaptationSB->CreateSRV();
		adaptationSB->CreateUAV();
	}

	logger::debug("Creating 2D textures...");
	{
		// texAdapt for adaptation
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texAdapt = std::make_unique<Texture2D>(texDesc);
		texAdapt->CreateSRV(srvDesc);
		texAdapt->CreateUAV(uavDesc);
	}

}

// (batch 16, item P14) Shader compilation is no longer part of SetupResources, so that
// the memory SetupResources allocates can be handed back while the effect is off
// without paying a D3DCompileFromFile on every re-enable.
void HistogramAutoExposure::SetupShaders()
{
	CompileComputeShaders();
}

// (batch 16, item P14) Mirror of SetupResources: every texture/buffer it assigns is
// nulled here. Shaders and sampler states are deliberately kept -- they are a few
// hundred bytes and keeping them is what makes turning the effect back on cheap.
void HistogramAutoExposure::ReleaseResources()
{
	autoExposureCB = nullptr;
	histogramSB = nullptr;
	adaptationSB = nullptr;
	texAdapt = nullptr;
}

void HistogramAutoExposure::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&histogramCS, &histogramAvgCS, &adaptCS, &histogramEyeCS, &histogramAvgEyeCS, &adaptEyeCS  // (50a) + the Eye Model variants
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void HistogramAutoExposure::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &histogramCS, "histogram.cs.hlsl", {}, "CS_Histogram" },
			{ &histogramAvgCS, "histogram.cs.hlsl", {}, "CS_Average" },
			{ &adaptCS, "adapt.cs.hlsl", {} },
			// (50a) Eye Model variants (EYE_MODE); the classic ones above compile from the same text as before.
			{ &histogramEyeCS, "histogram.cs.hlsl", { { "EYE_MODE", "" } }, "CS_Histogram" },
			{ &histogramAvgEyeCS, "histogram.cs.hlsl", { { "EYE_MODE", "" } }, "CS_Average" },
			{ &adaptEyeCS, "adapt.cs.hlsl", { { "EYE_MODE", "" } } },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\HistogramAutoExposure") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

void HistogramAutoExposure::GetExposureParameters(float& exposureCompensation, float2& adaptationRange) const
{
	GetUserExposureParameters(exposureCompensation, adaptationRange);
	// (50a) Eye Model: the adaptation buffer's [0] is the eye's divisor itself; consumers that clamp it with this range (Neural Rendering's
	// tone curve) get a pass-through range, so they apply the eye's exposure.
	if (EyeActive())
		adaptationRange = { -kEyePassThroughEV, kEyePassThroughEV };
}

void HistogramAutoExposure::GetUserExposureParameters(float& exposureCompensation, float2& adaptationRange) const
{
	auto& pp = globals::features::postProcessing;
	exposureCompensation = settings.ExposureCompensation;
	adaptationRange = settings.AdaptationRange;
	if (pp.imageSpaceManager != nullptr) {
		if (settings.EnableToD) {
			exposureCompensation = pp.imageSpaceManager->timeOfDay[0] * settings.ToDExposure[0] +
				pp.imageSpaceManager->timeOfDay[1] * settings.ToDExposure[1] +
				pp.imageSpaceManager->timeOfDay[2] * settings.ToDExposure[2] +
				pp.imageSpaceManager->timeOfDay[3] * settings.ToDExposure[3] +
				pp.imageSpaceManager->timeOfDay[4] * settings.ToDExposure[4] +
				pp.imageSpaceManager->timeOfDay[5] * settings.ToDExposure[5];
			adaptationRange = pp.imageSpaceManager->timeOfDay[0] * settings.ToDAdaptationRange[0] +
				pp.imageSpaceManager->timeOfDay[1] * settings.ToDAdaptationRange[1] +
				pp.imageSpaceManager->timeOfDay[2] * settings.ToDAdaptationRange[2] +
				pp.imageSpaceManager->timeOfDay[3] * settings.ToDAdaptationRange[3] +
				pp.imageSpaceManager->timeOfDay[4] * settings.ToDAdaptationRange[4] +
				pp.imageSpaceManager->timeOfDay[5] * settings.ToDAdaptationRange[5];
		}
		if (settings.EnableInterior && pp.imageSpaceManager->inInterior) {
			exposureCompensation = settings.InteriorExposure;
			adaptationRange = settings.InteriorAdaptationRange;
		}
	}
}

void HistogramAutoExposure::Draw(TextureInfo& inout_tex)
{
	auto context = globals::d3d::context;
	auto state = globals::state;

	float exposureCompensation = 0.f;
	float2 adaptationRange = {};
	GetUserExposureParameters(exposureCompensation, adaptationRange);

	AutoExposureCB cbData = {
		.AdaptArea = settings.AdaptArea,
		.AdaptationRange = { exp2(adaptationRange.x) * 0.125f, exp2(adaptationRange.y) * 0.125f },
		.AdaptLerp = std::clamp(1.f - exp(-RE::BSTimer::GetSingleton()->realTimeDelta * settings.AdaptSpeed), 0.f, 1.f),
		.ExposureCompensation = exp2(exposureCompensation),
		.PurkinjeStartEV = settings.PurkinjeStartEV,
		.PurkinjeMaxEV = settings.PurkinjeMaxEV,
		.PurkinjeStrength = settings.PurkinjeStrength,
	};
	if (freezeAdaptation)
		cbData.AdaptLerp = 0.f;  // (50b) Sky Light Wobble: the classic adaptation holds its level

	// (50a) Eye Model. Snap after a loading screen, a time skip (wait, sleep, fast travel: the game clock jumped over 15 min in one frame) or
	// when the mode was switched on, unless Keep Adapting After Load. The classic path and its constants are unchanged.
	const bool eye = EyeActive();
	{
		bool loading = false;
		if (auto* ui = RE::UI::GetSingleton())
			loading = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		float hours = -1.0f;
		if (auto* calendar = RE::Calendar::GetSingleton())
			hours = calendar->GetCurrentGameTime() * 24.0f;
		const bool skipped = eyeLastGameHours >= 0.0f && hours >= 0.0f && std::abs(hours - eyeLastGameHours) > 0.25f;
		eyeLastGameHours = hours;
		if (loading || skipped)
			eyeSnapFrames = 3;  // while the loading screen is up and the first 3 frames after it / after the skip
	}
	const bool eyeSwitchedOn = eye && !eyeWasActive;  // the eye's state is stale: always start from the measured level
	eyeWasActive = eye;
	if (eye) {
		auto& pp = globals::features::postProcessing;
		const auto& lePipe = pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::LocalExposure)];
		const bool localExposure = lePipe && lePipe->enabled && lePipe->RuntimeGateOpen();
		const float dt = std::clamp(RE::BSTimer::GetSingleton()->realTimeDelta, 0.0f, 0.25f);
		const float tauBright = 1.0f / std::max(settings.AdaptSpeed, 1e-3f);
		float wobble = 1.0f;
		if (eyeWobble) {
			const double t = static_cast<double>(GetTickCount64()) * 1e-3 - eyeWobbleStart;
			wobble = static_cast<float>(std::exp2(std::sin(2.0 * 3.14159265358979 * t / 4.0)));
		}
		// The consumers' range: pass-through (buffer [0] = the eye's divisor); the user's range goes to the eye itself.
		cbData.AdaptationRange = { exp2(-kEyePassThroughEV) * 0.125f, exp2(kEyePassThroughEV) * 0.125f };
		cbData.EyeLogLo = adaptationRange.x + std::log2(0.125f);
		cbData.EyeLogHi = std::max(adaptationRange.y, adaptationRange.x) + std::log2(0.125f);
		cbData.EyeNightDarkness = std::clamp(settings.EyeNightDarkness, 0.0f, 1.0f);
		cbData.EyeMinVisibility = std::clamp(settings.EyeMinVisibility, 0.0f, 1.0f);
		cbData.EyeTauBright = tauBright;
		cbData.EyeTauDark = tauBright * std::clamp(settings.EyeDarkAdaptSlower, 1.0f, 20.0f);
		cbData.EyeMaxRate = std::max(settings.EyeMaxChangeEVps, 0.0f);
		cbData.EyeDeadband = std::clamp(settings.EyeIgnoreSmallEV, 0.0f, 0.5f);
		cbData.EyeWobble = wobble;
		cbData.EyeMesopic = std::clamp(settings.EyeMesopic, 0.0f, 1.0f);
		cbData.EyeDt = dt;
		const bool snap = eyeSwitchedOn || (eyeSnapFrames > 0 && !settings.EyeKeepAdaptingAfterLoad && !freezeAdaptation);
		cbData.EyeFlags = (snap ? kEyeSnap : 0u) | (localExposure ? 0u : kEyeGlobalFloor) | (eyeWobble && !freezeAdaptation ? kEyeInstant : 0u);
		if (freezeAdaptation && !eyeSwitchedOn) {
			// (50b) Sky Light Wobble: the eye holds its level (no time passes for it).
			cbData.EyeDt = 0.f;
			cbData.EyeTauBright = cbData.EyeTauDark = 1e9f;
		}
		eyeLocalMinVisibility = localExposure ? cbData.EyeMinVisibility : 0.0f;
	} else {
		eyeLocalMinVisibility = 0.0f;
	}
	eyeSnapFrames = std::max(eyeSnapFrames - 1, 0);
	// (51a) Physical Outdoor Light: its perceptual factor on the natural light follows this auto exposure's speeds (both modes), and the Eye
	// Model's mesopic shift gets the real-luminance conversion of the natural light (cd/m2 per scene unit; the EYE_MODE variants read pad[0],
	// the classic ones never do; 0 = the shader's own constant).
	naturalTauBright = 1.0f / std::max(settings.AdaptSpeed, 1e-3f);
	naturalTauDark = naturalTauBright * std::clamp(settings.EyeDarkAdaptSlower, 1.0f, 20.0f);
	cbData.pad[0] = eye ? globals::features::physicalSky.EyeSceneToNits() : 0.0f;
	autoExposureCB->Update(cbData);
	lastExposureCompensation = cbData.ExposureCompensation;

	std::array<ID3D11ShaderResourceView*, 2> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { nullptr };
	ID3D11Buffer* cb = autoExposureCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	auto* timers = Util::GpuPassTimers::GetSingleton();
	timers->End(Util::GpuBucket::PostProcessing);
	timers->Begin(Util::GpuBucket::PPAutoExposure);

	context->CSSetConstantBuffers(1, 1, &cb);
	state->BeginPerfEvent("Histogram Auto Exposure");

	{
		state->BeginPerfEvent("Calculate Histogram");
		srvs[0] = inout_tex.srv;
		uavs[0] = histogramSB->UAV();
		uavs[1] = adaptationSB->UAV();

		context->CSSetShaderResources(0, (UINT)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (UINT)uavs.size(), uavs.data(), nullptr);

		// Calculate histogram - optimized for 32x32 threads
		// Reduced number of dispatches due to increased thread count and sampling optimization
		context->CSSetShader(eye ? histogramEyeCS.get() : histogramCS.get(), nullptr, 0);  // (50a)
		uint32_t dispatchX = ((texAdapt->desc.Width - 1) >> 5) + 1;
		uint32_t dispatchY = ((texAdapt->desc.Height - 1) >> 5) + 1;

		// Further reduce dispatches based on our sampling pattern.
		// CS_Histogram samples pixel `tid * (2 * SAMPLE_SPACING)` with SAMPLE_SPACING == 8,
		// i.e. a stride of 16 pixels per thread, so the thread grid is the image divided by 16
		// -- not by 8. Dividing by 8 launched twice the threads per axis (4x in total) and the
		// surplus clamped onto the last row/column in the shader. With Focus Area at 1.0 those
		// clamped taps still pass the box test, so each edge pixel was counted many times over
		// and skewed the exposure average toward the screen border; sizing the dispatch to the
		// real stride removes both the wasted threads and the skew.
		dispatchX = (dispatchX + 15) / 16;
		dispatchY = (dispatchY + 15) / 16;

		context->Dispatch(dispatchX, dispatchY, 1);

		// Calculate average
		context->CSSetShader(eye ? histogramAvgEyeCS.get() : histogramAvgCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		state->EndPerfEvent();
	}

	// (44e) Folded: Local Exposure applies the same exposure when it reads the image.
	lastFolded = foldIntoNext;
	if (foldIntoNext) {
		resetViews();
		cb = nullptr;
		context->CSSetConstantBuffers(1, 1, &cb);
		context->CSSetShader(nullptr, nullptr, 0);
		auto& fused = globals::features::postProcessing.fused;
		fused.exposure = true;
		fused.aeCB = autoExposureCB->CB();
		fused.aeAdaptation = adaptationSB->SRV();
		state->EndPerfEvent();
		timers->End(Util::GpuBucket::PPAutoExposure);
		timers->Begin(Util::GpuBucket::PostProcessing);
		return;
	}

	// Adapt
	{
		resetViews();
		state->BeginPerfEvent("Adapt Exposure");

		srvs[0] = inout_tex.srv;
		srvs[1] = adaptationSB->SRV();
		uavs[0] = texAdapt->uav.get();

		context->CSSetShaderResources(0, (UINT)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (UINT)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(eye ? adaptEyeCS.get() : adaptCS.get(), nullptr, 0);

		// Maintain the same number of threads for the adapt shader
		// Since we're not changing the sampling pattern of the adapt shader
		context->Dispatch(((texAdapt->desc.Width - 1) >> 5) + 1, ((texAdapt->desc.Height - 1) >> 5) + 1, 1);
		state->EndPerfEvent();
	}

	// Clean up
	resetViews();
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);

	inout_tex = { texAdapt->resource.get(), texAdapt->srv.get() };
	state->EndPerfEvent();
	timers->End(Util::GpuBucket::PPAutoExposure);
	timers->Begin(Util::GpuBucket::PostProcessing);
}
