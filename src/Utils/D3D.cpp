#include "D3D.h"

#include "ShaderCache.h"
#include "State.h"
#include "Utils/ContentHash.h"
#include "Utils/EngineTiming.h"
#include "Utils/FrameTrace.h"
#include "Utils/Format.h"

#include <d3dcompiler.h>
#include <fstream>
#include <mutex>
#include <optional>

namespace Util
{
	ID3D11ShaderResourceView* GetSRVFromRTV(const ID3D11RenderTargetView* a_rtv)
	{
		if (a_rtv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < GetRenderTargetCount(); i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_rtv == rt.RTV) {
						return rt.SRV;
					}
				}
			}
		}
		return nullptr;
	}

	ID3D11RenderTargetView* GetRTVFromSRV(const ID3D11ShaderResourceView* a_srv)
	{
		if (a_srv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < GetRenderTargetCount(); i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_srv == rt.SRV || a_srv == rt.SRVCopy) {
						return rt.RTV;
					}
				}
			}
		}
		return nullptr;
	}

	std::string GetNameFromSRV(const ID3D11ShaderResourceView* a_srv)
	{
		using RENDER_TARGET = RE::RENDER_TARGETS::RENDER_TARGET;

		if (a_srv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < GetRenderTargetCount(); i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_srv == rt.SRV || a_srv == rt.SRVCopy) {
						return std::string(magic_enum::enum_name(static_cast<RENDER_TARGET>(i)));
					}
				}
			}
		}
		return "NONE";
	}

	std::string GetNameFromRTV(const ID3D11RenderTargetView* a_rtv)
	{
		using RENDER_TARGET = RE::RENDER_TARGETS::RENDER_TARGET;
		if (a_rtv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < GetRenderTargetCount(); i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_rtv == rt.RTV) {
						return std::string(magic_enum::enum_name(static_cast<RENDER_TARGET>(i)));
					}
				}
			}
		}
		return "NONE";
	}

	GUID WKPDID_D3DDebugObjectNameT = { 0x429b8c22, 0x9188, 0x4b0c, 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 };

	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...)
	{
		if (!Resource)
			return;

		char buffer[1024];
		va_list va;

		va_start(va, Format);
		int len = _vsnprintf_s(buffer, _TRUNCATE, Format, va);
		va_end(va);

		Resource->SetPrivateData(WKPDID_D3DDebugObjectNameT, len, buffer);
		if (len > 0)
			Util::FrameTrace::OnResourceNamed(Resource, std::string_view(buffer, static_cast<size_t>(len)));  // (46q) VRAM per feature
	}

	struct CustomInclude : public ID3DInclude
	{
		HRESULT Open([[maybe_unused]] D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, [[maybe_unused]] LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override
		{
			std::filesystem::path filePath = pFileName;
			filePath = L"Data\\Shaders" / filePath;

			std::ifstream file(filePath, std::ios::binary);
			if (!file.is_open()) {
				*ppData = NULL;
				*pBytes = 0;
				return E_FAIL;
			}

			// Get filesize
			file.seekg(0, std::ios::end);
			UINT size = static_cast<UINT>(file.tellg());
			file.seekg(0, std::ios::beg);

			// Create buffer and read file
			char* data = new char[size];
			file.read(data, size);
			*ppData = data;
			*pBytes = size;
			return S_OK;
		}

		HRESULT Close(LPCVOID pData) override
		{
			if (pData)
				delete[] pData;
			return S_OK;
		}
	};

	// (44j, upstream #2848) Disk cache for CompileShader's shaders, which the main shader cache does not cover
	// (Data/ShaderCache/CompileShader).
	namespace
	{
		/**
		 * @brief Returns the cache entry path for one compile: a SHA-256 of the preprocessed source (includes and
		 *        defines expanded), the entry point, the target, the flags and D3D_COMPILER_VERSION.
		 *
		 * Entries are named by content and never replaced; they are dropped by Clear Shader Cache, a cache-version or
		 * installed-feature change, or (46j) when the folder outgrows its size limit. Source edits do not drop them.
		 * @return The path, or nullopt when the source cannot be read, preprocessed or hashed.
		 */
		std::optional<std::wstring> GetCompileShaderDiskPath(const wchar_t* filePath, const D3D_SHADER_MACRO* macros, ID3DInclude* include, const char* program, const char* programType, uint32_t flags)
		{
			std::string source;
			{
				std::ifstream file(filePath, std::ios::binary);
				if (!file.is_open())
					return std::nullopt;
				source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}

			winrt::com_ptr<ID3DBlob> preprocessed;
			winrt::com_ptr<ID3DBlob> errors;
			const auto sourceName = Util::WStringToString(filePath);
			if (FAILED(D3DPreprocess(source.data(), source.size(), sourceName.c_str(), macros, include, preprocessed.put(), errors.put())) || !preprocessed)
				return std::nullopt;

			ContentHash::Sha256 sha;
			sha.UpdateField(std::string_view(static_cast<const char*>(preprocessed->GetBufferPointer()), preprocessed->GetBufferSize()));
			sha.UpdateField(std::format("{}|{}|{:X}|{}", program, programType, flags, D3D_COMPILER_VERSION));
			std::array<uint8_t, 32> digest{};
			if (!sha.Finish(digest))
				return std::nullopt;

			std::wstring name;
			name.reserve(digest.size() * 2);
			for (const auto byte : digest)
				name += std::format(L"{:02x}", byte);
			return std::format(L"Data/ShaderCache/CompileShader/{}.cso", name);
		}

		/** @brief Writes a compiled shader to its cache entry; a failure only costs the next launch a compile. */
		void StoreCompileShaderBlob(const std::wstring& diskPath, ID3DBlob* blob)
		{
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(diskPath).parent_path(), ec);
			if (ec || !SIE::SShaderCache::WriteBlobAtomic(diskPath, blob))
				logger::debug("Failed to write utility shader cache entry {}", Util::WStringToString(diskPath));
		}

		/** @brief True for the targets CompileShader turns into a shader object: only those are read from or written to the cache. */
		bool IsCacheableTarget(const char* programType)
		{
			for (const auto* target : { "ps_5_0", "vs_5_0", "hs_5_0", "ds_5_0", "cs_5_0", "cs_4_0" })
				if (!_stricmp(programType, target))
					return true;
			return false;
		}

		/**
		 * @brief Creates the shader object for a CompileShader target.
		 * @return The device's result; a target without a creation path returns S_OK and no shader.
		 */
		HRESULT CreateShaderObject(ID3D11Device* device, const char* programType, ID3DBlob* blob, ID3D11DeviceChild** shader)
		{
			*shader = nullptr;
			const void* code = blob->GetBufferPointer();
			const SIZE_T size = blob->GetBufferSize();
			HRESULT result = S_OK;
			if (!_stricmp(programType, "ps_5_0")) {
				ID3D11PixelShader* regShader = nullptr;
				result = device->CreatePixelShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "vs_5_0")) {
				ID3D11VertexShader* regShader = nullptr;
				result = device->CreateVertexShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "hs_5_0")) {
				ID3D11HullShader* regShader = nullptr;
				result = device->CreateHullShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "ds_5_0")) {
				ID3D11DomainShader* regShader = nullptr;
				result = device->CreateDomainShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "cs_5_0") || !_stricmp(programType, "cs_4_0")) {
				ID3D11ComputeShader* regShader = nullptr;
				result = device->CreateComputeShader(code, size, nullptr, &regShader);
				*shader = regShader;
			}
			return result;
		}

		bool IsComputeTarget(const char* programType)
		{
			return !_stricmp(programType, "cs_5_0") || !_stricmp(programType, "cs_4_0");
		}
	}

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program)
	{
		EngineTiming::CompileTimer compileTimer;  // (46n) billed to the pass that asked for it (F7 trace pass_block "s")
		auto device = globals::d3d::device;

		CustomInclude include;

		// Build defines (aka convert vector->D3DCONSTANT array)
		std::vector<D3D_SHADER_MACRO> macros;
		std::string str = Util::WStringToString(FilePath);

		for (auto& i : Defines) {
			if (i.first && _stricmp(i.first, "") != 0) {
				macros.push_back({ i.first, i.second });
			} else {
				logger::error("Failed to process shader defines for {}", str);
			}
		}

		if (REL::Module::IsVR())
			macros.push_back({ "VR", "" });
		if (globals::state->IsDeveloperMode()) {
			macros.push_back({ "D3DCOMPILE_SKIP_OPTIMIZATION", "" });
			macros.push_back({ "D3DCOMPILE_DEBUG", "" });
		}
		auto shaderDefines = globals::state->GetDefines();
		if (!shaderDefines->empty()) {
			for (unsigned int i = 0; i < shaderDefines->size(); i++)
				macros.push_back({ shaderDefines->at(i).first.c_str(), shaderDefines->at(i).second.c_str() });
		}
		if (!_stricmp(ProgramType, "ps_5_0"))
			macros.push_back({ "PSHADER", "" });
		else if (!_stricmp(ProgramType, "vs_5_0"))
			macros.push_back({ "VSHADER", "" });
		else if (!_stricmp(ProgramType, "hs_5_0"))
			macros.push_back({ "HULLSHADER", "" });
		else if (!_stricmp(ProgramType, "ds_5_0"))
			macros.push_back({ "DOMAINSHADER", "" });
		else if (!_stricmp(ProgramType, "cs_5_0"))
			macros.push_back({ "COMPUTESHADER", "" });
		else if (!_stricmp(ProgramType, "cs_4_0"))
			macros.push_back({ "COMPUTESHADER", "" });
		else if (!_stricmp(ProgramType, "cs_5_1"))
			macros.push_back({ "COMPUTESHADER", "" });
		else
			return nullptr;

		// Add null terminating entry
		macros.push_back({ "WINPC", "" });
		macros.push_back({ "DX11", "" });
		macros.push_back({ nullptr, nullptr });

		// Compiler setup
		uint32_t flags = !globals::state->IsDeveloperMode() ? (D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3) : D3DCOMPILE_DEBUG;
		// (44j, upstream) Disk cache on = shipped, known-good shaders: skip fxc's validation pass on its own output.
		const bool useDiskCache = globals::shaderCache && globals::shaderCache->IsDiskCache();
		if (useDiskCache && !globals::state->IsDeveloperMode())
			flags |= D3DCOMPILE_SKIP_VALIDATION;

		// (S4.14) RAII, and not merely as tidiness: these were raw ID3DBlob* with no Release
		// on any path. The success path leaked the bytecode blob -- for a shader of a few
		// kilobytes, once per compile, and ClearShaderCache recompiles every shader of every
		// feature on a settings change that sets a recompile flag, so the leak is unbounded
		// over a session rather than a one-off. The failure path leaked the *error* blob:
		// `shaderErrors` was read for the log message and then abandoned, and an error blob
		// carries the full diagnostic text, which is the largest of the two.
		//
		// Both were also *uninitialised* raw pointers. D3DCompileFromFile does not write
		// ppErrorMsgs when there is nothing to report, so on a clean compile `shaderErrors`
		// kept whatever was on the stack and `if (shaderErrors)` then dereferenced it. That
		// it never crashed is luck about stack contents, not correctness.
		//
		// com_ptr's destructor covers every exit -- the two early returns, all six of the
		// per-profile returns below, the trailing return, and a throw out of
		// DX::ThrowIfFailed on the compute paths.
		winrt::com_ptr<ID3DBlob> shaderBlob;
		winrt::com_ptr<ID3DBlob> shaderErrors;

		if (!std::filesystem::exists(FilePath)) {
			logger::error("Failed to compile shader; {} does not exist", str);
			return nullptr;
		}
		std::optional<std::wstring> diskPath;
		if (useDiskCache && IsCacheableTarget(ProgramType))
			diskPath = GetCompileShaderDiskPath(FilePath, macros.data(), &include, Program, ProgramType, flags);

		if (const auto cachedBlob = diskPath && std::filesystem::exists(*diskPath) ? SIE::SShaderCache::ReadIntactBlob(*diskPath) : nullptr) {
			ID3D11DeviceChild* shader = nullptr;
			if (SUCCEEDED(CreateShaderObject(device, ProgramType, cachedBlob.get(), &shader)) && shader) {
				logger::debug("Loaded {} from {}", str, Util::WStringToString(*diskPath));
				return shader;
			}
			// An intact container can still hold bytecode the runtime rejects; drop the entry and compile from source.
			logger::warn("Discarding cached shader {} rejected by the device", Util::WStringToString(*diskPath));
			std::error_code ec;
			std::filesystem::remove(*diskPath, ec);
		}

		logger::debug("Compiling {} with {}", str, DefinesToString(macros));
		if (FAILED(D3DCompileFromFile(FilePath, macros.data(), &include, Program, ProgramType, flags, 0, shaderBlob.put(), shaderErrors.put()))) {
			logger::warn("Shader compilation failed:\n\n{}", shaderErrors ? static_cast<char*>(shaderErrors->GetBufferPointer()) : "Unknown error");
			return nullptr;
		}
		// A successful compile can still have produced warnings, and can equally have produced
		// no blob at all -- hence the test rather than an unconditional read.
		if (shaderErrors)
			logger::debug("Shader logs:\n{}", static_cast<char*>(shaderErrors->GetBufferPointer()));
		// Defensive: a SUCCEEDED return with no bytecode would make every GetBufferPointer
		// below a null dereference. Not observed, but it is one branch against a hard crash.
		if (!shaderBlob) {
			logger::error("Shader compilation reported success but produced no bytecode for {}", str);
			return nullptr;
		}
		ID3D11DeviceChild* shader = nullptr;
		const HRESULT createResult = CreateShaderObject(device, ProgramType, shaderBlob.get(), &shader);
		// Same failure policy as before 44j: compute creation failures throw, the other stages return whatever
		// the device produced (null on failure).
		if (IsComputeTarget(ProgramType))
			DX::ThrowIfFailed(createResult);
		if (SUCCEEDED(createResult) && shader && diskPath)
			StoreCompileShaderBlob(*diskPath, shaderBlob.get());
		return shader;
	}

	// RAII wrapper for D3D mapped resources
	class ScopedD3DMap
	{
	public:
		ScopedD3DMap(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP mapType, UINT mapFlags) :
			context_(context), resource_(resource), subresource_(subresource), mapped_(false)
		{
			if (context && resource) {
				HRESULT hr = context->Map(resource, subresource, mapType, mapFlags, &mappedResource_);
				mapped_ = SUCCEEDED(hr);
			}
		}

		~ScopedD3DMap()
		{
			if (mapped_ && context_ && resource_) {
				context_->Unmap(resource_, subresource_);
			}
		}

		// Non-copyable, non-movable for simplicity
		ScopedD3DMap(const ScopedD3DMap&) = delete;
		ScopedD3DMap& operator=(const ScopedD3DMap&) = delete;

		bool IsValid() const { return mapped_; }
		D3D11_MAPPED_SUBRESOURCE* GetMappedResource() { return mapped_ ? &mappedResource_ : nullptr; }

	private:
		ID3D11DeviceContext* context_;
		ID3D11Resource* resource_;
		UINT subresource_;
		bool mapped_;
		D3D11_MAPPED_SUBRESOURCE mappedResource_{};
	};

	// Helper function to validate highlight color components
	bool ValidateHighlightColor(const std::array<float, 4>& color)
	{
		for (size_t i = 0; i < 4; ++i) {
			if (color[i] < 0.0f || color[i] > 1.0f || !std::isfinite(color[i])) {
				return false;
			}
		}
		return true;
	}

	void ApplyHighlightTintToTexture(ID3D11Texture2D* texture, bool isHighlighted, const std::array<float, 4>& highlightColor)
	{
		if (!isHighlighted || !texture)
			return;

		// Validate input color values
		if (!ValidateHighlightColor(highlightColor)) {
			logger::error("ApplyHighlightTintToTexture: Invalid highlight color values. All components must be finite and in range [0.0, 1.0]");
			return;
		}

		// Get texture description and validate dimensions
		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);

		// Performance consideration: warn about large textures (only once)
		const UINT pixelCount = desc.Width * desc.Height;
		const UINT largeTextureThreshold = 1024 * 1024;  // 1 megapixel
		if (pixelCount > largeTextureThreshold) {
			static std::once_flag largeTextureWarning;
			std::call_once(largeTextureWarning, [&]() {
				logger::warn("ApplyHighlightTintToTexture: Processing large texture ({}x{} = {} pixels). Consider using compute shader for better performance. This warning will only be shown once.",
					desc.Width, desc.Height, pixelCount);
			});
		}  // Create a temporary staging texture to read from
		ID3D11Texture2D* stagingTexture = nullptr;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

		HRESULT hr = globals::d3d::device->CreateTexture2D(&desc, nullptr, &stagingTexture);
		if (FAILED(hr)) {
			logger::error("ApplyHighlightTintToTexture: Failed to create staging texture (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
			return;
		}

		// RAII wrapper ensures staging texture is always released
		std::unique_ptr<ID3D11Texture2D, void (*)(ID3D11Texture2D*)> stagingGuard(
			stagingTexture, [](ID3D11Texture2D* tex) { if (tex) tex->Release(); });

		// Copy the original texture to staging
		globals::d3d::context->CopyResource(stagingTexture, texture);

		// Use RAII wrapper for mapping/unmapping
		ScopedD3DMap scopedMap(globals::d3d::context, stagingTexture, 0, D3D11_MAP_READ_WRITE, 0);
		if (!scopedMap.IsValid()) {
			logger::error("ApplyHighlightTintToTexture: Failed to map staging texture");
			return;
		}

		D3D11_MAPPED_SUBRESOURCE* mapped = scopedMap.GetMappedResource();
		if (!mapped || !mapped->pData) {
			logger::error("ApplyHighlightTintToTexture: Invalid mapped resource data");
			return;
		}

		// Apply highlight tint to each pixel
		uint8_t* pixels = static_cast<uint8_t*>(mapped->pData);
		const float blendAlpha = highlightColor[3];

		for (UINT y = 0; y < desc.Height; ++y) {
			for (UINT x = 0; x < desc.Width; ++x) {
				uint8_t* pixel = pixels + (y * mapped->RowPitch + x * 4);

				// Only tint non-transparent pixels (alpha > 0)
				if (pixel[3] > 0) {
					// Apply configurable highlight tint
					// Blend the original color with the highlight color
					float originalR = pixel[0] / 255.0f;
					float originalG = pixel[1] / 255.0f;
					float originalB = pixel[2] / 255.0f;

					// Blend: original * (1 - alpha) + highlight * alpha
					pixel[0] = static_cast<uint8_t>((originalR * (1.0f - blendAlpha) + highlightColor[0] * blendAlpha) * 255.0f);
					pixel[1] = static_cast<uint8_t>((originalG * (1.0f - blendAlpha) + highlightColor[1] * blendAlpha) * 255.0f);
					pixel[2] = static_cast<uint8_t>((originalB * (1.0f - blendAlpha) + highlightColor[2] * blendAlpha) * 255.0f);
					// Alpha stays the same
				}
			}
		}

		// ScopedD3DMap destructor will automatically unmap the resource
		// Copy the modified texture back
		globals::d3d::context->CopyResource(texture, stagingTexture);

		// stagingGuard destructor will automatically release the staging texture
	}

	HRESULT CreateOverlayTextureAndRTV(ID3D11Device* device, int width, int height, ID3D11Texture2D** outTex, ID3D11RenderTargetView** outRTV)
	{
		// Input validation
		if (!device) {
			logger::error("CreateOverlayTextureAndRTV: device parameter is null");
			return E_INVALIDARG;
		}

		if (!outTex || !outRTV) {
			logger::error("CreateOverlayTextureAndRTV: output parameters cannot be null");
			return E_INVALIDARG;
		}

		if (width <= 0 || height <= 0) {
			logger::error("CreateOverlayTextureAndRTV: invalid dimensions ({}x{})", width, height);
			return E_INVALIDARG;
		}

		// Initialize output parameters
		*outTex = nullptr;
		*outRTV = nullptr;

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		HRESULT hr = device->CreateTexture2D(&desc, nullptr, outTex);
		if (FAILED(hr)) {
			logger::error("CreateOverlayTextureAndRTV: Failed to create texture (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
			return hr;
		}

		hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
		if (FAILED(hr)) {
			logger::error("CreateOverlayTextureAndRTV: Failed to create render target view (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
			// Clean up the texture if RTV creation failed
			if (*outTex) {
				(*outTex)->Release();
				*outTex = nullptr;
			}
			return hr;
		}

		return S_OK;
	}
}  // namespace Util
