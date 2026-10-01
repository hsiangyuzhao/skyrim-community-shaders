#pragma once

#include "Buffer.h"

struct ScreenSpaceGI : Feature
{
private:
	static constexpr std::string_view MOD_ID = "130375";

public:
	bool inline SupportsVR() override { return false; }

	virtual inline std::string GetName() override { return "Screen Space GI"; }
	virtual inline std::string GetShortName() override { return "ScreenSpaceGI"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return "Lighting"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		std::string desc =
			"Screen Space Global Illumination adds realistic indirect lighting and "
			"ambient occlusion to the game. This technique simulates how light "
			"bounces off surfaces to illuminate other objects naturally.";
		if (REL::Module::IsVR()) {
			desc +=
				"\n\nWarning: In VR, this feature may have visual artifacts and "
				"can have a significant performance impact due to the nature of "
				"screen space effects.";
		}
		return std::make_pair(
			desc,
			std::vector<std::string>{
				"Realistic indirect lighting",
				"Enhanced ambient occlusion",
				"Full-resolution contact shading at centimetre scale",
				"Improved visual depth and atmosphere",
				"Temporal denoising for smooth results",
				"Configurable quality and performance settings" });
	}

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
	bool ShadersOK();

	void DrawSSGI();
	void UpdateSB();

	//////////////////////////////////////////////////////////////////////////////////

	bool recompileFlag = false;
	/// @brief (P2.4 follow-up) Whether the contact AO pass is actually running this compile round.
	///
	/// settings.EnableContactAo is the request; this is the outcome. They differ only when the
	/// contact shader failed to compile, and keeping them separate is what stops one optional pass
	/// from being able to report itself as "SSGI's compute shaders failed to compile" -- see
	/// CompileComputeShaders and ShadersOK. It is also the flag the *shared* CONTACT_AO define was
	/// built from, so it, not the setting, is what the dispatch and the upsample binding must test.
	bool contactAoActive = false;
	uint outputAoIdx = 0;
	uint outputIlIdx = 0;
	/// @brief (contact AO) Index of the specular GI buffer to hand to consumers.
	///
	/// Used to be outputAoIdx as well, which stopped being true once the contact pass acquired its
	/// own composite step in full-resolution mode: that step ping-pongs the AO channel one more
	/// time than the specular channel, so the two indices diverge there.
	uint outputSpecularIdx = 0;

	struct Settings
	{
		bool Enabled = REL::Module::IsVR() ? false : true;   // disabled in VR by default
		bool EnableGI = REL::Module::IsVR() ? false : true;  // AO only for VR by default
		bool EnableExperimentalSpecularGI = false;
		// performance/quality
		uint NumSlices = REL::Module::IsVR() ? 1u : 4u;  // AO preset for VR
		uint NumSteps = REL::Module::IsVR() ? 6u : 8u;   // AO preset for VR
		int ResolutionMode = 1;                          // 0-full, 1-half, 2-quarter - DBF default
		// visual
		float MinScreenRadius = 0.01f;
		float AORadius = 256.f;
		float GIRadius = 256.f;
		// Fraction of view depth, not world units: gi.cs.hlsl scales it by viewspaceZ so the
		// occluder thickness is a constant angular size. 0.1 matches upstream's default and is
		// the old 32-unit default at ~320 units of depth.
		float Thickness = 0.1f;
		float2 DepthFadeRange = { 4e4, 5e4 };
		// gi
		float GISaturation = 0.8f;
		float GIDistanceCompensation = 0.f;
		// mix
		float AOPower = 1.0f;
		float GIStrength = 1.0f;
		// denoise
		bool EnableTemporalDenoiser = true;
		bool EnableBlur = true;
		float DepthDisocclusion = .1f;
		float NormalDisocclusion = .1f;
		uint MaxAccumFrames = 16;
		// Separate, shorter ceiling for the AO channel only. AO is multiplicative and gets no
		// spatial filtering of its own, so a long temporal window drags a moving object's
		// occlusion into a dark trail. See the AO lerp in gi.cs.hlsl.
		uint MaxAccumFramesAO = 4;
		float BlurRadius = 2.f;
		float DistanceNormalisation = 2.f;
		// contact AO -- a separate, full-resolution near-field pass with its own accumulator.
		// See features/Screen Space GI/Shaders/ScreenSpaceGI/contactAo.cs.hlsl.
		bool EnableContactAo = true;
		float ContactRadius = 15.f;  // centimetres
		float ContactStrength = 1.f;
		// (directional env) Composite-side channel: replace the flat ambient chroma with a
		// dynamic-cubemap sample along the bent normal. Consumed by DeferredCompositeCS via the
		// shared FeatureData block (see GetCommonBufferData), not by SSGI's own passes -- the
		// bent normal itself is always produced while SSGI runs.
		bool EnableDirectionalEnv = true;
		float EnvLevel = 1.0f;  // 0..4, linear brightness of the new channel; 1 = parity
		// (batch 36) While Screen Space Ray Tracing's diffuse is actually lighting the frame,
		// DeferredCompositeCS discards this feature's indirect light (and its directional
		// environment channel) and keeps only the AO. With this on, the IL is then not computed
		// either: the GI and radiance-reprojection passes run their AO-only permutations, and the
		// radiance prefilter (plus its copy) and the IL blur are not dispatched. The AO -- contact
		// term included -- is produced by the same code as before, so the picture does not change.
		// The moment SSRT diffuse stops lighting the frame (switched off, or its chain cannot
		// run) the full path comes back with its temporal history reset. Off = batch 34.
		bool SkipILUnderSSRTDiffuse = true;
	} settings;

	// (directional env) Mirror of SSGISettings in Common/SharedData.hlsli -- appended at the
	// END of the FeatureData cbuffer, so no existing offset moves. EnableDirectionalEnv is
	// pre-gated here on loaded && Enabled, so the shader-side test needs no knowledge of the
	// feature's runtime state.
	struct alignas(16) SSGISharedData
	{
		uint EnableDirectionalEnv;
		float EnvLevel;
		float pad[2];
	};
	STATIC_ASSERT_ALIGNAS_16(SSGISharedData);

	SSGISharedData GetCommonBufferData();

	struct alignas(16) SSGICB
	{
		float4x4 PrevInvViewMat[2];
		float2 NDCToViewMul[2];
		float2 NDCToViewAdd[2];

		float2 TexDim;
		float2 RcpTexDim;  //
		float2 FrameDim;
		float2 RcpFrameDim;  //
		uint FrameIndex;

		uint NumSlices;
		uint NumSteps;

		float MinScreenRadius;  //
		float AORadius;
		float GIRadius;
		float EffectRadius;
		float Thickness;  //
		float2 DepthFadeRange;
		float DepthFadeScaleConst;

		float GISaturation;  //
		float GIDistanceCompensation;
		float GICompensationMaxDist;
		// (contact AO) Took this buffer's two spare slots, so the layout is unchanged.
		float ContactRadius;

		float AOPower;  //
		float GIStrength;

		float DepthDisocclusion;
		float NormalDisocclusion;
		uint MaxAccumFrames;  //

		uint MaxAccumFramesAO;
		float BlurRadius;
		float DistanceNormalisation;

		float ContactStrength;

		// --- new row ---
		/// @brief (S4.13) FrameDim as of the frame that wrote the history textures.
		///
		/// Every reprojection in this feature reads a history texture that is TexDim-sized with
		/// the render extent at its origin, so a previous-frame screen position has to be scaled
		/// by the *previous* frame's render extent to land on the texel that was written. The
		/// passes were using FrameDim -- this frame's extent -- which is exact only while the
		/// dynamic-resolution ratio holds still. When it moves, every history fetch lands a
		/// fraction of the sub-rect away from where it should, in proportion to the ratio change.
		///
		/// Recorded here rather than derived, because a ratio is not recoverable after the fact.
		/// Filled at the end of UpdateSB from the value that call published, so it describes the
		/// frame whose output the history now holds.
		float2 PrevFrameDim;
		/// @brief Explicit, because a bare float2 tail would leave sizeof at 296 -- and HLSL
		/// would pad the row anyway, so the padding may as well be visible on both sides.
		float2 ssgiPad0;
	};
	STATIC_ASSERT_ALIGNAS_16(SSGICB);
	eastl::unique_ptr<ConstantBuffer> ssgiCB;

	/// @brief (S4.13) The dynamic-resolution render extent UpdateSB published last frame, i.e.
	/// the extent the history textures were written at. Zero until the first UpdateSB, which
	/// the consumers read as "no usable previous extent" and fall back on this frame's.
	float2 prevFrameDim{};

	eastl::unique_ptr<Texture2D> texNoise = nullptr;
	eastl::unique_ptr<Texture2D> texWorkingDepth = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavWorkingDepth[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texPrevGeo = nullptr;
	eastl::unique_ptr<Texture2D> texRadiance = nullptr;
	eastl::unique_ptr<Texture2D> texRadianceTemp = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavRadiance[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texAccumFrames[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texAo[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlY[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlCoCg[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texGiSpecular[2] = { nullptr };
	// (contact AO) Full-resolution ping-pong for the accumulated contact visibility. R8_UNORM, so
	// the signal is format-bounded to [0, 1] and nothing downstream needs a finiteness test.
	eastl::unique_ptr<Texture2D> texContactAo[2] = { nullptr };
	// (directional env) Bent normal + aperture ping-pong, R8G8B8A8_UNORM (octahedral RG,
	// aperture B, spare A -- see SSGI_EncodeBentNormal in the feature's common.hlsli). Rides the
	// IL chain's index through radianceDisocc -> gi -> blur -> upsample, so like texIlY the pair
	// holds working-res history in one slot and, in half/quarter modes, the full-res upsample in
	// the other.
	eastl::unique_ptr<Texture2D> texBentNormal[2] = { nullptr };
	// (directional env v2) Hemisphere environment irradiance ping-pong, R16G16B16A16_FLOAT:
	// RGB = linear irradiance integrated over the unoccluded bins (premultiplied), A = the
	// march's coverage/confidence. Radiance data, so it rides the IL chain's index exactly like
	// texIlCoCg -- reprojection, blur and upsample all filter it with the IL weights, never in
	// a vector domain.
	eastl::unique_ptr<Texture2D> texEnvIrradiance[2] = { nullptr };

	inline auto GetOutputTextures()
	{
		return (loaded && settings.Enabled) ?
		           std::make_tuple(
					   texAo[outputAoIdx]->srv.get(),
					   texIlY[outputIlIdx]->srv.get(),
					   texIlCoCg[outputIlIdx]->srv.get(),
					   texGiSpecular[outputSpecularIdx]->srv.get(),
					   texBentNormal[outputIlIdx]->srv.get(),
					   texEnvIrradiance[outputIlIdx]->srv.get()) :
		           std::make_tuple(
					   (ID3D11ShaderResourceView*)nullptr, (ID3D11ShaderResourceView*)nullptr,
					   (ID3D11ShaderResourceView*)nullptr, (ID3D11ShaderResourceView*)nullptr,
					   (ID3D11ShaderResourceView*)nullptr, (ID3D11ShaderResourceView*)nullptr);
	}

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterRadianceCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> radianceDisoccCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> upsampleCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> contactAoCompute = nullptr;

	// (batch 36) AO-only permutations of the two passes whose IL work can be dropped while SSRT
	// diffuse supplies the indirect light: the same define set as the pair above minus GI and
	// GI_SPECULAR, i.e. exactly what the "Indirect Lighting (IL)" checkbox off would compile.
	// Built only while EnableGI is on (with it off the pair above already is this pair).
	winrt::com_ptr<ID3D11ComputeShader> radianceDisoccAoOnlyCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giAoOnlyCompute = nullptr;

	/// @brief (batch 36) Whether this frame runs the AO-only path. See Settings::SkipILUnderSSRTDiffuse.
	[[nodiscard]] bool ShouldSkipIL() const;
	/// @brief (batch 36) Last frame's ShouldSkipIL(), for the history reset on the way back.
	bool lastFrameSkippedIL = false;
};