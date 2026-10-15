#pragma once

#include <cstdint>
#include <string>

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

	// ---- per-frame result (FrameTrace::OnPresentEnd) ----
	struct Frame
	{
		bool valid = false;
		float ms[static_cast<int>(Seg::Count)]{};
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
	};
	/// Closes the frame (since the previous call) and resets. a_detail: also build callsOver1ms.
	Frame TakeFrame(bool a_detail);
}
