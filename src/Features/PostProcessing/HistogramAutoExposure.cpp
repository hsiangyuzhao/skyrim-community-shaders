#include "HistogramAutoExposure.h"

#include "State.h"
#include "Util.h"
#include "Utils/GpuTimers.h"

#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"

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
	ExposureMode,
	PhysDarkCompensation,
	PhysFullAdaptationAbove,
	PhysDarkestAdapted,
	PhysBrightBoost,
	PhysAdaptIntoDark,
	PhysAdaptIntoBright,
	PhysIgnoreDarkest,
	PhysIgnoreBrightest,
	PhysCentreWeight,
	PhysHoldSmallChanges,
	PhysPurkinjeStartEV,
	PhysPurkinjeMaxEV)

bool HistogramAutoExposure::PhysicalMode() const
{
	// (49h stage 2) Auto follows Physical Sky's "Physical Lighting" master (also indoors: the curve applies everywhere).
	if (settings.ExposureMode == kExposurePhysical)
		return true;
	if (settings.ExposureMode == kExposureClassic)
		return false;
	const auto& physicalSky = globals::features::physicalSky;
	return physicalSky.PhysLightingOn(true);
}

float HistogramAutoExposure::LastExposureEV() const
{
	if (!readbackValid || !(readback[0] > 0.f))
		return 0.f;
	return std::log2(0.18f * std::exp2(settings.ExposureCompensation) / readback[0]);
}

std::string HistogramAutoExposure::DescribeForTrace() const
{
	if (!readbackValid)
		return std::format("Auto Exposure {} (no readback yet)", PhysicalMode() ? "Physical" : "Classic");
	const float sceneEV = readback[1] > 0.f ? std::log2(readback[1] / 0.125f) : 0.f;
	const float compensationEV = readback[3] > 0.f ? std::log2(readback[3]) : 0.f;
	return std::format("Auto Exposure {}: scene {:+.2f} EV, compensation {:+.2f} EV, exposure {:+.2f} EV (alpha {:.2f}, darkest {:+.1f} EV)",
		readbackPhysical ? "Physical" : "Classic", sceneEV, compensationEV, LastExposureEV(), settings.PhysDarkCompensation, settings.PhysDarkestAdapted);
}

void HistogramAutoExposure::QueueReadback()
{
	// (49h stage 2) Copy the 16-byte adaptation buffer into a staging ring; read the oldest copy without waiting.
	auto context = globals::d3d::context;
	auto device = globals::d3d::device;
	if (!adaptationSB || !adaptationSB->SRV())
		return;
	const uint32_t slot = readbackIndex % kReadbackCount;
	const uint32_t oldest = (readbackIndex + 1) % kReadbackCount;
	if (readbackBuffers[oldest] && readbackPending[oldest]) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(readbackBuffers[oldest].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) && mapped.pData) {
			float values[4];
			std::memcpy(values, mapped.pData, sizeof(values));
			context->Unmap(readbackBuffers[oldest].get(), 0);
			readbackPending[oldest] = false;
			bool finite = true;
			for (float v : values)
				finite = finite && std::isfinite(v);
			if (finite) {
				std::memcpy(readback, values, sizeof(values));
				readbackValid = true;
				readbackPhysical = lastModePhysical;
			}
		}
	}
	if (!readbackBuffers[slot]) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 4 * sizeof(float);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateBuffer(&desc, nullptr, readbackBuffers[slot].put())))
			return;
	}
	winrt::com_ptr<ID3D11Resource> source;
	adaptationSB->SRV()->GetResource(source.put());
	if (!source)
		return;
	context->CopyResource(readbackBuffers[slot].get(), source.get());
	readbackPending[slot] = true;
	++readbackIndex;
}

void HistogramAutoExposure::DrawSettings()
{
	// (49h stage 2) "Exposure Mode".
	{
		static constexpr const char* kModeNames[kExposureModeCount] = { "Auto (follows Physical Lighting)", "Classic", "Physical" };
		settings.ExposureMode = std::clamp(settings.ExposureMode, 0, kExposureModeCount - 1);
		ImGui::Combo("Exposure Mode", &settings.ExposureMode, kModeNames, kExposureModeCount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Auto (default): Physical while Physical Sky > Scene Lighting > Physical Lighting is on, else Classic.\n"
				"Classic: the controls as before (adaptation range, one speed).\n"
				"Physical: adapts over a wide range (bright noon to a moonless night) and keeps dark scenes darker on screen with\n"
				"one curve (Dark Scene Compensation); slow into the dark, quick into the light; jumps after loads, fast travel and\n"
				"waiting; holds while a menu is open.");
		const bool physical = PhysicalMode();
		if (readbackValid) {
			const float sceneEV = readback[1] > 0.f ? std::log2(readback[1] / 0.125f) : 0.f;
			const float targetEV = readback[2] > 0.f ? std::log2(readback[2] / 0.125f) : 0.f;
			const float compensationEV = readback[3] > 0.f ? std::log2(readback[3]) : 0.f;
			ImGui::TextDisabled("Now (%s): scene %+.2f EV, target %+.2f EV, compensation %+.2f EV, final exposure %+.2f EV",
				readbackPhysical ? "Physical" : "Classic", sceneEV, targetEV, compensationEV, LastExposureEV());
		}
		if (physical) {
			ImGui::SliderFloat("Exposure Compensation", &settings.ExposureCompensation, -5.f, 5.f, "%+.2f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra brightness for every scene, in EV (+1 = twice as bright).");
			ImGui::SliderFloat("Dark Scene Compensation", &settings.PhysDarkCompensation, 0.f, 1.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How much darker than the day a dark scene stays on screen: 0 = every scene as bright as noon, 1 = no brightening\n"
					"below Full Adaptation Above. 0.6 (default): full moon about -2.8 EV, moonless about -4.5 EV, torch-lit room -0.6 EV.");
			ImGui::SliderFloat("Full Adaptation Above", &settings.PhysFullAdaptationAbove, -4.f, 2.f, "%+.1f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Scenes brighter than this are shown at full brightness; darker ones by Dark Scene Compensation.");
			ImGui::SliderFloat("Darkest Adapted Scene", &settings.PhysDarkestAdapted, -10.f, -3.f, "%+.1f EV");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("The eye stops adapting here: darker scenes get darker on screen one to one.");
			ImGui::SliderFloat("Bright Scene Boost", &settings.PhysBrightBoost, 0.f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Scenes brighter than +1.5 EV (sunlit snow, a bright sky) stay a little brighter on screen.");
			ImGui::SliderFloat("Adapt Into Dark", &settings.PhysAdaptIntoDark, 0.2f, 10.f, "%.1f s");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Time to adapt when the scene gets darker (walking into a cave).");
			ImGui::SliderFloat("Adapt Into Bright", &settings.PhysAdaptIntoBright, 0.1f, 5.f, "%.1f s");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Time to adapt when the scene gets brighter (walking out into daylight).");
			ImGui::SliderFloat("Ignore Darkest %", &settings.PhysIgnoreDarkest, 0.f, 40.f, "%.0f%%");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("This share of the darkest screen samples does not count (black shadows, letterbox).");
			ImGui::SliderFloat("Ignore Brightest %", &settings.PhysIgnoreBrightest, 0.f, 40.f, "%.0f%%");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("This share of the brightest screen samples does not count (the sun, fire, highlights).");
			ImGui::SliderFloat("Centre Weight", &settings.PhysCentreWeight, 0.f, 1.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("0 = the whole screen counts the same; 1 = the middle counts three times.");
			ImGui::Checkbox("Hold Small Changes", &settings.PhysHoldSmallChanges);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Changes smaller than about 0.1-0.35 EV are not followed (no pumping when turning the camera).");
			if (ImGui::TreeNodeEx("Purkinje Effect##phys", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat("Max Strength", &settings.PurkinjeStrength, 0.f, 5.f, "%.2f");
				ImGui::SliderFloat("Fade In EV", &settings.PhysPurkinjeStartEV, -12.f, 2.f, "%.2f EV");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("The blue shift starts when the adapted scene is darker than this (Physical mode's own value).");
				ImGui::SliderFloat("Max Effect EV", &settings.PhysPurkinjeMaxEV, -12.f, 2.f, "%.2f EV");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("From this scene brightness down, the blue shift is the strongest.");
				ImGui::TreePop();
			}
			return;
		}
	}
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

		adaptationSB = std::make_unique<StructuredBuffer>(StructuredBufferDesc<float>(4u, false), 4);  // (49h stage 2) four slots (histogram.cs.hlsl)
		snapFrames = 2;
		readbackValid = false;
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
	for (int i = 0; i < kReadbackCount; ++i) {
		readbackBuffers[i] = nullptr;
		readbackPending[i] = false;
	}
	readbackValid = false;
}

void HistogramAutoExposure::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&histogramCS, &histogramAvgCS, &adaptCS
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
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\HistogramAutoExposure") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

void HistogramAutoExposure::GetExposureParameters(float& exposureCompensation, float2& adaptationRange) const
{
	auto& pp = globals::features::postProcessing;
	exposureCompensation = settings.ExposureCompensation;
	adaptationRange = settings.AdaptationRange;
	// (49h stage 2) Physical: the curve is in the divisor itself (histogram.cs.hlsl); no time-of-day / interior overrides and no clamp.
	if (PhysicalMode()) {
		adaptationRange = { -40.f, 40.f };
		return;
	}
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
	GetExposureParameters(exposureCompensation, adaptationRange);

	AutoExposureCB cbData = {
		.AdaptArea = settings.AdaptArea,
		.AdaptationRange = { exp2(adaptationRange.x) * 0.125f, exp2(adaptationRange.y) * 0.125f },
		.AdaptLerp = std::clamp(1.f - exp(-RE::BSTimer::GetSingleton()->realTimeDelta * settings.AdaptSpeed), 0.f, 1.f),
		.ExposureCompensation = exp2(exposureCompensation),
		.PurkinjeStartEV = settings.PurkinjeStartEV,
		.PurkinjeMaxEV = settings.PurkinjeMaxEV,
		.PurkinjeStrength = settings.PurkinjeStrength,
	};
	// (49h stage 2) Physical mode's constants; snaps after loads / time skips / mode switches, holds while the game is paused.
	const bool physical = PhysicalMode();
	{
		auto* ui = RE::UI::GetSingleton();
		if (ui && (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)))
			snapFrames = std::max(snapFrames, 4);
		const auto calendar = RE::Calendar::GetSingleton();
		const float hours = calendar ? calendar->GetHoursPassed() : 0.f;
		if (lastHoursPassed >= 0.f && !(std::abs(hours - lastHoursPassed) < 0.01f))
			snapFrames = std::max(snapFrames, 1);
		lastHoursPassed = hours;
		if (physical != lastModePhysical)
			snapFrames = std::max(snapFrames, 1);
		lastModePhysical = physical;
		const bool snap = snapFrames > 0;
		if (snapFrames > 0)
			--snapFrames;
		const bool hold = ui && ui->GameIsPaused();
		if (physical) {
			const auto& s = settings;
			cbData.PurkinjeStartEV = s.PhysPurkinjeStartEV;
			cbData.PurkinjeMaxEV = s.PhysPurkinjeMaxEV;
			cbData.PhysMinLogLum = kPhysMinLogLum;
			cbData.PhysLogLumRange = kPhysLogLumRange;
			cbData.PhysMode = 1u;
			cbData.PhysDeltaTime = std::clamp(RE::BSTimer::GetSingleton()->realTimeDelta, 0.f, 0.5f);
			cbData.PhysTrimLow = std::clamp(s.PhysIgnoreDarkest, 0.f, 40.f) * 0.01f;
			cbData.PhysTrimHigh = std::clamp(s.PhysIgnoreBrightest, 0.f, 40.f) * 0.01f;
			cbData.PhysCentreWeight = std::clamp(s.PhysCentreWeight, 0.f, 1.f);
			cbData.PhysDeadband = s.PhysHoldSmallChanges ? 1.f : 0.f;
			cbData.PhysTauDark = std::clamp(s.PhysAdaptIntoDark, 0.2f, 10.f);
			cbData.PhysTauBright = std::clamp(s.PhysAdaptIntoBright, 0.1f, 5.f);
			cbData.PhysSlewDark = kPhysSlewDark;
			cbData.PhysSlewBright = kPhysSlewBright;
			cbData.PhysAlpha = std::clamp(s.PhysDarkCompensation, 0.f, 1.f);
			cbData.PhysEVRef = std::clamp(s.PhysFullAdaptationAbove, -4.f, 2.f);
			cbData.PhysEVMin = std::clamp(s.PhysDarkestAdapted, -10.f, -3.f);
			cbData.PhysBeta = std::clamp(s.PhysBrightBoost, 0.f, 0.5f);
			cbData.PhysEVHi = kPhysEVHi;
			cbData.PhysAdaptMinEV = kPhysAdaptMinEV;
			cbData.PhysAdaptMaxEV = kPhysAdaptMaxEV;
			cbData.PhysFlags = (snap ? 1u : 0u) | (hold ? 2u : 0u);
		}
	}
	autoExposureCB->Update(cbData);
	// (49h stage 2) Local Exposure pivots around the middle grey this pass targets: x the compensation curve in Physical mode
	// (read back 2-3 frames late; it changes slowly).
	lastExposureCompensation = cbData.ExposureCompensation * (physical && readbackValid && readbackPhysical && readback[3] > 0.f ? readback[3] : 1.f);

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
		context->CSSetShader(histogramCS.get(), nullptr, 0);
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
		context->CSSetShader(histogramAvgCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		state->EndPerfEvent();
	}
	resetViews();
	QueueReadback();  // (49h stage 2)

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
		context->CSSetShader(adaptCS.get(), nullptr, 0);

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
