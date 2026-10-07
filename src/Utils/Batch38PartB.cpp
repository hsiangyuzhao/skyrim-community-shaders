// Batch 38, part B: table rows for items 3 (skin SSS upgrade), 4 (Local Exposure) and
// 5 (Skylighting fixes). Kept apart from Batch38.cpp so part A's rows merge without conflicts.
#include "Utils/Batch38.h"

#include <chrono>
#include <format>
#include <string>

#include "Features/PostProcessing.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Globals.h"

namespace Batch38
{
	namespace
	{
		std::string OnOff(bool a_on) { return a_on ? "On" : "Off"; }

		Row Toggle(const char* a_group, const char* a_name, bool a_installed, bool& a_value, std::string a_nowText, const char* a_where)
		{
			Row r{ a_group, a_name, a_installed, a_value, OnOff(a_value), std::move(a_nowText), a_where };
			r.toggle = &a_value;
			return r;
		}

		Row Fix(const char* a_group, const char* a_name, const char* a_where)
		{
			Row r{ a_group, a_name, true, true, "(fix, always on)", "On", a_where };
			r.governed = false;
			return r;
		}
	}

	void RowsSkinSss(std::vector<Row>& a_rows)
	{
		auto& sss = globals::features::subsurfaceScattering;
		auto& s = sss.settings;
		const char* group = "3. Skin SSS upgrade";
		const char* where = "Characters > Subsurface Scattering > Settings";

		std::string now;
		if (s.Batch38Upgrade) {
			const bool recent = sss.lastDrawTime != std::chrono::steady_clock::time_point{} &&
			                    std::chrono::steady_clock::now() - sss.lastDrawTime < std::chrono::seconds(1);
			now = !recent ? "Idle: no faces drawn" : (sss.lastDrawUsedUpgrade ? "On" : "Off");
		}
		a_rows.push_back(Toggle(group, "SSS upgrade: pre-pass, scatter modes, LL albedo fix (98ffc7f66, 2f93cb9a1)", sss.loaded, s.Batch38Upgrade, now, where));

		static constexpr const char* kModes[] = { "Pre-scatter", "Post-scatter", "Pre and Post" };
		const char* mode = kModes[std::clamp(s.ScatterMode, 0, 2)];
		a_rows.push_back({ group, "  Albedo handling (Separable only)", sss.loaded, s.Batch38Upgrade, mode,
			s.SSMode == 0 ? std::string(mode) : std::string("n/a: Burley in use"), where });

		a_rows.push_back(Fix(group, "Screen-space shadows read the shaded pixel, not its neighbour (8b91a37de)", "-"));
		a_rows.push_back(Fix(group, "No crash when the IsBeastRace keyword is missing (c321154fa)", "-"));
	}

	void RowsLocalExposure(std::vector<Row>& a_rows)
	{
		auto& pp = globals::features::postProcessing;
		const char* group = "4. Local Exposure";
		const char* where = "Post-Processing > Local Exposure (list entry; the bars button opens its settings)";

		auto* le = pp.loaded ? static_cast<LocalExposure*>(pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::LocalExposure)].get()) : nullptr;
		if (!le) {
			a_rows.push_back({ group, "Local Exposure", false, false, "", "", where });
			return;
		}

		std::string now;
		if (le->enabled)
			now = le->lastDrawRan ? std::string(le->lastDrawStatus) : std::format("Idle: {}", le->lastDrawStatus);
		a_rows.push_back(Toggle(group, "Local Exposure (upstream 86ae0fb3c..b4eb15b70)", true, le->enabled, now, where));

		const auto& s = le->settings;
		a_rows.push_back({ group, "  Strength / highlight / shadow contrast", true, le->enabled,
			std::format("{:.2f} / {:.2f} / {:.2f}", s.Strength, s.HighlightContrast, s.ShadowContrast), "", where });

		std::string res = "-";
		if (le->texOutput)
			res = std::format("{}x{} (output resolution, after upscaling)", le->texOutput->desc.Width, le->texOutput->desc.Height);
		a_rows.push_back({ group, "  Runs at", true, le->enabled, "", res, "-" });
	}

	void RowsSkylighting(std::vector<Row>& a_rows)
	{
		auto& sky = globals::features::skylighting;
		auto& s = sky.settings;
		const char* group = "5. Skylighting fixes";
		const char* where = "Lighting > Skylighting > Fixes (Batch 38)";
		const bool on = sky.loaded;

		a_rows.push_back(Toggle(group, "Roofs flagged as editor markers block the sky (ca63a41d5)", on, s.FixRoofMarkers, "", where));
		a_rows.push_back(Toggle(group, "Reset puts probes back to open sky (5b5361f53, probe part)", on, s.FixResetClearsProbes, "", where));
		a_rows.push_back(Toggle(group, "Max Zenith kept within 0-90 deg (bff82b03e)", on, s.FixZenithClamp,
			s.FixZenithClamp ? std::format("On ({:.0f} deg)", sky.EffectiveMaxZenith() * 57.29578f) : std::string(), where));
		a_rows.push_back(Toggle(group, "Sampling cone radius sin(zenith) (4b5b99783)", on, s.FixZenithRadius, "", where));
		a_rows.push_back(Toggle(group, "Edge fade-out from the probe grid's centre (4b5b99783)", on, s.FixFadeOutGridOffset, "", where));
		a_rows.push_back(Toggle(group, "Height map skips objects below the probe grid (816888f04)", on, s.SkipOccludersBelowGrid,
			s.SkipOccludersBelowGrid ? std::format("On ({} skipped last map)", sky.occludersSkippedBelowGridLast) : std::string(), where));
	}
}
