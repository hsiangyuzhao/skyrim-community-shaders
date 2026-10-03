#pragma once

#include <cstdint>
#include <d3d11.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

namespace Util
{
	/**
	 * @brief (batch 36e) Per-dispatch GPU timing of the SSRT denoiser chain.
	 *
	 * The GPU Passes table prices REBLUR as one row ("SSRT REBLUR"). Deciding what to make
	 * cheaper needs the split underneath it: every dispatch NRD schedules for the diffuse and
	 * the specular instance, and the passes of ours around them (guides, motion-vector copy,
	 * sparse resolve, unpack, the confidence filter stages).
	 *
	 * Each scope gets two plain GPU timestamps. D3D11 forbids nesting disjoint queries and
	 * GpuPassTimers holds one open around each of our passes, so - exactly like
	 * GpuPhaseTimeline - only the frame boundaries get a (degenerate) disjoint window of their
	 * own, both at Present where no pass interval is open. A frame's timestamps are valid if
	 * both windows report the same non-zero frequency and neither is Disjoint. Plain
	 * timestamps are allowed anywhere, including inside another timer's open interval.
	 *
	 * Nothing here changes what any pass does; the only GPU work added is the timestamps.
	 * Everything is a no-op unless the overlay is on screen with its "Denoiser breakdown"
	 * section expanded. Results are read back with DONOTFLUSH, never waited for.
	 * Render thread only.
	 */
	class DenoiserTimers
	{
	public:
		static DenoiserTimers* GetSingleton();

		/// @brief Opens the frame. Call right after the real Present returns.
		void BeginFrame();
		/// @brief Closes the frame. Call right before the real Present.
		void EndFrame();

		/**
		 * @brief Opens a timed scope and returns a token for End(), or -1 when timing is off.
		 *
		 * @param a_group   Row group, e.g. "NRD diffuse". Must be a string literal or otherwise
		 *                  outlive the call only as long as the call (it is copied on first use).
		 * @param a_pass    Pass name within the group (NRD's own dispatch name for NRD passes).
		 * @param a_groupsX Thread groups dispatched in X (0 for a copy).
		 * @param a_groupsY Thread groups dispatched in Y.
		 * @param a_threadsX Threads covered in X = groups x group size (or the copy's width).
		 * @param a_threadsY Threads covered in Y.
		 */
		int Begin(std::string_view a_group, std::string_view a_pass, uint32_t a_groupsX, uint32_t a_groupsY, uint32_t a_threadsX, uint32_t a_threadsY);
		void End(int a_token);

		/// @brief True while scopes are being recorded this frame (cheap check for call sites).
		bool IsRecording() const { return frameActive; }

		struct Row
		{
			std::string group;
			std::string pass;
			uint32_t groupsX = 0;
			uint32_t groupsY = 0;
			uint32_t threadsX = 0;
			uint32_t threadsY = 0;
			float lastMs = 0.0f;       ///< raw time in the most recently collected frame (sum of its calls)
			int lastCalls = 0;         ///< scopes folded into lastMs
			uint64_t lastFrame = 0;    ///< frame the row was last measured in
			int order = 0;             ///< first-seen sequence: pipeline order, new rows at the end
		};

		const std::vector<Row>& Rows() const { return rows; }
		/// Frame index of the most recently collected frame (0 = none yet).
		uint64_t LatestFrame() const { return latestFrame; }

		void Reset();

	private:
		static constexpr int kFramesInFlight = 5;
		static constexpr int kMaxStamps = 512;  // 256 scopes per frame; REBLUR x2 needs ~60
		static constexpr double kMaxPlausibleMs = 1000.0;

		struct Scope
		{
			int row = -1;
			int beginStamp = -1;
			int endStamp = -1;
		};

		struct FrameSlot
		{
			winrt::com_ptr<ID3D11Query> disjointBegin;
			winrt::com_ptr<ID3D11Query> disjointEnd;
			std::vector<winrt::com_ptr<ID3D11Query>> stamps;
			std::vector<Scope> scopes;
			int stampCount = 0;
			uint64_t frameIndex = 0;
			bool pending = false;
		};

		enum class SlotStatus
		{
			NotReady,
			Ready,
			Invalid
		};

		bool Wanted() const;
		bool EnsureDisjoint(FrameSlot& a_slot) const;
		bool EnsureStamp(FrameSlot& a_slot, int a_index) const;
		int FindOrAddRow(std::string_view a_group, std::string_view a_pass, int a_occurrence);
		void Collect();
		SlotStatus TryRead(FrameSlot& a_slot);

		FrameSlot slots[kFramesInFlight];
		int writeSlot = 0;
		bool frameActive = false;
		uint64_t frameIndex = 0;
		uint64_t latestFrame = 0;
		ID3D11Device* queryDevice = nullptr;

		std::vector<Row> rows;
		std::unordered_map<std::string, int> rowIndex;
		std::unordered_map<std::string, int> occurrences;  // per frame, for repeated pass names
		std::vector<UINT64> scratchTicks;
		std::vector<double> scratchMs;
		std::vector<int> scratchCalls;
		int nextOrder = 0;
	};

	/// @brief RAII scope for DenoiserTimers. Arguments as DenoiserTimers::Begin.
	struct DenoiserTimerScope
	{
		DenoiserTimerScope(std::string_view a_group, std::string_view a_pass, uint32_t a_groupsX, uint32_t a_groupsY, uint32_t a_threadsX, uint32_t a_threadsY) :
			token(DenoiserTimers::GetSingleton()->Begin(a_group, a_pass, a_groupsX, a_groupsY, a_threadsX, a_threadsY)) {}
		~DenoiserTimerScope() { DenoiserTimers::GetSingleton()->End(token); }

		DenoiserTimerScope(const DenoiserTimerScope&) = delete;
		DenoiserTimerScope& operator=(const DenoiserTimerScope&) = delete;

	private:
		int token;
	};
}
