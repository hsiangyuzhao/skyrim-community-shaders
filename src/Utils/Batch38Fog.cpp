// (batch 38, A) Rows of the Batch 38 tab for item 1 (volumetric fog) and item 2 (smoke and
// effects receiving sun shadow). Kept apart from Batch38.cpp so the B items can add their own
// provider without touching this file.
#include "Utils/Batch38.h"

#include <format>
#include <string>

#include "Features/ExponentialHeightFog.h"
#include "Features/VolumetricLighting.h"
#include "Features/VolumetricShadows.h"
#include "Globals.h"
#include "State.h"

namespace Batch38
{
	void RowsFogAndShadows(std::vector<Row>& a_rows)
	{
		const auto onOff = [](bool b) { return std::string(b ? "On" : "Off"); };

		// ---- A1. Volumetric fog ------------------------------------------------------------
		{
			auto& fog = globals::features::exponentialHeightFog;
			const char* group = "A1. Volumetric fog (default off)";
			const char* where = "Lighting > Exponential Height Fog > Volumetric Fog (Batch 38)";

			std::string now;
			if (fog.volumetric.Enabled) {
				if (!fog.settings.enabled)
					now = "Idle: height fog is off";
				else if (const auto reason = fog.VolumetricFogIdleReason(); !reason.empty())
					now = "Idle: " + reason;
				else
					now = std::format("On, {}x{}x{} + {}x{}x{}", fog.currentGridSize.x, fog.currentGridSize.y, fog.currentGridSize.z,
						fog.currentFarGridSize.x, fog.currentFarGridSize.y, fog.currentFarGridSize.z);
			}
			Row r{ group, "Volumetric fog (sun, sky, nearby lights)", fog.loaded, fog.volumetric.Enabled, onOff(fog.volumetric.Enabled), now, where };
			r.toggle = &fog.volumetric.Enabled;
			a_rows.push_back(std::move(r));

			a_rows.push_back({ group, "  Temporal history weight", fog.loaded, fog.volumetric.Enabled,
				std::format("{:.2f}", fog.volumetric.HistoryWeight), "", where });

			const bool built = fog.lastBuildFrame != UINT32_MAX && globals::state->frameCount - fog.lastBuildFrame <= 1u;
			a_rows.push_back({ group, "  Sun shadow in the fog (shared capture)", fog.loaded, fog.volumetric.Enabled,
				"(automatic)", fog.volumetric.Enabled ? (built ? onOff(fog.lastBuildHadShadows) : std::string("-")) : std::string(), where });

			auto& vl = globals::features::volumetricLighting;
			const bool paused = VolumetricLighting::PausedForVolumetricFog();
			a_rows.push_back({ group, "  Vanilla Volumetric Lighting paused", vl.loaded, fog.volumetric.Enabled,
				"(automatic)", fog.volumetric.Enabled ? (paused ? std::string("Paused") : std::string("Running")) : std::string(),
				"Lighting > Volumetric Lighting" });

			a_rows.push_back({ group, "Second height fog layer (#2831)", fog.loaded, fog.settings.fogDensity2 > 0.0f,
				fog.settings.fogDensity2 > 0.0f ? std::format("density {:.3f}", fog.settings.fogDensity2) : std::string("0 (off)"), "",
				"Lighting > Exponential Height Fog > Second Fog Layer (Batch 38)" });
		}

		// ---- A2. Smoke and effects receive sun shadow -----------------------------------------
		{
			auto& vs = globals::features::volumetricShadows;
			const char* group = "A2. Smoke & effects receive sun shadow";
			const char* where = "Lighting > Volumetric Shadows";
			const bool valid = vs.VsmValid();

			{
				Row r{ group, "Smoke & effects receive sun shadow (default on)", vs.loaded, vs.settings.ParticleShadows,
					onOff(vs.settings.ParticleShadows),
					vs.settings.ParticleShadows ? (valid ? std::string("On") : std::string("Idle: no sun shadow here")) : std::string(), where };
				r.toggle = &vs.settings.ParticleShadows;
				a_rows.push_back(std::move(r));
			}
			{
				Row r{ group, "Soft sun shadow on forward objects (default off)", vs.loaded, vs.settings.ForwardSoftShadows,
					onOff(vs.settings.ForwardSoftShadows),
					vs.settings.ForwardSoftShadows ? (valid ? std::string("On") : std::string("Idle: no sun shadow here")) : std::string(), where };
				r.toggle = &vs.settings.ForwardSoftShadows;
				a_rows.push_back(std::move(r));
			}
		}
	}
}
