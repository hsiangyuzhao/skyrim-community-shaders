#pragma once

#include <cstdint>

/**
 * @brief (47p) The performance package's switches: two one-click presets and one switch per item.
 *
 * Every item is a runtime switch (C++ or a bit in a feature's own constant buffer); no shader permutation depends on them,
 * so both presets share one shader set. Items tagged C change nothing visible (work whose result is multiplied by 0, or
 * skipped where it provably has no effect); items tagged A trade a small, deliberate amount of picture for time.
 *
 *   Conservative (the default after installing 47p): every C item on, every A item off.
 *   Aggressive: every item on.
 *   Custom: any switch changed by hand (the preset buttons set them all again).
 *
 * Saved in SettingsUser.json under Advanced > Performance. A saved Conservative / Aggressive preset is re-applied on load,
 * so items added later follow the preset; Custom keeps every saved switch.
 */
namespace PerfOptions
{
	enum class Preset : int
	{
		Conservative = 0,
		Aggressive = 1,
		Custom = 2,
	};

	struct Settings
	{
		Preset preset = Preset::Conservative;
		// ---- C: no visible change ----
		bool DryCharacterSkip = true;   ///< 2a: a character with no water on it skips the Physical wetness math
		bool WetCubemapSkip = true;     ///< 2b: no wetness reflection (cubemap reads) where the wet film weight is 0
		bool HairLightSkip = true;      ///< 2c: hair self-shadow / Marschner skipped for lights with no effect; tangent shift read once
		bool SssTransmitSkip = true;    ///< 2d: skin transmittance skipped where its strength (sssParams.w) is 0
		bool SsplsDistanceGate = true;  ///< 2e: the SSPLS light pre-pass only within its Max Distance
		bool RtSkinBatch = true;        ///< 6a: one skinning dispatch per character (all its parts) instead of one per part
		bool RtDropTinyParts = true;    ///< 6b: eyes, teeth, brows and similar tiny parts that cast nothing stay out of the BLAS
		bool RtBonesTolerant = true;    ///< 6c: "bones unchanged" ignores float noise far below a visible shift
		bool RtaoNormalFold = true;     ///< 7: RTAO Smooth Normals prep skipped where its result is not used
		bool SsgiSkipUnderRtao = true;  ///< 8: SSGI's own AO not traced where RTAO replaces it completely
		bool SssTileLimit = true;       ///< 9: Screen Space Shadows only in screen tiles that hold a non-RT caster (RT on)
		bool WaterPrepGate = true;      ///< 10: RT water prep only when a traced point light can reach the water drawn
		bool PostFuseGrading = true;    ///< 11a: colour grading writes the final image directly (no extra full-screen copy)
		// ---- A: small visible trade ----
		bool HairSsplsSkip = false;       ///< 2f: no SSPLS on hair in the forward / blended pass
		bool SmallCasterCull = false;     ///< 3: far sun cascade: small, far objects cast no shadow (characters always do)
		bool ReflSkipLodTerrain = false;  ///< 4: the water reflection cubemap leaves out LOD objects and LOD terrain
		bool VrsEffects = false;          ///< 5: soft effects (smoke, mist, glows) shaded at 2x2 under Variable Rate Shading
	};

	inline Settings settings;

	void Load(const json& a_json);
	json Save();
	void ApplyPreset(Preset a_preset);
	/// General > Performance tab.
	void DrawMenu();
	/// One line for logs / trace headers.
	std::string Describe();
}
