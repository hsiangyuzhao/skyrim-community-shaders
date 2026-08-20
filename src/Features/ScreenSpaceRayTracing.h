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
        float AmbientMult = 0.0f;
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
#ifdef ENABLE_SHARC
        bool EnableSharc = false;
#endif
    } settings;

    struct alignas(16) SharedData
    {
        uint EnableSpecular;
        float SpecularMult;
        float DiffuseMult;
        float AmbientMult;
    };

    struct alignas(16) SSRTCB
    {
        uint MaxSteps;
        uint MaxMips;
        uint UseDynamicCubemapsAsFallback;
        float Thickness;
        float NormalBias;
        float BRDFBias;
        float OcclusionStrength;
        float CubemapNormalization;
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
        float pad1[3];
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

    void DrawSSRTSpecular();
    void DrawSSRTDiffuse();
    /// @brief Snapshots the normal-roughness G-buffer into texHistoryNormals for next
    /// frame's SVGF temporal validation. Called exactly once per frame, by whichever of
    /// the two draw passes runs last (audit #13).
    void CopyHistoryNormals();
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
    eastl::unique_ptr<Texture2D> texHistory = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryDiffuse = nullptr;
    eastl::unique_ptr<Texture2D> texTemporal = nullptr;
    eastl::unique_ptr<Texture2D> texMoments = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryMoments = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryMomentsDiffuse = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryNormals = nullptr;
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
