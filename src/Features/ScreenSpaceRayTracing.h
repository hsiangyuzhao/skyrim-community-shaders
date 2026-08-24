#pragma once
#define ENABLE_SHARC

#include "NRD.h"

struct ScreenSpaceRayTracing : Feature
{
    static ScreenSpaceRayTracing* GetSingleton()
    {
        static ScreenSpaceRayTracing singleton;
        return &singleton;
    }

    virtual inline std::string GetName() override { return "Screen Space Ray Tracing"; }
    virtual inline std::string GetShortName() override { return "ScreenSpaceRayTracing"; }
    virtual inline std::string_view GetShaderDefineName() override { return "SSRT"; }
    virtual std::string_view GetCategory() const override { return "Lighting"; }
    virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Screen Space Ray Tracing provides high-quality global illumination with information in screen space.",
            {
                "Realistic indirect lighting",
                "Importance sampling for advanced reflections based on roughness",
                "Efficient ray marching with Hi-Z buffer",
                "Uses dynamic cubemaps as fallback for missing information",
                "Denoising via NVIDIA NRD REBLUR (default) or the in-house SVGF chain"
            }
		};
	}

    virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
#ifdef ENABLE_SHARC
	/// @brief Allocates the four SHARC structured buffers on first use (audit P6).
	/// Called from DrawSSRTDiffuse before any dispatch that binds them, so enabling
	/// SHARC at runtime cannot dispatch against null UAVs.
	void EnsureSharcResources();
#endif

    bool HasShaderDefine(RE::BSShader::Type) override { return true; };
    virtual bool SupportsVR() override { return true; };

    /// @brief (batch C1) Values of Settings::DenoiserMethod. An enum rather than a
    /// bool pair so the UI combo, the settings file and the dispatch gates all agree
    /// on one value, and so "denoising off" is a first-class state (the performance
    /// baseline the A/B guide measures against).
    enum DenoiserMethodValue : uint
    {
        kDenoiserOff = 0,
        kDenoiserSVGF = 1,
        kDenoiserREBLUR = 2,
    };

    struct Settings
    {
        bool EnableSpecular = true;
        /// @brief (S3.9) Default 64, down from 128. The slider is unchanged (1-256), so this
        /// is a default and nothing else -- anyone who wants the old reach types it back.
        ///
        /// What M actually buys, and what it does not. The intuition that a lower step count
        /// makes the traversal *miss* things does not hold for a Hi-Z traversal: the descent is
        /// conservative, so a ray either finds the first intersection along its path or runs
        /// out of budget before reaching it. Small objects are not skipped -- they are either
        /// inside the range the budget bought or beyond it. So M is a ray *range* parameter,
        /// not a quality-of-detection one, and the question is only how far indirect light has
        /// to travel before something else can stand in for it.
        ///
        /// The range at 64. A step is one render texel at the coarsest mip the ray reaches, so
        /// the distance covered is scene dependent. Two brackets, from the analysis in
        /// reviews/analysis-ssrt-tracing-reshape.md:
        ///   * open, uncluttered geometry -- the traversal climbs to the high mips almost
        ///     immediately and 64 steps still cross the whole screen. No change at all.
        ///   * cluttered geometry (dense forest, interiors full of clutter) -- the ray is
        ///     forced back down the pyramid repeatedly and the reach falls from roughly 3.5 m
        ///     to 1.7 m.
        /// 1.7 m is still well past the scale at which screen-space diffuse GI carries most of
        /// its signal, and past that distance Skylighting's own occlusion is what the pixel
        /// falls back on -- which is a better estimate of a 3 m bounce than a screen-space ray
        /// that has to guess at everything off screen anyway.
        ///
        /// Expected saving 10-35% of the ray-march pass depending on scene clutter; the open
        /// end of that range is where the budget was never being spent in the first place.
        ///
        /// The visual A/B to run is remote light leaking through dense forest: at 64 a distant
        /// bright surface seen through a lot of intervening geometry may stop contributing.
        /// That is the only failure direction this change has.
        uint MaxSteps = 64;
        /// @brief (spec F5) Reviewed, left at 6. Independent of the grid convention: it
        /// indexes pyramid levels, and freewins already clamped the slider to
        /// maxMips - 1 so the traversal cannot request a level that does not exist.
        uint MaxMips = 6;
        /// @brief (spec F1 / audit #3) Depth-buffer thickness, in *game units*, used by
        /// SSRT_ValidateHit as the range over which a hit's distance behind the validated
        /// surface fades confidence to zero (the shader adds roughness * 10 on top).
        ///
        /// 5 was never a defensible figure. It predates the mip-0 validation fix, so it
        /// was being compared against the surface's own 2x2 depth gradient rather than
        /// against a real ray/surface separation, and 5-15 game units is 7-21 cm: smaller
        /// than that gradient over most of a grazing ground plane, so confidence
        /// collapsed there. With validation on mip 0 a clean intersection scores
        /// distance ~= 0, and thickness now only gates the cases it should: a ray that
        /// was already behind the surface when the traversal descended to mip 0, and the
        /// single-texel straddle at the hit.
        ///
        /// 30 is derived from the residual, not guessed. confidence >= 0.5 requires
        /// distance <= 0.35 * thickness (confidence = (1 - smoothstep(0, t, d))^2), and
        /// diffuse roughness on ground/terrain is ~0.9, so the effective thickness is
        /// ~39 units and the tolerance ~13.6 units. The per-texel view-space depth
        /// gradient is z * (2 tan(fov/2) / width) * tan(incidence): at 2560 px render
        /// width that is ~3.4 units at z = 1000 / 80 deg and ~6.8 at 85 deg, scaling
        /// linearly with z -- so 30 covers the grazing band out to roughly z = 2000-4000
        /// game units, which is the whole visible ground. The other side of the trade is
        /// leak-through: 39 units is 56 cm, well under any Skyrim wall or floor slab, and
        /// the geometry thinner than that (foliage, hair) is exactly where accepting the
        /// near surface is the desired contact behaviour.
        ///
        /// Shared with the specular path, whose effective thickness goes from ~6 to ~31
        /// (43 cm). That direction is right for the same reason -- specular validates on
        /// mip 0 too and has the same residual -- at the cost of the usual SSR trade,
        /// reflections elongating behind silhouettes by up to that distance. The slider
        /// (0-500) is there if it reads as smearing.
        float Thickness = 30.f;
        /// @brief (spec F5) Reviewed against the repaired offset, left at 0.1.
        ///
        /// The F2 change is deliberately a no-op at near-normal incidence -- the bias
        /// direction normalize(N - D) equals N there and the 1 / max(|N.D|, 0.1) factor is
        /// 1 -- so 0.1 still means what it meant, ~1.4 game units at z = 1000. Only
        /// grazing incidence sees more (up to 10x), which is the fix.
        ///
        /// Keeping it small matters for the contact regions: the offset also pushes the
        /// origin off its own surface, so an over-large bias is what makes a ray *miss*
        /// nearby hair or foliage. Anyone who finds contact darkening weaker than before
        /// should reach for this slider downwards, not upwards.
        float NormalBias = 0.1f;
        float BRDFBias = 0.25f;
        bool UseDynamicCubemapsAsFallback = true;
        bool UseDynamicCubemapsAsFallbackSpecular = true;
        uint DiffuseSPP = 2;
        bool EnableDiffuse = true;
        float SpecularMult = 1.0f;
        float DiffuseMult = 1.0f;
        /// @brief Forward-side scale on the vanilla directional-ambient + IBL term while SSRT
        /// diffuse is active (Lighting.hlsl / RunGrass.hlsl / DistantTree.hlsl).
        ///
        /// Default deliberately left at 0, which is the legacy energy model: SSRT diffuse
        /// replaces the forward ambient outright and the cubemap fallback stands in for it.
        /// EnableAmbientReinjection *overrides* this to 1 rather than changing its default --
        /// see GetCommonBufferData -- so an existing configuration that saved 0 keeps the
        /// old look the moment the reinjection toggle comes off, which is what makes the A/B
        /// comparison exact.
        ///
        /// Intermediate values are only meaningful in the legacy model. Under reinjection the
        /// composite reconstructs the ambient as "A_est's chroma with Masks.z's luminance"
        /// (DeferredCompositeCS), and Masks.z is the only half of that pair this scale reaches;
        /// at m = 0.5 the reconstruction is right in luminance and half again too saturated in
        /// chroma, so the removal would leave a tint behind. Pinning it to 1 is what keeps the
        /// reconstruction faithful.
        float AmbientMult = 0.0f;
        /// @brief (direction B) Confidence-guided vanilla ambient reinjection.
        ///
        /// The problem it solves. With the legacy model the SSRT diffuse estimate is
        /// E = sum_i conf_i * albedo(hit_i) * L(hit_i) / N: the only energy in it comes from
        /// surfaces that are inside the hemisphere, on screen, *and* directly lit. A ray that
        /// leaves the screen, hits the sky, or lands on an unlit interior wall contributes
        /// nothing at all, and with the forward ambient zeroed there is nothing underneath it.
        /// That is a structural energy hole, not a tuning error, and it is why switching the
        /// cubemap fallback off leaves the frame near black. The same estimate is also the
        /// baseline cause of the directional smearing: a raw 2-spp stochastic signal reaching
        /// kMAIN with no motion vectors of its own gives the upscaler nothing to clamp
        /// against, so it smears the noise along motion.
        ///
        /// What it does. The forward ambient is left in place (AmbientMult pinned to 1) and the
        /// composite removes the fraction of it the rays actually resolved, so the pixel ends up
        /// at lerp(vanillaAmbient, tracedRadiance, confidence). Where the rays found geometry
        /// the SSRT result stands, as before; where they did not, the pixel falls back to the
        /// forward pass's own ambient -- which is exact, free, already in kMAIN, and perfectly
        /// stable in time. Two consequences follow, and both are the point:
        ///
        ///  * the energy hole closes without double counting anything, because the two terms
        ///    partition the hemisphere by confidence instead of being summed;
        ///  * the stochastic fraction of the signal entering kMAIN shrinks to conf * radiance,
        ///    so the low-confidence regions -- open ground, sky-facing surfaces, anything the
        ///    screen simply does not contain -- stop feeding the upscaler noise at all.
        ///
        /// It also makes the radiance source honest: because the forward ambient is no longer
        /// zeroed, kMAIN carries a hit point's *outgoing* radiance rather than its direct
        /// lighting alone, so a lit-wall bounce into a shaded corner finally has something to
        /// carry.
        ///
        /// Interaction with the cubemap fallback. The two are alternative answers to the same
        /// question and must not both run: the fallback's `confidence = 1` makes every pixel
        /// claim full coverage, which would remove all the vanilla ambient and then add the
        /// cubemap estimate of the same light back. DrawSSRTDiffuse therefore forces
        /// UseDynamicCubemapsAsFallback off while this is on (the specular fallback is a
        /// separate setting and is untouched). The vanilla ambient is the better of the two
        /// answers anyway -- it is the real DALC + IBL term with Skylighting already applied,
        /// where the fallback is a cubemap normalised towards it -- and skipping the fallback
        /// removes a cubemap sample plus a Skylighting probe fetch per sample per pixel.
        bool EnableAmbientReinjection = true;
        /// @brief (direction B) How much of the confidence is allowed to displace the ambient.
        ///
        /// 1 is the energy-conserving setting: confidence 1 removes all of the vanilla ambient
        /// and the traced radiance stands alone. 0 keeps the whole ambient and adds the traced
        /// radiance on top of it, i.e. a purely additive GI that is brighter than the truth but
        /// never darker than vanilla. The dial exists because the two ends bracket the honest
        /// answer for a screen-space estimator: confidence measures how much geometry the rays
        /// *found*, not how well its radiance is known, so a scene whose on-screen surfaces are
        /// unrepresentative of the whole environment is better served slightly under 1.
        float AmbientReinjectionStrength = 1.0f;
        /// @brief (spec F5) Reviewed against the corrected occlusion semantics, left at
        /// 1.0. It now scales occlusion that comes only from back-face hits -- the one
        /// case where the ray demonstrably entered geometry -- instead of also scaling
        /// the self-intersection failures that used to multiply the cubemap fallback to
        /// black (audit #5). Full strength on a source that is now meaningful is the
        /// right default, and it keeps as much of the contact darkening as the corrected
        /// mechanism can supply.
        float OcclusionStrength = 1.0f;
        float CubemapNormalization = 0.0f;
        /// @brief (batch C1) Which denoiser processes the two SSRT radiance signals.
        /// 0 = none, 1 = the in-house SVGF chain, 2 = NVIDIA NRD REBLUR (one
        /// REBLUR_DIFFUSE and one REBLUR_SPECULAR instance).
        ///
        /// Replaces the old EnableSVGF bool; an old config that saved EnableSVGF is
        /// simply ignored by the serializer and the new default (REBLUR) applies. The
        /// two chains coexist and hot-switch — that is the A/B mechanism — and under
        /// REBLUR none of the SVGF passes (preblur / temporal / variance / a-trous)
        /// is dispatched, so the unselected chain costs nothing.
        ///
        /// REBLUR takes over the whole temporal problem for both signals. In
        /// particular the specular chain stops sharing surface-motion reprojection
        /// with diffuse: REBLUR's virtual-position mechanism reprojects reflected
        /// content by its own motion, which is the root fix for the "surface MV is
        /// wrong for what a mirror shows" defect the SVGF chain carries.
        uint DenoiserMethod = kDenoiserREBLUR;
        /// @brief (S1.1) Feed texSSRTDiffuseConfidence to REBLUR's IN_DIFF_CONFIDENCE.
        ///
        /// **Default off, and off is the honest setting.** Batch C1 wired this input up on the
        /// strength of a shared word. NRD documents IN_DIFF_CONFIDENCE as "user-provided history
        /// confidence in range 0-1, i.e. antilag ... must be computed for the previous frame in
        /// the current frame" (NRDDescs.h) -- it answers *is the accumulated history still valid
        /// for this pixel*, and REBLUR shortens the accumulation window where it is low. What this
        /// surface carries is this frame's ray hit coverage: *what fraction of the hemisphere
        /// screen space managed to resolve*. The two are not merely different, they are
        /// anti-correlated in exactly the places that matter -- a newly disoccluded pixel or a
        /// lighting change can have full coverage (history invalid, confidence reported high),
        /// while open ground and sky-facing surfaces have low coverage and the most temporally
        /// stable light in the frame (history perfectly valid, confidence reported low, window
        /// shortened hardest).
        ///
        /// This fork has already ruled on the same question once, on the SVGF side: batch 1 item 3
        /// rejected wiring this surface to the temporal accumulation window, and the four reasons
        /// recorded at the texSSRTDiffuseConfidence declaration apply verbatim here. The first of
        /// them is on its own decisive: with the cubemap fallback active the surface is identically
        /// 1.0 over the whole screen, so the input is inert in one configuration and live in the
        /// other.
        ///
        /// Kept as a switch rather than deleted so the A/B can still be run in-game (see the
        /// closeout guide), not because the semantics are in doubt.
        bool ReblurFeedHitCoverageConfidence = false;
        /// @brief (batch C1) REBLUR tuning for the diffuse instance. NRD defaults.
        NRD::REBLURSettings ReblurDiffuse;
        /// @brief (batch C1) REBLUR tuning for the specular instance. Defaults taken
        /// from the upstream reference SSR integration: short history (20/1/0) and a
        /// tight fast-history clamp, which is what a glossy signal wants.
        NRD::REBLURSettings ReblurSpecular = {
            .MaxAccumulatedFrameNum = 20,
            .MaxFastAccumulatedFrameNum = 1,
            .MaxStabilizedFrameNum = 0,
            .FastHistoryClampingSigmaScale = 1.5f,
        };
        /// @brief (batch C1) REBLUR hit-distance normalization constants, the (A, B, C)
        /// of hitDist / ((A + B * |viewZ|) * lerp(C, 1, specMagicCurve(roughness))).
        /// NRD's defaults are (3, 0.1, 20) with A in meters; 1 game unit ~ 1.4 cm, so
        /// A starts at 3 m * 70 = 210 game units — the same scaling the upstream SSR
        /// integration ships. B is per-unit-viewZ and C is unitless, so both keep the
        /// NRD defaults verbatim. Consumed identically by the pack shader (front end)
        /// and by nrd::ReblurSettings::hitDistanceParameters (back end); the two must
        /// never diverge or REBLUR denormalizes with the wrong curve.
        float ReblurHitDistA = 210.0f;
        float ReblurHitDistB = 0.1f;
        float ReblurHitDistC = 20.0f;
        /// @brief (batch C1) REBLUR specular pre-pass, mirrored from the upstream SSR
        /// integration: a small radius search used only to improve the virtual-motion
        /// estimation, not to blur the signal (the second flag).
        float SpecularPrepassBlurRadius = 50.0f;
        bool UsePrepassOnlyForSpecularMotionEstimation = true;
        /// @brief (batch 1, item 1) Run ssrt_preblur.hlsl -- anti-firefly plus a 3x3
        /// geometry-guided spatial filter -- on the diffuse ray-march output *before* the
        /// temporal accumulation reads it.
        ///
        /// Defaults on because its absence is a defect rather than a missing luxury: every
        /// denoiser in the reference set (REBLUR, RELAX, Q2RTX's A-SVGF) puts its most important
        /// spatial filter in front of the temporal pass, because the accumulator writes whatever
        /// variance it is handed into a buffer the next frame reads back. See the block comment
        /// at the top of ssrt_preblur.hlsl for the pass order, the kernel derivation, and why
        /// the firefly clamp had to move into that pass rather than stay behind it.
        ///
        /// Diffuse only. The specular chain keeps its unmodified path -- a roughness-blind
        /// pre-blur on a near-delta reflection lobe is the one thing spec S3 exists to prevent --
        /// and gets its own pre-pass when the specular pipeline is split out.
        bool EnablePreBlur = true;
        uint MaxAccumulatedFrames = 16;
        /// @brief (spec A2) 2, not 3: with variance guidance repaired (audit #11) and the
        /// depth weight actually discriminating (audit #12), two guided iterations resolve
        /// more than three unguided ones did. The UI range is unchanged.
        uint AtrousIterations = 2;
        /// @brief sigma_l for the a-trous luminance edge-stopping function:
        /// phiLuminance = ColorPhi * sqrt(variance).
        ///
        /// Re-tuned from 0.5 to 2.0 as part of the BUG-1 fix. 0.5 was chosen while the
        /// variance channel carried roughly alpha * sigma^2 -- a ~34x underestimate -- so
        /// it was compensating for a broken input, and against a true sigma^2 it
        /// annihilates the kernel. With .w now equal to the real per-frame variance the
        /// luminance term reduces to weight = exp(-k / ColorPhi) for a tap k standard
        /// deviations from the centre, which makes the trade explicit. Writing N_eff for
        /// the effective sample count of the 3x3 binomial kernel under a uniform
        /// neighbour weight w -- (1 + 3w)^2 / (1 + 1.25 w^2), ceiling 7.11 at w = 1:
        ///
        ///   ColorPhi   w(1 sigma)   w(4 sigma)   noise:edge   N_eff
        ///     0.5        0.135        3.4e-4        403        1.93
        ///     1.0        0.368        0.018          20        3.79
        ///     2.0        0.607        0.135         4.5        5.45
        ///     4.0        0.779        0.368         2.1        6.33
        ///
        /// A tap that differs from the centre by ~1 sigma differs by *noise* and must be
        /// averaged in; one that differs by >=4 sigma is a real luminance edge and must be
        /// rejected. At 0.5 the filter treats its own noise as an edge (N_eff 1.93 of a
        /// possible 7.11, i.e. it barely averages at all); at the SVGF paper's sigma_l =
        /// 4.0 it barely discriminates at all (a real 4-sigma edge keeps 37% of its
        /// weight).
        ///
        /// 2.0 rather than the paper's 4.0 for two reasons specific to this pipeline:
        ///   * Reach. Schied et al. run five iterations of the 5x5 B3 spline at 2^i
        ///     strides -- second-moment sigma ~18 px. Spec S2 measured this fork's chain
        ///     (two iterations of 3x3 at strides 1, 2) at sigma 1.58 px. With an order of
        ///     magnitude less reach there are no later, wider levels to repair an edge that
        ///     level 0 crossed, so per-tap discrimination has to be real.
        ///   * The other two edge-stops are already stronger here than in the paper
        ///     (NormalPhi 512 against its 128, and audit #12 gave the depth term actual
        ///     discriminating power). That is what licenses loosening sigma_l from 0.5 at
        ///     all -- luminance need not carry edge preservation alone -- but 4.0 loosens
        ///     it past the point of being an edge-stop.
        ///
        /// The same constant feeds ssrt_variance.hlsl's 7x7 moment estimator, where a
        /// wider phiLuminance is likewise wanted: that path runs only on history <= 2
        /// pixels, and a moment estimate built from more taps is a less noisy seed.
        float ColorPhi = 2.0f;
        /// @brief (defect D7) sigma_n for the a-trous normal edge-stopping function:
        /// weight *= pow(max(0, dot(n, nP)), NormalPhi). Re-tuned from 512 to the SVGF
        /// paper's 128.
        ///
        /// The exponent is an angular gate: pow(cos t, p) = exp(-p t^2 / 2) for small t, so
        /// the half-power angle is sqrt(2 ln 2 / p) and the 1/e angle is sqrt(2/p). At 512
        /// that is 2.1 and 3.6 degrees; a tap 7 degrees off the centre normal keeps
        /// exp(-7.8) = 4e-4 of its weight and one 10 degrees off keeps 4e-7.
        ///
        /// Why that is wrong here specifically. The quantity being compared is the *shading*
        /// normal out of the deferred normal-roughness buffer, i.e. the geometric normal with
        /// the material's normal map applied. Skyrim's rock, gravel, dirt, bark and foliage
        /// normal maps swing tens of degrees between adjacent texels, so at 512 the kernel
        /// collapses to its centre tap over most of an outdoor scene -- and it does so
        /// *unevenly*, because smooth man-made surfaces keep their taps. That is both halves
        /// of the reported symptom at once: extra a-trous iterations change nothing (the taps
        /// they add are annihilated), and the filtering that does happen is patchy at the
        /// scale of the material rather than of the noise, which is what turns per-pixel
        /// noise into region-sized blotches.
        ///
        /// Reference SVGF uses 128 on primary-hit *geometric* normals, which are smoother than
        /// these -- so 128 here is if anything still on the tight side. It puts the 1/e angle
        /// at 7.2 degrees and leaves 2% of weight at 14, which is the right order for a signal
        /// whose dependence on the normal is a cosine-weighted hemisphere integral: indirect
        /// irradiance varies slowly with orientation, so averaging across a normal-map wiggle
        /// on one continuous surface loses nothing that was ever there.
        ///
        /// Coupling worth recording: the ColorPhi 2.0 derivation above cites "NormalPhi 512
        /// against the paper's 128" as part of what licenses loosening sigma_l from 0.5. That
        /// argument weakens by exactly this change, and 2.0 is still half the paper's 4.0 --
        /// the depth term (audit #12) remains the third, and now the strongest, edge-stop.
        float NormalPhi = 128.0f;
        /// @brief (batch 1, item 2) How hard the diffuse a-trous kernel narrows on a pixel whose
        /// rays hit something close by. 0 disables the mechanism exactly.
        ///
        /// The window multiplied into the per-axis kernel weights is exp(-beta * |k|^2 / R^2)
        /// with beta = HitRadiusStrength * (1 - f), where f is the ray's correlation length
        /// clamped to the iteration's own hard radius and R the kernel radius. At 4.0 a contact
        /// hit takes the chain's second-moment sigma from 2.24 px to 1.31 px while a miss or a
        /// distant hit keeps the full 2.24 px bit for bit; the inner ring never loses more than
        /// exp(-1) = 0.37 of its weight, so the filter narrows and never stops. See the
        /// derivation block in ssrt_spatial.hlsl.
        ///
        /// Diffuse only: the specular kernel is already sized by roughness, which is the
        /// specular equivalent of this mechanism, and its hit distance lives on a different
        /// surface with different semantics (DLSS-RR consumes it).
        float HitRadiusStrength = 4.0f;
        /// @brief (spec A1) Let a fully converged 8x8 tile skip an a-trous iteration.
        bool AdaptiveFiltering = true;
        /// @brief (spec A1) Accumulated frames a pixel needs before it may count as
        /// converged. The default matches MaxAccumulatedFrames: at that point the
        /// temporal blend weight has reached its floor, so the pixel is in steady state.
        uint AdaptiveHistoryThreshold = 16;
        /// @brief (spec A1, re-derived for BUG-2) Relative variance below which a pixel
        /// counts as converged -- a squared coefficient of variation, not an absolute
        /// luminance-squared threshold.
        ///
        /// ssrt_spatial.hlsl compares `.w < AdaptiveVarianceEps * L^2`, where L is the
        /// local mean luminance (floored at SSRT_ADAPTIVE_LUM_FLOOR for near-black).
        ///
        /// Why relative. Monte-Carlo radiance noise is multiplicative: for a fixed sample
        /// count sigma scales with the mean, so an absolute threshold selects on how
        /// *bright* a pixel is and not on how *noisy* it is. The old absolute 1e-4 was
        /// therefore a darkness gate. Before BUG-1 the .w channel held roughly
        /// 0.0285 * mean^2 + 0.0294 * sigma^2, and at the 2-spp diffuse default (sigma
        /// about 1.4 * mean) that is 0.0861 * mean^2, so `< 1e-4` reduced to
        /// `mean < 0.034`. Fixing BUG-1 does not rescue the absolute form -- with a true
        /// sigma^2 it becomes `mean < 0.0071`, stricter still -- so the criterion needed
        /// re-deriving rather than re-defaulting.
        ///
        /// Where 1.3e-2 comes from. Post-BUG-1 the .w channel at a-trous iteration 0 is
        /// the variance of the *per-frame samples* entering the accumulation, sigma_s^2,
        /// which is what SVGF prescribes for the edge-stop. The noise a player actually
        /// sees is the residual in the accumulated output, and for an EMA with weight
        /// alpha = 1 / (MaxAccumulatedFrames + 1) that is sigma_s^2 * alpha / (2 - alpha).
        /// At the default 16, alpha = 1/17 and the factor is 0.0303, i.e.
        /// sigma_out = 0.174 * sigma_s. Taking a 2% residual coefficient of variation as
        /// the visibility line (at or below the Weber limit for a noise pattern, and the
        /// SSRT term is further attenuated by albedo in the composite) gives
        /// sigma_s / L <= 0.02 / 0.174 = 0.115, hence a squared CoV of 1.3e-2.
        ///
        /// The derivation is alpha dependent and deliberately not coupled to
        /// MaxAccumulatedFrames: a longer accumulation window makes this default
        /// conservative (it would tolerate more per-frame variance for the same output
        /// noise), a shorter one makes it slightly optimistic.
        ///
        /// Migration note: a saved value from the old absolute band (1e-6..1e-2) reads as
        /// a very tight relative threshold, so A1 simply fires less often. That is a
        /// performance regression, never a visual one -- the failure direction is "filters
        /// more than it needs to".
        float AdaptiveVarianceEps = 1.3e-2f;
        /// @brief (spec S1) Clamp outlier radiance against its 3x3 neighbourhood before
        /// the temporal accumulation consumes it. On by default: it is what makes the
        /// A2 iteration default safe, because a spike that survives into the a-trous
        /// chain gets spread rather than removed.
        bool FireflyClamp = true;
        /// @brief (spec S1) How many neighbourhood standard deviations a pixel may exceed
        /// the neighbourhood mean before it counts as a firefly.
        ///
        /// The reference set is the 8 neighbours, centre excluded. For n = 8 the largest
        /// deviation any *member* of that set can have is sigma * sqrt(n - 1) = 2.646
        /// sigma, so any value at or above that makes the test reachable only by the
        /// centre pixel -- exactly the intent. 3.0 clears that bound with margin for the
        /// ~26% relative error of an 8-sample sigma estimate, which puts the limit at
        /// roughly 4x the neighbourhood mean under 1-spp GI noise: invisible on genuine
        /// bright features, ~25x on a two-orders-of-magnitude spike. Lower it to clamp
        /// harder; the shader floors the limit at the brightest neighbour so even K = 1
        /// cannot cut into a feature two pixels wide.
        float FireflyClampSigma = 3.0f;
        /// @brief (spec S3) Roughness at or below which a specular pixel counts as
        /// mirror-like, letting a whole 8x8 tile of them skip the a-trous kernel. 0
        /// disables the mechanism.
        ///
        /// This is not a quality trade: the specular path already scales its
        /// edge-stopping functions by roughness (phiLuminance *= r, phiNormal /= r), and
        /// at r = 0.05 with the default ColorPhi 2.0 / NormalPhi 512 that leaves
        /// phiNormal = 10240 -- a tap must match the centre normal to within 0.81 degrees
        /// to keep 1/e of its weight -- and phiLuminance = 0.1 * sigma, so a tap must
        /// also match the centre luminance to within 10% of a standard deviation (a tap a
        /// full sigma away keeps exp(-10) = 4.5e-5). Under
        /// both conditions every non-centre tap is annihilated and the kernel returns the
        /// centre pixel it was handed. 0.05 is where that becomes true with margin;
        /// raising it starts skipping genuinely glossy surfaces that the filter would
        /// still have something to say about.
        float SpecularDenoiseRoughnessCutoff = 0.05f;
        /// @brief (defect D1) Width, in standard deviations, of the neighbourhood box the
        /// reprojected temporal history is clamped into. 0 disables the mechanism.
        ///
        /// The box is built per pixel in YCoCg from the 3x3 neighbourhood of *this* frame's
        /// radiance, centred on the neighbourhood mean, with a half-width of
        /// HistoryClampSigma * max(spatial sigma, accumulated sample sigma). The full
        /// derivation lives at SSRTClampHistory in ssrt_temporal.hlsl; the parts that matter
        /// for choosing a value:
        ///
        /// Why 1.0. Writing sigma for the per-frame sample sigma, the distance between a
        /// healthy converged history and the box centre has standard deviation
        /// sqrt((sigma/3)^2 + (0.174 sigma)^2) = 0.376 sigma -- the first term the standard
        /// error of a nine-tap mean, the second the residual noise an EMA at
        /// MaxAccumulatedFrames 16 leaves behind. At 1.0 the clamp therefore only engages
        /// past 2.66 of those, i.e. on under 1% of frames, and then only moves the value to
        /// the box edge: it is a no-op on static converged content by construction. Ghosts,
        /// which are errors of order the local mean itself (a moving limb's indirect light
        /// against a lit wall), are outside a +-0.7x-mean box immediately and are cut on
        /// their first frame instead of decaying over MaxAccumulatedFrames.
        ///
        /// Going lower trades convergence for shorter trails and does so sharply: 0.5 puts
        /// the engagement point at 1.33 standard deviations, i.e. ~18% of frames, at which
        /// rate the clamp is continuously pulling the history back towards a nine-sample
        /// mean and re-injecting that mean's own noise -- the classic failure of naive
        /// neighbourhood clamping on a Monte-Carlo signal. Below ~0.75 is not recommended.
        ///
        /// Deliberately *not* coupled to MaxAccumulatedFrames. The floor uses the per-frame
        /// sample sigma rather than the accumulated output's residual, so the box width does
        /// not shrink as the accumulation window lengthens and a value tuned at 16 frames
        /// stays valid at 64.
        ///
        /// (defect D8) This value is the *converged* width. ssrt_temporal.hlsl scales it up
        /// while a pixel is still accumulating, so that the fraction of frames on which the
        /// clamp engages stays what the derivation above assumes instead of rising to ~7% three
        /// frames into a chain -- at which rate the clamp keeps pulling the history back onto a
        /// nine-tap mean of this frame's noise and the pixel never converges at all. The scale
        /// is >= 1 by construction and reaches exactly 1 at MaxAccumulatedFrames, so this
        /// setting still means precisely what it says for any pixel that has finished
        /// accumulating.
        float HistoryClampSigma = 1.0f;
        /// @brief (diagnostic T2) Freeze the per-frame phase of the ray-direction noise.
        ///
        /// Feeds SSRTCB::FreezeNoisePhase, which makes ssrt_raymarch.hlsl seed its
        /// Hammersley jitter from 0 instead of SharedData::FrameCount. Every frame then
        /// traces the *same* ray directions, so the 2-spp Monte-Carlo signal stops changing
        /// from frame to frame.
        ///
        /// This exists to separate the two candidate causes of directional smearing that
        /// the audit could not separate observationally: an unfiltered stochastic signal
        /// being smeared by the upscaler's history clamp (which needs a *changing* noise
        /// pattern and must therefore disappear when the phase is frozen) versus the SVGF
        /// temporal pass's own reprojection defects (which are independent of the noise
        /// phase and must survive). Not a quality setting -- with the phase frozen the noise
        /// becomes a fixed, screen-space-locked pattern that no denoiser can average away.
        /// @brief (S3.10) Take the ray-direction sample scramble from the baked blue-noise
        /// array (noise.dds, t6) rather than from a pcg3d hash. Default on.
        ///
        /// The scramble is what decides where in the Hammersley sequence each pixel starts, so
        /// its *spatial* distribution is the spatial distribution of the 2-spp estimate's error.
        /// A hash makes neighbouring pixels independent, i.e. white error: energy spread evenly
        /// over all spatial frequencies including the low ones, which read as blotching and
        /// which neither a small a-trous kernel nor REBLUR's spatial pass nor an upscaler can
        /// remove. Blue noise pushes that same energy into the high frequencies, where every
        /// one of those stages -- and the eye's own contrast sensitivity -- attenuates it. The
        /// samples, the sample count and the cost are unchanged; only where the error sits is.
        ///
        /// The baked path existed but was commented out, and reading noise.dds' own DX10 header
        /// says why it was abandoned rather than fixed: the array has 64 slices and the code
        /// indexed slice 64 for its second coordinate, which returns 0 out of range. This
        /// revival uses the third axis as what it is -- 64 frames of temporal blue noise -- and
        /// takes the second coordinate from a second tap within the slice.
        ///
        /// A switch rather than a silent change, so the A/B can be run: turn it off and the
        /// noise grain goes from a fine even stipple to coarser, patchier clumping.
        bool UseBlueNoise = true;
        bool FreezeNoisePhase = false;
        /// @brief (diagnostic D3) Bypass the defect D3 history depth-disocclusion test.
        ///
        /// Feeds DenoiserCB::disableHistoryDepthTest, which makes ssrt_temporal.hlsl's
        /// IsValidHistory skip the "did this history texel hold the depth the current pixel's
        /// surface point should have had last frame?" comparison. Nothing else changes: the
        /// screen-bounds tests, the 30 degree normal agreement and the guard G4 finiteness
        /// rejection all stay in force, so the predicate becomes exactly what it was before
        /// D3 landed rather than "accept anything".
        ///
        /// It exists because D1 (the neighbourhood history clamp) and D3 (this test) both
        /// present as "the accumulation is not accumulating", and only isolating them one at a
        /// time distinguishes them in-game. D1's own bypass is HistoryClampSigma = 0, which
        /// the shader already tests for -- no separate switch is needed for it.
        ///
        /// Diagnostic, not a quality setting: with the test off, the accumulation will read
        /// history across depth discontinuities again, which is the ghost source D3 exists to
        /// remove.
        bool DisableHistoryDepthTest = false;
        /// @brief (diagnostic H) Bypass the 30 degree normal agreement test.
        ///
        /// Feeds DenoiserCB::disableHistoryNormalTest. The counterpart of
        /// DisableHistoryDepthTest for the other geometric gate, and it exists for the same
        /// reason: both gates present identically -- the accumulation refuses to build -- so
        /// only switching them off one at a time says which is responsible.
        ///
        /// The pair is what turns History Debug View's colour reading from a hypothesis into a
        /// confirmation. A screen that comes back green should start accumulating the moment
        /// this is set; a screen that comes back red should start accumulating the moment the
        /// depth-test switch is.
        bool DisableHistoryNormalTest = false;
        /// @brief (diagnostic H) Accept whatever history the reprojection lands on.
        ///
        /// Feeds DenoiserCB::forceAcceptHistory, which reduces the acceptance predicate to its
        /// upper bound: screen bounds, the guard G4 non-finite rejection and the requirement
        /// that the candidate carry a non-zero frame count. Both geometric gates are skipped.
        ///
        /// The two per-gate bypasses can only show that a gate *is* the blocker; they cannot
        /// show that the gates are the only blockers. If the accumulation still refuses to
        /// build with this set, the fault is not in the acceptance test at all -- it is in the
        /// bounds arithmetic, the history contents, or the alpha path -- and that is a
        /// different repair. This is the test that separates those.
        ///
        /// Strictly diagnostic: with it set the accumulation reads history straight across
        /// silhouettes and depth layers, i.e. maximal ghosting.
        bool ForceAcceptHistory = false;
        /// @brief (diagnostic H) Compare the *rotated* normal in the 30 degree agreement test.
        ///
        /// Feeds DenoiserCB::rotatedNormalGate. Off, the gate compares the current view-space
        /// normal against the stored previous-frame normal, which is what this pass did for its
        /// whole shipped life: biased by the inter-frame camera rotation, so it over-rejects
        /// during fast turns and passes everything else. On, it compares the current normal
        /// rotated into the previous frame's view space, which is the algebraically correct
        /// form that the defect D3 work introduced.
        ///
        /// (defect P3) The rotation no longer composes two matrices or inverts one: it reads the
        /// previous view-space components out of a single forward multiply by
        /// CameraPreviousViewProjUnjittered and undoes only the projection's own first two rows,
        /// whose four entries come off CameraProjUnjittered. The one remaining assumption -- that
        /// the two frames share a field of view -- is tested per pixel by the rotation's
        /// unit-length self-check, which falls back to the un-rotated normal and paints the pixel
        /// magenta in the diagnostic view when it fails. So this can no longer reject the screen
        /// silently.
        ///
        /// (defect P5) This flag is now the *only* consumer of that rotation. The acceptance
        /// plane's row used to be built from it as well, and that is what let one bad direction
        /// multiply cost the plane test its effect over whole surfaces; the row takes the
        /// un-rotated normal unconditionally now (see the derivation in ssrt_temporal.hlsl), which
        /// costs a plane tilt of one frame of camera rotation -- about 5% of the tolerance band at
        /// 2 degrees per frame -- and removes the dependence entirely. With this flag off, its
        /// default, nothing
        /// in the history acceptance path multiplies a direction by a previous-frame matrix at all.
        /// The default stays off for that reason as much as for the earlier one: the rotation is
        /// the step whose in-game correctness is least vouched for, and the un-rotated form is the
        /// one with measured in-game behaviour behind it.
        bool RotatedNormalGate = false;
        /// @brief (diagnostic H) Render the history-acceptance diagnostic into texDebugHistory.
        ///
        /// Feeds DenoiserCB::historyDebugView on the *diffuse* dispatch only. Inspect the
        /// result under Buffer Viewer -> texDebugHistory:
        ///
        ///   * grey, brightening over a second or two -- history is being accepted and the
        ///     accumulation is building. White is fully converged. This is the healthy picture.
        ///   * red -- rejected by the plane-distance test.
        ///   * green -- rejected by the 30 degree normal agreement test.
        ///   * blue -- rejected by screen bounds, a non-finite history sample, or a zero frame
        ///     count.
        ///   * black -- sky or far plane, where there is no history question to ask.
        ///   * (defect P5) one of seven construction colours -- the acceptance plane could not be
        ///     built, so the plane test did not run on that pixel and the history was judged by
        ///     bounds plus normal agreement alone. The hue names which *segment* of the
        ///     construction gave up, i.e. which matrices to look at, and where a segment has two
        ///     codes the darker tier is the second of them:
        ///       - bright cyan / dark teal -- the previous frame's image of this pixel. Cyan: the
        ///         point has no image inside the previous depth range, a legitimate geometric
        ///         outcome. Teal: that projection produced nothing usable at all.
        ///       - violet -- the closed loop through CameraViewProjUnjittered did not return the
        ///         depth the depth buffer gave.
        ///       - apple green / dark green -- this frame's own reconstruction. Apple: the
        ///         unprojection through CameraViewProjInverse produced no usable world position.
        ///         Dark: no finite normal direction, or a degenerate projection diagonal.
        ///       - lemon / dark amber -- the plane that reconstruction implies. Lemon: the
        ///         tolerance came out non-positive. Amber: the plane passes through the previous
        ///         camera (an edge-on surface), or the finished row is non-finite.
        ///   * magenta -- the rotated normal gate is on and its rotation failed its self-check.
        ///     (defect P5) Visible at every switch setting now, and a fact independent of the
        ///     construction colours: the plane row no longer uses that rotation, so a failed
        ///     rotation costs the gate its preferred operand and nothing else.
        ///   * orange -- unreachable by construction. It means the failCode was out of range,
        ///     i.e. a genuinely new coding error, and nothing else in this view is orange.
        ///
        /// (defect P4) The construction colours used to be four shades of yellow, on the theory
        /// that the shared hue made the *class* read at a glance. It did, and that was the
        /// problem: an in-game reading came back "orange-ish, textured, granular", which fitted
        /// all four shades equally and so identified nothing. (defect P5) The tiers above are not
        /// a return to that: there the tier *was* the diagnosis, so a misread left nothing behind,
        /// while here the hue carries it and the tier only separates two guards that already share
        /// a segment and a repair.
        ///
        /// Two pairs are worth knowing about in advance, and one switch settles both. Apple green
        /// sits 0.20 from the pure green of a normal-gate rejection on the luminance-weighted
        /// metric the CPU harness asserts on, and dark teal sits 0.28 from the pure blue of a
        /// bounds-or-data rejection -- comfortable margins, but if a reading is ever unsure, turn
        /// Disable History Depth Test on: every construction colour is suppressed by it, so a
        /// green or blue that survives is a rejection and one that vanishes was a construction
        /// failure.
        ///
        /// One reading caveat the palette cannot remove: this view writes flat colours, so a
        /// *speckle* of two colours averages to a third under any downscale or screenshot
        /// compression -- a fine red/green mixture reads as olive, and as orange once JPEG has
        /// had it. Zoom to 1:1 before naming the colour of a granular region, and prefer a
        /// lossless screenshot.
        ///
        /// (defect P3) A full pale-yellow screen with a stationary camera was the finding that
        /// identified the last arithmetic error in the plane construction: at rest the projection
        /// chain must return the depth it started from, so a screen-wide range failure could only
        /// be a matrix that was not the transform its name claimed. The construction now proves
        /// its own reconstruction against the depth buffer every frame, so that class of failure
        /// reports itself in violet instead of silently costing the test its effect.
        ///
        /// A uniform colour over the whole screen is the finding: it means one gate is turning
        /// away every candidate everywhere, which is what makes the accumulation degenerate to
        /// alpha = 1 and the denoiser a passthrough. Speckles of colour along silhouettes and
        /// moving edges are normal and correct.
        bool HistoryDebugView = false;
#ifdef ENABLE_SHARC
        bool EnableSharc = false;
#endif
    } settings;

    /// @brief Mirrored by the `SSRTSettings` struct in Common/SharedData.hlsli, which lives
    /// inside the shared FeatureData constant buffer with ExponentialHeightFogSettings behind
    /// it -- so sizeof is load bearing and must stay a multiple of 16. AmbientReinjection and
    /// its strength opened a second float4 row; the SSGI contact pair below took the two slots
    /// that row had left, so the buffer did not grow and the HLSL side no longer needs the
    /// explicit `float2 ssrtPad0` it used to declare in their place.
    struct alignas(16) SharedData
    {
        uint EnableSpecular;
        float SpecularMult;
        float DiffuseMult;
        float AmbientMult;
        // --- row 1 ---
        uint AmbientReinjection;
        float AmbientReinjectionStrength;
        /// @brief (contact AO) Non-zero iff Screen Space GI is loaded and enabled with its
        /// contact-occlusion pass on. Read by ssrt_raymarch.hlsl for one purpose only: deciding
        /// whether the near-field occlusion vote may be suppressed, which is only safe while a
        /// deterministic term is standing in for it.
        ///
        /// This is Screen Space GI's setting living in SSRT's block, which is deliberate: SSGI
        /// publishes no FeatureData block of its own, and adding one to carry two scalars would
        /// shift every offset behind it. Filled from ScreenSpaceGI::settings in
        /// GetCommonBufferData. These two slots previously held the retired reinjection contact
        /// pair, so the layout is unchanged.
        uint SsgiContactAoActive;
        /// @brief (contact AO) That pass's search radius in centimetres, i.e. the range the vote
        /// suppression hands over. Same value the kernel measures itself in, so the handover
        /// cannot drift out of alignment with the kernel's own falloff.
        float SsgiContactRadius;
    };
    static_assert(sizeof(SharedData) == 32,
        "ScreenSpaceRayTracing::SharedData must stay 32 bytes (two constant buffer rows); "
        "ExponentialHeightFogSettings sits behind it in FeatureData.");

    /// @brief Mirrored by the `SSRTCB` declaration in ssrt_raymarch.hlsl, which is the only
    /// shader that binds b1 in this feature.
    ///
    /// The first two float4 rows were full, so FreezeNoisePhase opens a third; sizeof is 48,
    /// still a multiple of 16 as D3D11 requires. The shader declares only the nine scalars
    /// and not the padding -- a shader may declare a prefix of a larger constant buffer, and
    /// a trailing `float pad0[3]` would *not* mirror this layout in HLSL, where each array
    /// element is padded to its own 16-byte row.
    struct alignas(16) SSRTCB
    {
        uint MaxSteps;
        uint MaxMips;
        uint UseDynamicCubemapsAsFallback;
        float Thickness;
        // --- row 1 ---
        float NormalBias;
        float BRDFBias;
        float OcclusionStrength;
        float CubemapNormalization;
        // --- row 2 ---
        /// @brief (diagnostic T2) Non-zero replaces SharedData::FrameCount with 0 in the
        /// ray-direction noise seed. See Settings::FreezeNoisePhase.
        uint FreezeNoisePhase;
        /// @brief (S3.10) Non-zero takes the sample scramble from the baked blue-noise array;
        /// zero takes it from a pcg3d hash. See Settings::UseBlueNoise.
        uint UseBlueNoise;
        float pad0[2];
    };
    static_assert(sizeof(SSRTCB) == 48,
        "ScreenSpaceRayTracing::SSRTCB must stay three whole 16-byte constant buffer rows; "
        "the SSRTCB declaration in ssrt_raymarch.hlsl mirrors these offsets up to UseBlueNoise "
        "and must move with them.");

    /// @brief Mirrored by the `DenoiserCB` declaration in ssrt_spatial.hlsl. Whole float4
    /// rows exactly, so no member straddles a 16-byte boundary and the HLSL packing rules
    /// reproduce this layout verbatim. ssrt_temporal.hlsl declares all four rows,
    /// ssrt_spatial.hlsl the first two and ssrt_variance.hlsl only the first, which is legal
    /// -- a shader may declare a prefix of a larger constant buffer. (batch 1, item 2:
    /// ssrt_spatial.hlsl now declares all four, because hitRadiusStrength sits on the last one;
    /// ssrt_preblur.hlsl declares the first two.)
    ///
    /// The mirroring was checked against fxc's own reflection rather than by reading, because
    /// a silent mismatch here presents as a diagnostic switch that does nothing -- which is
    /// indistinguishable from the mechanism it toggles being innocent. Every offset agreed.
    /// The static_assert below is what keeps it that way as fields are added: it fails the
    /// build if a member is inserted without the HLSL side moving with it.
    struct alignas(16) DenoiserCB
    {
        float invMaxAccumulatedFrames;
        uint atrousIterations;
        float colorPhi;
        float normalPhi;
        // --- row 1 ---
        uint adaptiveFiltering;
        float adaptiveHistoryThreshold;
        float adaptiveVarianceEps;
        /// @brief (spec S1) Firefly clamp width in neighbourhood standard deviations;
        /// 0 switches the clamp and its LDS prefetch off. Took over the A-layer's pad
        /// slot, so the buffer did not grow.
        float fireflyClampSigma;
        // --- row 2 ---
        /// @brief (spec S3) Roughness cutoff for the specular mirror skip; 0 = off. Read
        /// only by the SSRT_SPECULAR permutation of ssrt_spatial.hlsl.
        float specularRoughnessCutoff;
        /// @brief (defect D1) Neighbourhood history-clamp width in standard deviations;
        /// 0 switches the clamp off. Read by ssrt_temporal.hlsl, which is why that shader
        /// now declares all three rows of this buffer instead of two. Took the first of the
        /// three pad slots row 2 had spare, so the buffer did not grow.
        float historyClampSigma;
        /// @brief (diagnostic D3) Non-zero makes ssrt_temporal.hlsl skip the defect D3
        /// plane-distance disocclusion test in IsValidHistory. Took the second of row 2's pad
        /// slots; ssrt_spatial.hlsl still declares the whole tail as `float3 denoiserPad1` and
        /// reads none of it.
        uint disableHistoryDepthTest;
        /// @brief (diagnostic H) Non-zero makes ssrt_temporal.hlsl skip the 30 degree normal
        /// agreement test and nothing else. Took row 2's last pad slot, so the buffer did not
        /// grow for it. See Settings::DisableHistoryNormalTest.
        uint disableHistoryNormalTest;
        // --- row 3 ---
        /// @brief (diagnostic H) Non-zero reduces the acceptance predicate to bounds plus the
        /// guard G4 finiteness and accumFrames > 0 requirements -- both geometric gates off.
        /// See Settings::ForceAcceptHistory.
        uint forceAcceptHistory;
        /// @brief (diagnostic H) Non-zero selects the previous-view-space rotation of the
        /// current normal for the 30 degree comparison; zero, the default, uses the un-rotated
        /// normal. See Settings::RotatedNormalGate.
        uint rotatedNormalGate;
        /// @brief (diagnostic H) Non-zero makes ssrt_temporal.hlsl write texDebugHistory.
        /// Deliberately set on the diffuse dispatch only -- the two chains share one shader and
        /// one debug surface, so the specular pass always passes 0 and leaves the picture the
        /// diffuse pass drew. See Settings::HistoryDebugView.
        uint historyDebugView;
        /// @brief (batch 1, item 2) Strength of the hit-distance kernel narrowing in the diffuse
        /// a-trous permutation; 0 leaves the kernel bit-identical to the unmodulated one. Took
        /// row 3's last pad slot, so the buffer did not grow -- but ssrt_spatial.hlsl now has to
        /// declare all four rows instead of two, since the field it needs is on the last one.
        /// See Settings::HitRadiusStrength.
        float hitRadiusStrength;
    };
    static_assert(sizeof(DenoiserCB) == 64,
        "ScreenSpaceRayTracing::DenoiserCB must stay four whole 16-byte constant buffer rows; "
        "the DenoiserCB declarations in ssrt_temporal.hlsl (all four rows), ssrt_spatial.hlsl "
        "(two) and ssrt_variance.hlsl (one) mirror these offsets and must move with it.");

    /// @brief (batch C1) Mirrored by the `NRDPackCB` declaration in ssrt_nrd_pack.hlsl.
    /// One row: the REBLUR hit-distance normalization constants. The unpack shader
    /// declares no constant buffer at all.
    struct alignas(16) NRDPackCB
    {
        float hitDistA;
        float hitDistB;
        float hitDistC;
        float pad0;
    };

    eastl::unique_ptr<ConstantBuffer> ssrtCB;
    eastl::unique_ptr<ConstantBuffer> denoiserCB;
    eastl::unique_ptr<ConstantBuffer> nrdPackCB;

    bool recompileFlag = false;

    /// @brief Cached once per frame in Prepass() and read by both draw passes, instead
    /// of repeating the player-cell lookup twice (audit P9).
    bool inInterior = true;

    /// @brief Dynamic-resolution extent the depth pyramid was last cleared for; a change
    /// retriggers the far-plane clear of every mip (audit #8).
    ///
    /// Integer, not float2: the value comes from screenSize * a dynamic-resolution ratio, and
    /// an exact float comparison on that product turns any ratio wobble -- which is what
    /// dynamic resolution *is* -- into a spurious change event. The pyramid is addressed by
    /// whole texels, so whole texels are the quantity that can actually go stale.
    uint lastDepthExtentX = 0;
    uint lastDepthExtentY = 0;

    /// @brief (guard G8, repaired) Output extent the denoiser history was last cleared for.
    ///
    /// Deliberately the *output* resolution and not the dynamic-resolution sub-rect. The
    /// history textures are allocated at output resolution and are addressed by pixel, so only
    /// a change of that extent can leave a texel describing a pixel that no longer exists. A
    /// change of the DRS ratio alone needs no clear at all: ssrt_temporal.hlsl rejects every
    /// texel outside the *previous* frame's sub-rect per-tap (audit #16's
    /// DynamicResolutionParams1.zw bounds test) and the defect D3 depth test rejects whatever
    /// stale content survives that.
    ///
    /// Keying the clear on the sub-rect instead is what made the mechanism dangerous: with a
    /// live per-frame ratio the latch fires every frame, ClearDenoiserHistory() zeroes all four
    /// history textures plus texHistoryDepth every frame, and every pixel reseeds with
    /// accumFrames = 1, i.e. alpha = 1. That reads in-game as SVGF being a passthrough --
    /// no denoising, and no ghosting either, because nothing is being accumulated to smear.
    uint lastHistoryExtentX = 0;
    uint lastHistoryExtentY = 0;

    /// @brief (guard G8) A denoiser-history clear is owed before anything reads it.
    ///
    /// Starts true: the history textures are created without initial data, so their contents
    /// are undefined until something writes them, and "undefined R16G16B16A16_FLOAT" includes
    /// every NaN and Inf bit pattern.
    bool historyClearPending = true;

    /// @brief (guard G8) Previous frame's values of the settings that decide whether
    /// last frame wrote a history worth reading. These start at "nothing ran" so the
    /// first frame of any enabled configuration counts as a transition — for the SVGF
    /// history clear and for the REBLUR reset alike.
    ///
    /// (S1.3) These track the *effective* denoiser per chain, not Settings::DenoiserMethod.
    /// A fallback from REBLUR to SVGF is exactly the transition the SVGF history clear
    /// exists for, and keying the latch on the requested method would have missed it.
    uint lastEffectiveDenoiserDiffuse = kDenoiserOff;
    uint lastEffectiveDenoiserSpecular = kDenoiserOff;
    bool lastEnableDiffuse = false;
    bool lastEnableSpecular = false;

    /// @brief (batch C1) Convenience predicates over Settings::DenoiserMethod — what the
    /// *user asked for*. The UI reads these; the dispatch gates read EffectiveDenoiser().
    [[nodiscard]] bool SVGFSelected() const { return settings.DenoiserMethod == kDenoiserSVGF; }
    [[nodiscard]] bool ReblurSelected() const { return settings.DenoiserMethod == kDenoiserREBLUR; }

    /// @brief (S1.3) The denoiser that will actually process a chain this frame, resolved
    /// once per frame by ResolveDenoisers() before anything allocates or dispatches.
    ///
    /// Never returns REBLUR unless the whole REBLUR chain for that signal is ready, and
    /// never returns Off just because REBLUR is unavailable: an unavailable REBLUR falls
    /// back to SVGF, because "any reasonable combination of settings must work on its own"
    /// is a hard rule of this project and raw 2-spp noise is not a working configuration.
    /// Off is reached only when the user asked for it, or when *both* chains failed to
    /// compile — in which case there is nothing left to fall back to.
    [[nodiscard]] uint EffectiveDenoiser(bool a_specular) const
    {
        return a_specular ? effectiveDenoiserSpecular : effectiveDenoiserDiffuse;
    }
    [[nodiscard]] bool AnyChainSVGF() const
    {
        return effectiveDenoiserDiffuse == kDenoiserSVGF || effectiveDenoiserSpecular == kDenoiserSVGF;
    }

    /// @brief (S1.3) Resolved effective denoiser per chain; written only by ResolveDenoisers().
    uint effectiveDenoiserDiffuse = kDenoiserOff;
    uint effectiveDenoiserSpecular = kDenoiserOff;

    /// @brief (S1.3) Why the effective denoiser differs from the requested one, for the UI.
    /// Points at a string literal or is null; never owns storage.
    const char* denoiserFallbackReason = nullptr;

    /// @brief (S1.4) DiffuseSPP as it stood the last time CompileComputeShaders ran.
    ///
    /// DIFFUSE_SPP is a compile-time macro, so a change of the setting is only real once the
    /// ray-march permutations have been rebuilt. The UI used to be the only path that set
    /// recompileFlag, which left LoadSettings free to install a value the compiled shader
    /// does not implement — silently tracing the previous session's sample count. Comparing
    /// against this instead makes the recompile unconditional on *any* path that can change
    /// the setting, at the cost of one uint.
    uint compiledDiffuseSPP = 0;

    /// @brief (S1.4) Clamp every setting into the range its consumer can actually honour.
    ///
    /// The UI sliders carry ImGuiSliderFlags_AlwaysClamp, so they are not the hazard; a
    /// hand-edited or future-version json is. DiffuseSPP is the sharpest case — 0 makes the
    /// ray march's sample loop degenerate and >16 overruns the Hammersley table's assumption
    /// — but a negative ColorPhi or an AtrousIterations of 400 are dispatch-count and
    /// numerical hazards of the same family.
    void SanitizeSettings();

    /// @brief (S1.3) Whether the SVGF chain for one signal has every shader it needs.
    ///
    /// Atomic on purpose: a chain missing one pass must not run the others and must not
    /// leave a consumer reading a surface the missing pass was supposed to write. The
    /// pre-blur is deliberately absent — it degrades on its own (the temporal pass simply
    /// reads the raw surface and keeps the firefly clamp), which is a documented path.
    [[nodiscard]] bool SvgfChainReady(bool a_specular) const;

    /// @brief (S1.3) Whether the REBLUR path *may be brought up* for one signal, judged
    /// without touching any REBLUR allocation.
    ///
    /// Split from ReblurReady so the decision can be made before EnsureNRDResources runs:
    /// the old order asked "is the instance valid" before anything had created one, which
    /// is why a REBLUR selection could reach the dispatch gates and find nothing there.
    /// Everything here is a fact about this frame's configuration, not about SSRT's own
    /// allocations.
    [[nodiscard]] bool ReblurStaticallyAvailable(bool a_specular) const;

    /// @brief (S1.3) ReblurStaticallyAvailable plus this chain's own surfaces and instance.
    ///
    /// Deliberately does *not* ask whether this frame's guides have been published, and that
    /// omission is load-bearing: NRD::PrepareGuides is gated on a consumer having resolved to
    /// REBLUR (S2.6), so a predicate that consulted AreGuidesReady() would make the two
    /// decisions mutually dependent and both would answer "no" forever from the first frame.
    [[nodiscard]] bool ReblurResourcesReady(bool a_specular) const;

    /// @brief (batch C1) Whether the REBLUR path can actually run this frame: everything
    /// ReblurStaticallyAvailable checks, plus this frame's guides being published and the
    /// per-chain integration instance and surfaces existing.
    [[nodiscard]] bool ReblurReady(bool a_specular) const;

    /// @brief (S1.3) Resolve EffectiveDenoiser for both chains and bring up whatever the
    /// answer needs. Called once per frame from Prepass, before UpdateHistoryValidity so the
    /// history latches see the effective values, and before any dispatch.
    void ResolveDenoisers();

    /// @brief (S2.5) Allocate the SVGF-only surfaces on first need.
    ///
    /// Ten textures -- the two colour histories, the two moment histories, the temporal and
    /// variance scratch, the moments surface, the normal and depth history snapshots and the
    /// debug view -- 68 bytes per output pixel between them, i.e. ~140 MB at 1080p and
    /// ~560 MB at a 4K allocation. Every one of them is read by exactly one shader,
    /// ssrt_temporal.hlsl and the passes around it, and none of those runs under REBLUR or
    /// Off. They were nevertheless allocated at boot for every user of the feature.
    ///
    /// Called from ResolveDenoisers, and only on the path that has already established SVGF
    /// is what this frame will use -- which includes the REBLUR-unavailable fallback and the
    /// Buffer Viewer being open, so neither can find a null surface.
    void EnsureSvgfResources();

    /// @brief (S2.5) Whether the SVGF history for one signal has to be maintained this frame.
    ///
    /// The history *maintenance* -- two full-screen RGBA16F colour copies plus the
    /// normal-roughness snapshot, ~40 bytes per pixel of read+write traffic, ~330 MB/frame at
    /// a 4K allocation -- ran unconditionally, including under REBLUR and Off where nothing
    /// reads any of it. ssrt_temporal.hlsl is the sole reader of all three surfaces; the t0
    /// declarations in ssrt_raymarch / ssrt_variance / ssrt_spatial are vestigial (declared,
    /// never referenced, so fxc strips them).
    ///
    /// The Buffer Viewer counts as a consumer. It is the only other thing that looks at these
    /// surfaces, and a debug view that silently freezes the moment the default denoiser is
    /// selected would be worse than the waste.
    [[nodiscard]] bool SvgfHistoryNeeded(bool a_specular) const
    {
        return EffectiveDenoiser(a_specular) == kDenoiserSVGF || bufferViewerActive;
    }

    /// @brief (S2.5) Latched once per frame in Prepass from bufferViewerOpen, which
    /// DrawSettings raises while its Buffer Viewer tree is expanded.
    ///
    /// Two variables rather than one because the menu draws at present time, i.e. after
    /// Prepass: the raise is consumed by the *following* frame and then cleared, so an open
    /// tree keeps it set for as long as the menu is up and it decays one frame after the menu
    /// closes. A single variable would either never clear or clear before it was read.
    bool bufferViewerOpen = false;
    bool bufferViewerActive = false;

    /// @brief (S4.15) Put every piece of cross-frame state back to "nothing has run yet".
    ///
    /// SetupResources is not only the boot path: BSShaderRenderTargets_Create re-runs
    /// State::Setup() whenever the game recreates its render targets, i.e. on a resolution
    /// change. That rebuilt every texture in this feature and left the state machine that
    /// describes them untouched — the extent latches still held the old extents (so the
    /// far-plane clear of a same-extent rebuild never fired), historyClearPending was
    /// false (so freshly allocated, undefined history was consumed as if valid), and the
    /// lazily allocated NRD surfaces and instances kept the *previous* resolution's
    /// dimensions forever. Extents, history validity and the REBLUR bring-up are one
    /// state machine and are now reset as one.
    void ResetFrameState();

    /// @brief (batch C1) Allocates the NRD input/output surfaces and initializes the
    /// two REBLUR instances on first use, exactly like EnsureSharcResources: a user
    /// who stays on SVGF never pays for REBLUR's permanent/transient pools.
    void EnsureNRDResources();

    /// @brief (batch C1) One flag per REBLUR instance rather than one shared, because
    /// diffuse dispatches earlier in the frame than specular and each instance must
    /// clear its own flag only after it has actually consumed the reset.
    bool resetReblurDiffuse = true;
    bool resetReblurSpecular = true;

    /// @brief (guard G8) Player cell the history belongs to. A different pointer means a
    /// load, a fast travel, a coc or a door transition -- i.e. the whole screen changed
    /// while the history textures did not.
    RE::TESObjectCELL* lastCell = nullptr;

    /// @brief (guard G8) Zeroes the four textures that survive across frames --
    /// texHistoryDiffuse / texHistoryMomentsDiffuse and their specular counterparts.
    ///
    /// Zero is the neutral state rather than merely a blank one: a zero accumulated frame
    /// count makes the temporal pass's alpha exactly 1, so a cleared pixel takes this
    /// frame's sample whole and seeds a fresh chain, which is the same behaviour a
    /// disocclusion already produces.
    void ClearDenoiserHistory();

    /// @brief (guard G8) Latches historyClearPending whenever the accumulated history has
    /// stopped describing what is on screen. Cheap: one player-cell pointer read plus three
    /// bool comparisons, and it must run before anything consumes the history.
    void UpdateHistoryValidity();

    void DrawSSRTSpecular();
    void DrawSSRTDiffuse();

    /// @brief (batch C1) The whole REBLUR leg for one chain: front-end pack of the
    /// ray-march outputs, the NRD instance dispatch, back-end unpack into the surface
    /// the rest of the pipeline reads. Callers gate on ReblurReady(a_specular).
    /// Restores the sampler and b1 bindings the surrounding draw set up before it
    /// returns, since the NRD dispatch owns those slots while it runs.
    ///
    /// @return true iff the NRD dispatch actually ran to completion and the denoised
    /// result was unpacked over the chain's radiance surface. On false the radiance
    /// surface is left exactly as the ray march wrote it — undenoised, but this frame's
    /// — and the chain's REBLUR reset stays pending. (S1.2: the old void signature let a
    /// skipped dispatch clear the reset flag and unpack whatever the output surface
    /// happened to hold, which on the first frame is uninitialised RGBA16F.)
    [[nodiscard]] bool RunReblur(bool a_specular);
    /// @brief Snapshots the normal-roughness G-buffer into texHistoryNormals, and (defect
    /// D3, when SVGF is on) mip 0 of the Hi-Z pyramid into texHistoryDepth, for next frame's
    /// SVGF temporal validation. Called exactly once per frame, by whichever of the two draw
    /// passes runs last (audit #13).
    void CopyHistoryGeometry();
    virtual void Prepass() override;

    /// @brief (perf 3) Copies one full-screen surface into another over the dynamic-resolution
    /// sub-rectangle only, instead of over the whole allocation.
    ///
    /// Every texture in this feature is allocated at the full output resolution while every
    /// dispatch covers `Util::ConvertToDynamic(screenSize)`, so at DLSS Quality (0.667 linear)
    /// 56% of every CopyResource in the chain moved texels that no pass had written and no pass
    /// would read. The region outside the sub-rect is not merely redundant, it is *stale on both
    /// sides*: the source's copy of it was never written by any dispatch either, so restricting
    /// the copy replaces one surface's untouched content with another's.
    ///
    /// Safe for every consumer in the chain, and that is a property of the readers rather than a
    /// hope:
    ///   * texHistoryDiffuse / texHistoryMomentsDiffuse (and their specular twins) are read only
    ///     by ssrt_temporal.hlsl, and only through IsValidHistory, which rejects any tap outside
    ///     `prevRenderSize` -- the previous frame's sub-rect, i.e. exactly the region the
    ///     previous frame's copy filled. The t0 declarations in ssrt_raymarch / ssrt_variance /
    ///     ssrt_spatial are vestigial: none of the three reads the texture.
    ///   * texSSRTDiffuseColor / texSSRColor are read at SV_DispatchThreadID by the composite and
    ///     the temporal pass, through the clamp-to-edge tile fills (clamped to screen_size - 1),
    ///     and at bounds-checked positions by the a-trous taps. All inside the sub-rect.
    ///   * texHistoryDepth's outside region is *better* off: it keeps the far plane that
    ///     ClearDenoiserHistory wrote, which is the value defect D3 wants there, instead of
    ///     inheriting texDepth's own untouched content.
    ///
    /// One texel of margin is added on each axis and the result clamped to the allocation, so a
    /// float-rounding disagreement between this and the shader's
    /// `BufferDim.xy * DynamicResolutionParams1.xy` cannot leave a seam.
    ///
    /// @param a_dst Destination resource; the sub-rect lands at (0, 0), as CopyResource put it.
    /// @param a_src Source resource, which must have the destination's format and dimensions.
    void CopyDynamicRegion(ID3D11Resource* a_dst, ID3D11Resource* a_src) const;

    SharedData GetCommonBufferData();

    /// @brief Builds the denoiser constant buffer from the current settings. Shared by
    /// DrawSSRTSpecular and DrawSSRTDiffuse so the two cannot drift apart as fields are
    /// added; `atrousIterations` is overwritten per a-trous iteration by both callers.
    ///
    /// @param a_isDiffuseChain Only the diffuse chain may write the history debug view. The two
    /// chains share one shader permutation and one debug surface, and specular runs second, so
    /// letting both write it would leave the picture showing whichever pass ran last. Passed
    /// explicitly at both call sites rather than defaulted, so a third caller has to decide.
    DenoiserCB GetDenoiserCBData(bool a_isDiffuseChain) const;

    /// @brief (perf 1) Whether any of the five temporal-pass diagnostic switches is set.
    ///
    /// The production permutation of ssrt_temporal.hlsl has those five gates folded to literal
    /// zero, so it cannot honour any of them; this is the predicate that decides whether a
    /// dispatch has to use temporalDiagCS instead. Both chains ask it, and it deliberately
    /// includes HistoryDebugView even though only the diffuse dispatch writes the debug surface:
    /// the specular pass shares the shader, and picking the two permutations independently per
    /// chain would buy one dispatch's worth of instructions in exchange for a second PSO switch
    /// per frame.
    ///
    /// @return true while the user is diagnosing, false in every shipping configuration.
    [[nodiscard]] bool AnyDenoiserDiagnostic() const
    {
        return settings.DisableHistoryDepthTest || settings.DisableHistoryNormalTest ||
               settings.ForceAcceptHistory || settings.RotatedNormalGate || settings.HistoryDebugView;
    }

    /// @brief (perf 1) The temporal-pass shader this frame's configuration requires.
    ///
    /// Falls back to the production permutation when the diagnostic one is unavailable (a failed
    /// compile), which degrades a diagnostic switch to "reads as off" rather than dropping the
    /// denoiser. Returns nullptr only if the production permutation itself is missing, which the
    /// callers already have to handle.
    [[nodiscard]] ID3D11ComputeShader* SelectTemporalShader() const
    {
        if (AnyDenoiserDiagnostic() && temporalDiagCS)
            return temporalDiagCS.get();
        return temporalCS.get();
    }

    eastl::unique_ptr<Texture2D> texDepth = nullptr;
    eastl::unique_ptr<Texture2D> texColor = nullptr;
    eastl::unique_ptr<Texture2D> texSSRColor = nullptr;
    eastl::unique_ptr<Texture2D> texSSRTDiffuseColor = nullptr;
    /// @brief (ambient reinjection) Raw per-pixel diffuse hit confidence as the ray march
    /// resolved it, R8_UNORM.
    ///
    /// It exists because .w of texSSRTDiffuseColor cannot carry this past the denoiser:
    /// ssrt_temporal.hlsl overwrites .w with the luminance variance and the variance and
    /// a-trous passes keep it there. One byte per texel against the ~8 bytes of every other
    /// full-screen surface here, and UNORM storage means every read is a [0,1] value by
    /// construction.
    ///
    /// @warning (batch 1, item 3 -- assessed and rejected) This is NOT NRD's `confidence`, and it
    /// must not be wired to the temporal accumulation window. NRD documents an externally
    /// supplied confidence as the best available tool against temporal lag, with
    /// `historyLength *= lerp(conf, 1, 1 / (1 + len))`, and the shared name is the whole trap.
    /// Four independent reasons, most conclusive first:
    ///
    /// 1. It is identically 1.0 over the whole screen in the default configuration. Every sample
    ///    that takes the dynamic-cubemap fallback ends with `confidence = 1` right after the
    ///    `lerp(envColor, sampleColor, confidence)` in ssrt_raymarch.hlsl -- the fallback has
    ///    *supplied* the unresolved directions, so reporting less would double-count when the
    ///    composite subtracts ambient. And EnableAmbientReinjection forces
    ///    UseDynamicCubemapsAsFallback off, so this surface means "real hit coverage" in one mode
    ///    and "1.0 everywhere" in the other. Anything keyed off it would be inert in one
    ///    configuration and live in the other, which is precisely the cross-setting coupling the
    ///    project rules forbid.
    /// 2. Where it does vary, it is anti-correlated with what the NRD formula wants. NRD's
    ///    confidence answers "is the accumulated history still valid?" -- a lighting-change
    ///    detector. This answers "what fraction of the hemisphere did screen space resolve?".
    ///    Low coverage means the light came from the cubemap and the world-space cache, which are
    ///    the most temporally *stable* inputs in the pipeline: open ground, sky-facing surfaces.
    ///    Those pixels should accumulate longest, and the formula would shorten their window
    ///    hardest.
    /// 3. Used as a change detector rather than as a level, it would need the previous frame's
    ///    coverage reprojected and validated -- another history surface with its own disocclusion
    ///    problem -- and at DiffuseSPP 2 the per-frame estimate is a two-sample mean whose own
    ///    standard deviation is ~0.35 at mid coverage, so the detector would fire on sampling
    ///    noise unless it were first temporally accumulated itself.
    /// 4. Both failure modes a coverage jump would indicate are already covered by mechanisms
    ///    that measure them directly: a radiometric change by the defect D1 neighbourhood clamp,
    ///    a geometric one by the defect D3 plane test.
    ///
    /// What *would* be a legitimate confidence source here is a global illumination-change
    /// signal -- a per-frame sun-direction or ambient-colour delta, of the kind Sky Sync already
    /// tracks -- supplied as a scalar rather than as a per-pixel coverage. Different input,
    /// different work, and not this surface.
    eastl::unique_ptr<Texture2D> texSSRTDiffuseConfidence = nullptr;
    /// @brief (ambient reinjection) The same signal after ssrt_diffuse_composite.hlsl's
    /// depth-aware 7x7 spatial mean; this is what DeferredCompositeCS lerps with. A separate
    /// surface because a blur cannot run in place.
    eastl::unique_ptr<Texture2D> texSSRTDiffuseConfidenceSmooth = nullptr;
    /// @brief (batch 1, item 2) Per-pixel diffuse hit distance, R8_UNORM, written by
    /// ssrt_raymarch.hlsl at u6 and read by the diffuse permutation of ssrt_spatial.hlsl at t5.
    ///
    /// Not a distance in game units: the payload is t / (t + SSRT_HITT_REF_TEXELS), where t is
    /// the light's correlation length expressed in render texels at this pixel's depth. That is
    /// the quantity the kernel actually needs, it is dimensionless, and it makes the surface
    /// dynamic-resolution and FOV proof; the reciprocal form is what lets eight bits hold t's
    /// four-orders-of-magnitude range across an exterior. A miss, a rejected hit and a SHARC
    /// cache hit all encode exactly 1.0 -- "no screen-space hit at all" -- which the consumer
    /// maps to the unmodified kernel bit for bit. See the derivation at that constant in
    /// ssrt_common.hlsli.
    ///
    /// One byte per texel, on the same argument as the confidence pair above, and UNORM so no
    /// consumer needs a finiteness guard.
    eastl::unique_ptr<Texture2D> texSSRTDiffuseHitDistance = nullptr;
    eastl::unique_ptr<Texture2D> texHistory = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryDiffuse = nullptr;
    eastl::unique_ptr<Texture2D> texTemporal = nullptr;
    eastl::unique_ptr<Texture2D> texMoments = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryMoments = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryMomentsDiffuse = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryNormals = nullptr;
    /// @brief (defect D3) Previous frame's raw depth, snapshotted from mip 0 of the Hi-Z
    /// pyramid once per frame by CopyHistoryGeometry. Read by ssrt_temporal.hlsl at t7 as
    /// the observed side of its depth disocclusion test.
    eastl::unique_ptr<Texture2D> texHistoryDepth = nullptr;
    /// @brief (diagnostic H) Per-pixel picture of what the temporal pass's history acceptance
    /// test decided, written by ssrt_temporal.hlsl at u2 when Settings::HistoryDebugView is on
    /// and inspected under Buffer Viewer. See that setting for the colour key.
    ///
    /// R8G8B8A8_UNORM: the payload is three display colours plus an ignored alpha, so a byte
    /// per channel is exactly enough and a UNORM read cannot be non-finite. One quarter the
    /// footprint of the RGBA16F surfaces around it, and the Buffer Viewer draws with blending
    /// disabled so the alpha channel does not need to carry anything meaningful.
    eastl::unique_ptr<Texture2D> texDebugHistory = nullptr;
    eastl::unique_ptr<Texture2D> texVariance = nullptr;
    /// @brief Specular hit distance; consumed by Upscaling.cpp as the DLSS-RR guide.
    /// Was a raw `Texture2D*` from a bare `new` and leaked (audit #20).
    eastl::unique_ptr<Texture2D> texHitDistance = nullptr;
    // (audit P6 / #20) texHitPDF (was u1) and texOutput (a redundant full-screen copy
    // of texSSRColor) had no consumer anywhere and are gone.

    /// @brief (batch C1) REBLUR-path scratch, allocated lazily by EnsureNRDResources.
    /// texNRDPackInput carries the front-end packed signal (YCoCg radiance + normalized hit
    /// distance, RGBA16F); texNRDPackOutput is what REBLUR writes and the unpack pass reads
    /// back into the chain's own surface. SSRT's ray-march outputs are untouched — the
    /// pack/unpack passes adapt around them, which is what keeps the tracing side
    /// byte-identical between denoisers.
    ///
    /// (S2.7) **One pair, shared by both chains.** Batch C1 gave diffuse and specular a pair
    /// each: four full-screen RGBA16F surfaces, 32 bytes per output pixel, ~265 MB at a 4K
    /// allocation. The two are pure scratch with a lifetime entirely inside RunReblur — pack
    /// writes the input, the NRD instance reads it and writes the output, the unpack reads the
    /// output — and the two chains execute strictly serially, DrawSSRTDiffuse before
    /// DrawSSRTSpecular within one Deferred::DeferredPasses. Nothing in either surface has to
    /// survive the call, so nothing has to be duplicated: 127 MiB back at 4K.
    ///
    /// What is emphatically **not** shared is anything NRD keeps: each chain owns its own
    /// nrd::Instance below, and with it its own permanent pool — the accumulated radiance,
    /// history length and fast history that make REBLUR a denoiser rather than a blur. Those
    /// are per-signal by construction and sharing them would be nonsense, not an optimisation.
    eastl::unique_ptr<Texture2D> texNRDPackInput = nullptr;
    eastl::unique_ptr<Texture2D> texNRDPackOutput = nullptr;

    NRDReblurIntegration nrdReblurDiffuse;
    NRDReblurIntegration nrdReblurSpecular;
    nrd::ReblurSettings reblurDiffuseSettings{};
    nrd::ReblurSettings reblurSpecularSettings{};

#ifdef ENABLE_SHARC
    eastl::unique_ptr<Buffer> sharcHashEntries = nullptr;
    eastl::unique_ptr<Buffer> sharcHashCopyOffsets = nullptr;
    eastl::unique_ptr<Buffer> sharcVoxelData = nullptr;
    eastl::unique_ptr<Buffer> sharcVoxelDataPrev = nullptr;
#endif

    winrt::com_ptr<ID3D11ShaderResourceView> noiseSRV = nullptr;

    // (audit P3) Mip levels 0..maxMips-1 exist in texDepth. The Hi-Z traversal never
    // reads above SSRTCB::MaxMips, whose slider is clamped to maxMips - 1, so 7 levels
    // (mip 0..6) cover the whole usable range; the old 9 allocated two mips and ran two
    // downsample dispatches that no ray could ever sample.
    static const uint maxMips = 7;

    /// @brief The coarsest pyramid level Prepass actually filled on the most recent frame that
    /// built it, i.e. levels 0..hiZTopMipBuilt hold this frame's depth.
    ///
    /// texDepth is always *allocated* with all maxMips levels, because MaxMips is a runtime
    /// slider, but only levels up to the traversal's ceiling are downsampled -- at the default
    /// of 6 that is every level, at 4 it saves two dispatches and two pass boundaries. The
    /// builder reads the same clamped setting the traversal does, so raising the slider refills
    /// the new levels on the very next Prepass, before any ray can reach them. This member is
    /// the belt to that braces: SSRTCB::MaxMips is clamped to it, so no traversal can climb into
    /// a level that has not been written even if the setting were to move between Prepass and a
    /// draw. Zero until the first pyramid build, which makes the traversal mip-0-only rather
    /// than letting it read the far-plane clear.
    uint hiZTopMipBuilt = 0;

    static const uint sharcNumEntries = 0x100000;

    std::array<winrt::com_ptr<ID3D11ShaderResourceView>, maxMips> depthSRVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, maxMips> depthUAVs = { nullptr };

    winrt::com_ptr<ID3D11SamplerState> linearSampler = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> preprocessDepthCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> raymarchSpecularCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> raymarchDiffuseCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> prepareColorCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> depthDownsampleCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> diffuseCompositeCS = nullptr;
    /// @brief (batch 1, item 1) ssrt_preblur.hlsl. Nullptr if it failed to compile, in which
    /// case the diffuse chain runs exactly as it did before the pass existed -- including
    /// handing the temporal pass the real FireflyClampSigma back.
    winrt::com_ptr<ID3D11ComputeShader> preblurCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> temporalCS = nullptr;
    /// @brief (perf 1) ssrt_temporal.hlsl compiled with SSRT_DENOISER_DIAG, i.e. with the five
    /// diagnostic gates wired to their constant-buffer fields instead of folded to zero.
    ///
    /// The production permutation above cannot honour any of the five switches -- fxc has removed
    /// the branches -- so AnyDenoiserDiagnostic() decides which one is dispatched, and the answer
    /// is this one only while a switch is actually set. See the derivation block above the macro
    /// definitions in ssrt_temporal.hlsl: the generated code of this permutation is byte-identical
    /// to the single shader that preceded the split, so no switch changes behaviour.
    ///
    /// Nullptr if it failed to compile, in which case the switches silently read as off rather
    /// than the whole denoiser failing -- the production permutation is the one gameplay uses.
    winrt::com_ptr<ID3D11ComputeShader> temporalDiagCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> varianceCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> spatialCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> spatialSpecularCS = nullptr;
    /// @brief (batch C1) REBLUR front-end pack (per-chain permutation: the diffuse one
    /// decodes the R8 reciprocal hit-distance surface, the specular one reads the R32
    /// world-space one plus G-buffer roughness) and the shared back-end unpack.
    /// Any of them failing to compile makes ReblurReady() false for its chain, so the
    /// failure degrades to "no denoising" rather than to a bad dispatch.
    winrt::com_ptr<ID3D11ComputeShader> nrdPackDiffuseCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> nrdPackSpecularCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> nrdUnpackCS = nullptr;
#ifdef ENABLE_SHARC
    winrt::com_ptr<ID3D11ComputeShader> raymarchDiffuseSharcCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> sharcUpdateRaymarchCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> sharcResolveCS = nullptr;
#endif
};
