#pragma once

#include "Buffer.h"
#include "OverlayFeature.h"

/// (52a) Sky > "Physical Lighting": unified physical units, package A (stages 0 + 1 of the X audit,
/// reviews/audit-unified-units-X-2026-11.en.md). Nothing here changes a value: every light carrier gets an explicit per-class
/// factor f(class) = cal(class) x S with cal = 1 (the 50e look), S = P (pre-exposure) x the Scene Scale Wobble self-test, and the eye
/// divides S back out. Master off: every factor is exactly 1.0f, P is not applied, the engine's own light / ambient values are never
/// touched, the shader scalars (FeatureData pads, EffectSceneCB tail, AE CB pad, cubemap CB) are exactly 1.0f.
/// The light meter (crosshair readout) works with the master on or off.
struct PhysicalLighting : OverlayFeature
{
	virtual inline std::string GetName() override { return "Physical Lighting"; }
	virtual inline std::string GetShortName() override { return "PhysicalLighting"; }
	virtual std::string_view GetCategory() const override { return "Sky"; }
	virtual bool IsCore() const override { return true; }
	virtual bool SupportsVR() override { return false; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Unified physical light units, package A: a light meter and the plumbing for physical light values (the picture does not change).",
			{ "Light meter at the crosshair: lux, cd/m2, EV100, split by light source",
				"Every light source goes through one conversion factor per light class",
				"Pre-exposure keeps the frame buffer in range; the eye divides it back out",
				"Self-tests: Scene Scale Wobble, Class Wobble, unit registry" }
		};
	}

	/// Light classes. Each has one factor f = cal x S (cal = 1 in 52a); the Class Wobble self-test pulses one of them.
	enum class Class : uint32_t
	{
		Sun = 0,              // outdoor directional light while the sun is the caster; clouds' sun light
		Moon,                 // outdoor directional light while a moon is the caster; clouds' moon light
		InteriorDirectional,  // interior directional light (lighting template / Interior Sun)
		Ambient,              // directional ambient (DALC) and everything built from it
		Sky,                  // physical sky LUTs (sun / moon glow in-scatter), vanilla dome, cloud vanilla colour, water fallback reflection
		NightGlow,            // physical sky night base light
		SunDisc,              // vanilla sun quad and glare
		MoonDisc,             // moon discs
		Stars,                // stars and Milky Way
		Lamps,                // point lights (Light Limit Fix, engine strict lights, effect point lights)
		LampsISL,             // point lights with the Linear flag (skip Linear Lighting's multipliers)
		Window,               // window lights and window glass (50c)
		Emissive,             // EmitColor (emissive meshes)
		Glow,                 // glow maps (incl. PBR emissive)
		FireMagic,            // fire and magic effects (additive / palette)
		EffectSelfLit,        // self-lit part of other effects, effect lights not following the scene (DLightColor)
		Precipitation,        // rain / snow particles, unlit smoke and mist
		Fog,                  // vanilla fog colours (forward, deferred, effects, water), EHF sky light, volumetric fog weather colours
		Water,                // water body colour
		CharacterLight,       // character rim light
		SnowRim,              // snow rim light
		SnowSparkle,          // snow sparkles
		Count
	};
	static constexpr uint32_t kClassCount = static_cast<uint32_t>(Class::Count);
	static const char* ClassName(Class a_class);

	struct Settings
	{
		bool Enabled = false;  // master "Physical Lighting"
	} settings;

	// ---- per-frame state (BeginFrame) ----
	struct Frame
	{
		bool active = false;       // master on and the prerequisites met and in the world: S applied
		bool prerequisites = false;
		std::string reason;        // why the master is greyed / inactive
		float S = 1.0f;            // P x wobble (1 exactly when inactive)
		float invS = 1.0f;
		float P = 1.0f;            // pre-exposure (smoothed, rate limited)
		float pTarget = 1.0f;      // previous frames' applied exposure / reference exposure
		float wobble = 1.0f;       // Scene Scale Wobble factor this frame
		float classWobble = 1.0f;  // Class Wobble factor this frame (on the selected class)
		uint32_t frame = 0;
		bool snapped = false;      // P snapped this frame (loading screen / time skip / switch-on)
		bool menuExcluded = false; // an item-preview / map / pause menu is open: S = 1
	} frame;

	// ---- self-tests (Debug; not saved) ----
	bool meterOn = false;
	bool sceneWobble = false;
	bool classWobbleOn = false;
	int classWobbleIndex = 0;
	double wobbleStart = 0.0;

	// ---- registry ----
	struct RegistryEntry
	{
		uint32_t lastFrame = UINT32_MAX;
		float lastFactor = 1.0f;
	};
	std::array<RegistryEntry, kClassCount> registry{};

	/// The factor of one light class this frame: cal (1) x S x (Class Wobble if this class). Exactly 1.0f when inactive. Marks the
	/// registry (applied this frame).
	float Scale(Class a_class);
	[[nodiscard]] bool Active() const { return frame.active; }
	[[nodiscard]] float SceneScale() const { return frame.S; }
	[[nodiscard]] float SceneScaleInv() const { return frame.invS; }

	/// Called first thing in State::Reset (once per frame, between Presents): P readback, S, wobble, menu exclusion.
	void BeginFrame();
	/// Top of State::UpdateSharedData: the engine's directional light colour and DALC carry this frame's class factor (written over the
	/// engine's own values, which are kept and put back as soon as the factor is 1 again; nothing is touched while it stays 1).
	void ApplyEngineLights();
	/// After GetFeatureBufferData: the FeatureData-carried light values x their class factor; the shader scalars in unread pad slots.
	void PatchFeatureData(unsigned char* a_data, size_t a_size);

	/// Unscaled engine values for CPU readers (meter, Linear Lighting's unlit scene scale, Ray Tracing's sun gate).
	[[nodiscard]] RE::NiColor EngineDirLightDiffuse(const RE::NiDirectionalLight* a_light) const;
	[[nodiscard]] RE::NiPoint3 EngineDalcTranslate() const;
	[[nodiscard]] const RE::NiTransform& EngineDalc() const;

	/// Light Limit Fix upload (after its culling): linear-flag lights and window lights get their class factor through the fade
	/// (ordinary lights through Linear Lighting's pointLightMult in FeatureData).
	float LightFadeScale(RE::NiLight* a_light, bool a_linear);
	/// Window glass (50c): the emissive colour scale (window / emissive class, through the emissive gamma); 1 at identity.
	float GlassColorScale();
	/// Dynamic cubemap history: S now / S at the previous update of that capture (0 = environment, 1 = reflections). 1 exactly off.
	float CubemapHistoryScale(int a_which);

	// ---- EffectSceneCB tail (Effect.hlsl) ----
	struct EffectScalars
	{
		float selfLit = 1.0f;     // EffectSelfLit
		float fire = 1.0f;        // FireMagic
		float unlit = 1.0f;       // Precipitation (unlit smoke and mist)
		float dLight = 1.0f;      // EffectSelfLit: engine DLightColor (effects not following the scene)
		float fog = 1.0f;         // Fog
		float normDiv = 1.0f;     // IBL effect normalisation x 1 / S (the floor stays absolute)
		float ambientRaw = 1.0f;  // raw (gamma) DALC readers: S / S^(1/ambientGamma)
	};
	EffectScalars GetEffectScalars();

	// ---- light meter (stage 0) ----
	void SetupResources() override;
	void ClearShaderCache() override;
	/// After the deferred composite (Deferred::DeferredPasses): a 3x3 read of the composite's buffers at the crosshair into a readback
	/// ring. Only while the meter is on, only in the world.
	void DispatchMeter(ID3D11ShaderResourceView* a_main, ID3D11ShaderResourceView* a_albedo, ID3D11ShaderResourceView* a_normalRoughness,
		ID3D11ShaderResourceView* a_masks, ID3D11ShaderResourceView* a_specular, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_shadowMask);

	struct Reading
	{
		bool valid = false;
		uint32_t frame = 0;
		// raw buffer values (this frame's buffer units, i.e. x S)
		float3 rawLit{}, rawAlbedo{}, rawSpecular{}, normalWS{};
		float rawMasksAmbientY = 0.f, rawShadow = 1.f, depth = 1.f, distance = 0.f;
		float S = 1.f, P = 1.f;
		// illuminance in lux (provisional R0), split
		float eMeasured = 0.f, eDirectional = 0.f, eAmbient = 0.f, eDalc = 0.f, eIbl = 0.f, eLamps = 0.f, eSplit = 0.f, residual = 0.f;
		int lampCount = 0;
		float luminance = 0.f;   // cd/m2 at the crosshair (lit diffuse + specular, before fog)
		float emissive = 0.f;    // cd/m2 estimate: L - albedo x E / pi
		float ev100Reflected = 0.f, ev100Incident = 0.f;
		float aeAdapted = 0.f, aeTarget = 0.f, aeDivisor = 0.f;  // adaptation buffer (cd/m2 of the eye's levels)
		std::string adaptation;  // brighter / darker / steady
		bool directionalIsSun = true;
		// context
		std::string location, cell, time;
		uint32_t weather = 0;
		float sunElevation = 0.f, masserElevation = 0.f, secundaElevation = 0.f;
		int moonPhase = -1;
	};
	Reading current;
	std::deque<Reading> captured;  // "Capture Reading" (last 32)
	static constexpr size_t kMaxCaptured = 32;
	static constexpr float kR0 = 25000.0f;  // provisional: cd/m2 per buffer unit at S = 1 (the 50a eye's kSceneToNits)

	nlohmann::json MeterJson() const;
	std::string DescribeForTrace() const;

	// ---- UI ----
	void DrawSettings() override;
	void DrawOverlay() override;
	bool IsOverlayVisible() const override { return meterOn; }
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;
	void RestoreDefaultSettings() override;

private:
	void UpdatePreExposure(float a_dt);
	void ReadMeterResults();
	void ComputeReading(const float4* a_raw);
	void FillContext(Reading& a_reading) const;
	bool MenuExcludes() const;
	std::string PrerequisiteReason() const;
	static nlohmann::json ReadingJson(const Reading& a_reading);

	// pre-exposure readback ring (adaptation buffer, 4 floats)
	static constexpr int kRing = 3;
	winrt::com_ptr<ID3D11Buffer> aeStaging[kRing];
	bool aeStagingPending[kRing] = {};
	int aeRingIndex = 0;
	float logP = 0.0f;
	bool pInitialised = false;
	float lastGameHours = -1.0f;
	bool wasActive = false;
	float aeLastRead[4] = {};
	bool aeLastEye = false;

	// engine values kept while ours are written
	RE::NiTransform engineDalc{};
	RE::NiTransform writtenDalc{};
	bool dalcWritten = false;
	const RE::NiDirectionalLight* dirLightObject = nullptr;
	RE::NiColor engineDiffuse{};
	RE::NiColor writtenDiffuse{};
	bool diffuseWritten = false;
	float cubemapLastS[2] = { 1.0f, 1.0f };
	uint32_t cubemapLastFrame[2] = { UINT32_MAX, UINT32_MAX };

	// meter GPU side
	winrt::com_ptr<ID3D11ComputeShader> meterCS;
	bool meterCompileFailed = false;
	std::unique_ptr<StructuredBuffer> meterOut;
	winrt::com_ptr<ID3D11Buffer> meterStaging[kRing];
	bool meterPending[kRing] = {};
	uint32_t meterFrameOf[kRing] = {};
	float meterSOf[kRing] = {};
	float meterPOf[kRing] = {};
	int meterRingIndex = 0;
	static constexpr uint32_t kMeterSlots = 8;
};
