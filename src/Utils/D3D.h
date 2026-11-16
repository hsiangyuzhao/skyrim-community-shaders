#pragma once
#include <array>
#include <d3d11.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <winrt/base.h>

namespace Util
{
	ID3D11ShaderResourceView* GetSRVFromRTV(ID3D11RenderTargetView* a_rtv);
	ID3D11RenderTargetView* GetRTVFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromRTV(ID3D11RenderTargetView* a_rtv);
	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...);

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");

	/**
	 * @brief (48e) A cs_5_0 shader compiled by CompileShader (disk cache included) on a background thread.
	 *
	 * Request() starts the compile (once per path + defines; another key forgets the old job) and returns at once. Poll() never
	 * blocks: it returns the shader once the thread has finished, else nullptr (the caller keeps its old path meanwhile). Reset()
	 * forgets the job; a compile still running finishes into its own shared state and is dropped. One info line per job (wall
	 * time, disk cache on / off) when Poll() first sees it finished. Main thread only (the worker touches only its job).
	 * (48g) The worker compiles bytecode only; the shader object is created (and released) on the main thread.
	 */
	class AsyncComputeShader
	{
	public:
		enum class Status
		{
			Idle,       ///< nothing requested
			Compiling,  ///< the background thread is still at it
			Ready,      ///< Poll() returns the shader
			Failed      ///< the compile (or the shader's creation) failed
		};

		AsyncComputeShader() = default;
		~AsyncComputeShader() = default;
		AsyncComputeShader(const AsyncComputeShader&) = delete;
		AsyncComputeShader& operator=(const AsyncComputeShader&) = delete;

		/// Starts the compile unless a job with the same path and defines exists. a_label names it in the log.
		void Request(const wchar_t* a_path, const std::vector<std::pair<std::string, std::string>>& a_defines, std::string_view a_label);
		/// Non-blocking: the compiled shader (owned here, valid until Reset / a Request with another key), else nullptr.
		ID3D11ComputeShader* Poll();
		/// Non-blocking (polls).
		Status GetStatus();
		void Reset();
		/// Wall time of the finished compile (ms; 0 before).
		[[nodiscard]] double CompileMs() const { return compileMs; }

	private:
		struct Job;
		std::shared_ptr<Job> job;
		winrt::com_ptr<ID3D11ComputeShader> shader;  // (48g) created on the main thread from the worker's bytecode
		bool created = false;
		std::string key;
		std::string label;
		double compileMs = 0.0;
		bool reported = false;
	};

	// Texture manipulation utilities
	void ApplyHighlightTintToTexture(ID3D11Texture2D* texture, bool isHighlighted, const std::array<float, 4>& highlightColor = { 1.0f, 0.5f, 0.0f, 0.3f });
	HRESULT CreateOverlayTextureAndRTV(ID3D11Device* device, int width, int height, ID3D11Texture2D** outTex, ID3D11RenderTargetView** outRTV);

	// VR-aware counts for render targets
	inline int GetRenderTargetCount()
	{
		return REL::Module::IsVR() ? RE::RENDER_TARGETS::kVRTOTAL : RE::RENDER_TARGETS::kTOTAL;
	}

	inline int GetDepthStencilCount()
	{
		return REL::Module::IsVR() ? RE::RENDER_TARGETS_DEPTHSTENCIL::kVRTOTAL : RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL;
	}
}  // namespace Util
