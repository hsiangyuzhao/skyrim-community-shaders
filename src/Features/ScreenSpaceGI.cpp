#include "ScreenSpaceGI.h"

#include <DirectXTex.h>

#include "Deferred.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	EnableExperimentalSpecularGI,
	NumSlices,
	NumSteps,
	ResolutionMode,
	MinScreenRadius,
	AORadius,
	GIRadius,
	Thickness,
	DepthFadeRange,
	GISaturation,
	GIDistanceCompensation,
	AOPower,
	GIStrength,
	EnableTemporalDenoiser,
	EnableBlur,
	DepthDisocclusion,
	NormalDisocclusion,
	MaxAccumFrames,
	MaxAccumFramesAO,
	BlurRadius,
	DistanceNormalisation,
	EnableContactAo,
	ContactRadius,
	ContactStrength)

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::RestoreDefaultSettings()
{
	settings = {};
	recompileFlag = true;
}

void ScreenSpaceGI::DrawSettings()
{
	static bool showAdvanced;

	if (!ShadersOK())
		ImGui::TextColored({ 1, 0, 0, 1 }, "Compute shaders failed to compile!");

	///////////////////////////////
	ImGui::SeparatorText("Toggles");

	ImGui::Checkbox("Show Advanced Options", &showAdvanced);

	if (ImGui::BeginTable("Toggles", 3)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox("Enabled", &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enable Screen Space Global Illumination. When disabled, all other settings are ignored.");
		}

		ImGui::TableNextColumn();
		{
			auto ilToggleGuard = Util::DisableGuard(!settings.Enabled);
			recompileFlag |= ImGui::Checkbox("Indirect Lighting (IL)", &settings.EnableGI);
		}
		ImGui::TableNextColumn();
		if (showAdvanced) {
			recompileFlag |= ImGui::Checkbox("(Experimental) HQ Specular IL", &settings.EnableExperimentalSpecularGI);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("An experimental specular GI that is more accurate but requires more samples. Won't be blurred.");
		}

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Quality/Performance");

	{
		auto qualityGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::BeginTable("Presets", 5)) {
			ImGui::TableNextColumn();
			if (ImGui::Button("AO only", { -1, 0 })) {
				settings.NumSlices = 1;
				settings.NumSteps = 6;
				settings.EnableBlur = true;
				settings.EnableGI = false;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("1 Slice, 6 Steps, blur enabled, no GI\n");

			ImGui::TableNextColumn();
			if (ImGui::Button("Low", { -1, 0 })) {
				settings.NumSlices = 10;
				settings.NumSteps = 12;
				settings.ResolutionMode = 2;
				settings.EnableBlur = true;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Quarter res and blurry.");

			ImGui::TableNextColumn();
			if (ImGui::Button("Standard", { -1, 0 })) {
				settings.NumSlices = 4;
				settings.NumSteps = 8;
				settings.ResolutionMode = 1;
				settings.EnableBlur = true;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Half res and somewhat stable.");

			ImGui::TableNextColumn();
			if (ImGui::Button("Extreme", { -1, 0 })) {
				settings.NumSlices = 4;
				settings.NumSteps = 8;
				settings.ResolutionMode = 0;
				settings.EnableBlur = true;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Full res and clean.");

			ImGui::TableNextColumn();
			if (ImGui::Button("Reference", { -1, 0 })) {
				settings.NumSlices = 8;
				settings.NumSteps = 10;
				settings.ResolutionMode = 0;
				settings.EnableBlur = true;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Reference mode.");

			ImGui::EndTable();
		}

		if (showAdvanced) {
			ImGui::SliderInt("Slices", (int*)&settings.NumSlices, 1, 10);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How many directions do the samples take.\n"
					"Controls noise.");

			ImGui::SliderInt("Steps Per Slice", (int*)&settings.NumSteps, 1, 20);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How many samples does it take in one direction.\n"
					"Controls accuracy of lighting, and noise when effect radius is large.");
		}

		if (ImGui::BeginTable("Less Work", 3)) {
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::RadioButton("Full Res", &settings.ResolutionMode, 0);
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::RadioButton("Half Res", &settings.ResolutionMode, 1);
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::RadioButton("Quarter Res", &settings.ResolutionMode, 2);

			ImGui::EndTable();
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual");

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);

		ImGui::SliderFloat("AO Power", &settings.AOPower, 0.f, 6.f, "%.2f");

		{
			auto ilGuard = Util::DisableGuard(!settings.EnableGI);
			ImGui::SliderFloat("IL Source Brightness", &settings.GIStrength, 0.f, 6.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"1.0 is one energy-correct screen-space bounce: the integrator is",
					"analytically normalised, so a surface fully enclosed by unit radiance",
					"receives exactly its own albedo.",
					"Because the vanilla/Environment Ambient term is still present and already",
					"contains indirect light, the visually balanced value is usually below 1.",
					"Settings carried over from before the normalisation need roughly 5x their",
					"old value to look the same."
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}

		ImGui::Separator();

		ImGui::SliderFloat("AO radius", &settings.AORadius, 10.f, 1024.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				"A smaller radius produces tighter AO.",
				Util::Units::FormatDistance(settings.AORadius)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		{
			auto ilRadiusGuard = Util::DisableGuard(!settings.EnableGI);

			ImGui::SliderFloat("IL radius", &settings.GIRadius, 10.f, 1024.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"A larger radius produces wider IL.",
					Util::Units::FormatDistance(settings.GIRadius)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}

		if (showAdvanced) {
			ImGui::SliderFloat("Min Screen Radius", &settings.MinScreenRadius, 0.f, 0.05f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"The minimum screen-space effect radius as proportion of display width, to prevent far field AO being too small.");
		}

		ImGui::SliderFloat2("Depth Fade Range", &settings.DepthFadeRange.x, 1e4, 5e4, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				"Distance range where depth-based effects fade out.",
				"Near: " + Util::Units::FormatDistance(settings.DepthFadeRange.x),
				"Far: " + Util::Units::FormatDistance(settings.DepthFadeRange.y)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		if (showAdvanced) {
			ImGui::Separator();

			ImGui::SliderFloat("Thickness", &settings.Thickness, 0.f, 0.5f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"How thick the occluders are, as a fraction of view depth.",
					"Relative rather than absolute, so one value holds at every distance;",
					"the old 32-unit default corresponds to 0.1 at around 320 units of depth.",
					"Affects both AO and indirect light."
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual - IL");

	{
		auto visualILGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI);

		if (showAdvanced) {
			ImGui::SliderFloat("IL Distance Compensation", &settings.GIDistanceCompensation, -5.0f, 5.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brighten/Dimming further radiance samples.");

			ImGui::Separator();
		}

		Util::PercentageSlider("IL Saturation", &settings.GISaturation);
	}

	///////////////////////////////
	ImGui::SeparatorText("Contact AO");

	{
		auto contactGuard = Util::DisableGuard(!settings.Enabled);

		recompileFlag |= ImGui::Checkbox("Enable Contact AO", &settings.EnableContactAo);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				"Adds the very small-scale shadows the main occlusion above is too coarse to see:",
				"where hair touches a face, where clothing meets skin, where a window frame meets",
				"the wall behind it.",
				"",
				"It always runs at full resolution, even when the setting above says half or",
				"quarter, because a few centimetres is smaller than one pixel of a half-resolution",
				"image. It has its own small smoothing over the last few frames, so it does not",
				"sparkle and does not need anti-aliasing or upscaling to look right.",
				"",
				"The result is mixed into the same occlusion the rest of the game already uses, so",
				"everything that reacts to occlusion picks it up automatically.",
				"",
				"Turning this off is free: nothing is computed and nothing is stored."
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		// (P2.4 follow-up) The one place a contact AO problem is allowed to be reported, now that
		// it can no longer masquerade as a whole-feature compile failure. Deliberately after the
		// tooltip block rather than straight after the checkbox: HoverTooltipWrapper tests the
		// *last* submitted item, so a widget between the two would steal the checkbox's tooltip.
		// recompileFlag also has to be clear, so the line does not flash during the one frame
		// between ticking the box and the rebuild that answers it.
		if (settings.EnableContactAo && !contactAoActive && !recompileFlag)
			ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "Contact AO shader failed to compile; the rest of SSGI is running.");

		{
			auto contactValueGuard = Util::DisableGuard(!settings.EnableContactAo);

			ImGui::SliderFloat("Contact Radius", &settings.ContactRadius, 2.0f, 60.0f, "%.1f cm", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"How far this looks for something touching the surface, in real-world",
					"centimetres. 15 cm is roughly the scale of a strand of hair against a cheek or",
					"a fold of cloth against skin.",
					"",
					"Raising it starts doing the main occlusion's job with far fewer samples, which",
					"looks less steady, not better. If you want wider shadows, use AO radius above."
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}

			ImGui::SliderFloat("Contact Strength", &settings.ContactStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"How dark these small contacts get. At 1.0 a tight contact goes fully dark and",
					"an ordinary inside corner lands around 40% -- the upper half of the slider is",
					"headroom, not the working range.",
					"",
					"0.0 leaves the shadows out but still pays for them; use the checkbox above to",
					"turn it off properly."
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Denoising");

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::BeginTable("denoisers", 2)) {
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::Checkbox("Temporal Denoiser", &settings.EnableTemporalDenoiser);

			ImGui::TableNextColumn();
			ImGui::Checkbox("Blur", &settings.EnableBlur);

			ImGui::EndTable();
		}

		if (showAdvanced) {
			ImGui::Separator();

			{
				auto temporalGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser);
				ImGui::SliderInt("Max Frame Accumulation", (int*)&settings.MaxAccumFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("How many past frames to accumulate results with. Higher values are less noisy but potentially cause ghosting.");

				ImGui::SliderInt("Max Frame Accumulation (AO)", (int*)&settings.MaxAccumFramesAO, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"Same as above, but for the ambient occlusion channel only.\n\n"
						"AO is multiplicative and gets no spatial filtering of its own, so a long "
						"temporal window drags a moving object's occlusion into a dark trail behind it. "
						"4 is a compromise between that trailing and the flickering that returns at 1. "
						"Set this equal to Max Frame Accumulation to restore the previous behaviour.");
			}

			ImGui::Separator();

			{
				auto disocclusionGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser && !settings.EnableGI);

				Util::PercentageSlider("Movement Disocclusion", &settings.DepthDisocclusion, 0.f, 20.f);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"If a pixel has moved too far from the last frame, its radiance will not be carried to this frame.\n"
						"Lower values are stricter.");

				ImGui::Separator();
			}

			{
				auto blurGuard = Util::DisableGuard(!settings.EnableBlur);
				ImGui::SliderFloat("Blur Radius", &settings.BlurRadius, 0.f, 30.f, "%.1f px");

				if (showAdvanced) {
					ImGui::SliderFloat("Geometry Weight", &settings.DistanceNormalisation, 0.f, 5.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text(
							"Higher value makes the blur more sensitive to differences in geometry.");
				}
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texNoise, debugRescale)
		BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
		BUFFER_VIEWER_NODE(texPrevGeo, debugRescale)
		BUFFER_VIEWER_NODE(texRadiance, debugRescale)
		BUFFER_VIEWER_NODE(texAo[0], debugRescale)
		BUFFER_VIEWER_NODE(texAo[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[1], debugRescale)
		BUFFER_VIEWER_NODE(texContactAo[0], debugRescale)
		BUFFER_VIEWER_NODE(texContactAo[1], debugRescale)

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::LoadSettings(json& o_json)
{
	settings = o_json;

	recompileFlag = true;
}

void ScreenSpaceGI::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		ssgiCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSGICB>());
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

		{
			texRadiance = eastl::make_unique<Texture2D>(texDesc);
			texRadiance->CreateSRV(srvDesc);
			texRadiance->CreateUAV(uavDesc);  // Create default UAV for mip 0

			// Create individual UAVs for each mip level for prefiltering
			for (uint i = 0; i < 5; ++i) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = DXGI_FORMAT_R11G11B10_FLOAT,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texRadiance->resource.get(), &mipUavDesc, uavRadiance[i].put()));
			}

			// Create temporary texture for prefiltering (single mip level, used as SRV input)
			D3D11_TEXTURE2D_DESC tempTexDesc = texDesc;
			tempTexDesc.MipLevels = 1;
			tempTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

			D3D11_SHADER_RESOURCE_VIEW_DESC tempSrvDesc = {
				.Format = DXGI_FORMAT_R11G11B10_FLOAT,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MostDetailedMip = 0,
					.MipLevels = 1 }
			};

			texRadianceTemp = eastl::make_unique<Texture2D>(tempTexDesc);
			texRadianceTemp->CreateSRV(tempSrvDesc);
		}

		texDesc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;

		{
			texWorkingDepth = eastl::make_unique<Texture2D>(texDesc);
			texWorkingDepth->CreateSRV(srvDesc);
			for (int i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth[i].put()));
			}
		}

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		{
			texIlY[0] = eastl::make_unique<Texture2D>(texDesc);
			texIlY[0]->CreateSRV(srvDesc);
			texIlY[0]->CreateUAV(uavDesc);

			texIlY[1] = eastl::make_unique<Texture2D>(texDesc);
			texIlY[1]->CreateSRV(srvDesc);
			texIlY[1]->CreateUAV(uavDesc);

			texGiSpecular[0] = eastl::make_unique<Texture2D>(texDesc);
			texGiSpecular[0]->CreateSRV(srvDesc);
			texGiSpecular[0]->CreateUAV(uavDesc);

			texGiSpecular[1] = eastl::make_unique<Texture2D>(texDesc);
			texGiSpecular[1]->CreateSRV(srvDesc);
			texGiSpecular[1]->CreateUAV(uavDesc);
		}
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		{
			texIlCoCg[0] = eastl::make_unique<Texture2D>(texDesc);
			texIlCoCg[0]->CreateSRV(srvDesc);
			texIlCoCg[0]->CreateUAV(uavDesc);

			texIlCoCg[1] = eastl::make_unique<Texture2D>(texDesc);
			texIlCoCg[1]->CreateSRV(srvDesc);
			texIlCoCg[1]->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8_UNORM;
		{
			texAo[0] = eastl::make_unique<Texture2D>(texDesc);
			texAo[0]->CreateSRV(srvDesc);
			texAo[0]->CreateUAV(uavDesc);

			texAo[1] = eastl::make_unique<Texture2D>(texDesc);
			texAo[1]->CreateSRV(srvDesc);
			texAo[1]->CreateUAV(uavDesc);

			texAccumFrames[0] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[0]->CreateSRV(srvDesc);
			texAccumFrames[0]->CreateUAV(uavDesc);

			texAccumFrames[1] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[1]->CreateSRV(srvDesc);
			texAccumFrames[1]->CreateUAV(uavDesc);

			// (contact AO) Full resolution regardless of ResolutionMode -- these are the only SSGI
			// buffers whose *used* extent does not follow it. One byte per texel, so the pair costs
			// about 7 MB at 1440p and 17 MB at 4K. Never cleared: the neighbourhood fence in
			// contactAo.cs.hlsl drags whatever they contain into the current frame's own range
			// before it is used, so there is no uninitialised-history state to defend against.
			texContactAo[0] = eastl::make_unique<Texture2D>(texDesc);
			texContactAo[0]->CreateSRV(srvDesc);
			texContactAo[0]->CreateUAV(uavDesc);

			texContactAo[1] = eastl::make_unique<Texture2D>(texDesc);
			texContactAo[1]->CreateSRV(srvDesc);
			texContactAo[1]->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		{
			texPrevGeo = eastl::make_unique<Texture2D>(texDesc);
			texPrevGeo->CreateSRV(srvDesc);
			texPrevGeo->CreateUAV(uavDesc);
		}
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\ScreenSpaceGI\\fast_2uges.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(srvDesc);
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
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
	}

	CompileComputeShaders();
}

void ScreenSpaceGI::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&prefilterDepthsCompute, &prefilterRadianceCompute, &radianceDisoccCompute, &giCompute, &blurCompute, &upsampleCompute, &contactAoCompute
	};

	for (auto shader : shaderPtrs)
		*shader = nullptr;

	CompileComputeShaders();
}

void ScreenSpaceGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	auto compile = [](std::string_view filename, const std::vector<std::pair<const char*, const char*>>& defines) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / filename;
		return reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0"));
	};

	// The permutation defines every pass in one compile round shares. Built by one function so
	// that the round is internally consistent by construction -- in particular on CONTACT_AO,
	// which upsample.cs.hlsl uses to decide whether to read a texture only contactAo.cs.hlsl
	// writes. A round that gave the two different answers would bind an unwritten SRV.
	auto commonDefines = [this](bool contactAo) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (REL::Module::IsVR())
			defines.push_back({ "VR", "" });
		if (settings.ResolutionMode == 1)
			defines.push_back({ "HALF_RES", "" });
		if (settings.ResolutionMode == 2)
			defines.push_back({ "QUARTER_RES", "" });
		if (settings.EnableTemporalDenoiser)
			defines.push_back({ "TEMPORAL_DENOISER", "" });
		if (settings.EnableGI)
			defines.push_back({ "GI", "" });
		if (settings.EnableExperimentalSpecularGI)
			defines.push_back({ "GI_SPECULAR", "" });
		// Only upsample.cs.hlsl reads this; contactAo.cs.hlsl derives its own composite gate from
		// the resolution defines instead. Handed to the whole set rather than to one entry because
		// an unreferenced macro changes no bytecode, and singling out one shader here has been the
		// source of more mistakes in this file than it has saved lines.
		if (contactAo)
			defines.push_back({ "CONTACT_AO", "" });
		return defines;
	};

	// (contact AO) Resolved first, and the answer drives the shared define, so contact AO can
	// never cost more than itself.
	//
	// (P2.4 follow-up) The previous shape put contactAoCompute in ShadersOK() instead, which made
	// the pass load-bearing for the entire feature and produced a false "Compute shaders failed to
	// compile!" the moment the setting was switched on: the recompile that would have built the
	// shader was itself gated behind ShadersOK() in DrawSSGI, so turning the checkbox on left
	// SSGI permanently dead with nothing in the log, because nothing had ever been compiled. The
	// availability of one optional pass is now a property this function decides and records,
	// rather than a verdict on the whole feature.
	contactAoCompute = nullptr;
	contactAoActive = false;
	if (settings.EnableContactAo) {
		if (auto rawPtr = compile("contactAo.cs.hlsl", commonDefines(true))) {
			contactAoCompute.attach(rawPtr);
			contactAoActive = true;
		} else {
			logger::error("ScreenSpaceGI: contact AO compute shader failed to compile; running without contact AO. The rest of SSGI is unaffected.");
		}
	}

	const auto defines = commonDefines(contactAoActive);

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", { { "LINEAR_FILTER", "" } } },
			{ &prefilterRadianceCompute, "prefilterRadiance.cs.hlsl", {} },
			{ &radianceDisoccCompute, "radianceDisocc.cs.hlsl", {} },
			{ &giCompute, "gi.cs.hlsl", {} },
			{ &blurCompute, "blur.cs.hlsl", {} },
			{ &upsampleCompute, "upsample.cs.hlsl", {} },
		};

	for (auto& info : shaderInfos) {
		info.defines.insert(info.defines.end(), defines.begin(), defines.end());
		if (auto rawPtr = compile(info.filename, info.defines))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceGI::ShadersOK()
{
	// (P2.4 follow-up) Deliberately no contact AO term. Contact AO is an optional pass that
	// degrades on its own (see contactAoActive), so a problem confined to it must not read as
	// "SSGI's compute shaders failed to compile" and must not take indirect lighting and ambient
	// occlusion down with it.
	return texNoise && prefilterDepthsCompute && prefilterRadianceCompute && radianceDisoccCompute && giCompute && blurCompute && upsampleCompute;
}

void ScreenSpaceGI::UpdateSB()
{
	float2 res = { (float)texRadiance->desc.Width, (float)texRadiance->desc.Height };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	static float4x4 prevInvView[2] = {};

	SSGICB data;
	{
		for (int eyeIndex = 0; eyeIndex < (1 + REL::Module::IsVR()); ++eyeIndex) {
			auto eye = Util::GetCameraData(eyeIndex);

			data.PrevInvViewMat[eyeIndex] = prevInvView[eyeIndex];
			data.NDCToViewMul[eyeIndex] = { 2.0f / eye.projMat(0, 0), -2.0f / eye.projMat(1, 1) };
			data.NDCToViewAdd[eyeIndex] = { -1.0f / eye.projMat(0, 0), 1.0f / eye.projMat(1, 1) };
			if (REL::Module::IsVR())
				data.NDCToViewMul[eyeIndex].x *= 2;

			prevInvView[eyeIndex] = eye.viewMat.Invert();
		}

		data.TexDim = res;
		data.RcpTexDim = float2(1.0f) / res;
		data.FrameDim = dynres;
		data.RcpFrameDim = float2(1.0f) / dynres;
		data.FrameIndex = globals::state->frameCount;

		data.NumSlices = settings.NumSlices;
		data.NumSteps = settings.NumSteps;
		data.MinScreenRadius = settings.MinScreenRadius * dynres.x;

		data.EffectRadius = std::max(settings.AORadius, settings.GIRadius);
		data.AORadius = settings.AORadius / data.EffectRadius;
		data.GIRadius = settings.GIRadius / data.EffectRadius;
		data.Thickness = settings.Thickness;
		data.DepthFadeRange = settings.DepthFadeRange;
		data.DepthFadeScaleConst = 1 / (settings.DepthFadeRange.y - settings.DepthFadeRange.x);

		data.GISaturation = settings.GISaturation;
		data.GIDistanceCompensation = settings.GIDistanceCompensation;
		data.GICompensationMaxDist = settings.AORadius;

		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;

		data.DepthDisocclusion = settings.DepthDisocclusion;
		data.NormalDisocclusion = settings.NormalDisocclusion;
		data.MaxAccumFrames = settings.MaxAccumFrames;
		data.MaxAccumFramesAO = settings.MaxAccumFramesAO;
		data.BlurRadius = settings.BlurRadius;
		data.DistanceNormalisation = settings.DistanceNormalisation;

		data.ContactRadius = settings.ContactRadius;
		data.ContactStrength = settings.ContactStrength;
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI()
{
	auto context = globals::d3d::context;

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISSAOBlurH, imageSpaceManager);

	// Disable vanilla SSAO
	bool* enableSSAO = reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(BSImagespaceShaderISSAOBlurH.get()) + 0x50LL);
	*enableSSAO = false;

	// (P2.4 follow-up) The pending recompile is consumed *before* anything can gate on
	// ShadersOK(), and this ordering is load-bearing rather than tidiness.
	//
	// recompileFlag is set by the settings that change the shader permutation, and some of those
	// settings invalidate ShadersOK() by definition -- switching a pass on means the shader for it
	// does not exist yet. Consuming the flag after the guard below therefore deadlocked: the guard
	// returned, the recompile never ran, ShadersOK() stayed false, and the feature reported a
	// compile failure for a permutation it had never attempted. Recovery required a restart or the
	// global shader-cache button. Nothing about the flag's meaning depends on the feature being
	// enabled or on the current shaders being good, so it is honoured unconditionally; the flag is
	// cleared inside the recompile, so a genuine compile failure cannot spin here.
	if (recompileFlag)
		ClearShaderCache();

	if (!(settings.Enabled && ShadersOK())) {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texAo[outputAoIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texIlY[outputIlIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texIlCoCg[outputIlIdx]->uav.get(), clr);
		return;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSGI");

	static uint lastFrameAoTexIdx = 0;
	static uint lastFrameGITexIdx = 0;
	static uint lastFrameAccumTexIdx = 0;
	uint inputAoTexIdx = lastFrameAoTexIdx;
	uint inputGITexIdx = lastFrameGITexIdx;

	//////////////////////////////////////////////////////

	UpdateSB();

	//////////////////////////////////////////////////////

	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = globals::deferred;

	float2 size = Util::ConvertToDynamic(globals::state->screenSize);
	auto resolution = std::array{ (uint)size.x, (uint)size.y };
	auto resChoices = std::array{
		resolution, std::array{ resolution[0] >> 1, resolution[1] >> 1 }, std::array{ resolution[0] >> 2, resolution[1] >> 2 }
	};
	auto internalRes = resChoices[settings.ResolutionMode];

	std::array<ID3D11ShaderResourceView*, 11> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 6> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssgiCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	//////////////////////////////////////////////////////

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// prefilter depths
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Depths");

		srvs.at(0) = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
		for (int i = 0; i < 5; ++i)
			uavs.at(i) = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);
	}

	// fetch radiance and disocclusion
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Radiance Disocc");

		resetViews();
		srvs.at(0) = rts[deferred->forwardRenderTargets[0]].SRV;
		srvs.at(1) = texWorkingDepth->srv.get();
		srvs.at(2) = rts[NORMALROUGHNESS].SRV;
		srvs.at(3) = texPrevGeo->srv.get();
		srvs.at(4) = rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
		srvs.at(5) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(6) = texAo[inputAoTexIdx]->srv.get();
		srvs.at(7) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(8) = texIlCoCg[inputGITexIdx]->srv.get();
		srvs.at(9) = texGiSpecular[inputAoTexIdx]->srv.get();
		srvs.at(10) = nullptr;

		uavs.at(0) = texRadiance->uav.get();
		uavs.at(1) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
		uavs.at(2) = texAo[!inputAoTexIdx]->uav.get();
		uavs.at(3) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(4) = texIlCoCg[!inputGITexIdx]->uav.get();
		uavs.at(5) = texGiSpecular[!inputAoTexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(radianceDisoccCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		// Prefilter radiance texture instead of using GenerateMips for proper dynamic resolution handling
		{
			TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Radiance");

			// First copy mip 0 from radiance to temporary texture to avoid read/write conflict
			context->CopySubresourceRegion(
				texRadianceTemp->resource.get(), 0, 0, 0, 0,
				texRadiance->resource.get(), 0, nullptr);

			resetViews();
			srvs.at(0) = texRadianceTemp->srv.get();  // Use temporary texture as input
			uavs.at(0) = uavRadiance[0].get();        // Mip 0
			uavs.at(1) = uavRadiance[1].get();        // Mip 1
			uavs.at(2) = uavRadiance[2].get();        // Mip 2
			uavs.at(3) = uavRadiance[3].get();        // Mip 3
			uavs.at(4) = uavRadiance[4].get();        // Mip 4

			context->CSSetShaderResources(0, 1, srvs.data());
			context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
			context->CSSetShader(prefilterRadianceCompute.get(), nullptr, 0);
			context->Dispatch((internalRes[0] + 15u) >> 4, (internalRes[1] + 15u) >> 4, 1);
		}

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// GI
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - GI");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = texRadiance->srv.get();
		srvs.at(3) = texNoise->srv.get();
		srvs.at(4) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(5) = texAo[inputAoTexIdx]->srv.get();
		srvs.at(6) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(7) = texIlCoCg[inputGITexIdx]->srv.get();
		srvs.at(8) = texGiSpecular[inputAoTexIdx]->srv.get();

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
		uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();
		uavs.at(4) = texPrevGeo->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(giCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAoTexIdx = inputAoTexIdx;
	}

	// blur
	if (settings.EnableBlur) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Diffuse Blur");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(3) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(4) = texIlCoCg[inputGITexIdx]->srv.get();

		uavs.at(0) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(blurCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// contact AO
	//
	// Placed after the GI/blur chain and before the upsample, because in half and quarter res the
	// upsample is the pass that folds this term into the AO channel and therefore has to be able to
	// read the result. Runs at the full render extent in every resolution mode.
	//
	// Note which AO buffer this must NOT write: texAo[inputAoTexIdx] is the pure GI occlusion the
	// GI pass just produced, and radianceDisocc.cs.hlsl reads it as *history* next frame
	// (lastFrameAoTexIdx). Folding contact occlusion into it would feed the term back through
	// SSGI's own temporal chain and compound it once per frame. Full-resolution mode therefore
	// composites into the other slot of the pair, which is free at this point in the frame, and
	// leaves inputAoTexIdx alone.
	static uint lastFrameContactIdx = 0;
	uint contactIdx = lastFrameContactIdx;
	uint aoOutIdx = inputAoTexIdx;

	// (P2.4 follow-up) contactAoActive, not settings.EnableContactAo: the setting says what the
	// user asked for, this says what the current compile round actually produced, and it is what
	// upsample.cs.hlsl was given CONTACT_AO for. The two can only differ when the shader failed to
	// compile, and in that case the pass must be skipped rather than dispatched with a null shader.
	if (contactAoActive) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Contact AO");

		resetViews();
		srvs.at(0) = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
		srvs.at(3) = texContactAo[contactIdx]->srv.get();

		uavs.at(0) = texContactAo[!contactIdx]->uav.get();

		if (settings.ResolutionMode == 0) {
			srvs.at(4) = texAo[inputAoTexIdx]->srv.get();
			uavs.at(1) = texAo[!inputAoTexIdx]->uav.get();
			aoOutIdx = !inputAoTexIdx;
		}

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(contactAoCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

		contactIdx = !contactIdx;
		lastFrameContactIdx = contactIdx;
	}

	// upsample
	if (settings.ResolutionMode != 0) {
		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = texAo[inputAoTexIdx]->srv.get();
		srvs.at(2) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(3) = texIlCoCg[inputGITexIdx]->srv.get();
		srvs.at(4) = texGiSpecular[inputAoTexIdx]->srv.get();
		if (contactAoActive)
			srvs.at(5) = texContactAo[contactIdx]->srv.get();

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
		uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(upsampleCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		aoOutIdx = inputAoTexIdx;
	}

	outputAoIdx = aoOutIdx;
	outputIlIdx = inputGITexIdx;
	outputSpecularIdx = inputAoTexIdx;

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
}