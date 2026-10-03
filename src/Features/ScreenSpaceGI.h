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
				"Fine contact shadows where hair, cloth and small objects touch",
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
	} settings;

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

	inline auto GetOutputTextures()
	{
		return (loaded && settings.Enabled) ?
		           std::make_tuple(
					   texAo[outputAoIdx]->srv.get(),
					   texIlY[outputIlIdx]->srv.get(),
					   texIlCoCg[outputIlIdx]->srv.get(),
					   texGiSpecular[outputSpecularIdx]->srv.get()) :
		           std::make_tuple(nullptr, nullptr, nullptr, nullptr);
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
};