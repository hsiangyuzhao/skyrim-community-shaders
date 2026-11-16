#include "D3D.h"

#include "ShaderCache.h"
#include "State.h"
#include "Utils/ContentHash.h"
#include "Utils/EngineTiming.h"
#include "Utils/FrameTrace.h"
#include "Utils/Format.h"

#include <atomic>
#include <chrono>
#include <d3dcompiler.h>
#include <fstream>
#include <thread>
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

	namespace
	{
		/// (48g) What CompileShader reads from the plugin's state, captured on the calling thread so a background compile never reads
		/// globals::state (its define list) while the main thread may change it.
		struct CompileEnv
		{
			bool vr = false;
			bool developer = false;
			bool diskCache = false;
			std::vector<std::pair<std::string, std::string>> defines;
		};

		CompileEnv CaptureCompileEnv()
		{
			CompileEnv env;
			env.vr = REL::Module::IsVR();
			env.developer = globals::state->IsDeveloperMode();
			env.diskCache = globals::shaderCache && globals::shaderCache->IsDiskCache();
			if (const auto shaderDefines = globals::state->GetDefines(); shaderDefines && !shaderDefines->empty())
				env.defines.assign(shaderDefines->begin(), shaderDefines->end());
			return env;
		}

		struct Bytecode
		{
			winrt::com_ptr<ID3DBlob> blob;
			std::optional<std::wstring> diskPath;  // the cache entry (read from, or to write once the shader object exists)
			bool fromCache = false;
		};

		/// (48g) The compile half of CompileShader (macros, flags and disk-cache key exactly as before 48g): bytecode only, no D3D11 object,
		/// so it may run on a worker thread. a_skipCache: compile from source (a cached entry was rejected by the device).
		Bytecode CompileBytecode(const CompileEnv& env, const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines,
			const char* ProgramType, const char* Program, bool a_skipCache)
		{
			Bytecode result;
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

			if (env.vr)
				macros.push_back({ "VR", "" });
			if (env.developer) {
				macros.push_back({ "D3DCOMPILE_SKIP_OPTIMIZATION", "" });
				macros.push_back({ "D3DCOMPILE_DEBUG", "" });
			}
			for (const auto& [name, value] : env.defines)
				macros.push_back({ name.c_str(), value.c_str() });
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
				return result;

			// Add null terminating entry
			macros.push_back({ "WINPC", "" });
			macros.push_back({ "DX11", "" });
			macros.push_back({ nullptr, nullptr });

			// Compiler setup
			uint32_t flags = !env.developer ? (D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3) : D3DCOMPILE_DEBUG;
			// (44j, upstream) Disk cache on = shipped, known-good shaders: skip fxc's validation pass on its own output.
			const bool useDiskCache = env.diskCache;
			if (useDiskCache && !env.developer)
				flags |= D3DCOMPILE_SKIP_VALIDATION;

			// (S4.14) RAII: both blobs are com_ptr (they leaked as raw pointers before, and the error blob was read uninitialised on a
			// clean compile). com_ptr's destructor covers every exit.
			winrt::com_ptr<ID3DBlob> shaderBlob;
			winrt::com_ptr<ID3DBlob> shaderErrors;

			if (!std::filesystem::exists(FilePath)) {
				logger::error("Failed to compile shader; {} does not exist", str);
				return result;
			}
			if (useDiskCache && IsCacheableTarget(ProgramType))
				result.diskPath = GetCompileShaderDiskPath(FilePath, macros.data(), &include, Program, ProgramType, flags);

			if (!a_skipCache && result.diskPath && std::filesystem::exists(*result.diskPath)) {
				if (auto cachedBlob = SIE::SShaderCache::ReadIntactBlob(*result.diskPath)) {
					result.blob = std::move(cachedBlob);
					result.fromCache = true;
					return result;
				}
			}

			logger::debug("Compiling {} with {}", str, DefinesToString(macros));
			if (FAILED(D3DCompileFromFile(FilePath, macros.data(), &include, Program, ProgramType, flags, 0, shaderBlob.put(), shaderErrors.put()))) {
				logger::warn("Shader compilation failed:\n\n{}", shaderErrors ? static_cast<char*>(shaderErrors->GetBufferPointer()) : "Unknown error");
				return result;
			}
			// A successful compile can still have produced warnings, and can equally have produced
			// no blob at all -- hence the test rather than an unconditional read.
			if (shaderErrors)
				logger::debug("Shader logs:\n{}", static_cast<char*>(shaderErrors->GetBufferPointer()));
			// Defensive: a SUCCEEDED return with no bytecode would make every GetBufferPointer
			// below a null dereference. Not observed, but it is one branch against a hard crash.
			if (!shaderBlob) {
				logger::error("Shader compilation reported success but produced no bytecode for {}", str);
				return result;
			}
			result.blob = std::move(shaderBlob);
			return result;
		}

		/// (48g) The creation half, on the calling thread (the main thread for every background compile). A cached entry the device rejects
		/// is dropped and the shader compiled again from source (as before 48g); a fresh compile is written to the cache once its object exists.
		HRESULT CreateFromBytecode(Bytecode& a_bytecode, const CompileEnv& env, const wchar_t* FilePath,
			const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program, ID3D11DeviceChild** o_shader)
		{
			*o_shader = nullptr;
			auto device = globals::d3d::device;
			if (!a_bytecode.blob)
				return E_FAIL;
			HRESULT createResult = CreateShaderObject(device, ProgramType, a_bytecode.blob.get(), o_shader);
			if (a_bytecode.fromCache) {
				if (SUCCEEDED(createResult) && *o_shader) {
					logger::debug("Loaded {} from {}", Util::WStringToString(FilePath), Util::WStringToString(*a_bytecode.diskPath));
					return createResult;
				}
				if (*o_shader) {
					(*o_shader)->Release();
					*o_shader = nullptr;
				}
				// An intact container can still hold bytecode the runtime rejects; drop the entry and compile from source.
				logger::warn("Discarding cached shader {} rejected by the device", Util::WStringToString(*a_bytecode.diskPath));
				std::error_code ec;
				std::filesystem::remove(*a_bytecode.diskPath, ec);
				a_bytecode = CompileBytecode(env, FilePath, Defines, ProgramType, Program, true);
				if (!a_bytecode.blob)
					return E_FAIL;
				createResult = CreateShaderObject(device, ProgramType, a_bytecode.blob.get(), o_shader);
			}
			if (SUCCEEDED(createResult) && *o_shader && a_bytecode.diskPath)
				StoreCompileShaderBlob(*a_bytecode.diskPath, a_bytecode.blob.get());
			return createResult;
		}
	}

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program)
	{
		EngineTiming::CompileTimer compileTimer;  // (46n) billed to the pass that asked for it (F7 trace pass_block "s")
		const CompileEnv env = CaptureCompileEnv();
		Bytecode bytecode = CompileBytecode(env, FilePath, Defines, ProgramType, Program, false);
		if (!bytecode.blob)
			return nullptr;
		ID3D11DeviceChild* shader = nullptr;
		const HRESULT createResult = CreateFromBytecode(bytecode, env, FilePath, Defines, ProgramType, Program, &shader);
		if (!bytecode.blob)
			return nullptr;  // the cached entry was rejected and the compile from source failed
		// Same failure policy as before 44j: compute creation failures throw, the other stages return whatever
		// the device produced (null on failure).
		if (IsComputeTarget(ProgramType))
			DX::ThrowIfFailed(createResult);
		return shader;
	}

	// (48e) Background compiles. (48g) The worker produces bytecode only (defines captured on the main thread at Request); the shader object
	// is created and released on the main thread (GetStatus / Reset), so an orphaned job (Reset while compiling) holds only a blob, never a
	// D3D11 object.
	struct AsyncComputeShader::Job
	{
		std::atomic<int> state{ 1 };  // 1 compiling, 2 bytecode ready, 3 failed (release-stored by the worker)
		Bytecode bytecode;            // written by the worker before state
		CompileEnv env;               // captured on the main thread at Request
		std::wstring path;
		std::vector<std::pair<std::string, std::string>> defines;
		double ms = 0.0;
	};

	void AsyncComputeShader::Request(const wchar_t* a_path, const std::vector<std::pair<std::string, std::string>>& a_defines, std::string_view a_label)
	{
		std::string newKey = WStringToString(a_path);
		for (const auto& [name, value] : a_defines)
			newKey += std::format("|{}={}", name, value);
		if (job && newKey == key)
			return;
		Reset();
		key = std::move(newKey);
		label = a_label;
		auto newJob = std::make_shared<Job>();
		newJob->env = CaptureCompileEnv();
		newJob->path = a_path;
		newJob->defines = a_defines;
		job = newJob;
		logger::info("[Shaders] {}: compiling in the background (disk cache {})", label, newJob->env.diskCache ? "on" : "off");
		try {
			std::thread([newJob]() {
				// Below normal: a cache miss compiles for seconds; the game's own threads come first.
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				const auto start = std::chrono::steady_clock::now();
				bool ok = false;
				try {
					std::vector<std::pair<const char*, const char*>> macros;
					macros.reserve(newJob->defines.size());
					for (const auto& [name, value] : newJob->defines)
						macros.push_back({ name.c_str(), value.c_str() });
					newJob->bytecode = CompileBytecode(newJob->env, newJob->path.c_str(), macros, "cs_5_0", "main", false);
					ok = newJob->bytecode.blob != nullptr;
				} catch (...) {
					ok = false;
				}
				newJob->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				newJob->state.store(ok ? 2 : 3, std::memory_order_release);
			}).detach();
		} catch (const std::exception& e) {
			newJob->state.store(3, std::memory_order_release);
			logger::warn("[Shaders] {}: no background thread ({})", label, e.what());
		}
	}

	AsyncComputeShader::Status AsyncComputeShader::GetStatus()
	{
		if (!job)
			return Status::Idle;
		const int state = job->state.load(std::memory_order_acquire);
		if (state == 1)
			return Status::Compiling;
		if (!reported) {
			reported = true;
			compileMs = job->ms;
			if (state == 2) {
				// (48g) The shader object, here on the main thread.
				std::vector<std::pair<const char*, const char*>> macros;
				macros.reserve(job->defines.size());
				for (const auto& [name, value] : job->defines)
					macros.push_back({ name.c_str(), value.c_str() });
				ID3D11DeviceChild* object = nullptr;
				HRESULT hr = E_FAIL;
				try {
					hr = CreateFromBytecode(job->bytecode, job->env, job->path.c_str(), macros, "cs_5_0", "main", &object);
				} catch (...) {
					hr = E_FAIL;
				}
				job->bytecode.blob = nullptr;
				if (SUCCEEDED(hr) && object)
					shader.attach(static_cast<ID3D11ComputeShader*>(object));
				else if (object)
					object->Release();
				created = shader != nullptr;
			}
			if (created)
				logger::info("[Shaders] {}: ready after {:.0f} ms in the background", label, compileMs);
			else
				logger::warn("[Shaders] {}: did not compile ({:.0f} ms); the old path keeps running", label, compileMs);
		}
		return created ? Status::Ready : Status::Failed;
	}

	ID3D11ComputeShader* AsyncComputeShader::Poll()
	{
		return GetStatus() == Status::Ready ? shader.get() : nullptr;
	}

	void AsyncComputeShader::Reset()
	{
		job.reset();
		shader = nullptr;
		created = false;
		key.clear();
		compileMs = 0.0;
		reported = false;
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
