#pragma once

#include <functional>
#include <string>

// Forward declaration
class Menu;

class AdvancedSettingsRenderer
{
public:
	static void RenderAdvancedSettings(
		const std::function<void()>& drawTruePBRSettings,
		const std::function<void()>& drawDisableAtBootSettings);

private:
	static void RenderLoggingSection();
	static void RenderShaderDebugSection();
	static void RenderPBRSection(const std::function<void()>& drawTruePBRSettings);
	static void RenderDisableAtBootSection(const std::function<void()>& drawDisableAtBootSettings);
	static void RenderDeveloperSection();
	// (batch 36f) Master switch for the batch 36f denoiser savings and its status table.
	static void RenderBatch36fSection();
	// (batch 36g) Master switch, experiment switches and status of the batch 36g diagnostic matrix.
	static void RenderBatch36gSection();
};