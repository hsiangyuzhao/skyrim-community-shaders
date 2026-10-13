#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

/**
 * @brief (45, RT prototype) The ray tracer's own D3D12 side: device, queue, BLAS/TLAS, trace pass.
 *
 * Interop follows Upscaling/NeuralRendering/D3D12Interop.cpp: textures are created on D3D11 with
 * NT shared handles and opened on D3D12, and one shared fence orders the two queues -- D3D11
 * signals, the D3D12 queue waits on the GPU, D3D12 signals, the D3D11 queue waits on the GPU.
 * The CPU never waits for the GPU: when every command context is still in flight the frame's
 * trace is skipped (counted in stats) instead.
 *
 * Note: D3D12CreateDevice returns one device object per adapter and process, so "our" device is
 * the same object Streamline / DLSS-G and Neural Rendering use; everything of ours on it (queue,
 * lists, fence, resources) is separate. The device is created on first use, never with Ray
 * Tracing off.
 *
 * Render thread only.
 */
namespace RayTracingRT
{
	using Microsoft::WRL::ComPtr;

	/// Triangles of one BLAS, positions relative to the owner's origin (a cell corner).
	struct ChunkGeometry
	{
		std::vector<float> positions;  // xyz
		std::vector<std::uint32_t> indices;
		[[nodiscard]] std::uint32_t Triangles() const { return static_cast<std::uint32_t>(indices.size() / 3); }
		[[nodiscard]] std::uint32_t Vertices() const { return static_cast<std::uint32_t>(positions.size() / 3); }
	};

	using BlasHandle = std::uint32_t;
	inline constexpr BlasHandle kInvalidBlas = UINT32_MAX;

	struct TraceConstants
	{
		float invViewProj[16];
		float toSun[4];      // xyz, w = tan(cone half angle)
		float params[4];     // rays, frame, flags (bit pattern), max ray length
		float renderSize[4]; // w, h, 1/w, 1/h
		float rect[4];       // camera-relative xy min / max
		float bias[4];       // constant, per unit distance, coverage probe
	};
	static_assert(sizeof(TraceConstants) == 36 * 4);

	struct Instance
	{
		BlasHandle blas;
		double x, y, z;  // world translation of the BLAS origin
	};

	struct FrameInput
	{
		TraceConstants constants{};
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		double cameraX = 0.0, cameraY = 0.0, cameraZ = 0.0;
		const std::vector<Instance>* instances = nullptr;
		std::uint32_t buildTriangleBudget = 0;
	};

	struct Stats
	{
		std::uint32_t blasCount = 0;        // live BLASes with GPU memory
		std::uint32_t blasQueued = 0;       // waiting to be built
		std::uint64_t blasTriangles = 0;    // triangles in built BLASes
		std::uint64_t blasBytes = 0;
		std::uint64_t scratchBytes = 0;
		std::uint64_t tlasBytes = 0;
		std::uint64_t uploadBytes = 0;      // instance rings and in-flight geometry uploads
		std::uint64_t sharedTextureBytes = 0;
		std::uint32_t instancesLastFrame = 0;
		std::uint32_t buildsLastFrame = 0;
		std::uint32_t compactionsLastFrame = 0;
		std::uint64_t buildsTotal = 0;
		std::uint64_t compactedSavedBytes = 0;
		std::uint32_t skippedFramesBusy = 0;  // every command context still in flight
		float lastGpuMs = 0.0f;               // trace + builds on our queue, from D3D12 timestamps
		float smoothedGpuMs = 0.0f;
	};

	class Backend
	{
	public:
		~Backend();

		/// Creates the device, queue, fence, pipeline. On failure fills o_reason and returns false.
		bool Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string& o_reason);
		/// Releases everything. a_waitForGpu: a bounded CPU wait so resources can go now (exit / disable only).
		void Shutdown(bool a_waitForGpu);
		[[nodiscard]] bool IsReady() const { return ready_; }

		/// Shared textures sized to the game's shadow mask. Recreated when the size changes.
		bool EnsureTextures(std::uint32_t a_width, std::uint32_t a_height, std::string& o_reason);
		[[nodiscard]] ID3D11UnorderedAccessView* DepthMaskUAV() const { return depthMask_.uav11.Get(); }
		[[nodiscard]] ID3D11ShaderResourceView* DepthMaskSRV() const { return depthMask_.srv11.Get(); }
		[[nodiscard]] ID3D11ShaderResourceView* VisibilitySRV() const { return visibility_.srv11.Get(); }

		/// Queues a BLAS build for this geometry (takes it). Built over the next frames.
		BlasHandle CreateBlas(std::unique_ptr<ChunkGeometry> a_geometry);
		/// The BLAS goes away once the GPU is done with every submission that may use it.
		void ReleaseBlas(BlasHandle a_handle);
		void ReleaseAllBlas();
		[[nodiscard]] bool IsBlasBuilt(BlasHandle a_handle) const;

		/// Records and submits this frame's D3D12 work between two GPU-side fence waits.
		/// Returns false when nothing was traced (busy ring or a failure; see LastError()).
		bool ExecuteFrame(const FrameInput& a_input);

		/// Frees retired resources the GPU is done with (also while nothing is traced).
		void Tick()
		{
			if (ready_)
				CollectRetired();
		}

		/// True when the device was removed or a D3D12 call failed; the feature then turns itself off.
		[[nodiscard]] bool HasFailed() const { return failed_; }
		[[nodiscard]] const std::string& LastError() const { return lastError_; }
		[[nodiscard]] const Stats& GetStats() const { return stats_; }
		[[nodiscard]] std::string AdapterDescription() const { return adapterDescription_; }

	private:
		static constexpr std::size_t kContexts = 8;
		static constexpr std::uint32_t kPostbuildSlots = 1024;
		// Both on D3D11's guaranteed-shareable list; the shaders' declared types follow these.
		static constexpr DXGI_FORMAT kDepthMaskFormat = DXGI_FORMAT_R32_FLOAT;        // depth; negative = engine mask already 0
		static constexpr DXGI_FORMAT kVisibilityFormat = DXGI_FORMAT_R8G8B8A8_UNORM;  // r visibility, g coverage

		struct SharedTexture
		{
			ComPtr<ID3D11Texture2D> resource11;
			ComPtr<ID3D11ShaderResourceView> srv11;
			ComPtr<ID3D11UnorderedAccessView> uav11;
			ComPtr<ID3D12Resource> resource12;
		};

		struct CommandContext
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12GraphicsCommandList4> list;
			ComPtr<ID3D12Resource> instanceBuffer;  // upload heap, persistently mapped
			void* instanceMapped = nullptr;
			std::uint32_t instanceCapacity = 0;
			std::uint64_t fenceValue = 0;
			bool timestampsPending = false;
		};

		enum class BlasState : std::uint8_t
		{
			Free,
			Queued,
			Built,      // usable; compaction size not read back yet
			Ready       // compacted (or compaction not worth it)
		};

		struct Blas
		{
			BlasState state = BlasState::Free;
			std::unique_ptr<ChunkGeometry> geometry;
			ComPtr<ID3D12Resource> resource;
			std::uint64_t bytes = 0;
			std::uint32_t triangles = 0;
			std::uint32_t postbuildSlot = UINT32_MAX;
			std::uint64_t buildFence = 0;
		};

		struct Retired
		{
			std::uint64_t fence;
			ComPtr<ID3D12Pageable> object;
			std::uint64_t bytes;
			std::uint64_t* counter;  // stat to subtract from when released (may be null)
		};

		bool Fail(const std::string& a_operation, HRESULT a_result);
		void Retire(ComPtr<ID3D12Pageable> a_object, std::uint64_t a_bytes, std::uint64_t* a_counter);
		void CollectRetired();
		void CollectTimestamps(std::size_t a_index);
		ComPtr<ID3D12Resource> CreateBuffer(std::uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, bool a_uav);
		bool CheckSharedFormat(DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store, const char* a_name);
		bool CreateSharedTexture(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store,
			SharedTexture& o_texture, const char* a_name);
		bool CreatePipeline(std::string& o_reason);
		void WriteDescriptors();
		bool EnsureScratch(std::uint64_t a_bytes);
		bool EnsureTlas(std::uint32_t a_instances);
		bool EnsureInstanceBuffer(CommandContext& a_context, std::uint32_t a_instances);
		void ProcessCompaction(ID3D12GraphicsCommandList4* a_list, std::uint64_t a_completed);
		std::uint32_t BuildQueued(ID3D12GraphicsCommandList4* a_list, std::uint32_t a_budget);
		BlasHandle AllocateHandle();

		ComPtr<ID3D11Device5> device11_;
		ComPtr<ID3D11DeviceContext4> context11_;
		ComPtr<ID3D11Fence> fence11_;
		ComPtr<ID3D12Device5> device_;
		ComPtr<ID3D12CommandQueue> queue_;
		ComPtr<ID3D12Fence> fence_;
		std::array<CommandContext, kContexts> contexts_;
		std::size_t cursor_ = 0;
		std::uint64_t fenceValue_ = 0;
		std::uint64_t lastSubmitted_ = 0;

		ComPtr<ID3D12RootSignature> rootSignature_;
		ComPtr<ID3D12PipelineState> pipeline_;
		ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
		UINT descriptorSize_ = 0;

		SharedTexture depthMask_;   // kDepthMaskFormat
		SharedTexture visibility_;  // kVisibilityFormat
		std::uint32_t textureWidth_ = 0, textureHeight_ = 0;

		ComPtr<ID3D12Resource> tlas_;
		ComPtr<ID3D12Resource> scratch_;
		std::uint64_t tlasBytes_ = 0, scratchBytes_ = 0;
		std::uint32_t tlasCapacity_ = 0;

		ComPtr<ID3D12Resource> postbuild_;           // default heap, UAV
		ComPtr<ID3D12Resource> postbuildReadback_;   // readback heap
		const std::uint64_t* postbuildMapped_ = nullptr;
		std::uint32_t postbuildCursor_ = 0;

		ComPtr<ID3D12QueryHeap> timestampHeap_;
		ComPtr<ID3D12Resource> timestampReadback_;
		std::uint64_t timestampFrequency_ = 0;

		std::vector<Blas> blas_;
		std::vector<BlasHandle> freeHandles_;
		std::deque<BlasHandle> buildQueue_;
		std::vector<Retired> retired_;

		HANDLE fenceEvent_ = nullptr;
		Stats stats_{};
		std::string lastError_;
		std::string adapterDescription_;
		bool ready_ = false;
		bool failed_ = false;
	};
}
