#pragma once

// Robust Contrast Adaptive Sharpening (AMD FidelityFX FSR1 RCAS, MIT).
// Ported from jiayev/skyrim-community-shaders (fd3f6f994, 83635b4f8, 499687986).

#include "Buffer.h"

#include <d3d11_4.h>
#include <memory>
#include <winrt/base.h>

/**
 * @brief Standalone RCAS sharpening pass for the DLSS output.
 *
 * (45a) Runs on the linear scene right after DLSS (before post-processing), so one result serves
 * SDR and HDR alike. The shader wraps its taps around their local mean before sharpening, so it
 * is safe on unbounded HDR values.
 */
class RCAS
{
public:
	RCAS() = default;

	/**
	 * @brief Sharpens a_input into a_output (same size). Compiles the shader on first use.
	 *
	 * @param a_input SRV of the image to sharpen.
	 * @param a_output UAV the sharpened image is written to (must not alias a_input).
	 * @param a_width Output width in pixels.
	 * @param a_height Output height in pixels.
	 * @param a_sharpness RCAS attenuation (FSR's con: exp2(2*slider - 2)); > 0.
	 * @return false when nothing was written (shader unavailable); the caller must then copy.
	 */
	bool ApplySharpen(ID3D11ShaderResourceView* a_input, ID3D11UnorderedAccessView* a_output, uint32_t a_width, uint32_t a_height, float a_sharpness);

	/// Drops the compiled shader so the next use recompiles it.
	void ClearShaderCache();

private:
	bool EnsureResources();

	winrt::com_ptr<ID3D11ComputeShader> computeShader;
	std::unique_ptr<ConstantBuffer> configCB;
	bool compileFailed = false;
};
