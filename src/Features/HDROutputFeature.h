#pragma once

#include "Feature.h"

/**
 * @brief (44a) "HDR Output": native HDR10 output as its own feature (category Display).
 *
 * The work is done by the HDROutput namespace (src/Features/Upscaling/HDROutput.*), which the
 * D3D12 / DLSS-G presentation path calls; this feature owns its settings, its menu page (with
 * the Display / HDR info section) and its shader ini. Settings saved by 43b under
 * Upscaling > hdrOutput are taken over the first time.
 */
struct HDROutputFeature : Feature
{
	virtual inline std::string GetName() override { return "HDR Output"; }
	virtual inline std::string GetShortName() override { return "HDROutput"; }
	virtual inline std::string_view GetCategory() const override { return "Display"; }
	virtual inline bool SupportsVR() override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Native HDR10 output for HDR monitors (DLSS-G frame generation path).",
			{ "10-bit display chain on HDR monitors, HDR switches on and off instantly",
				"Peak, paper white and HUD brightness",
				"HUD on its own layer, menus shown as SDR",
				"Display / HDR diagnostics" }
		};
	}

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;
};
