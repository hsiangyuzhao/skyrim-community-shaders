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
        uint MaxSteps = 128;
        uint MaxMips = 6;
        float Thickness = 5.f;
        float NormalBias = 0.1f;
        float BRDFBias = 0.25f;
        bool UseDynamicCubemapsAsFallback = true;
        bool UseDynamicCubemapsAsFallbackSpecular = true;
        uint DiffuseSPP = 2;
        bool EnableDiffuse = true;
        float SpecularMult = 1.0f;
        float DiffuseMult = 1.0f;
        float AmbientMult = 0.0f;
        float OcclusionStrength = 1.0f;
        float CubemapNormalization = 0.0f;
        bool EnableSVGF = false;
        uint MaxAccumulatedFrames = 16;
        /// @brief (spec A2) 2, not 3: with variance guidance repaired (audit #11) and the
        /// depth weight actually discriminating (audit #12), two guided iterations resolve
        /// more than three unguided ones did. The UI range is unchanged.
        uint AtrousIterations = 2;
        float ColorPhi = 0.5f;
        float NormalPhi = 512.0f;
        /// @brief (spec A1) Let a fully converged 8x8 tile skip an a-trous iteration.
        bool AdaptiveFiltering = true;
        /// @brief (spec A1) Accumulated frames a pixel needs before it may count as
        /// converged. The default matches MaxAccumulatedFrames: at that point the
        /// temporal blend weight has reached its floor, so the pixel is in steady state.
        uint AdaptiveHistoryThreshold = 16;
        /// @brief (spec A1) Variance below which a pixel counts as converged.
        ///
        /// The quantity in the .w channel of the denoiser ping-pong is a variance of
        /// scene luminance (ssrt_temporal.hlsl: moment.y - moment.x^2 over
        /// Color::RGBToLuminance of the linear radiance), so it has units of luminance
        /// squared and is resolution independent. 1e-4 is a standard deviation of 0.01,
        /// i.e. 1% of a mid-grey surface -- below the point where the a-trous kernel's
        /// own luminance term (phiLuminance = ColorPhi * sqrt(variance)) can still
        /// distinguish signal from noise. Deliberately conservative: raising it trades
        /// residual noise for more skipped tiles.
        float AdaptiveVarianceEps = 1e-4f;
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
        /// at r = 0.05 with the default ColorPhi 0.5 / NormalPhi 512 that leaves
        /// phiNormal = 10240 -- a tap must match the centre normal to within 0.81 degrees
        /// to keep 1/e of its weight -- and phiLuminance = 0.025 * sigma, so a tap must
        /// also match the centre luminance to within 2.5% of a standard deviation. Under
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
