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

	/// @brief (spec C1) Reconciles the half-resolution texture set with the current
	/// DiffuseHalfRes setting, and resets the diffuse history whenever the path changes.
	/// Called at the top of DrawSSRTDiffuse, before any dispatch, so the resolution a
	/// frame runs at is fixed before its first binding.
	void UpdateHalfResResources();
	/// @brief Allocates and clears the six half-resolution diffuse textures. Idempotent.
	void CreateHalfResResources();
	/// @brief Drops the half-resolution textures. The D3D11 runtime keeps the underlying
	/// resources alive until the command buffers referencing them retire, so this is safe
	/// to call between frames.
	void ReleaseHalfResResources();
	/// @brief Zeroes the diffuse history and its moments on both resolution paths. A zero
	/// accumulated-frame count makes ssrt_temporal.hlsl blend with alpha 1, i.e. snap to
	/// the current frame, which is what a resolution switch requires.
	void ResetDiffuseHistory();

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
        /// @brief (spec C1) Run the diffuse raymarch and its whole SVGF chain at half
        /// resolution, then upsample to full resolution with a depth/normal-aware
        /// bilateral filter. Specular and the composite are untouched. Off by default:
        /// the current look stays the default look.
        bool DiffuseHalfRes = false;
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

    /// @brief Mirrored by the `DenoiserCB` declaration in ssrt_spatial.hlsl. Two float4
    /// rows exactly, so no member straddles a 16-byte boundary and the HLSL packing rules
    /// reproduce this layout verbatim. ssrt_temporal.hlsl and ssrt_variance.hlsl declare
    /// only the first row, which is legal -- a shader may declare a prefix of a larger
    /// constant buffer.
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
        uint pad0;
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

    // (spec C1) Half-resolution mirror of the diffuse chain, allocated only while
    // DiffuseHalfRes is on. Dimensions are exactly mip 1 of texDepth, which is the depth
    // source the half-resolution passes read, so the two line up texel for texel.
    eastl::unique_ptr<Texture2D> texSSRTDiffuseColorHalf = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryDiffuseHalf = nullptr;
    eastl::unique_ptr<Texture2D> texTemporalHalf = nullptr;
    eastl::unique_ptr<Texture2D> texVarianceHalf = nullptr;
    eastl::unique_ptr<Texture2D> texMomentsHalf = nullptr;
    eastl::unique_ptr<Texture2D> texHistoryMomentsDiffuseHalf = nullptr;

    /// @brief Whether the half-resolution set is currently allocated and in use. Compared
    /// against the effective setting each frame to detect a toggle.
    bool halfResActive = false;

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
    // (spec C1) SSRT_HALF_RES permutations of the diffuse chain, plus the upsample. Always
    // compiled, so toggling DiffuseHalfRes never triggers a recompile.
    winrt::com_ptr<ID3D11ComputeShader> raymarchDiffuseHalfResCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> temporalHalfResCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> varianceHalfResCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> spatialHalfResCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> diffuseUpsampleCS = nullptr;
#ifdef ENABLE_SHARC
    winrt::com_ptr<ID3D11ComputeShader> raymarchDiffuseSharcCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> sharcUpdateRaymarchCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> sharcResolveCS = nullptr;
#endif
};
