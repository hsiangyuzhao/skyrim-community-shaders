#pragma once

#include <filesystem>
#include <string>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

/// (49h stage 1, S9) "Reflection Census": runs with the Performance Overlay's frame save (F12 freeze + save), never otherwise.
/// Request() marks the saved JSON; the next deferred frame classifies every render pixel by its reflection (ReflectionCensusCS:
/// sky / none / smooth / semi / rough, the 48d RT importance, SSRT's outcomes), the counts are read back a few frames later
/// without waiting, and a "reflection_census" block is added to that JSON. No cost and no change on screen otherwise.
namespace ReflectionCensus
{
	/// From PerformanceOverlay::SaveFrameJson (the switch is checked by the caller).
	void Request(const std::filesystem::path& a_jsonPath);
	/// From Deferred, right before the deferred composite: dispatches the pass when a request is pending.
	void Dispatch(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_normalRoughness, ID3D11ShaderResourceView* a_reflectance,
		ID3D11ShaderResourceView* a_depth);
	/// Every frame (Deferred): reads a finished pass back (no wait) and writes it into the JSON.
	void Poll();
	/// One-line result of the last census ("" before the first).
	[[nodiscard]] std::string Summary();
}
