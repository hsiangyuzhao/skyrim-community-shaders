#pragma once

#include "Feature.h"

#include <chrono>

struct PostProcessFeatureConstructor;

struct PostProcessFeature
{
	virtual ~PostProcessFeature() = default;

	bool enabled = true;

	virtual std::string GetType() const = 0;
	std::string name;
	virtual std::string GetDesc() const = 0;
	virtual bool SupportsVR() const { return true; }
	virtual bool DrawBeforeUpscaling() const { return false; }
	virtual bool DrawAfterColorGrading() const { return false; }
	virtual bool DisableInMainLoadingMenu() const { return false; }

	/// @brief Allocate this effect's GPU memory (textures, buffers, samplers).
	///
	/// (batch 16, item P14) This is no longer called for every sub-feature at boot. It runs
	/// the first time an *enabled* effect is about to draw, and again after a settings load
	/// or a resolution change. Shader compilation deliberately does NOT live here any more
	/// -- see SetupShaders().
	virtual inline void SetupResources() = 0;

	/// @brief Compile this effect's shaders. Called once per sub-feature at boot, for every
	/// sub-feature, on or off.
	///
	/// Shaders are a few kilobytes each; the reason to keep them unconditional is the other
	/// direction. Util::CompileShader is a live D3DCompileFromFile with no cache, so folding
	/// it into SetupResources would put a multi-hundred-millisecond stall on the exact frame
	/// where the user ticks a checkbox, every time they tick it.
	virtual void SetupShaders() {}

	/// @brief Release everything SetupResources() allocated, and nothing else.
	///
	/// The contract is literal: every texture/buffer member that SetupResources() assigns has
	/// to be nulled here. If one is missed, turning the effect off silently keeps its memory,
	/// which is the bug this whole mechanism exists to fix. Shaders and sampler states are
	/// intentionally left alone -- they are tiny and keeping them is what makes a re-enable
	/// cheap.
	virtual void ReleaseResources() {}

	/// @brief True iff SetupResources() has run and ReleaseResources() has not run since.
	///
	/// Owned by PostProcessing, not by the effect itself. Effects must not read it.
	bool resourcesResident = false;

	/// @brief When this effect was first seen disabled while still holding resources.
	///
	/// Default-constructed means "not currently counting down". PostProcessing uses it to
	/// hold the memory for a grace period rather than freeing on the frame the checkbox
	/// flips, so toggling in the menu does not thrash a few hundred MiB.
	std::chrono::steady_clock::time_point disabledSince{};

	virtual void ClearShaderCache() = 0;
	virtual void RestoreDefaultSettings() = 0;

	virtual void LoadSettings(json& o_json) = 0;
	virtual void SaveSettings(json& o_json) = 0;
	virtual void DrawSettings() = 0;

	struct TextureInfo
	{
		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
	};
	virtual void Draw(TextureInfo& inout_tex) = 0;  // read from last pass, do the thing, and replace it with output texture

	/// @brief (batch 37b, C-1) Where the effect's full-screen result would go: its own output
	/// texture. PostProcessing compares it with the game's buffer to decide whether the effect
	/// may write there directly. Null = this effect cannot write anywhere but its own texture.
	virtual ID3D11Texture2D* GetOwnOutputTexture() const { return nullptr; }

	/// @brief (batch 37b, C-1) Target PostProcessing hands the last effect of the chain when the
	/// game's buffer matches that effect's own output texture exactly (format and size). The
	/// effect then stores into this UAV instead of its own texture and hands back {tex, srv}: the
	/// same shader writes the same values, only into the buffer they would have been copied to
	/// anyway. Lives in PostProcessing (directOutput / directOutputFor), not here, so that no
	/// effect's memory layout changes.
	struct DirectOutput
	{
		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;
	};

	virtual inline void Reset(){};
};

struct PostProcessFeatureConstructor
{
	std::function<PostProcessFeature*()> fn;
	std::string name;
	std::string desc;
	static const ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor>& GetFeatureConstructors();
};