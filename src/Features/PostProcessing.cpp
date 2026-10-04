#include "PostProcessing.h"

#include "IconsFontAwesome5.h"
#include "imgui_stdlib.h"

#include "JiayeStatement.h"
#include "State.h"
#include "Util.h"
#include "Utils/Batch37b.h"
#include "Utils/GpuTimers.h"

#include "Features/Upscaling.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PostProcessing::Settings,
	DisableVanillaTonemapping)

void PostProcessing::DrawSettings()
{
	// 0 for list of feats
	// 1 for feat settings
	// static int pageNum = 0;
	// static int featIdx = 0;
	static int pipelinePageNum = 0;
	static int pipelineFeatIdx = 0;
	static int presetIdx = -1;
	// const float _iconButtonSize = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.x;
	// const ImVec2 iconButtonSize{ _iconButtonSize, _iconButtonSize };

	ImGui::BeginGroup();
	std::string currentPreset = (presetIdx >= 0 && presetIdx < presets.size()) ? presets[presetIdx] : "Select a preset";

	if (ImGui::BeginCombo("##PresetCombo", currentPreset.c_str())) {
		presets = LoadPresets();

		for (int i = 0; i < presets.size(); ++i) {
			bool isSelected = presetIdx == i;
			if (ImGui::Selectable(presets[i].c_str(), isSelected))
				presetIdx = i;
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine();
	if (ImGui::Button("Load")) {
		if (presetIdx >= 0 && presetIdx < presets.size()) {
			LoadPresetFrom(presets[presetIdx]);
		}
	}

	ImGui::EndGroup();
	ImGui::BeginGroup();
	static std::string newPresetName = "";
	ImGui::InputText("##NewPresetName", &newPresetName);

	ImGui::SameLine();
	if (ImGui::Button("Save")) {
		if (!newPresetName.empty())
			SavePresetTo(newPresetName);
	}

	ImGui::EndGroup();

	ImGui::Separator();
	ImGui::Checkbox("Bypass", &bypass);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Temporarily skips all post-processing below, for quick before/after comparison. Not saved.");
	ImGui::SameLine();
	ImGui::Checkbox("Disable Vanilla Tonemapping", (bool*)&settings.DisableVanillaTonemapping);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Skips Skyrim's own final image pass (tonemapping, vanilla bloom and colour grading), so only the effects below shape the look.");
	Batch37b::DrawPostProcessCheckbox();

	ImGui::Separator();

	if (pipelinePageNum == 0) {
		for (int i = 0; i < pipeline.size(); ++i) {
			auto& feat = pipeline[i];
			if (feat) {
				ImGui::PushID(feat->GetType().c_str());
				ImGui::Checkbox("##Enabled", &feat->enabled);
				ImGui::SameLine();
				if (ImGui::Button(ICON_FA_BARS)) {
					pipelineFeatIdx = i;
					pipelinePageNum = 1;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Edit settings for this feature.");
				ImGui::SameLine();
				ImGui::Text("%s", feat->GetType().c_str());
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(feat->GetDesc().c_str());
				ImGui::PopID();
			}
		}
	} else if (pipelinePageNum == 1) {
		if (ImGui::Button(ICON_FA_ARROW_LEFT " Back to Pipeline")) {
			pipelinePageNum = 0;
		}
		ImGui::Separator();
		if (pipelineFeatIdx >= 0 && pipelineFeatIdx < pipeline.size()) {
			auto& feat = pipeline[pipelineFeatIdx];
			if (feat) {
				ImGui::PushID(feat->GetType().c_str());
				
				ImGui::SeparatorText(feat->GetType().c_str());
				ImGui::TextWrapped(feat->GetDesc().c_str());
				
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::Checkbox("Enabled", &feat->enabled);
				if (feat->enabled)
				{
					ImGui::Indent();
					feat->DrawSettings();
					ImGui::Unindent();
				} else {
					ImGui::TextDisabled("Enable the feature to see its settings.");
				}
				
				ImGui::PopID();
			} else {
				ImGui::TextDisabled("Selected feature is not valid.");
				pipelinePageNum = 0;
			}
		} else {
			ImGui::TextDisabled("Invalid feature selected. Returning to list.");
			pipelinePageNum = 0;
		}
	}

	ImGui::Separator();

	if (ImGui::TreeNode("Debug")) {
		ImGui::Text("In Interior: %s", imageSpaceManager->inInterior ? "Yes" : "No");
		ImGui::Text("Time of Day:");
		ImGui::Text("Dawn: %.2f\nSunrise: %.2f\nDay: %.2f\nSunset: %.2f\nDusk: %.2f\nNight: %.2f",
			imageSpaceManager->timeOfDay[0],
			imageSpaceManager->timeOfDay[1],
			imageSpaceManager->timeOfDay[2],
			imageSpaceManager->timeOfDay[3],
			imageSpaceManager->timeOfDay[4],
			imageSpaceManager->timeOfDay[5]);
		if (ImGui::TreeNode("Game ImageSpace Values")) {
			ImGui::Text("Base Amount: %.3f", imageSpaceManager->gameISData.baseAmount);
			ImGui::Text("Base Data:");
			ImGui::Text("Cinematic Values:");
			ImGui::Text("Saturation: %.3f\nBrightness: %.3f\nContrast: %.3f",
				imageSpaceManager->gameISData.baseData.cinematic.saturation,
				imageSpaceManager->gameISData.baseData.cinematic.brightness,
				imageSpaceManager->gameISData.baseData.cinematic.contrast);

			ImGui::Text("HDR Values:");
			ImGui::Text("Eye Adapt Speed: %.3f\nBloom Blur Radius: %.3f\nBloom Threshold: %.3f\nBloom Scale: %.3f\nReceive Bloom Threshold: %.3f\nWhite: %.3f\nSunlight Scale: %.3f\nSky Scale: %.3f\nEye Adapt Strength: %.3f",
				imageSpaceManager->gameISData.baseData.hdr.eyeAdaptSpeed,
				imageSpaceManager->gameISData.baseData.hdr.bloomBlurRadius,
				imageSpaceManager->gameISData.baseData.hdr.bloomThreshold,
				imageSpaceManager->gameISData.baseData.hdr.bloomScale,
				imageSpaceManager->gameISData.baseData.hdr.receiveBloomThreshold,
				imageSpaceManager->gameISData.baseData.hdr.white,
				imageSpaceManager->gameISData.baseData.hdr.sunlightScale,
				imageSpaceManager->gameISData.baseData.hdr.skyScale,
				imageSpaceManager->gameISData.baseData.hdr.eyeAdaptStrength);

			ImGui::Text("Tint Values:");
			ImGui::Text("Tint Amount: %.3f\nTint Color: (%.3f, %.3f, %.3f)",
				imageSpaceManager->gameISData.baseData.tint.amount,
				imageSpaceManager->gameISData.baseData.tint.color.red,
				imageSpaceManager->gameISData.baseData.tint.color.green,
				imageSpaceManager->gameISData.baseData.tint.color.blue);

			ImGui::Text("Depth of Field Values:");
			ImGui::Text("DOF Strength: %.3f\nDOF Distance: %.3f\nDOF Range: %.3f\nDOF Flags: %d\nDOF Sky Blur Radius: %d",
				imageSpaceManager->gameISData.baseData.depthOfField.strength,
				imageSpaceManager->gameISData.baseData.depthOfField.distance,
				imageSpaceManager->gameISData.baseData.depthOfField.range,
				imageSpaceManager->gameISData.baseData.depthOfField.flags,
				static_cast<int>(imageSpaceManager->gameISData.baseData.depthOfField.skyBlurRadius.get()));

			ImGui::Text("Mod Amount: %.3f", imageSpaceManager->gameISData.modAmount);
			ImGui::Text("Mod Data:");
			ImGui::Text("Fade Amount: %.3f\nFade Color: (%.3f, %.3f, %.3f)\nBlur Radius: %.3f\nDouble Vision Strength: %.3f\n",
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeAmount],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeR],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeG],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeB],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kBlurRadius],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDoubleVisionStrength]);
			ImGui::Text("Radial Blur Strength: %.3f\nRadial Blur Rampup: %.3f\nRadial Blur Start: %.3f\nRadial Blur Rampdown: %.3f\nRadial Blur Down Start: %.3f\nRadial Blur Center: (%.3f, %.3f)",
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurStrength],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurRampup],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurStart],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurRampdown],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurDownStart],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurCenterX],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurCenterY]);
			ImGui::Text("DOF Strength: %.3f\nDOF Distance: %.3f\nDOF Range: %.3f\nDOF Mode: %d",
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFStrength],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFDistance],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFRange],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFMode]);
			ImGui::Text("Motion Blur Strength: %.3f", imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kMotionBlurStrength]);
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

void PostProcessing::LoadSettings(json& o_json)
{
	pendingSettings = o_json;
}

void PostProcessing::ProcessSettings(json& o_json)
{
	logger::info("Loading post processing settings...");

	for (auto& feat : pipeline) {
		if (feat && o_json.contains(feat->GetType())) {
			feat->enabled = o_json.value(feat->GetType(), json::object()).value("enabled", true);
			json featSettings = o_json.value(feat->GetType(), json::object()).value("settings", json::object());
			feat->LoadSettings(featSettings);
			// (batch 16, item P14) This used to be an unconditional feat->SetupResources(),
			// which meant loading a preset reallocated all ten sub-features' textures whether
			// they were on or not -- and at boot it ran a second time on top of the loop in
			// SetupResources, so the whole 945.7 MiB was built twice.
			//
			// Settings really can change what the resources look like (which LUT file, which
			// tile sizes), so they still have to be dropped. They just get rebuilt lazily,
			// and only for effects that are actually on.
			if (loaded) {
				feat->ReleaseResources();
				feat->resourcesResident = false;
				feat->disabledSince = {};
			}
		}
	}

	if (o_json.contains("ppsettings"))
		settings = o_json["ppsettings"];
}

void PostProcessing::SaveSettings(json& o_json)
{
	for (auto& pipe : pipeline) {
		if (pipe) {
			json featureSetting{};
			pipe->SaveSettings(featureSetting);
			o_json[pipe->GetType()] = {
				{ "enabled", pipe->enabled },
				{ "settings", featureSetting }
			};
		}
	}

	o_json["ppsettings"] = settings;
}

std::vector<std::string> PostProcessing::LoadPresets()
{
	std::vector<std::string> o_presets = {};

	try {
		std::filesystem::create_directories(ppPresetPath);
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("Error creating preset directory during Load ({}) : {}\n", ppPresetPath, e.what());
		return o_presets;
	}

	for (const auto& entry : std::filesystem::directory_iterator(ppPresetPath)) {
		if (entry.is_regular_file() && entry.path().extension() == ".json") {
			o_presets.push_back(entry.path().stem().string());
		}
	}

	return o_presets;
}

void PostProcessing::LoadPresetFrom(std::string a_name)
{
	json a_presets = {};

	// if the name has .json, remove it
	if (a_name.ends_with(".json"))
		a_name = a_name.substr(0, a_name.size() - 5);

	try {
		logger::info("Loading preset: {}", a_name);
		std::ifstream i{ std::format("{}\\{}.json", ppPresetPath, a_name) };
		i >> a_presets;
	} catch (const std::exception& e) {
		logger::warn("Failed to load preset: {}. Error: {}", a_name, e.what());
		return;
	}

	ProcessSettings(a_presets);
}

void PostProcessing::SavePresetTo(std::string a_name)
{
	json a_presets = {};
	SaveSettings(a_presets);
	a_presets["preset_name"] = a_name;

	// Check if the name is valid
	if (a_name.empty()) {
		logger::warn("Invalid preset name.");
		return;
	}

	try {
		std::filesystem::create_directories(ppPresetPath);
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("Error creating preset directory during Save ({}) : {}\n", ppPresetPath, e.what());
		return;
	}

	std::string presetPath = std::format("{}\\{}.json", ppPresetPath, a_name);
	std::ofstream o{ presetPath };

	try {
		o << std::setw(4) << a_presets;
		logger::info("Saving preset to {}", presetPath);
	} catch (const std::exception& e) {
		logger::warn("Failed to write preset to file: {}. Error: {}", presetPath, e.what());
	}
}

void PostProcessing::RestoreDefaultSettings()
{	
	try {
		LoadPresetFrom("default");
	} catch (const std::exception& e) {
		logger::warn("Failed to load default preset. Error: {}", e.what());
		settings = {};
		pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)].get()->enabled = true;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)].get()->enabled = true;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::LUT)].get()->enabled = false;

		if (!REL::Module::IsVR()) {
			pipeline[static_cast<size_t>(FeaturePipelineIndex::MotionBlur)].get()->enabled = false;
			pipeline[static_cast<size_t>(FeaturePipelineIndex::DoF)].get()->enabled = false;
			pipeline[static_cast<size_t>(FeaturePipelineIndex::CODBloom)].get()->enabled = true;
			pipeline[static_cast<size_t>(FeaturePipelineIndex::LensFlare)].get()->enabled = false;
			pipeline[static_cast<size_t>(FeaturePipelineIndex::Vignette)].get()->enabled = true;
			pipeline[static_cast<size_t>(FeaturePipelineIndex::Camera)].get()->enabled = false;
		}

		for (auto& pipe : pipeline) {
			if (pipe) {
				pipe->RestoreDefaultSettings();
			}
		}
	}
}

void PostProcessing::ClearShaderCache()
{
	for (auto& pipe : pipeline) {
		if (pipe) {
			pipe->ClearShaderCache();
			// (batch 16, item P14) Some sub-features drop more than shaders here --
			// MotionBlur::ClearShaderCache nulls its constant buffers and grid textures too.
			// Nothing used to put those back, so motion blur was quietly broken after any
			// shader-cache clear. Invalidating the residency flag makes the next draw rebuild
			// them, which fixes that as a side effect of keeping our bookkeeping honest.
			pipe->resourcesResident = false;
			pipe->disabledSince = {};
		}
	}
}

void PostProcessing::SetupResources()
{
	{
		auto renderer = globals::game::renderer;
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

		texCopy = eastl::make_unique<Texture2D>(texDesc);
		texCopy->CreateUAV(uavDesc);
	}

	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PostProcessing\\copy.cs.hlsl", {}, "cs_5_0")))
		copyCS.attach(rawPtr);

	pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)] = std::make_unique<HistogramAutoExposure>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)].get()->enabled = true;
	pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)] = std::make_unique<ColorGrading>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)].get()->enabled = true;
	pipeline[static_cast<size_t>(FeaturePipelineIndex::LUT)] = std::make_unique<LUT>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::LUT)].get()->enabled = false;

	if (!REL::Module::IsVR()) {
		pipeline[static_cast<size_t>(FeaturePipelineIndex::MotionBlur)] = std::make_unique<MotionBlur>();
		pipeline[static_cast<size_t>(FeaturePipelineIndex::MotionBlur)].get()->enabled = false;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::DoF)] = std::make_unique<DoF>();
		pipeline[static_cast<size_t>(FeaturePipelineIndex::DoF)].get()->enabled = false;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::CODBloom)] = std::make_unique<CODBloom>();
		pipeline[static_cast<size_t>(FeaturePipelineIndex::CODBloom)].get()->enabled = true;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::LensFlare)] = std::make_unique<LensFlare>();
		pipeline[static_cast<size_t>(FeaturePipelineIndex::LensFlare)].get()->enabled = false;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::Vignette)] = std::make_unique<Vignette>();
		pipeline[static_cast<size_t>(FeaturePipelineIndex::Vignette)].get()->enabled = true;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::Camera)] = std::make_unique<Camera>();
		pipeline[static_cast<size_t>(FeaturePipelineIndex::Camera)].get()->enabled = false;
		pipeline[static_cast<size_t>(FeaturePipelineIndex::Border)] = std::make_unique<Border>();
		pipeline[static_cast<size_t>(FeaturePipelineIndex::Border)].get()->enabled = false;
	}

	// Shaders for every sub-feature, on or off. See PostProcessFeature::SetupShaders for why
	// this half stays unconditional.
	for (auto& pipe : pipeline) {
		if (pipe) {
			pipe->SetupShaders();
		}
	}

	// (batch 16, item P14) This loop used to call pipe->SetupResources() unconditionally, for
	// every sub-feature, ignoring pipe->enabled entirely: 119.5 bytes per output pixel, 945.7
	// MiB at 4K, of which 743.4 MiB belonged to effects that ship switched off (LensFlare
	// 379.7, DoF 268.9, LUT / Camera / Border 31.6 each).
	//
	// Nothing is allocated here now. Two things replace it:
	//   * EnsureResources(), called immediately before an effect's Draw, brings its memory up
	//     on the first frame it is actually used. "Immediately before use" is the whole point:
	//     an effect can never be drawn with released resources, whatever order the engine
	//     happens to call our hooks in on a given frame.
	//   * ReleaseIdleResources(), called from Prepass, hands memory back after an effect has
	//     been off for a grace period.
	//
	// SetupResources also re-runs on a resolution change (BSShaderRenderTargets_Create
	// re-enters State::Setup), and everything a sub-feature allocates is sized from
	// kMAIN_COPY, so nothing may survive that re-entry describing the old extent.
	//
	// In practice the make_unique calls above already handle it: they replace the whole
	// pipeline, and the old objects' unique_ptr members free the old-size textures on
	// destruction. This loop is the belt to that braces -- it makes the residency flags true
	// by construction rather than by relying on the objects being new, so the invariant still
	// holds if someone later makes the pipeline persist across a re-entry.
	for (auto& pipe : pipeline) {
		if (pipe) {
			pipe->ReleaseResources();
			pipe->resourcesResident = false;
			pipe->disabledSince = {};
		}
	}

	ProcessSettings(pendingSettings);
	pendingSettings = {};
}

// (batch 16, item P14) Bring one sub-feature's resources up if they are not already up.
//
// Called from the draw loops rather than from a per-frame reconcile so that allocation is
// ordered against *use* and not against some other hook. The failure mode this avoids is
// specific: Prepass and PreProcess are separate hooks, and there is no guarantee every frame
// runs both, so a reconcile in Prepass could be skipped on the frame PreProcess draws.
void PostProcessing::EnsureResources(PostProcessFeature* a_pipe)
{
	if (a_pipe->resourcesResident)
		return;

	// Marked resident before the call, not after: if SetupResources throws we do not want to
	// retry it every single frame for the rest of the session. One log line and the effect
	// draws with whatever it managed to build, which is the same behaviour a failure in the
	// old boot-time loop produced.
	a_pipe->resourcesResident = true;
	a_pipe->disabledSince = {};

	try {
		a_pipe->SetupResources();
	} catch (const std::exception& e) {
		logger::error("[Post Processing] {} failed to allocate its resources: {}", a_pipe->GetType(), e.what());
	}
}

// (batch 16, item P14) Give back the memory of effects that have been off for a while.
//
// The grace period is the part that matters. Freeing on the frame the checkbox flips would
// mean a user clicking LensFlare on and off in the menu allocates and frees 379.7 MiB per
// click, which is worse for the driver than never freeing at all. Ten seconds of wall clock
// (not frames -- menu framerates vary by an order of magnitude) is long enough that ordinary
// menu fiddling costs nothing, and short enough that the memory is back before the user has
// walked anywhere.
void PostProcessing::ReleaseIdleResources()
{
	static constexpr auto graceperiod = std::chrono::seconds(10);
	const auto now = std::chrono::steady_clock::now();

	for (auto& pipe : pipeline) {
		if (!pipe || !pipe->resourcesResident)
			continue;

		if (pipe->enabled) {
			pipe->disabledSince = {};
			continue;
		}

		if (pipe->disabledSince == std::chrono::steady_clock::time_point{}) {
			pipe->disabledSince = now;
			continue;
		}

		if (now - pipe->disabledSince < graceperiod)
			continue;

		logger::debug("[Post Processing] Releasing {} resources after {} s disabled", pipe->GetType(), std::chrono::duration_cast<std::chrono::seconds>(now - pipe->disabledSince).count());
		pipe->ReleaseResources();
		pipe->resourcesResident = false;
		pipe->disabledSince = {};
	}
}

void PostProcessing::Reset()
{
	for (auto& pipe : pipeline) {
		if (pipe)
			pipe->Reset();
	}
}

// from doodlum
void PostProcessing::UpdateToD()
{
	if (bypass)
		return;

	auto sky = globals::game::sky;
	if (!sky)
		return;

	imageSpaceManager->inInterior = Util::IsInterior();

	if (globals::game::isVR)
		return;  // for now

	float currentTime = sky->currentGameHour;

	float sunriseBegin = sky->GetSunriseBegin();
	float sunriseEnd = sky->GetSunriseEnd();
	float sunsetBegin = sky->GetSunsetBegin();
	float sunsetEnd = sky->GetSunsetEnd();

	float dawnMid = sunriseBegin + (sunriseEnd - sunriseBegin) * 0.5f;
	float duskMid = sunsetBegin + (sunsetEnd - sunsetBegin) * 0.5f;

	auto range01 = [](float t, float a, float b) {
		// Handles wrap-around if b < a
		float range = b - a;
		if (range < 0.0f)
			range += 24.0f;
		float value = t - a;
		if (value < 0.0f)
			value += 24.0f;
		return std::clamp(value / range, 0.0f, 1.0f);
	};

	for (int i = 0; i < 6; ++i) {
		imageSpaceManager->timeOfDay[i] = 0.0f;
	}

	// Dawn → Sunrise
	if (currentTime >= sunriseBegin && currentTime < dawnMid) {
		float f = range01(currentTime, sunriseBegin, dawnMid);
		imageSpaceManager->timeOfDay[0] = 1.0f - f;  // dawn
		imageSpaceManager->timeOfDay[1] = f;         // sunrise
	} else if (currentTime >= dawnMid && currentTime < sunriseEnd) {
		float f = range01(currentTime, dawnMid, sunriseEnd);
		imageSpaceManager->timeOfDay[1] = 1.0f - f;  // sunrise
		imageSpaceManager->timeOfDay[2] = f;         // day
	}
	// Day → Sunset
	else if (currentTime >= sunriseEnd && currentTime < sunsetBegin) {
		float f = range01(currentTime, sunriseEnd, sunsetBegin);
		imageSpaceManager->timeOfDay[2] = 1.0f - f;  // day
		imageSpaceManager->timeOfDay[3] = f;         // sunset
	}
	// Sunset → Dusk
	else if (currentTime >= sunsetBegin && currentTime < duskMid) {
		float f = range01(currentTime, sunsetBegin, duskMid);
		imageSpaceManager->timeOfDay[3] = 1.0f - f;  // sunset
		imageSpaceManager->timeOfDay[4] = f;         // dusk
	} else if (currentTime >= duskMid && currentTime < sunsetEnd) {
		float f = range01(currentTime, duskMid, sunsetEnd);
		imageSpaceManager->timeOfDay[4] = 1.0f - f;  // dusk
		imageSpaceManager->timeOfDay[5] = f;         // night
	}
	// Night → Dawn (wrap)
	else {
		float f = range01(currentTime, sunsetEnd, sunriseBegin);
		imageSpaceManager->timeOfDay[5] = 1.0f - f;  // night
		imageSpaceManager->timeOfDay[0] = f;         // dawn
	}
}

void PostProcessing::DrawBeforeUpscaling()
{
	if (bypass)
		return;

	auto& upscaling = globals::features::upscaling;
	if (!upscaling.loaded)
		return;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto state = globals::state;

	bool inMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
	auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };

	auto drawsHere = [&](const std::unique_ptr<PostProcessFeature>& a_pipe) {
		return a_pipe && a_pipe->enabled && !a_pipe->DrawAfterColorGrading() &&
		       !(inMainLoadingMenu && a_pipe->DisableInMainLoadingMenu()) && a_pipe->DrawBeforeUpscaling();
	};

	// Nothing opts into the pre-upscale leg today: PostProcessFeature::DrawBeforeUpscaling()
	// defaults to false and the only override (HistogramAutoExposure) is commented out. With
	// no qualifying effect the loop below cannot advance lastTexColor past gameTexMain, so the
	// write-back degenerated into a same-resource, same-subresource CopySubresourceRegion --
	// undefined behaviour in D3D11 -- on a full-screen target, every frame. Bail out before
	// touching the context so the leg costs nothing instead of a wasted full-screen copy.
	if (std::ranges::none_of(pipeline, drawsHere))
		return;

	state->BeginPerfEvent("[Post Processing] Pre-Upscale");
	// First of the two post-processing legs. Both accumulate into one bucket so the row
	// reads as the cost of the whole chain.
	Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::PostProcessing);

	// go through each fx
	for (auto& pipe : pipeline) {
		if (drawsHere(pipe)) {
			EnsureResources(pipe.get());
			pipe->Draw(lastTexColor);
		}
	}

	D3D11_TEXTURE2D_DESC desc;
	lastTexColor.tex->GetDesc(&desc);
	if (desc.Format == texCopy->desc.Format) {
		// Only a real hand-off needs the copy; an effect that wrote straight back into MAIN
		// would otherwise trigger the same self-copy UB described above.
		if (lastTexColor.tex != gameTexMain.texture)
			context->CopySubresourceRegion(gameTexMain.texture, 0, 0, 0, 0, lastTexColor.tex, 0, nullptr);
	} else {
		ID3D11ShaderResourceView* srv = lastTexColor.srv;
		ID3D11UnorderedAccessView* uav = texCopy->uav.get();

		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetShader(copyCS.get(), nullptr, 0);
		context->Dispatch((texCopy->desc.Width + 7) >> 3, (texCopy->desc.Height + 7) >> 3, 1);

		srv = nullptr;
		uav = nullptr;

		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetShader(nullptr, nullptr, 0);

		context->CopySubresourceRegion(gameTexMain.texture, 0, 0, 0, 0, texCopy->resource.get(), 0, nullptr);
	}

	Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::PostProcessing);
	state->EndPerfEvent();
}

void PostProcessing::PreProcess()
{
	if (bypass)
		return;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto& upscaling = globals::features::upscaling;

	bool inMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));

	auto gameTexMain = isrefraction ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY] : renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };
	auto gameTexMainAlt = isrefraction ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN] : renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

	// Second post-processing leg; accumulates into the same bucket as the pre-upscale leg.
	Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::PostProcessing);

	// The draw order is unchanged: every regular effect in pipeline order, then every
	// after-colour-grading one. Collected first so the last effect of the chain is known.
	auto drawsHere = [&](const std::unique_ptr<PostProcessFeature>& a_pipe, bool a_afterColorGrading) {
		return a_pipe && a_pipe->enabled && a_pipe->DrawAfterColorGrading() == a_afterColorGrading && !(inMainLoadingMenu && a_pipe->DisableInMainLoadingMenu()) &&
		       (!a_pipe->DrawBeforeUpscaling() || !upscaling.loaded);
	};
	std::vector<PostProcessFeature*> chain;
	chain.reserve(pipeline.size());
	for (auto& pipe : pipeline) {
		if (drawsHere(pipe, false))
			chain.push_back(pipe.get());
	}
	for (auto& pipe : pipeline) {
		if (drawsHere(pipe, true))
			chain.push_back(pipe.get());
	}

	// (batch 37b, C-1) Direct output. The old write-back copied the last effect's texture into
	// BOTH game buffers: two full-screen copies at output resolution every frame. When the last
	// effect can store into the game buffer itself -- same format, same size as its own texture,
	// so the same shader stores the same bits -- it does, and only the copy into the other buffer
	// remains, restricted to the same rectangle the old full-texture copy covered.
	directOutputUsed = false;
	if (!Batch37b::PostProcessDirectOutputActive())
		directOutputStatus = "Off";
	else if (chain.empty())
		directOutputStatus = "No effect running";
	else
		directOutputStatus = "Last effect cannot write into the game buffer";

	D3D11_BOX directBox{};
	for (size_t i = 0; i < chain.size(); ++i) {
		PostProcessFeature* pipe = chain[i];
		EnsureResources(pipe);

		bool direct = false;
		if (i + 1 == chain.size() && Batch37b::PostProcessDirectOutputActive()) {
			ID3D11Texture2D* own = pipe->GetOwnOutputTexture();
			if (own && gameTexMain.texture && gameTexMain.UAV && gameTexMainAlt.texture && own != gameTexMain.texture &&
				lastTexColor.tex != gameTexMain.texture) {  // the effect must not read the buffer it writes
				D3D11_TEXTURE2D_DESC ownDesc{}, mainDesc{}, altDesc{};
				own->GetDesc(&ownDesc);
				gameTexMain.texture->GetDesc(&mainDesc);
				gameTexMainAlt.texture->GetDesc(&altDesc);
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				gameTexMain.UAV->GetDesc(&uavDesc);
				// Exact size match, not just "fits": the dispatch is rounded up to 8x8 groups and the
				// stores past the edge must fall outside the target exactly as they did on the
				// effect's own texture. Alt must take a raw copy of the same format, and the old
				// path's first branch (formats equal to texCopy) is the only one reproduced here.
				const bool match = ownDesc.Format == mainDesc.Format && ownDesc.Format == altDesc.Format && ownDesc.Format == texCopy->desc.Format &&
				                   uavDesc.Format == ownDesc.Format && uavDesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && uavDesc.Texture2D.MipSlice == 0 &&
				                   ownDesc.Width == mainDesc.Width && ownDesc.Height == mainDesc.Height && ownDesc.Width <= altDesc.Width &&
				                   ownDesc.Height <= altDesc.Height && mainDesc.SampleDesc.Count == 1 && altDesc.SampleDesc.Count == 1;
				if (match) {
					direct = true;
					directBox = { 0, 0, 0, ownDesc.Width, ownDesc.Height, 1 };
				} else {
					directOutputStatus = "Game buffer differs from the effect's texture (format or size)";
				}
			} else if (own && !gameTexMain.UAV) {
				directOutputStatus = "Game buffer has no UAV";
			}
		}

		if (!direct) {
			pipe->Draw(lastTexColor);
			continue;
		}

		// Binding the game buffer as a compute UAV makes D3D11 unbind it from every shader-resource
		// slot and render target it may already sit in for the pass that follows. Put those
		// bindings back afterwards so the engine finds exactly the state it left.
		constexpr UINT kSlots = 16;
		ID3D11ShaderResourceView* psSrvs[kSlots]{};
		ID3D11ShaderResourceView* vsSrvs[kSlots]{};
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv = nullptr;
		context->PSGetShaderResources(0, kSlots, psSrvs);
		context->VSGetShaderResources(0, kSlots, vsSrvs);
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);

		directOutput = { gameTexMain.texture, gameTexMain.SRV, gameTexMain.UAV };
		directOutputFor = pipe;
		pipe->Draw(lastTexColor);
		directOutput = {};
		directOutputFor = nullptr;

		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		{
			// Only touch the output merger if the UAV binding actually knocked a target out:
			// OMSetRenderTargets has side effects of its own (output-merger UAV slots).
			ID3D11RenderTargetView* rtvsAfter[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* dsvAfter = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvsAfter, &dsvAfter);
			bool omChanged = dsvAfter != dsv;
			UINT numViews = 0;
			for (UINT r = 0; r < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++r) {
				omChanged |= rtvsAfter[r] != rtvs[r];
				if (rtvs[r])
					numViews = r + 1;
				if (rtvsAfter[r])
					rtvsAfter[r]->Release();
			}
			if (dsvAfter)
				dsvAfter->Release();
			if (omChanged)
				context->OMSetRenderTargets(numViews, rtvs, dsv);
		}
		context->VSSetShaderResources(0, kSlots, vsSrvs);
		context->PSSetShaderResources(0, kSlots, psSrvs);
		for (auto* p : psSrvs) {
			if (p)
				p->Release();
		}
		for (auto* p : vsSrvs) {
			if (p)
				p->Release();
		}
		for (auto* p : rtvs) {
			if (p)
				p->Release();
		}
		if (dsv)
			dsv->Release();

		directOutputUsed = lastTexColor.tex == gameTexMain.texture;
		directOutputStatus = directOutputUsed ? "On: one write-back copy instead of two" : "Effect did not take the game buffer";
	}

	D3D11_TEXTURE2D_DESC desc;
	lastTexColor.tex->GetDesc(&desc);
	if (directOutputUsed) {
		// (batch 37b, C-1) The image is already in gameTexMain; the second buffer gets the same
		// rectangle the old full-texture copy wrote, from the same bits.
		context->CopySubresourceRegion(gameTexMainAlt.texture, 0, 0, 0, 0, gameTexMain.texture, 0, &directBox);
	} else if (desc.Format == texCopy->desc.Format) {
		// either MAIN_COPY or MAIN is used as input for HDR pass
		// so we copy to both so whatever the game wants we're not failing it
		//
		// The self-copy guards are not cosmetic: whenever the pipeline leaves the image in the
		// buffer we are writing back to (no effect advanced lastTexColor, or one wrote straight
		// into it) this was a same-resource, same-subresource CopySubresourceRegion, which D3D11
		// leaves undefined. Skipping it is exactly the intended result, only defined.
		if (lastTexColor.tex != gameTexMain.texture)
			context->CopySubresourceRegion(gameTexMain.texture, 0, 0, 0, 0, lastTexColor.tex, 0, nullptr);
		if (lastTexColor.tex != gameTexMainAlt.texture)
			context->CopySubresourceRegion(gameTexMainAlt.texture, 0, 0, 0, 0, lastTexColor.tex, 0, nullptr);
	} else {
		ID3D11ShaderResourceView* srv = lastTexColor.srv;
		ID3D11UnorderedAccessView* uav = texCopy->uav.get();

		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetShader(copyCS.get(), nullptr, 0);
		context->Dispatch((texCopy->desc.Width + 7) >> 3, (texCopy->desc.Height + 7) >> 3, 1);

		srv = nullptr;
		uav = nullptr;

		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetShader(nullptr, nullptr, 0);

		context->CopySubresourceRegion(gameTexMain.texture, 0, 0, 0, 0, texCopy->resource.get(), 0, nullptr);
		context->CopySubresourceRegion(gameTexMainAlt.texture, 0, 0, 0, 0, texCopy->resource.get(), 0, nullptr);
	}

	Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::PostProcessing);

	isrefraction = false;
}

void PostProcessing::Prepass()
{
	// (batch 16, item P14) Release-only pass. It never allocates, so a frame that skips
	// Prepass can only ever delay a release, never leave an enabled effect without memory.
	ReleaseIdleResources();

	if (!pendingSettings.empty()) {
		logger::info("Processing pending post processing settings...");
		ProcessSettings(pendingSettings);
		pendingSettings = {};
	}

	UpdateToD();

	// Update gameISData
	const auto ImageSpace = RE::ImageSpaceManager::GetSingleton();
	if (globals::game::isVR) {
		const auto& iSRuntimeData = ImageSpace->GetVRRuntimeData();
		imageSpaceManager->gameISData = iSRuntimeData.data;
		if (const auto& overrideBaseData = iSRuntimeData.overrideBaseData) {
			imageSpaceManager->gameISData.baseData = *overrideBaseData;
		} else {
			imageSpaceManager->gameISData.baseData = *iSRuntimeData.currentBaseData;
		}
	} else {
		const auto& iSRuntimeData = ImageSpace->GetRuntimeData();
		imageSpaceManager->gameISData = iSRuntimeData.data;
		if (const auto& overrideBaseData = iSRuntimeData.overrideBaseData) {
			imageSpaceManager->gameISData.baseData = *overrideBaseData;
		} else {
			imageSpaceManager->gameISData.baseData = *iSRuntimeData.currentBaseData;
		}
	}
}

void PostProcessing::PostPostLoad()
{
	logger::info("Hooking preprocess passes");
	stl::write_vfunc<0x2, BSImagespaceShaderRefraction_SetupTechnique>(RE::VTABLE_BSImagespaceShaderRefraction[0]);
	stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematic_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematic[0]);
	stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematicFade_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematicFade[0]);
}