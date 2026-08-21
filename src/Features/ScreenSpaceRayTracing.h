#pragma once
#define ENABLE_SHARC

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
                "Spatiotemporal Variance-Guided Filtering (SVGF) denoiser"
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

    struct Settings
    {
        bool EnableSpecular = true;
        /// @brief (spec F5) Reviewed against the repaired traversal, left at 128.
        /// The render-resolution grid unification means a step is now a whole render
        /// texel instead of s ~= 0.667 of one, so the same 128 steps reach ~1.5x further
        /// under DLSS Quality than they did -- the budget was previously being spent
        /// re-testing texels, not travelling. Lowering it is a performance question and
        /// belongs to the performance work, not here.
        uint MaxSteps = 128;
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
        /// @brief (reinjection contact occlusion) Shape the ambient the reinjection keeps with
        /// Environment Ambient's centimetre-scale contact-occlusion kernel.
        ///
        /// Default on, because it restores a term the reinjection path lost rather than adding a
        /// new one. The legacy model's cubemap fallback multiplied its ambient by
        /// MultiBounceAO(albedo, occlusion * ssgiAo) inside the ray march, and that `occlusion` was
        /// per-ray, full-resolution self-intersection: a contact-scale signal. Reinjection replaces
        /// the fallback with the vanilla ambient, which is shaped by SSGI's half-resolution AO and
        /// nothing finer, and selects it with a confidence that has been through a 7x7 spatial mean
        /// -- so the near-field contrast in the unlit parts of the frame simply is not there any
        /// more. This puts it back, on the term that is the fallback's structural counterpart.
        ///
        /// Requires the Environment Ambient feature to be *installed*, not enabled: the kernel
        /// lives in that feature's shader folder, so DeferredCompositeCS gates the whole term on
        /// ENV_AMBIENT and an SSRT-only install keeps the previous behaviour. It does not read that
        /// feature's Enabled flag, and the radius/strength it does read are plain struct defaults
        /// that stay valid while the feature is off.
        bool EnableReinjectionContactOcclusion = true;
        /// @brief (reinjection contact occlusion) How much of the kernel's occlusion to apply.
        ///
        /// 1 applies it exactly as Environment Ambient's own Contact Radius / Contact Strength
        /// sliders define it, which is what makes the two consumers of the shared kernel agree; 0
        /// is off; above 1 deepens it past what the kernel reports (`1 - (1 - ao) * s`, clamped).
        /// A separate dial from the L1-side strength on purpose -- the reinjected frame has a
        /// different amount of ambient left to darken than the L1 frame does, so the useful level
        /// is not necessarily the same.
        float ReinjectionContactStrength = 1.0f;
        /// @brief (spec F5) Reviewed against the corrected occlusion semantics, left at
        /// 1.0. It now scales occlusion that comes only from back-face hits -- the one
        /// case where the ray demonstrably entered geometry -- instead of also scaling
        /// the self-intersection failures that used to multiply the cubemap fallback to
        /// black (audit #5). Full strength on a source that is now meaningful is the
        /// right default, and it keeps as much of the contact darkening as the corrected
        /// mechanism can supply.
        float OcclusionStrength = 1.0f;
        float CubemapNormalization = 0.0f;
        bool EnableSVGF = false;
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
        float NormalPhi = 512.0f;
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
        bool FreezeNoisePhase = false;
#ifdef ENABLE_SHARC
        bool EnableSharc = false;
#endif
    } settings;

    /// @brief Mirrored by the `SSRTSettings` struct in Common/SharedData.hlsli, which lives
    /// inside the shared FeatureData constant buffer with ExponentialHeightFogSettings behind
    /// it -- so sizeof is load bearing and must stay a multiple of 16. AmbientReinjection and
    /// its strength opened a second float4 row; the contact-occlusion pair took the two slots
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
        /// @brief (reinjection contact occlusion) Already ANDed with AmbientReinjection by
        /// GetCommonBufferData, so the shader's own gate is belt and braces.
        uint ReinjectionContactOcclusion;
        float ReinjectionContactStrength;
    };
    static_assert(sizeof(SharedData) == 32,
        "ScreenSpaceRayTracing::SharedData must stay 32 bytes (two constant buffer rows); "
        "ExponentialHeightFogSettings and EnvAmbientSettings sit behind it in FeatureData.");

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
        float pad0[3];
    };

    /// @brief Mirrored by the `DenoiserCB` declaration in ssrt_spatial.hlsl. Whole float4
    /// rows exactly, so no member straddles a 16-byte boundary and the HLSL packing rules
    /// reproduce this layout verbatim. ssrt_temporal.hlsl declares the first two rows and
    /// ssrt_variance.hlsl only the first, which is legal -- a shader may declare a prefix
    /// of a larger constant buffer.
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
        float pad1[2];
    };

    eastl::unique_ptr<ConstantBuffer> ssrtCB;
    eastl::unique_ptr<ConstantBuffer> denoiserCB;

    bool recompileFlag = false;

    /// @brief Cached once per frame in Prepass() and read by both draw passes, instead
    /// of repeating the player-cell lookup twice (audit P9).
    bool inInterior = true;

    /// @brief Dynamic-resolution extent the depth pyramid was last cleared for; a change
    /// retriggers the far-plane clear of every mip (audit #8).
    float2 lastDepthExtent = { 0.0f, 0.0f };

    /// @brief (guard G8) A denoiser-history clear is owed before anything reads it.
    ///
    /// Starts true: the history textures are created without initial data, so their contents
    /// are undefined until something writes them, and "undefined R16G16B16A16_FLOAT" includes
    /// every NaN and Inf bit pattern.
    bool historyClearPending = true;

    /// @brief (guard G8) Previous frame's values of the three settings that decide whether
    /// last frame wrote a history worth reading. Initialised to false so the first frame of
    /// any enabled configuration counts as a transition.
    bool lastEnableSVGF = false;
    bool lastEnableDiffuse = false;
    bool lastEnableSpecular = false;

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
    /// @brief Snapshots the normal-roughness G-buffer into texHistoryNormals, and (defect
    /// D3, when SVGF is on) mip 0 of the Hi-Z pyramid into texHistoryDepth, for next frame's
    /// SVGF temporal validation. Called exactly once per frame, by whichever of the two draw
    /// passes runs last (audit #13).
    void CopyHistoryGeometry();
    virtual void Prepass() override;

    SharedData GetCommonBufferData();

    /// @brief Builds the denoiser constant buffer from the current settings. Shared by
    /// DrawSSRTSpecular and DrawSSRTDiffuse so the two cannot drift apart as fields are
    /// added; `atrousIterations` is overwritten per a-trous iteration by both callers.
    DenoiserCB GetDenoiserCBData() const;

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
    eastl::unique_ptr<Texture2D> texSSRTDiffuseConfidence = nullptr;
    /// @brief (ambient reinjection) The same signal after ssrt_diffuse_composite.hlsl's
    /// depth-aware 7x7 spatial mean; this is what DeferredCompositeCS lerps with. A separate
    /// surface because a blur cannot run in place.
    eastl::unique_ptr<Texture2D> texSSRTDiffuseConfidenceSmooth = nullptr;
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
    eastl::unique_ptr<Texture2D> texVariance = nullptr;
    /// @brief Specular hit distance; consumed by Upscaling.cpp as the DLSS-RR guide.
    /// Was a raw `Texture2D*` from a bare `new` and leaked (audit #20).
    eastl::unique_ptr<Texture2D> texHitDistance = nullptr;
    // (audit P6 / #20) texHitPDF (was u1) and texOutput (a redundant full-screen copy
    // of texSSRColor) had no consumer anywhere and are gone.

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
    winrt::com_ptr<ID3D11ComputeShader> temporalCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> varianceCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> spatialCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> spatialSpecularCS = nullptr;
#ifdef ENABLE_SHARC
    winrt::com_ptr<ID3D11ComputeShader> raymarchDiffuseSharcCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> sharcUpdateRaymarchCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> sharcResolveCS = nullptr;
#endif
};
