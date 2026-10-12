#include "ColorGrading.h"

#include "State.h"
#include "Util.h"

#include "ColourSpace.h"
#include "Features/PostProcessing.h"
#include "Features/Upscaling/HDROutput.h"
#include "Utils/UI.h"

#include <DDSTextureLoader.h>
#include <DirectXPackedVector.h>
#include <DirectXTex.h>

#include "IconsFontAwesome5.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ColorGrading::ColorProfile,
    params)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ColorGrading::TonemapHDR,
    mode,
    knee,
    option)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ColorGrading::Settings,
    useToDInterior,
    skipLDR,
    profiles,
    currentTonemapper,
    tonemapParams,
    gameCinematicBlend,
    gameFadeBlend,
    gameTintBlend,
    useLog,
    logType,
    invertLog,
    enableTonemap,
    enableColorSpaceTransform,
    inputColorSpace,
    processColorSpace,
    outputColorSpace,
	colorSpaceTransform,
	invColorSpaceTransform,
	tonemapHDR
)

template <int num = 3>
bool shiftSlider(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
	static_assert(num > 1 && num < 5);

	if (ImGui::GetIO().KeyShift) {
		auto changed = ImGui::SliderFloat(label, v, v_min, v_max, format, flags);
		if (changed)
			for (int i = 1; i < num; i++)
				v[i] = v[0];
		return changed;
	} else {
		if constexpr (num == 2)
			return ImGui::SliderFloat2(label, v, v_min, v_max, format, flags);
		else if constexpr (num == 3)
			return ImGui::SliderFloat3(label, v, v_min, v_max, format, flags);
		else if constexpr (num == 4)
			return ImGui::SliderFloat4(label, v, v_min, v_max, format, flags);
	}
}

template <int num = 1>
bool exposureSlider(float* val)
{
	float tempVal[num];
	for (int i = 0; i < num; i++)
		tempVal[i] = log2(val[i]);

	bool retval;
	if constexpr (num == 1)
		retval = ImGui::SliderFloat("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 2)
		retval = shiftSlider<2>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 3)
		retval = shiftSlider<3>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 4)
		retval = shiftSlider<4>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");

	for (int i = 0; i < num; i++)
		val[i] = exp2(tempVal[i]);

	return retval;
}

// Profjack Design
struct TonemapperInfo
{
	std::string_view name;
	std::string_view func_name;
	std::string_view desc;

	using CTP = std::array<float4, 2>;
	std::function<void(CTP&)> draw_settings_func;
	CTP default_settings;

	CTP cached_settings;

    static auto& GetTonemappers()
	{
		using f4 = float4;
		constexpr auto shiftHint = []() { ImGui::TextWrapped("Press Shift to control all channels at the same time."); };

        static std::vector<TonemapperInfo> tonemappers = {
			{ "Reinhard"sv, "Reinhard"sv,
				"Mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002."sv,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Reinhard Extended"sv, "ReinhardExt"sv,
				"Extended mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002. "
				"An additional user parameter specifies the smallest luminance that is mapped to 1, which allows high luminances to burn out."sv,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("White Point", &params[0].y, 0.f, 10.f, "%.2f"); },
				{ f4{ 1.f, 2.f, 0.f, 0.f } } },

			{ "Hejl Burgess-Dawson Filmic"sv, "HejlBurgessDawsonFilmic"sv,
				"Variation of the Hejl and Burgess-Dawson filmic curve done by Graham Aldridge. "
				"See his blog post about \"Approximating Film with Tonemapping\"."sv,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Aldridge Filmic"sv, "AldridgeFilmic"sv,
				"Variation of the Hejl and Burgess-Dawson filmic curve done by Graham Aldridge. "
				"See his blog post about \"Approximating Film with Tonemapping\"."sv,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Cutoff", &params[0].y, 0.f, .5f, "%.2f"); },
				{ f4{ 1.f, .19f, 0.f, 0.f } } },

			{ "Lottes Filmic/AMD Curve"sv, "LottesFilmic"sv,
				"Filmic curve by Timothy Lottes, described in his GDC talk \"Advanced Techniques and Optimization of HDR Color Pipelines\". "
				"Also known as the \"AMD curve\"."sv,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Contrast", &params[0].y, 1.f, 2.f, "%.2f");
					ImGui::SliderFloat("Shoulder", &params[0].z, 0.01f, 2.f, "%.2f");
					ImGui::SliderFloat("Maximum HDR Value", &params[0].w, 1.f, 10.f, "%.2f");
					ImGui::SliderFloat("Input Mid-Level", &params[1].x, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Output Mid-Level", &params[1].y, 0.f, 1.f, "%.2f"); },
				{ f4{ 1.f, 1.6f, 0.977f, 8.f }, f4{ 0.18f, 0.267f, 0.f, 0.f } } },

			{ "Day Filmic/Insomniac Curve"sv, "DayFilmic"sv,
				"Filmic curve by Mike Day, described in his document \"An efficient and user-friendly tone mapping operator\". "
				"Also known as the \"Insomniac curve\"."sv,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Black Point", &params[0].y, 0.f, 5.f, "%.2f");
					ImGui::SliderFloat("White Point", &params[0].z, 0.f, 5.f, "%.2f");

					ImGui::SliderFloat("Cross-over Point", &params[0].w, 0.f, 5.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Point where the toe and shoulder are pieced together into a single curve.");
					ImGui::SliderFloat("Shoulder Strength", &params[1].x, 0.f, 1.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the shoulder.");
					ImGui::SliderFloat("Toe Strength", &params[1].y, 0.f, 1.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the toe."); },
				{ f4{ 1.f, 0.f, 2.f, 0.3f }, f4{ 0.8f, 0.7f, 0.f, 0.f } } },

			{ "Uchimura/Grand Turismo Curve"sv, "UchimuraFilmic"sv,
				"Filmic curve by Hajime Uchimura, described in his CEDEC talk \"HDR Theory and Practice\". Characterised by its middle linear section. "
				"Also known as the \"Gran Turismo curve\"."sv,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Max Brightness", &params[0].y, 0.01f, 2.f, "%.2f");
					ImGui::SliderFloat("Contrast", &params[0].z, 0.f, 5.f, "%.2f");
					ImGui::SliderFloat("Linear Section Start", &params[0].w, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Linear Section Length", &params[1].x, .01f, .99f, "%.2f");
					ImGui::SliderFloat("Black Tightness Shape", &params[1].y, 1.f, 3.f, "%.2f");
					ImGui::SliderFloat("Black Tightness Offset", &params[1].z, 0.f, 1.f, "%.2f"); },
				{ f4{ 1.f, 1.f, 1.f, .22f }, f4{ 0.4f, 1.33f, 0.f, 0.f } } },

			{ "ACES (Hill)"sv, "AcesHill"sv,
				"ACES curve fit by Stephen Hill."sv,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "ACES (Narkowicz)"sv, "AcesNarkowicz"sv,
				"ACES curve fit by Krzysztof Narkowicz. See his blog post \"ACES Filmic Tone Mapping Curve\"."sv,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "ACES (Guy)"sv, "AcesGuy"sv,
				"Curve from Unreal 3 adapted by to close to the ACES curve by Romain Guy."sv,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "AgX Minimal"sv, "AgxMinimal"sv,
				"Minimal version of Troy Sobotka's AgX using a 6th order polynomial approximation. "
				"Originally created by bwrensch, and improved by Troy Sobotka."sv,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Slope", &params[0].y, 0.f, 2.f, "%.2f");
					ImGui::SliderFloat("Power", &params[0].z, 0.f, 2.f, "%.2f");
					ImGui::SliderFloat("Offset", &params[0].w, -1.f, 1.f, "%.2f");
					ImGui::SliderFloat("Saturation", &params[1].x, 0.f, 2.f, "%.2f"); },
				{ f4{ 1.f, 1.f, 1.f, 0.f }, f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Melon"sv, "MelonTonemap"sv,
				"Tonemapper designed by TripleMelon to fix the ACES issue of intense colour being shifted."sv,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Kajiya"sv, "KajiyaTonemap"sv,
				"Tonemapper designed by Tomasz Stachowiak/Embark for their real time ray tracing engine Kajiya."sv,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "GT7"sv, "GT7ToneMapping"sv,
				"Tonemapper designed for Gran Turismo 7."sv,
				[](CTP& params) {
                    exposureSlider(&params[0].x);
                    // (44a) The old "HDR Output" checkbox (params[0].y) is gone: HDR is HDR Mode below.
                },
				{ f4{ 1.f, 0.f, 400.f, 0.f } } },

			// (44a) Neutwo (RenoDX, MIT): neutral, luminance-only; has a Native HDR form.
			{ "Neutwo"sv, "NeutwoTonemap"sv,
				"Neutwo by Carlos Lopez (RenoDX): a neutral curve on brightness only, so colours keep their hue and saturation. "
				"Its HDR form simply raises the peak, the smoothest change from SDR to HDR."sv,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Clip Point", &params[0].y, 1.f, 100.f, "%.2f", ImGuiSliderFlags_Logarithmic);
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Scene brightness that reaches the peak. Higher = softer shoulder.");
				},
				{ f4{ 1.f, 100.f, 0.f, 0.f } } }
		};

		static std::once_flag flag;
		std::call_once(flag,
			[&]() {
				for (auto& t : tonemappers)
					t.cached_settings = t.default_settings;
			});

		return tonemappers;
	}

    static void GetDefaultParams(int& tonemapperType, CTP& params)
	{
		auto& tonemappers = GetTonemappers();
		if (auto it = std::ranges::find_if(tonemappers, [&](TonemapperInfo& x) { return "GT7"sv == x.name; });
			it != tonemappers.end()) {
			tonemapperType = (int)(it - tonemappers.begin());
			params = it->default_settings;
		} else
			logger::error("Somehow, the default settings are invalid. Please contact the author.");
	}
};

void ColorGrading::DrawSettings()
{
    static int page = 0;
	if (globals::game::isVR)
		settings.useToDInterior = false;
	else
    	ImGui::Checkbox("Use ToD and Interior Settings", &settings.useToDInterior);
    ImGui::SameLine();
    ImGui::Checkbox("Skip LDR Color Grading", &settings.skipLDR);
    if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Skip color grading after tonemapping. This includes Lift Gamma Gain and Oklch adjustments.");

    ImGui::Checkbox("Convert Linear to Log Before HDR Color Grading", &settings.useLog);
    if (settings.useLog) {
        ImGui::Checkbox("Convert Log to Linear After HDR Color Grading", &settings.invertLog);
        ImGui::Combo("Log Type", (int*)&settings.logType, "ACEScct\0ARRILogC4\0SonySLog3\0");
    }

    if (settings.useToDInterior) {
        ImGui::Combo("Profile Page", &page, "Dawn\0Sunrise\0Day\0Sunset\0Dusk\0Night\0Interior\0");
    }
    int realPage = settings.useToDInterior ? page + 1 : 0;
    auto& profile = settings.profiles[realPage];

    ImGui::SeparatorText("Color Grading");
    ImGui::PushID(realPage);
    {
		ImGui::Text("Profile: %s", profileNames[realPage].data());
        ImGui::SliderFloat("Input Gamma", &profile.params[6].z, 0.f, 3.f, "%.3f");
        ImGui::SliderFloat("Output Gamma", &profile.params[6].w, 0.f, 3.f, "%.3f");

		ImGui::Text("Pre-Tonemapping Settings");
		if (ImGui::TreeNode("Exposure/Temperature/Tint")) {
			exposureSlider(&profile.params[17].x);
			ImGui::SliderFloat("Temperature", &profile.params[17].y, 10.f, 150.f, "%1.f00K");
			ImGui::SliderFloat("Tint", &profile.params[17].z, -1.f, 1.f, "%.3f");
			ImGui::TreePop();
		}

        if (ImGui::TreeNode("ASC CDL")) {
            shiftSlider("Slope", &profile.params[0].x, 0.f, 2.f, "%.2f");
            shiftSlider("Power", &profile.params[1].x, 0.f, 2.f, "%.2f");
            shiftSlider("Offset", &profile.params[2].x, -1.f, 1.f, "%.2f");
            ImGui::TreePop();
        }

		if (ImGui::TreeNode("Saturation/Hue")) {
            ImGui::SliderFloat("Saturation", &profile.params[6].x, 0.f, 3.f, "%.3f");
            ImGui::SliderFloat("Hue Shift", &profile.params[6].y, -1.f, 1.f, "%.3f");
            ImGui::TreePop();
        }

		if (ImGui::TreeNode("Shadows/Midtones/Highlights")) {
            shiftSlider("Shadows", &profile.params[18].x, 0.f, 2.f, "%.3f");
            shiftSlider("Midtones", &profile.params[19].x, 0.f, 2.f, "%.3f");
            shiftSlider("Highlights", &profile.params[20].x, 0.f, 2.f, "%.3f");
            ImGui::InputFloat2("Shadows Start/End", &profile.params[21].x, "%.3f");
            ImGui::InputFloat2("Highlights Start/End", &profile.params[21].z, "%.3f");
            ImGui::TreePop();
        }

		if (ImGui::TreeNode("Contrast")) {
			shiftSlider("Contrast", &profile.params[15].x, 0.f, 2.f, "%.3f");
			shiftSlider("Pivot", &profile.params[16].x, 0.f, 1.f, "%.3f");
			ImGui::TreePop();
		}

		ImGui::Text("Post-Tonemapping Settings");
        if (ImGui::TreeNode("Lift Gamma Gain")) {
            ImGui::DragFloat4("Lift", &profile.params[3].x, 1e-3f, -1.f, 1.f, "%.3f");
            ImGui::DragFloat4("Gamma", &profile.params[4].x, 1e-3f, -1.5f, 1.5f, "%.3f");
            ImGui::DragFloat4("Gain", &profile.params[5].x, 1e-3f, 0.f, 2.f, "%.3f");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("OKLCH Saturation")) {
            ImGui::SliderFloat("Saturation", &profile.params[7].x, 0.f, 2.f, "%.3f");
            ImGui::SliderFloat("Vibrance", &profile.params[7].y, 0.f, 3.f, "%.3f");
            ImGui::SliderFloat("Hue Shift", &profile.params[7].z, -1.f, 1.f, "%.3f");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("OKLCH Color Mixer")) {
            ImGui::Text("Adjust brightness, vibrance and hue shift of specific hues in the perceptually uniform OKLCH space.");
            constexpr std::array<ImColor, 7> hues = { {
                { 255, 0, 0 },
                { 182, 124, 1 },
                { 87, 159, 0 },
                { 0, 161, 145 },
                { 0, 149, 217 },
                { 133, 100, 255 },
                { 255, 35, 189 },
            } };
            static int hueId = 0;
            if (ImGui::BeginTable("##HueTable", 7)) {
                for (int i = 0; i < 7; i++) {
                    ImGui::TableNextColumn();

                    ImGui::PushID(i);
                    ImGui::PushStyleColor(ImGuiCol_Text, hues[i].Value);
                    ImGui::RadioButton(ICON_FA_SQUARE, &hueId, i);
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::SliderFloat("Hue Shift", &profile.params[8 + hueId].x, -1.f, 1.f, "%.3f");
            ImGui::SliderFloat("Vibrance", &profile.params[8 + hueId].y, 0.f, 3.f, "%.3f");
            ImGui::SliderFloat("Brightness", &profile.params[8 + hueId].z, -1.f, 1.f, "%.3f");
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Tonemapping");
    ImGui::Checkbox("Enable Tonemapping", &settings.enableTonemap);
    if (settings.enableTonemap) {
        auto& tonemappers = TonemapperInfo::GetTonemappers();

        if (ImGui::BeginCombo("Tonemapper", tonemappers[tonemapperType].name.data(), ImGuiComboFlags_HeightLargest)) {
            for (int i = 0; i < tonemappers.size(); ++i) {
                if (ImGui::Selectable(tonemappers[i].name.data(), i == tonemapperType)) {
                    tonemappers[tonemapperType].cached_settings = settings.tonemapParams;
                    settings.tonemapParams = tonemappers[i].cached_settings;
                    tonemapperType = i;
                    recompileFlag = true;
                }

                if (auto _tt = Util::HoverTooltipWrapper())
                    ImGui::Text(tonemappers[i].desc.data());
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::TextWrapped(tonemappers[tonemapperType].desc.data());
        ImGui::Spacing();
        if (ImGui::Button("Reset", { -1, 0 }))
            settings.tonemapParams = tonemappers[tonemapperType].default_settings;
        ImGui::Spacing();

        ImGui::PushID(tonemapperType);
        tonemappers[tonemapperType].draw_settings_func(settings.tonemapParams);
        ImGui::PopID();

        // (44a) HDR: this page owns the curve in SDR and in HDR.
        {
            const std::string tmName(tonemappers[tonemapperType].name);
            const bool native = HasNativeHDR(tmName);
            const bool isGT7 = tmName == "GT7";
            auto hdr = GetTonemapHDR(tmName);
            const auto levels = HDROutput::GetDisplayLevels();

            ImGui::SeparatorText("HDR");
            ImGui::TextDisabled("HDR output %s. Peak %.0f nits, paper white %.0f nits (peak = %.2f x white). Set on the HDR Output page.",
                levels.hdr ? "on" : "off", levels.peakNits, levels.paperWhiteNits, levels.peakRel);

            const char* modes[] = { "Extend (keeps the SDR look)", "Native" };
            int mode = native && hdr.mode == 1 ? 1 : 0;
            ImGui::BeginDisabled(!native);
            if (ImGui::Combo("HDR Mode", &mode, modes, IM_ARRAYSIZE(modes)))
                hdr.mode = static_cast<uint>(mode);
            ImGui::EndDisabled();
            if (auto _tt = Util::HoverTooltipWrapper()) {
                ImGui::TextUnformatted("How this tone mapper behaves with HDR output on (saved for each tone mapper).");
                ImGui::TextUnformatted("Extend: the picture is exactly the SDR one up to the roll-off start; only brighter highlights go above white, up to the peak.");
                ImGui::TextUnformatted("Native: the tone mapper's own HDR form (Lottes: the curve re-solved for the peak, mid-grey kept;");
                ImGui::TextUnformatted("GT7: its HDR curve matched to its SDR mid-tones; Neutwo: its peak raised). Only Lottes, GT7 and Neutwo have one.");
            }
            const bool kneeUsed = hdr.mode == 0 || !native || isGT7;
            if (kneeUsed) {
                ImGui::SliderFloat("Highlight Roll-off Start", &hdr.knee, 0.3f, 0.9f, "%.2f");
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    if (native && hdr.mode == 1 && isGT7)
                        ImGui::Text("GT7: the curve stays straight up to this fraction of the peak (now %.0f nits), then bends toward it.", hdr.knee * levels.peakNits);
                    else {
                        ImGui::Text("Extend: everything up to this fraction of SDR white (now %.0f nits) is exactly as in SDR;", hdr.knee * levels.paperWhiteNits);
                        ImGui::TextUnformatted("above it highlights bend smoothly up toward the peak. Lower = more highlight punch, higher = closer to SDR.");
                    }
                }
            }
            if (native && hdr.mode == 1 && isGT7) {
                bool reference = hdr.option == 1;
                if (ImGui::Checkbox("Upstream Reference Brightness", &reference))
                    hdr.option = reference ? 1u : 0u;
                if (auto _tt = Util::HoverTooltipWrapper()) {
                    ImGui::TextUnformatted("Off: GT7's HDR curve with the same mid-tones as GT7 in SDR.");
                    ImGui::TextUnformatted("On: GT7's HDR curve as upstream uses it: everything about 2.5x (1.3 stops) brighter than SDR.");
                }
            }
            settings.tonemapHDR[tmName] = hdr;

            if (ImGui::TreeNode("Curve Preview")) {
                curvePreviewOpen = true;
                DrawCurvePreview();
                ImGui::TreePop();
            }
        }
    }

    ImGui::SeparatorText("Game Color Grading");
    ImGui::SliderFloat3("Cinematic Blend", &settings.gameCinematicBlend.x, 0.f, 1.f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Saturation, Brightness and Contrast.");
    ImGui::SliderFloat("Fade Blend", &settings.gameFadeBlend, 0.f, 1.f, "%.3f");
    ImGui::SliderFloat("Tint Blend", &settings.gameTintBlend, 0.f, 1.f, "%.3f");
    ImGui::SeparatorText("Color Space Transform");
    ImGui::Checkbox("Enable Color Space Transform", &settings.enableColorSpaceTransform);
    if (settings.enableColorSpaceTransform) {
        auto& spaces = getAvailableColourSpaces();
        ImGui::Combo("Input Color Space", &settings.inputColorSpace, spaces.data(), (int)spaces.size());
        ImGui::Combo("Process Color Space", &settings.processColorSpace, spaces.data(), (int)spaces.size());
        // (44a) The output space is automatic: sRGB in SDR, BT.2020 with HDR output on.
        settings.outputColorSpace = 0;
        ImGui::Text("Output Color Space: auto (SDR -> sRGB, HDR -> BT.2020)%s", HDROutput::GetDisplayLevels().hdr ? ", now BT.2020" : "");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::TextUnformatted("With HDR output on, the result goes from the process space to BT.2020 through sRGB, so greys match SDR exactly and colours beyond sRGB are kept.");

        auto colorSpaceTransformMatrix = getRGBMatrix(spaces[settings.inputColorSpace], spaces[settings.processColorSpace]);
        auto invColorSpaceTransformMatrix = getRGBMatrix(spaces[settings.processColorSpace], spaces[settings.outputColorSpace]);

        settings.colorSpaceTransform = {
            float3{ colorSpaceTransformMatrix(0, 0), colorSpaceTransformMatrix(0, 1), colorSpaceTransformMatrix(0, 2) },
            float3{ colorSpaceTransformMatrix(1, 0), colorSpaceTransformMatrix(1, 1), colorSpaceTransformMatrix(1, 2) },
            float3{ colorSpaceTransformMatrix(2, 0), colorSpaceTransformMatrix(2, 1), colorSpaceTransformMatrix(2, 2) }
        };

		settings.invColorSpaceTransform = {
			float3{ invColorSpaceTransformMatrix(0, 0), invColorSpaceTransformMatrix(0, 1), invColorSpaceTransformMatrix(0, 2) },
			float3{ invColorSpaceTransformMatrix(1, 0), invColorSpaceTransformMatrix(1, 1), invColorSpaceTransformMatrix(1, 2) },
			float3{ invColorSpaceTransformMatrix(2, 0), invColorSpaceTransformMatrix(2, 1), invColorSpaceTransformMatrix(2, 2) }
		};
    }

	if (ImGui::Button("Save LUT and Output Image")) {
		saveImagesFlag = true;
	}
	ImGui::SameLine();
	ImGui::Text("Output will be saved to: %s", outputPath.c_str());

	ImGui::PopID();
}

void ColorGrading::RestoreDefaultSettings()
{
    settings = {};
	TonemapperInfo::GetDefaultParams(tonemapperType, settings.tonemapParams);
	recompileFlag = true;
}

void ColorGrading::LoadSettings(json& o_json)
{
	try {
		settings = o_json;
    
		auto& tonemappers = TonemapperInfo::GetTonemappers();
		if (auto it = std::ranges::find_if(tonemappers, [&](TonemapperInfo& x) { return settings.currentTonemapper == x.name; });
			it != tonemappers.end()) {
			tonemapperType = (int)(it - tonemappers.begin());
		} else {
			TonemapperInfo::GetDefaultParams(tonemapperType, settings.tonemapParams);
		}

		// (44a) The curve moved here from Upscaling > HDR Output (43b: toneCurve, rolloffStart). Taken
		// once, when these settings have no per-tone-mapper HDR entries yet.
		uint legacyCurve = 1;
		float legacyRolloff = 0.6f;
		const bool hasOwn = o_json.is_object() && o_json.contains("tonemapHDR");
		if (HDROutput::TakeLegacyCurve(legacyCurve, legacyRolloff) && !hasOwn) {
			const std::string current(tonemappers[tonemapperType].name);
			if (legacyCurve == 2) {
				// "GT7 HDR curve" -> GT7 with HDR Mode Native.
				if (auto it = std::ranges::find_if(tonemappers, [](TonemapperInfo& x) { return x.name == "GT7"sv; }); it != tonemappers.end()) {
					const int gt7 = (int)(it - tonemappers.begin());
					if (gt7 != tonemapperType) {
						tonemappers[tonemapperType].cached_settings = settings.tonemapParams;
						settings.tonemapParams = tonemappers[gt7].cached_settings;
						tonemapperType = gt7;
					}
				}
				settings.tonemapHDR["GT7"] = TonemapHDR{ 1u, std::clamp(legacyRolloff, 0.3f, 0.9f), 0u };
				logger::info("[Color Grading] Migrated 43b HDR tone curve: GT7, HDR Mode Native, roll-off {:.2f}", legacyRolloff);
			} else {
				// "Extend my tone mapper" -> the current tone mapper, HDR Mode Extend, same roll-off.
				settings.tonemapHDR[current] = TonemapHDR{ 0u, std::clamp(legacyRolloff, 0.3f, 0.9f), 0u };
				logger::info("[Color Grading] Migrated 43b HDR tone curve: {}, HDR Mode Extend, roll-off {:.2f}", current, legacyRolloff);
			}
		}
		// (44a) The old GT7 "HDR Output" checkbox lived in tonemapParams[0].y; it is gone.
		if (tonemappers[tonemapperType].name == "GT7"sv)
			settings.tonemapParams[0].y = 0.0f;
		settings.outputColorSpace = 0;  // (44a) automatic: sRGB in SDR, BT.2020 in HDR
	} catch (const json::exception& e) {
		logger::error("Failed to load Color Grading settings: {}", e.what());
		RestoreDefaultSettings();
	}

    recompileFlag = true;
}

ColorGrading::TonemapHDR ColorGrading::GetTonemapHDR(std::string_view a_name) const
{
	if (auto it = settings.tonemapHDR.find(std::string(a_name)); it != settings.tonemapHDR.end())
		return it->second;
	TonemapHDR defaults{};
	if (a_name == "Neutwo")
		defaults.mode = 1;  // made for HDR: its Native form is the point of it
	return defaults;
}

bool ColorGrading::HasNativeHDR(std::string_view a_name)
{
	return a_name == "Lottes Filmic/AMD Curve" || a_name == "GT7" || a_name == "Neutwo";
}

void ColorGrading::SaveSettings(json& o_json)
{
    auto& tonemappers = TonemapperInfo::GetTonemappers();
    settings.currentTonemapper = tonemappers[tonemapperType].name.data();
    o_json = settings;
}

void ColorGrading::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		colorCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorCB>());
	}

	logger::debug("Creating 2D textures...");
	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

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

		texColor = std::make_unique<Texture2D>(texDesc);
		texColor->CreateSRV(srvDesc);
		texColor->CreateUAV(uavDesc);

		D3D11_TEXTURE3D_DESC lutTexDesc = {
			.Width = LUTDim,
			.Height = LUTDim,
			.Depth = LUTDim,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		D3D11_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {
			.Format = lutTexDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MostDetailedMip = 0, .MipLevels = lutTexDesc.MipLevels }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC lutUavDesc = {
			.Format = lutTexDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = lutTexDesc.Depth }
		};

		texLUT = std::make_unique<Texture3D>(lutTexDesc);
		texLUT->CreateSRV(lutSrvDesc);
		texLUT->CreateUAV(lutUavDesc);

		// A brand new texture holds undefined contents, so the next Draw must bake.
		lutValid = false;
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	}

}

// (batch 16, item P14) Shader compilation is no longer part of SetupResources, so that
// the memory SetupResources allocates can be handed back while the effect is off
// without paying a D3DCompileFromFile on every re-enable.
void ColorGrading::SetupShaders()
{
	CompileComputeShaders();
}

// (batch 16, item P14) Mirror of SetupResources: every texture/buffer it assigns is
// nulled here. Shaders and sampler states are deliberately kept -- they are a few
// hundred bytes and keeping them is what makes turning the effect back on cheap.
void ColorGrading::ReleaseResources()
{
	colorCB = nullptr;
	texColor = nullptr;
	texLUT = nullptr;
}

void ColorGrading::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&colorgradingCS,
		&lutgenCS,
		&colorgradingHdrCS,
		&lutgenHdrCS
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	// A fresh lutgenCS may bake differently from the one that filled texLUT, so force a rebake.
	lutValid = false;

	CompileComputeShaders();
}

void ColorGrading::CompileComputeShaders()
{
	const auto& tonemappers = TonemapperInfo::GetTonemappers();

	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &colorgradingCS, "colorgrading.cs.hlsl", { { "TONEMAP_FUNC", tonemappers[tonemapperType].func_name.data() } }, "CSColorGrading" },
			{ &lutgenCS, "colorgrading.cs.hlsl", { { "TONEMAP_FUNC", tonemappers[tonemapperType].func_name.data() } }, "CSLUTGen" }
		};

	// (batch 43) The HDR variants only exist in a native-HDR session, so an SDR session compiles exactly
	// what it did before.
	if (HDROutput::IsSession()) {
		std::vector<std::pair<const char*, const char*>> hdrDefines = { { "TONEMAP_FUNC", tonemappers[tonemapperType].func_name.data() }, { "HDR_OUTPUT", "" } };
		if (tonemappers[tonemapperType].name == "GT7"sv)
			hdrDefines.push_back({ "TONEMAP_GT7", "" });  // (44a) GT7's Native form lives in ColorGrading()
		shaderInfos.push_back({ &colorgradingHdrCS, "colorgrading.cs.hlsl", hdrDefines, "CSColorGrading" });
		shaderInfos.push_back({ &lutgenHdrCS, "colorgrading.cs.hlsl", hdrDefines, "CSLUTGen" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\ColorGrading") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

void ColorGrading::Draw(TextureInfo& inout_tex)
{
	auto context = globals::d3d::context;
	auto state = globals::state;

	if (recompileFlag)
		ClearShaderCache();

	state->BeginPerfEvent("Color Grading and Tonemapping");

    auto& pp = globals::features::postProcessing;

    RE::ImageSpaceData imageSpaceData = pp.imageSpaceManager->gameISData;
	bool isInInterior = pp.imageSpaceManager->inInterior;

    auto profile = settings.profiles[0];
    if (settings.useToDInterior) {
        if (isInInterior) {
            profile = settings.profiles[7];
        } else {
            for (int i = 0; i < 6; i++) {
                for (int j = 0; j < 22; j++) {
                    profile.params[j] = (i == 0 ? float4{ 0.f, 0.f, 0.f, 0.f } : profile.params[j]) + settings.profiles[i + 1].params[j] * pp.imageSpaceManager->timeOfDay[i];
                }
            }
        }
    }

    ColorCB colorCBData = {
        .asccdl = {
            profile.params[0],
            profile.params[1],
            profile.params[2]
        },
        .liftgammagain = {
            profile.params[3],
            profile.params[4],
            profile.params[5]
        },
        .saturationHueInOutGamma = profile.params[6],
        .oklchSaturation = profile.params[7],
        .oklchColorMixer = {
            profile.params[8],
            profile.params[9],
            profile.params[10],
            profile.params[11],
            profile.params[12],
            profile.params[13],
            profile.params[14]
        },
		.contrast = profile.params[15],
		.pivot = profile.params[16],
		.exposureTemperatureTint = profile.params[17],
		.shadows = profile.params[18],
		.midtones = profile.params[19],
		.highlights = profile.params[20],
		.shadowsHighlightsRange = profile.params[21],
        .tonemapParams = {
            settings.tonemapParams[0],
            settings.tonemapParams[1]
        },
        .colorSpaceTransform = {
            float4{ settings.colorSpaceTransform[0].x, settings.colorSpaceTransform[0].y, settings.colorSpaceTransform[0].z, 0.f },
            float4{ settings.colorSpaceTransform[1].x, settings.colorSpaceTransform[1].y, settings.colorSpaceTransform[1].z, 0.f },
            float4{ settings.colorSpaceTransform[2].x, settings.colorSpaceTransform[2].y, settings.colorSpaceTransform[2].z, 0.f }
        },
        .invColorSpaceTransform = {
            float4{ settings.invColorSpaceTransform[0].x, settings.invColorSpaceTransform[0].y, settings.invColorSpaceTransform[0].z, 0.f },
            float4{ settings.invColorSpaceTransform[1].x, settings.invColorSpaceTransform[1].y, settings.invColorSpaceTransform[1].z, 0.f },
            float4{ settings.invColorSpaceTransform[2].x, settings.invColorSpaceTransform[2].y, settings.invColorSpaceTransform[2].z, 0.f }
        },
        .cinematic = float4{
            std::lerp(1.f, imageSpaceData.baseData.cinematic.saturation, settings.gameCinematicBlend.x),
            std::lerp(1.f, imageSpaceData.baseData.cinematic.brightness, settings.gameCinematicBlend.y),
            std::lerp(1.f, imageSpaceData.baseData.cinematic.contrast, settings.gameCinematicBlend.z),
            imageSpaceData.baseAmount
        },
		.fade = float4{
			imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeR],
			imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeG],
			imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeB],
			imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeAmount] * settings.gameFadeBlend
		},
		.tint = float4{
			imageSpaceData.baseData.tint.color.red,
			imageSpaceData.baseData.tint.color.green,
			imageSpaceData.baseData.tint.color.blue,
			imageSpaceData.baseData.tint.amount * settings.gameTintBlend
		},
		.logType = settings.useLog ? ((1u << settings.logType) | (settings.invertLog ? (1u << 3u) : 0u)) : 0u,
		.skipLDR = settings.skipLDR,
		.enableTonemap = settings.enableTonemap,
		.enableColorSpaceTransform = settings.enableColorSpaceTransform,
		.hdrParams = float4{ 0.0f, 0.0f, 0.0f, 0.0f },
		.hdrParams2 = float4{ 0.0f, 0.0f, 0.0f, 0.0f },
		.hdrOutputTransform = {}
	};
	auto& tonemapperList = TonemapperInfo::GetTonemappers();
	const std::string_view tonemapperName = tonemapperList[tonemapperType].name;
	// (44a) The old GT7 "HDR Output" checkbox (tonemapParams[0].y) never reaches the shader.
	if (tonemapperName == "GT7"sv)
		colorCBData.tonemapParams[0].y = 0.0f;
	// (batch 43, 44a) HDR: this page's per-tone-mapper HDR Mode, the levels from HDR Output, and the
	// BT.2020 output. Without the HDR_OUTPUT pair (not a 10-bit session, or it failed to compile) the
	// frame is graded as SDR, which the HDR output then shows at paper white.
	const auto levels = HDROutput::GetSceneLevels();
	const bool hdrGrading = levels.hdr && colorgradingHdrCS && lutgenHdrCS;
	if (hdrGrading) {
		const auto hdr = GetTonemapHDR(tonemapperName);
		const bool native = hdr.mode == 1 && HasNativeHDR(tonemapperName);
		colorCBData.hdrParams = float4{ native ? 2.0f : 1.0f, levels.peakRel, std::clamp(hdr.knee, 0.3f, 0.9f), levels.displayGamma ? 1.0f : 0.0f };
		colorCBData.hdrParams2 = float4{ (native && hdr.option == 1) ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f };
		// processing -> BT.2020 = (709 -> 2020) x (processing -> sRGB): greys exactly as in SDR.
		const int process = settings.enableColorSpaceTransform ? settings.processColorSpace : -1;
		if (process != cachedOutputProcessSpace) {
			auto& spaces = getAvailableColourSpaces();
			DirectX::SimpleMath::Matrix toSRGB = DirectX::SimpleMath::Matrix::Identity;
			if (process >= 0 && process < static_cast<int>(spaces.size()))
				toSRGB = getRGBMatrix(spaces[process], "sRGB");
			const DirectX::SimpleMath::Matrix to2020{ DirectX::XMFLOAT3X3{
				0.6274039f, 0.3292830f, 0.0433131f,
				0.0690973f, 0.9195404f, 0.0113623f,
				0.0163914f, 0.0880133f, 0.8955953f } };
			const auto m = DirectX::XMMatrixMultiply(to2020, toSRGB);
			for (int r = 0; r < 3; ++r)
				cachedOutputTransform[r] = float4{ DirectX::XMVectorGetByIndex(m.r[r], 0), DirectX::XMVectorGetByIndex(m.r[r], 1), DirectX::XMVectorGetByIndex(m.r[r], 2), 0.0f };
			cachedOutputProcessSpace = process;
		}
		for (int r = 0; r < 3; ++r)
			colorCBData.hdrOutputTransform[r] = cachedOutputTransform[r];
	}
	colorCB->Update(colorCBData);

	ID3D11Buffer* cb = colorCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);

	std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };
    context->CSSetSamplers(0, 1, samplers.data());
	ID3D11UnorderedAccessView* uav = nullptr;

	// LUT Gen. CSLUTGen is a pure function of ColorCB -- no textures, no frame counter -- so
	// while the settings and the game's imagespace data hold still (the common case: weather
	// and time-of-day move slowly, and both are already smoothed into ColorCB), the bake would
	// produce a bit-identical LUT. Skipping it removes a 64^3 dispatch and, more importantly,
	// the hard dependency between it and the full-screen apply below that samples texLUT.
	// ColorCB is float4s plus a four-uint tail, so it has no padding and memcmp is exact; a
	// false "changed" would only cost a redundant bake, never a stale LUT.
	if (!lutValid || std::memcmp(&bakedColorCBData, &colorCBData, sizeof(ColorCB)) != 0) {
		uav = texLUT->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(hdrGrading ? lutgenHdrCS.get() : lutgenCS.get(), nullptr, 0);
		context->Dispatch(LUTDim >> 3, LUTDim >> 3, LUTDim >> 3);

		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);

		bakedColorCBData = colorCBData;
		lutValid = true;
	}

	// Apply LUT
	std::array<ID3D11ShaderResourceView*, 2> srvs = { inout_tex.srv, texLUT->srv.get() };
	// (batch 37b, C-1) Same shader and inputs; only the store target changes when PostProcessing
	// handed us the game's buffer (same format and size as texColor, checked by the caller).
	const auto directOutput = pp.directOutputFor == this ? pp.directOutput : DirectOutput{};
	const bool direct = directOutput.uav != nullptr;
	uav = direct ? directOutput.uav : texColor->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 2, srvs.data());
	context->CSSetShader(hdrGrading ? colorgradingHdrCS.get() : colorgradingCS.get(), nullptr, 0);

	context->Dispatch((texColor->desc.Width + 7) >> 3, (texColor->desc.Height + 7) >> 3, 1);

	// clean up
	srvs.fill(nullptr);
	uav = nullptr;
	cb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 2, srvs.data());
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);

	// (44a) The output passes must not convert this scene to BT.2020 a second time.
	if (hdrGrading)
		HDROutput::NoteSceneBT2020();

	UpdateCurvePreview();

	if (saveImagesFlag) {
		saveImagesFlag = false;
		OutputTextures();
	}

	if (direct)
		inout_tex = { directOutput.tex, directOutput.srv };
	else
		inout_tex = { texColor->resource.get(), texColor->srv.get() };

	state->EndPerfEvent();
}

void ColorGrading::OutputTextures()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	DirectX::ScratchImage lutImage;
	DirectX::ScratchImage colorImage;

	if (texLUT->resource)
	{
		DirectX::CaptureTexture(device, context, texLUT->resource.get(), lutImage);
	}

	if (texColor->resource)
	{
		DirectX::CaptureTexture(device, context, texColor->resource.get(), colorImage);
	}

	if (std::filesystem::create_directories(outputPath)) {
		logger::info("Missing pp directory created: {}", outputPath);
	}

	std::filesystem::path savePath = outputPath;

	std::filesystem::path lutPath = savePath / "PP_ColorGrading_BakedLUT.dds";
	std::filesystem::path colorPath = savePath / "PP_ColorGrading_ColorOutput.dds";

	DX::ThrowIfFailed(SaveToDDSFile(lutImage.GetImages(), lutImage.GetImageCount(), lutImage.GetMetadata(), DirectX::DDS_FLAGS::DDS_FLAGS_NONE, lutPath.c_str()));
	DX::ThrowIfFailed(SaveToDDSFile(colorImage.GetImages(), colorImage.GetImageCount(), colorImage.GetMetadata(), DirectX::DDS_FLAGS::DDS_FLAGS_NONE, colorPath.c_str()));
}
// (44a) Curve preview. The LUT's neutral diagonal (64 texels) is copied to a small staging texture
// while the preview is open and read two frames later without stalling, so what is drawn is exactly
// what the GPU bakes: the grading, the tone mapper and, with HDR output on, its HDR Mode.
void ColorGrading::UpdateCurvePreview()
{
	if (!curvePreviewOpen || !texLUT)
		return;
	curvePreviewOpen = false;

	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;
	if (!curveStaging) {
		D3D11_TEXTURE3D_DESC desc{};
		desc.Width = LUTDim;
		desc.Height = 1;
		desc.Depth = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateTexture3D(&desc, nullptr, curveStaging.put())))
			return;
	}

	const uint64_t frame = globals::state->frameCount;
	if (curveCopyPending && frame >= curveCopyFrame + 2) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(curveStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			const auto* texels = static_cast<const uint16_t*>(mapped.pData);
			const bool extend = bakedColorCBData.hdrParams.x > 0.5f && bakedColorCBData.hdrParams.x < 1.5f;
			const bool gt7Scaled = bakedColorCBData.hdrParams.x > 1.5f;  // GT7 Native keeps its factor in alpha too
			for (int i = 0; i < LUTDim; ++i) {
				const float r = DirectX::PackedVector::XMConvertHalfToFloat(texels[i * 4 + 0]);
				const float g = DirectX::PackedVector::XMConvertHalfToFloat(texels[i * 4 + 1]);
				const float b = DirectX::PackedVector::XMConvertHalfToFloat(texels[i * 4 + 2]);
				const float a = DirectX::PackedVector::XMConvertHalfToFloat(texels[i * 4 + 3]);
				const float scale = (extend || gt7Scaled) ? std::max(a, 1.0f) : 1.0f;
				curveValues[i] = std::max({ r, g, b }) * scale;
			}
			context->Unmap(curveStaging.get(), 0);
			curveHDR = bakedColorCBData.hdrParams.x > 0.5f;
			curvePeakRel = curveHDR ? std::max(bakedColorCBData.hdrParams.y, 1.0f) : 1.0f;
			curveValid = true;
			curveCopyPending = false;
		}
	}
	if (!curveCopyPending && lutValid) {
		for (UINT i = 0; i < static_cast<UINT>(LUTDim); ++i) {
			const D3D11_BOX box{ i, i, i, i + 1, i + 1, i + 1 };
			context->CopySubresourceRegion(curveStaging.get(), 0, i, 0, 0, texLUT->resource.get(), 0, &box);
		}
		curveCopyPending = true;
		curveCopyFrame = frame;
	}
}

void ColorGrading::DrawCurvePreview()
{
	if (!curveValid) {
		ImGui::TextDisabled("Reading the curve...");
		return;
	}
	// Input of texel i: the LUT's log shaper (CSLUTGen), 14 stops around 0.18.
	auto inputOf = [](int i) {
		const auto logToLinear = [](float v) { return std::exp2((v - 444.0f / 1023.0f) * 14.0f) * 0.18f; };
		return logToLinear((static_cast<float>(i) + 0.5f) / static_cast<float>(LUTDim - 1)) - logToLinear(0.0f);
	};
	const float yMax = std::max(curvePeakRel, 1.0f) * 1.05f;
	const float xMin = std::log2(std::max(inputOf(1), 1e-4f));
	const float xMax = std::log2(inputOf(LUTDim - 1));

	const ImVec2 size{ std::max(ImGui::GetContentRegionAvail().x, 200.0f), 160.0f };
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const ImVec2 p1{ p0.x + size.x, p0.y + size.y };
	ImGui::InvisibleButton("##curve_preview", size);
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255));
	draw->AddRect(p0, p1, IM_COL32(90, 90, 100, 255));
	auto toScreen = [&](float x, float y) {
		const float u = (std::log2(std::max(x, 1e-6f)) - xMin) / (xMax - xMin);
		const float v = std::clamp(y / yMax, 0.0f, 1.0f);
		return ImVec2{ p0.x + std::clamp(u, 0.0f, 1.0f) * size.x, p1.y - v * size.y };
	};
	// Reference lines: SDR white / paper white (1.0) and, in HDR, the peak.
	const float yWhite = p1.y - (1.0f / yMax) * size.y;
	draw->AddLine({ p0.x, yWhite }, { p1.x, yWhite }, IM_COL32(120, 120, 120, 200));
	if (curveHDR) {
		const float yPeak = p1.y - (curvePeakRel / yMax) * size.y;
		draw->AddLine({ p0.x, yPeak }, { p1.x, yPeak }, IM_COL32(200, 150, 60, 200));
	}
	const float xGrey = toScreen(0.18f, 0.0f).x;
	draw->AddLine({ xGrey, p0.y }, { xGrey, p1.y }, IM_COL32(80, 80, 90, 200));
	for (int i = 2; i < LUTDim; ++i)
		draw->AddLine(toScreen(inputOf(i - 1), curveValues[i - 1]), toScreen(inputOf(i), curveValues[i]), IM_COL32(230, 230, 230, 255), 2.0f);
	ImGui::TextDisabled("Neutral grey in (log scale, vertical line = mid grey 0.18) -> out. Grey line = %s white (1.0)%s.",
		curveHDR ? "paper" : "SDR", curveHDR ? std::format(", orange = peak ({:.2f} x)", curvePeakRel).c_str() : "");
}
