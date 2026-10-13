#include "RCAS.h"

#include "State.h"
#include "Util.h"

namespace
{
	struct RCASConfig
	{
		float sharpness;
		float pad[3];
	};
	static_assert(sizeof(RCASConfig) % 16 == 0);
}

bool RCAS::EnsureResources()
{
	if (!configCB)
		configCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<RCASConfig>());

	if (!computeShader && !compileFailed) {
		logger::debug("[RCAS] Compiling RCAS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines;
		computeShader.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/RCAS/RCAS.hlsl", defines, "cs_5_0"));
		if (!computeShader) {
			compileFailed = true;
			logger::warn("[RCAS] Compute shader not compiled; DLSS output is copied unsharpened");
		}
	}
	return computeShader != nullptr;
}

void RCAS::ClearShaderCache()
{
	computeShader = nullptr;
	compileFailed = false;
}

bool RCAS::ApplySharpen(ID3D11ShaderResourceView* a_input, ID3D11UnorderedAccessView* a_output, uint32_t a_width, uint32_t a_height, float a_sharpness)
{
	if (!a_input || !a_output || a_width == 0 || a_height == 0 || !EnsureResources())
		return false;

	auto state = globals::state;
	auto context = globals::d3d::context;

	state->BeginPerfEvent("RCAS Sharpening");

	RCASConfig config{};
	config.sharpness = a_sharpness;
	configCB->Update(config);
	auto buffer = configCB->CB();

	context->CSSetShader(computeShader.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &buffer);

	ID3D11ShaderResourceView* srvs[] = { a_input };
	context->CSSetShaderResources(0, 1, srvs);

	ID3D11UnorderedAccessView* uavs[] = { a_output };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);

	ID3D11ShaderResourceView* nullSRVs[] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);
	ID3D11UnorderedAccessView* nullUAVs[] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	context->CSSetShader(nullptr, nullptr, 0);

	state->EndPerfEvent();
	return true;
}
