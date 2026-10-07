// Batch 38, group E: DLSS 5 Neural Rendering rows of the Advanced -> "Batch 38" table.
// Kept in its own file so the other batch 38 groups add theirs without touching this one.
#include "Utils/Batch38.h"

#include <algorithm>
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
		rows.push_back({ group, "Model resolution (38c: our own downscale; the DLL ignored 38a's)", installed, true,
			std::format("{}%", nr.modelResolutionPercent),
			!idle.empty() ? idle : std::format("{}% = {}x{}", status.modelPercent, status.workWidth, status.workHeight),
			where + std::string(" > Model Resolution") });
		{
			Row r{ group, "38c: pad the network's picture to a multiple of 8 (Balanced tone fix)", installed, nr.padToNetworkGrid,
				onOff(nr.padToNetworkGrid),
				!nr.padToNetworkGrid ? std::string() :
				!idle.empty()        ? idle :
									   std::format("On, network {}x{}", status.paddedWidth, status.paddedHeight),
				where + std::string(" > Pad to network grid") };
			r.toggle = &nr.padToNetworkGrid;
			rows.push_back(std::move(r));
		}
		{
			Row r{ group, "38c: tone-matched input (before upscaling only)", installed, nr.toneMatchedInput,
				onOff(nr.toneMatchedInput),
				!nr.toneMatchedInput    ? std::string() :
				!idle.empty()           ? idle :
				!status.beforeUpscaling ? std::string("Idle: running after upscaling") :
										  std::format("On ({})", NeuralRendering::ToneSourceText()),
				where + std::string(" > Tone-matched input") };
			r.toggle = &nr.toneMatchedInput;
			rows.push_back(std::move(r));
		}
		{
			const char* precisionNames[] = { "8-bit", "10-bit", "16-bit float" };
			const char* precision = precisionNames[std::min(nr.inputPrecision, 2u)];
			rows.push_back({ group, "38c: input precision (before upscaling only)", installed, nr.inputPrecision != 0, precision,
				!idle.empty()           ? idle :
				!status.beforeUpscaling ? std::string("Idle: running after upscaling") :
										  std::string(precisionNames[std::min(status.inputPrecision, 2u)]),
				where + std::string(" > Input Precision") });
		}
		{
			const float strength = status.beforeUpscaling ? nr.tonePreservationBefore : nr.tonePreservationAfter;
			rows.push_back({ group, "38c: tone preservation (before / after upscaling)", installed, strength > 0.0f,
				std::format("{:.2f} / {:.2f}", nr.tonePreservationBefore, nr.tonePreservationAfter),
				!idle.empty() ? idle : (status.toneStrength > 0.0f ? std::format("On, {:.2f}", status.toneStrength) : std::string("Off (0)")),
				where + std::string(" > Tone Preservation") });
		}
		{
			Row r{ group, "38c: give the model its tuning at creation", installed, nr.tuningAtCreate,
				onOff(nr.tuningAtCreate),
				!nr.tuningAtCreate ? std::string() : (!idle.empty() ? idle : std::string("On")),
				where + std::string(" > Apply tuning at creation") };
			r.toggle = &nr.tuningAtCreate;
			rows.push_back(std::move(r));
		}
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
