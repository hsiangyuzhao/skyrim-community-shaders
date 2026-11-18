#pragma once

/**
 * @brief (47p) The performance package's items. (48o) Fixed: every item takes the value the user had saved (Custom, all on
 * except ReflSkipLodTerrain); the General > Performance tab, its Conservative / Aggressive presets and the
 * Advanced > Performance block of SettingsUser.json are gone (an old block is ignored on load and not written).
 *
 * Every item is a runtime decision in C++ or a bit in a feature's own constant buffer; no shader permutation depends on them.
 */
namespace PerfOptions
{
	struct Settings
	{
		// ---- C: no visible change ----
		static constexpr bool DryCharacterSkip = true;   ///< 2a: a character with no water on it skips the Physical wetness math
		static constexpr bool WetCubemapSkip = true;     ///< 2b: no wetness reflection (cubemap reads) where the wet film weight is 0
		static constexpr bool HairLightSkip = true;      ///< 2c: hair self-shadow / Marschner skipped for lights with no effect; tangent shift read once
		static constexpr bool SssTransmitSkip = true;    ///< 2d: skin transmittance skipped where its strength (sssParams.w) is 0
		static constexpr bool SsplsDistanceGate = true;  ///< 2e: the SSPLS light pre-pass only within its Max Distance
		static constexpr bool RtDropTinyParts = true;    ///< 6b: eyes, teeth, brows and similar tiny parts that cast nothing stay out of the BLAS
		static constexpr bool RtBonesTolerant = true;    ///< 6c: "bones unchanged" ignores float noise far below a visible shift
		static constexpr bool RtaoNormalFold = true;     ///< 7: RTAO Smooth Normals prep skipped where its result is not used
		static constexpr bool WaterPrepGate = true;      ///< 10: RT water prep only when a traced point light can reach the water drawn
		// ---- A: small visible trade (48o: as the user chose them) ----
		static constexpr bool HairSsplsSkip = true;        ///< 2f: no SSPLS on hair in the forward / blended pass
		static constexpr bool SmallCasterCull = true;      ///< 3: far sun cascade: small, far objects cast no shadow (characters always do)
		static constexpr bool ReflSkipLodTerrain = false;  ///< 4: the water reflection cubemap leaves out LOD objects and LOD terrain
		static constexpr bool VrsEffects = true;           ///< 5: soft effects (smoke, mist, glows) shaded at 2x2 under Variable Rate Shading
	};

	inline constexpr Settings settings{};
}
