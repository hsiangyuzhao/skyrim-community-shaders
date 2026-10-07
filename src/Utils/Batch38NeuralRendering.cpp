// Batch 38, group E: DLSS 5 Neural Rendering rows of the Advanced -> "Batch 38" table.
// Kept in its own file so the other batch 38 groups add theirs without touching this one.
#include "Utils/Batch38.h"

#include <format>
#include <string>

#include "Features/Upscaling.h"
#include "Features/Upscaling/NeuralRendering/Integration.h"
#include "Features/Upscaling/NeuralRendering/Renderer.h"
#include "Globals.h"

namespace Batch38
{
	void RowsNeuralRendering(std::vector<Row>& rows)
	{
		auto& upscaling = globals::features::upscaling;
		auto& nr = upscaling.settings.neuralRendering;
		const auto& status = NeuralRendering::GetFrameStatus();
		const char* group = "E. DLSS 5 Neural Rendering";
		const char* where = "Display > Upscaling > Neural Rendering (DLSS 5, experimental)";
		const bool installed = upscaling.loaded && !globals::game::isVR;
		const auto onOff = [](bool b) { return std::string(b ? "On" : "Off"); };

		// What the pass itself is doing, so every "Now" below can say Idle when it is not running.
		const std::string idle = !nr.enabled    ? std::string("Idle: Neural Rendering is off") :
		                         status.running ? std::string() :
		                                          std::format("Idle: {}", status.blockedReason[0] ? status.blockedReason : "not running");

		{
			Row r{ group, "Run together with Frame Generation (37c blocked it)", installed, nr.allowWithFrameGeneration,
				onOff(nr.allowWithFrameGeneration),
				!nr.allowWithFrameGeneration ? std::string() :
				status.frameGeneration       ? (status.running ? std::string("On (Frame Generation running)") : idle) :
											   std::string("On (Frame Generation off now)"),
				where + std::string(" > Allow with Frame Generation") };
			r.toggle = &nr.allowWithFrameGeneration;
			rows.push_back(std::move(r));
		}
		{
			Row r{ group, "Run before upscaling (default off = after, as 37c)", installed, nr.runBeforeUpscaling,
				onOff(nr.runBeforeUpscaling),
				!idle.empty() ? idle :
								std::format("{} upscaling, {}x{}", status.beforeUpscaling ? "Before" : "After", status.width, status.height),
				where + std::string(" > Run before upscaling") };
			r.toggle = &nr.runBeforeUpscaling;
			// A placement is always in effect; "own" only colours the cell.
			r.own = true;
			rows.push_back(std::move(r));
		}
		rows.push_back({ group, "Model resolution", installed, true, std::format("{}%", nr.modelResolutionPercent),
			!idle.empty() ? idle : std::format("{}%", status.modelPercent), where + std::string(" > Model Resolution") });
		{
			Row r{ group, "Jitter-aware motion (before upscaling only)", installed, nr.jitterAwareMotion,
				onOff(nr.jitterAwareMotion),
				!nr.jitterAwareMotion  ? std::string() :
				!idle.empty()          ? idle :
				status.beforeUpscaling ? std::string("On") :
										 std::string("Idle: running after upscaling"),
				where + std::string(" > Jitter-Aware Motion") };
			r.toggle = &nr.jitterAwareMotion;
			rows.push_back(std::move(r));
		}
		rows.push_back({ group, "Fix: real motion vectors, guides on the colour's grid", installed, true, "(fix, no own switch)",
			!idle.empty() ? idle : std::string("On"), "-" });
		rows.push_back({ group, "Fix: 8-deep command ring, no CPU stall mid-frame", installed, true, "(fix, no own switch)",
			!idle.empty() ? idle : std::string("On"), "-" });
		rows.push_back({ group, "Fix: free its video memory when off, blocked or changed", installed, true, "(fix, no own switch)", "On", "-" });
		{
			const auto& renderer = NeuralRendering::Renderer::Instance();
			Row r{ group, "Diagnostics: overlay row, model GPU time, state log", installed, true, "(always on)",
				renderer.ModelGpuMs() > 0.0f ? std::format("model {:.2f} ms", renderer.ModelGpuMs()) : std::string("On"),
				"Performance overlay > Neural Rendering (DLSS 5); " + std::string(where) };
			r.governed = false;
			rows.push_back(std::move(r));
		}
	}
}
