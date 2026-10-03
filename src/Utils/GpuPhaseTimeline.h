#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <vector>
#include <winrt/base.h>

namespace RE
{
	class BSShader;
}

namespace Util
{
	/**
	 * @brief (batch 36, item 0) The rows of the overlay's "Engine passes (GPU)" table.
	 *
	 * Every GPU microsecond between two Presents is billed to exactly one of these, so the
	 * rows add up to the frame. Order here is display order inside each group; the overlay
	 * owns the grouping.
	 */
	enum class GpuPhase : uint8_t
	{
		Untracked = 0,  // nothing below covers it

		// Shadows
		ShadowSunCascade1,
		ShadowSunCascade2,
		ShadowSunCascade3,
		ShadowSunCascade4,
		ShadowLocalLights,  // spot + omni (torches, lamps, magic lights)
		ShadowMask,         // the full-screen passes that apply shadow maps to the screen
		ShadowOther,        // shadow pass work outside the per-light calls

		// Main view, before the world
		WaterPrep,     // engine water work before the main view (reflections, ripples)
		DepthPrepass,  // engine depth-only prepass

		// Opaque G-buffer pass (Deferred::StartDeferred -> EndDeferred), split per draw
		OpaqueTerrain,
		OpaqueObjects,
		OpaqueCharacters,
		OpaqueTrees,
		OpaqueGrass,
		OpaqueDistant,  // terrain / object / tree LOD
		OpaqueOther,    // decals, effect meshes, anything else drawn in the opaque pass

		// Rest of RenderWorld, split per draw
		Sky,
		Water,
		Transparent,  // alpha-blended objects, effects, particles
		WorldOther,   // world work before any draw could be classified

		FirstPerson,
		Reflections,  // BSCubeMapCamera cubemaps
		Imagespace,   // the engine's post-processing chain (and imagespace draws inside the world)
		UI,

		// Community Shaders' own work
		CsPasses,     // everything inside a GpuPassTimers interval (the GPU Passes table)
		CsOther,      // our untimed work: prepasses, deferred composite
		CsUpscaling,  // DLSS / FSR / NIS and the copies around them
		CsOverlay,    // ImGui: this overlay and the CS menu

		Count
	};

	/// @brief What a hook pushes. A scope either maps to one phase or splits per draw.
	enum class GpuScope : uint8_t
	{
		ShadowMaps,
		ShadowSun,  // splits per cascade
		ShadowLocal,
		ShadowMask,
		WaterPrep,
		DepthPrepass,
		World,   // splits into sky / water / transparent / imagespace
		Opaque,  // splits into terrain / objects / characters / trees / grass / LOD
		FirstPerson,
		Reflections,
		Imagespace,
		UI,
		CsPasses,
		CsOther,
		CsUpscaling,
		CsOverlay,
		Count
	};

	/**
	 * @brief One chain of GPU timestamps per frame, one timestamp per stage switch.
	 *
	 * The frame is opened right after Present returns and closed right before the next
	 * Present. In between, hooks push and pop scopes, and inside the scopes that split per
	 * draw (shadow cascades, the opaque pass, the rest of the world) State::Draw reclassifies
	 * every draw. A timestamp is issued only when the resulting stage actually changes, so a
	 * frame costs tens to a few hundred queries, never one per draw.
	 *
	 * The interval between two consecutive timestamps belongs to the stage that was active
	 * after the first of them. That makes every row EXCLUSIVE - a scope nested inside
	 * another (one of our passes inside the opaque pass, a shadow light inside the shadow
	 * pass) takes its time away from the outer one - and the rows add up to the whole chain
	 * by construction, so nothing is counted twice.
	 *
	 * Query rules, same as GpuFrameTimer: D3D11 forbids nesting disjoint queries, and
	 * GpuPassTimers holds one open around each of our passes. Only the first and the last
	 * timestamp get a (degenerate) disjoint window of their own, both at Present where no
	 * pass interval is open; a frame is valid if both report the same non-zero frequency and
	 * no Disjoint flag. The timestamps in between are plain End() calls, which D3D11 allows
	 * anywhere. Results are read back with DONOTFLUSH out of a small ring, never waited for.
	 *
	 * Everything is a no-op unless the overlay's pass table is on screen. Render thread only.
	 */
	class GpuPhaseTimeline
	{
	public:
		static GpuPhaseTimeline* GetSingleton();

		/// @brief Installs the engine hooks this timeline needs that nobody else owns.
		static void InstallHooks();

		/// @brief Opens a frame. Call right after the real Present returns.
		void BeginFrame();
		/// @brief Closes the frame. Call right before the real Present.
		void EndFrame();

		void Push(GpuScope a_scope);
		/// @brief Pops a_scope and anything still above it. Ignored if it is not on the stack.
		void Pop(GpuScope a_scope);

		/// @brief Per-draw hook (State::Draw). Reclassifies the draw in split scopes and counts it.
		void OnDraw(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
		{
			if (frameActive)
				OnDrawActive(a_shader, a_vertexDescriptor, a_pixelDescriptor);
		}

		/// RE::BSShader::Type::Total (checked in the .cpp).
		static constexpr size_t kShaderTypeCount = 11;

		struct Report
		{
			bool hasSample = false;
			std::array<float, static_cast<size_t>(GpuPhase::Count)> ms{};
			std::array<float, static_cast<size_t>(GpuPhase::Count)> draws{};
			float totalMs = 0.0f;             ///< first to last timestamp of the frame
			float timestampsPerFrame = 0.0f;  ///< smoothed, for the tooltip
			uint32_t droppedFrames = 0;       ///< frames that hit the timestamp cap
			/// Draws that landed in Untracked, by RE::BSShader::Type: what to hook next if that row is big.
			std::array<float, kShaderTypeCount> untrackedDrawsByType{};
		};

		const Report& Get() const { return report; }

		void Reset();

	private:
		static constexpr int kFramesInFlight = 5;
		// Per-frame cap. Normal frames need tens to a few hundred; a frame that runs out is
		// dropped (its attribution after the cap would be wrong) and counted in the report.
		static constexpr int kMaxTimestamps = 2048;
		static constexpr int kMaxDepth = 16;
		static constexpr int kMaxCascades = 4;
		static constexpr float kSmoothingOld = 0.95f;
		static constexpr float kSmoothingNew = 0.05f;
		static constexpr double kMinPlausibleMs = 0.05;
		static constexpr double kMaxPlausibleMs = 2000.0;

		struct FrameSlot
		{
			winrt::com_ptr<ID3D11Query> disjointBegin;
			winrt::com_ptr<ID3D11Query> disjointEnd;
			std::vector<winrt::com_ptr<ID3D11Query>> stamps;
			std::vector<GpuPhase> phases;  // phases[i] owns the interval stamps[i] -> stamps[i + 1]
			int count = 0;
			bool pending = false;
			bool truncated = false;
		};

		struct StackEntry
		{
			GpuScope scope;
			GpuPhase phase;
		};

		enum class SlotStatus
		{
			NotReady,
			Ready,
			Invalid
		};

		bool OverlayWantsTimings() const;
		bool EnsureStamp(FrameSlot& a_slot, int a_index) const;
		bool EnsureDisjoint(FrameSlot& a_slot) const;
		void SwitchTo(GpuPhase a_phase);
		void OnDrawActive(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor);
		GpuPhase ClassifySunCascade();
		void Collect();
		SlotStatus TryRead(FrameSlot& a_slot, std::array<double, static_cast<size_t>(GpuPhase::Count)>& a_outMs, double& a_outTotalMs);

		FrameSlot slots[kFramesInFlight];
		int writeSlot = 0;
		bool frameActive = false;
		bool reseed = true;       // next timing sample replaces the averages instead of blending in
		bool reseedDraws = true;  // same for the draw counts
		ID3D11Device* queryDevice = nullptr;

		StackEntry stack[kMaxDepth]{};
		int depth = 0;
		int overflowDepth = 0;  // pushes beyond kMaxDepth, popped without effect
		GpuPhase current = GpuPhase::Untracked;

		uint64_t cascadeKeys[kMaxCascades]{};
		int cascadeCount = 0;

		std::array<uint32_t, static_cast<size_t>(GpuPhase::Count)> frameDraws{};
		std::array<uint32_t, kShaderTypeCount> frameUntrackedTypes{};
		std::vector<UINT64> scratchTicks;
		Report report;
	};

	/// @brief RAII push/pop of a GpuPhaseTimeline scope.
	struct GpuPhaseScope
	{
		explicit GpuPhaseScope(GpuScope a_scope) :
			scope(a_scope)
		{
			GpuPhaseTimeline::GetSingleton()->Push(scope);
		}
		~GpuPhaseScope() { GpuPhaseTimeline::GetSingleton()->Pop(scope); }

		GpuPhaseScope(const GpuPhaseScope&) = delete;
		GpuPhaseScope& operator=(const GpuPhaseScope&) = delete;

	private:
		GpuScope scope;
	};
}
