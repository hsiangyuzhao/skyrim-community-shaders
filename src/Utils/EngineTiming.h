#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11Device;

/**
 * @brief (44f, C) Where the engine's own CPU time goes inside Main::Update, per real frame.
 *
 * Measurement only: nothing here changes what the game does.
 *
 * Main::Update (SkyrimSE.exe 1.5.97, ID 35565) runs the game's update -- a long list of engine
 * calls -- and then, at +0x5D2, the whole frame render (ID 35556), which contains Present. Each
 * direct call in Main::Update gets a probe: the call is redirected through a small stub that
 * notes "the engine is now in call N" and jumps on to the original target with every register
 * and the stack untouched. Inside the render, the existing render-phase scopes (shadow maps, the
 * world, water and reflection cubemaps, image space, the HUD, our own passes) split the time
 * further. Every millisecond of the main thread between two Presents lands in exactly one
 * segment, so the segments add up to the frame.
 *
 * The game update has no names in the Address Library, so it is reported per engine call (its
 * Address Library ID and offset in Main::Update): the trace names the slowest call of every
 * frame and lists every call that took over 1 ms. A spike can then be pinned on one call.
 *
 * Cost: one QPC read per probe and scope switch (about 120 a frame) while the overlay is on
 * screen or a trace is recording, a branch otherwise. SE 1.5.97 only; on any other runtime, or
 * if Main::Update does not look as expected, no probe is installed and the split is not shown.
 *
 * Escape hatches (both read once at startup, the probes are never installed later): Performance
 * Overlay > Engine Timing Probes (restart), and a file EngineTimingOff.txt next to SettingsUser.json,
 * which works even when the game cannot get as far as the menu.
 */
namespace Util::EngineTiming
{
	enum class Seg : int
	{
		Outer,       ///< outside Main::Update: the outer frame loop (Reflex sleep excluded)
		Sleep,       ///< Reflex sleep
		Logic,       ///< the game update: AI, scripts, physics, animation, ... (per engine call)
		RenderOther, ///< the frame render minus the phases below: culling, scene traversal, setup
		Shadows,     ///< shadow maps and the shadow mask
		Reflections, ///< water reflections / refraction and the reflection cubemap
		World,       ///< the depth prepass, the main scene and first person
		ImageSpace,  ///< the engine's image-space passes
		UI,          ///< the HUD and menus (Scaleform)
		OurPasses,   ///< Community Shaders' own passes (deferred composite, SSRT, upscaling, ...)
		Overlay,     ///< our overlay and menu (ImGui)
		Present,     ///< inside Present (the wait for the GPU / the display)
		Tail,        ///< Main::Update after the render
		Count
	};

	/// Short column / row name of a segment.
	const char* SegName(Seg a_seg);

	/// Installs the Main::Update probes (SE 1.5.97 only). Call once, at hook installation.
	void InstallUpdateProbes();
	/// Installs the device Create* timers (call once, after the device exists).
	void InstallDeviceHooks(ID3D11Device* a_device);
	/// True when the Main::Update probes are in place.
	bool ProbesInstalled();
	/// Why the probes are (not) in place, for the overlay and the Reflex menu: "installed",
	/// "off (setting)", "off (EngineTimingOff.txt)", "rejected (...)", "SE 1.5.97 only".
	const char* ProbeStatus();
	/// Name of the kill-switch file, looked for next to SettingsUser.json.
	inline constexpr const char* kKillSwitchFile = "EngineTimingOff.txt";
	/// (46l) Why engine probes must not be installed (SE only, kill switch, setting), or nullptr.
	const char* ProbeBlocker();
	/// (46l) Writes a probe stub: calls a_callback(a_index) with every register preserved, then jumps
	/// to a_target as if it had been called directly (shared with Util::WorldTiming).
	void EmitProbeStub(Xbyak::CodeGenerator& a_gen, int a_index, uintptr_t a_target, void (*a_callback)(int));

	// ---- frame structure (Hooks.cpp) ----
	/// Top of the Main_Update hook, after the Reflex sleep. a_frameToken: Streamline frame timing started this frame.
	void OnUpdateBegin(bool a_frameToken);
	/// Bottom of the Main_Update hook.
	void OnUpdateEnd();

	/// RAII segment for the hook sites that are not render-phase scopes (Reflex sleep, Present).
	struct ScopedSeg
	{
		explicit ScopedSeg(Seg a_seg);
		~ScopedSeg();
		ScopedSeg(const ScopedSeg&) = delete;
		ScopedSeg& operator=(const ScopedSeg&) = delete;

	private:
		bool pushed = false;
	};

	/// Render-phase scopes (GpuPhaseTimeline::Push / Pop, GpuScope as an int) mirrored here.
	void OnPhasePush(int a_gpuScope);
	void OnPhasePop(int a_gpuScope);

	// ---- (44f, A) Reflex markers at the start of the frame render ----
	/// True when this frame's SIMULATION_END and RENDERSUBMIT_START were already sent at the
	/// start of the frame render (the "Reflex Markers at Render Start" switch, with the probe in place).
	bool RenderStartMarked();

	// ---- (46n) eng_cs_passes split per pass ----
	//
	// Every main-thread millisecond of the cs_passes segment is billed to the innermost open pass:
	// a CpuPassScope key (feature name), a GpuPassTimers bucket ("g.<bucket>") or a PassScope below.
	// Time inside the segment with no pass open goes to "untimed.<scope>". The split is exclusive, so
	// the passes add up to eng_cs_passes. Blocking D3D11 calls, device Create* calls and CompileShader
	// on the main thread inside the segment are billed to the same pass. Only while FrameTrace::Active()
	// (overlay on screen or a trace recording); one branch otherwise.

	/// What a pass spent blocked in (all on the main thread, inside cs_passes).
	enum class PassBlock : int
	{
		Map,      ///< Map / Unmap
		Upload,   ///< CopyResource / CopySubresourceRegion / UpdateSubresource
		Flush,    ///< Flush
		Misc,     ///< GetData, Clear*, GenerateMips, ResolveSubresource
		Submit,   ///< Draw* / Dispatch*
		Create,   ///< device Create* calls
		Compile,  ///< Util::CompileShader (disk cache read or compile; includes the Create of the shader)
		Count
	};

	void PassBegin(std::string_view a_key);
	void PassEnd(std::string_view a_key);
	/// GpuPassTimers buckets (a_bucket = GpuBucket as int, a_name = its enum name).
	void PassBeginGpu(int a_bucket, std::string_view a_name);
	void PassEndGpu(int a_bucket);
	void AddPassBlock(PassBlock a_kind, double a_ms);
	/// Index of the frame in progress (TakeFrame closes it), for matching late GPU samples to rows.
	uint64_t FrameId();
	/// (46p) The pass open right now on the main thread (innermost; empty when none, off the main thread, or not tracking).
	std::string_view CurrentPassName();
	/// Interned pass name of a GPU bucket ("g.<bucket>"), or empty while it has not run.
	std::string_view GpuPassName(int a_bucket);

	/// RAII pass for our work that has no CpuPassScope (attribution only, no overlay row). The key must outlive the scope.
	struct PassScope
	{
		explicit PassScope(std::string_view a_key) :
			key(a_key) { PassBegin(key); }
		~PassScope() { PassEnd(key); }
		PassScope(const PassScope&) = delete;
		PassScope& operator=(const PassScope&) = delete;

	private:
		std::string_view key;
	};

	/// Times Util::CompileShader (main thread, while active).
	struct CompileTimer
	{
		CompileTimer();
		~CompileTimer();
		CompileTimer(const CompileTimer&) = delete;
		CompileTimer& operator=(const CompileTimer&) = delete;

	private:
		int64_t start = 0;
	};

	struct PassItem
	{
		std::string_view name;  ///< interned (lives as long as the process)
		int gpuBucket = -1;     ///< GpuBucket as int, -1 for CPU-only passes
		float cpuMs = 0.0f;     ///< exclusive main-thread ms inside cs_passes
		float gpuMs = -1.0f;    ///< GPU ms of the bucket's intervals this frame (filled in a few frames later; -1 = none)
		uint32_t calls = 0;     ///< times the pass was opened
		float blockMs[static_cast<int>(PassBlock::Count)]{};
		uint32_t blockCalls[static_cast<int>(PassBlock::Count)]{};
		bool untimed = false;   ///< "untimed.<scope>": cs_passes time with no pass open
	};

	// ---- (46o) M1: main-thread waits per segment ----
	//
	// At every segment change (not at the game-update call switches inside Logic) the main thread's CPU time is read
	// (QueryThreadCycleTime: one syscall, calibrated against QPC once on a background thread at startup). Per segment,
	// wait = wall time - thread CPU time: time the thread was blocked (inside D3D11 waiting for the GPU, on a lock, an event)
	// or not scheduled. Runs while a trace records, and while the pacer counts hidden GPU waits (F1a); a branch otherwise.

	/// Main thread, every frame before the pacer: the pacer wants the render segments' wait (F1a).
	void SetPacerWaitTracking(bool a_on);
	/// Main thread: the main thread's wait so far this frame in the render segments (render_other .. present, i.e. from the
	/// engine's render call on; not logic / tail / the frame-start sleeps), closing the open interval now. -1 when not
	/// measured (tracking off, not calibrated, probes not installed, not the main thread).
	double RenderWaitMsSoFar();
	/// Thread cycles per ms (0 until calibrated) and the measured cost of one QueryThreadCycleTime call (ns, -1 = not yet).
	double CyclesPerMs();
	double CycleReadCostNs();

	// ---- (46o) M2: stall sampler (trace only) ----
	//
	// While a trace records with Performance Overlay > Stall Sampler (trace) on, a watchdog thread looks at the main thread
	// every ~1 ms. When it has been in the same segment / game-update call for 8 ms (then every 4 ms more, at most 3 samples per
	// frame) it suspends the main thread, reads its instruction pointer and 64 qwords of its stack from RSP into a preallocated
	// buffer (GetThreadContext + ReadProcessMemory: no allocation, no lock, no logging while suspended), resumes it at once,
	// and only then turns the stack words that point into a loaded module right after a call instruction into module+offset.

	/// Per frame (FrameTrace::OnPresentEnd): on = recording with the switch on.
	void SetStallSampler(bool a_on);
	/// The samples taken since the last call ("seg@ms:mod+off>ret>...|..."), empty if none.
	std::string TakeStallSamples();

	// ---- per-frame result (FrameTrace::OnPresentEnd) ----
	struct Frame
	{
		uint64_t id = 0;             ///< (46n) FrameId() of this frame
		bool valid = false;
		float ms[static_cast<int>(Seg::Count)]{};
		/// (46o) M1: per segment wall - thread CPU time (-1 = not measured this frame), the thread's CPU time of the frame.
		float waitMs[static_cast<int>(Seg::Count)]{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
		float mainCpuMs = -1.0f;
		float inputMs = 0.0f;        ///< the input poll call (BSInputDeviceManager, ID 67315), part of Logic
		uint32_t topCallId = 0;      ///< Address Library ID of the slowest game-update call (0 = none)
		uint32_t topCallOffset = 0;  ///< its call site, offset in Main::Update
		float topCallMs = 0.0f;
		std::string callsOver1ms;    ///< "id@offset:ms;" for every update call over 1 ms (recording only)
		float engineDtMs = -1.0f;    ///< BSTimer delta this frame (game time step, after the engine's clamp)
		float engineRealDtMs = -1.0f;
		float createMs = 0.0f;       ///< main thread inside device Create* calls
		uint32_t creates = 0;
		float createOtherMs = 0.0f;  ///< other threads inside device Create* calls
		uint32_t createsOther = 0;
		// (46n) cs_passes split: sum over passes (equals ms[OurPasses]), the untimed part; -1 = not measured.
		float passSumMs = -1.0f;
		float passUntimedMs = -1.0f;
		std::vector<PassItem> passes;  ///< filled only when asked for (see TakeFrame), costliest CPU first
	};
	/// Closes the frame (since the previous call) and resets. a_detail: also build callsOver1ms.
	/// (46n) a_passWindow: also list the passes (with a_detail; they are listed too whenever cs_passes > 10 ms).
	Frame TakeFrame(bool a_detail, bool a_passWindow = false);
	/// (46n) cs_passes above this lists the passes in the trace and logs the frame.
	inline constexpr float kPassSpikeMs = 10.0f;
}
