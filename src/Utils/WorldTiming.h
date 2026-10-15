#pragma once

#include <cstdint>
#include <string>

/**
 * @brief (46l) Where the main thread's CPU time goes inside the world render (eng_world_ms).
 *
 * Measurement only: nothing here changes what the game or the GPU does.
 *
 * The world segment of Util::EngineTiming (the depth prepass, the main scene and first person) is
 * split two ways at once, both adding up to it:
 *  - by place: every direct call inside Main::RenderWorld (ID 100424), the standard
 *    FinishAccumulating (99938), Main::RenderDepth (100421) and Main::RenderFirstPersonView (100411)
 *    gets a probe ("the engine is now in call N", same stub as the 44f Main::Update probes), and
 *    BSShaderAccumulator::RenderBatches (99963) names the pass group it renders. Time goes to the
 *    newest place entered;
 *  - by kind of work: the engine's own code, each render pass's glue (BSBatchRenderer::
 *    RenderPassImmediately), BeginTechnique with our shader lookups, SetupMaterial / SetupGeometry
 *    (the engine's part and ours, by an innermost and an outermost wrapper on the shader vtables),
 *    RestoreGeometry, and our other world-render work.
 *    Exclusive: a nested kind's time is taken out of the one around it.
 * Alongside ("of which", not exclusive): D3D11 calls, State::Draw, shader-cache lookups (the lock
 * wait) and misses, Advanced Skin's per-character work (wetness, shelter picks, water search), and
 * device Create* calls made inside the world render.
 *
 * Cost: only while an F7 trace records and the main thread is in the world segment, a TSC read at
 * each switch (about 6 per draw); otherwise a branch per hook. Needs the 44f Main::Update probes
 * (same escape hatches: Performance Overlay > Engine Timing Probes, EngineTimingOff.txt). SE 1.5.97 only.
 */
namespace Util::WorldTiming
{
	/// Kinds of world-render work (exclusive).
	enum class Kind : int
	{
		Engine,          ///< engine code outside the per-pass calls below (traversal, sorting, group set-up, state)
		Pass,            ///< per-pass glue in RenderPassImmediately (technique / material switch, SetDirtyStates, the draw call)
		Technique,       ///< BeginTechnique: the engine's and our shader lookup / ModifyShaderLookup
		MaterialOurs,    ///< SetupMaterial: our hooks (TruePBR, Terrain Helper, Advanced Skin)
		MaterialEngine,  ///< SetupMaterial: the engine's own
		SetupOurs,       ///< SetupGeometry: our hooks (Advanced Skin, LLF, SSS, Linear Lighting, ...)
		SetupEngine,     ///< SetupGeometry: the engine's own (bones, constants, lights)
		Restore,         ///< RestoreGeometry (engine + ours)
		OursOther,       ///< our other world-render work (start / end of the deferred pass, terrain blending)
		Count
	};
	const char* KindName(Kind a_kind);

	/// True while the main thread is in the world segment and a trace records (hooks time then).
	extern bool g_on;
	bool OnMainThread();
	inline bool On() { return g_on && OnMainThread(); }

	/// RAII exclusive kind: pushes when timing is on, pops what it pushed.
	struct Scope
	{
		explicit Scope(Kind a_kind)
		{
			if (On())
				Begin(a_kind);
		}
		~Scope()
		{
			if (pushed)
				End();
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		void Begin(Kind a_kind);
		void End();
		bool pushed = false;
	};

	/// TSC now (cheap; converted per frame).
	uint64_t Stamp();

	// ---- "of which" (durations from a_start = Stamp(); call only when On()) ----
	void AddLookup(uint64_t a_start);      ///< shader cache: lookup lock acquired
	void AddMiss(uint64_t a_start);        ///< shader cache: a miss handled (queued for async compile / compiled now)
	void AddSkinActor(uint64_t a_start);   ///< Advanced Skin: one character's wetness / snow computed this frame
	void AddShelterPick(uint64_t a_start); ///< Advanced Skin: one Havok shelter pick
	void AddWaterSearch(uint64_t a_start); ///< Advanced Skin: one water-object search
	// QPC-based durations from the existing hooks (call only when On()).
	void AddDriverMs(double a_ms);
	void AddOwnDrawMs(double a_ms);
	void AddCreate(int a_vfuncIndex, double a_ms);

	// ---- engine structure (EngineTiming) ----
	/// The main thread entered / left the world segment.
	void OnWorldSegment(bool a_in);
	/// A world render-phase scope was pushed (DepthPrepass, World, FirstPerson; GpuScope as int).
	void OnScopePush(int a_gpuScope);
	/// Main::Update start: drops any state a broken frame left behind.
	void OnFrameStart();

	// ---- installation ----
	/// Innermost shader-vtable wrappers (engine SetupGeometry / SetupMaterial). Call first at PostPostLoad.
	void InstallInner();
	/// Outermost wrappers, pass / group detours and the call-site probes. Call after every other hook (InitD3D).
	void InstallOuter();
	const char* Status();

	// ---- per-frame result ----
	struct Frame
	{
		bool valid = false;  ///< the split ran this frame (installed, recording, world rendered)
		float totalMs = 0.0f;
		float kindMs[static_cast<int>(Kind::Count)]{};
		float driverMs = 0.0f, ownDrawMs = 0.0f;
		float lookupMs = 0.0f, missMs = 0.0f;
		uint32_t lookups = 0, misses = 0;
		float skinMs = 0.0f, shelterMs = 0.0f, waterMs = 0.0f;
		uint32_t skinActors = 0, shelterPicks = 0, waterSearches = 0;
		float createMs = 0.0f;
		uint32_t creates = 0, textureCreates = 0, viewCreates = 0;
		uint32_t passes = 0, techniques = 0, materials = 0, setups = 0, groups = 0;
		uint32_t stamps = 0;
		std::string top3;      ///< the three costliest places "name:ms;..."
		std::string placesOver1ms;  ///< every place over 1 ms (recording only)
		std::string kindsText; ///< "kind:ms;..." for the log
	};
	Frame TakeFrame(bool a_detail);
}
