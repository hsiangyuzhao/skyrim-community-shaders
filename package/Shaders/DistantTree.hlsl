#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

struct VS_INPUT
{
	float3 Position : POSITION0;
	float2 TexCoord0 : TEXCOORD0;
	float4 InstanceData1 : TEXCOORD4;
	float4 InstanceData2 : TEXCOORD5;
	float4 InstanceData3 : TEXCOORD6;
	float4 InstanceData4 : TEXCOORD7;
#if defined(VR)
	uint InstanceID : SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
	float3 TexCoord : TEXCOORD0;

#if defined(RENDER_DEPTH)
	float4 Depth : TEXCOORD3;
#else
	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
#endif  // RENDER_DEPTH
	float4 ViewPosition : POSITION3;
#if defined(DYNAMIC_SNOW) && !defined(RENDER_DEPTH)
	// (batch 39c) 0 at the billboard's base, 1 at its top: drives the LOD-tree snow tint.
	float SnowHeight : TEXCOORD1;
#endif

#if defined(VR)
	float ClipDistance : SV_ClipDistance0;  // o11
	float CullDistance : SV_CullDistance0;  // p11
	uint EyeIndex : EYEIDX0;
#endif  // VR
};

#ifdef VSHADER
cbuffer PerTechnique : register(b0)
{
	float4 FogParam : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	row_major float4x4 WorldViewProj[1] : packoffset(c0);
	row_major float4x4 World[1] : packoffset(c4);
	row_major float4x4 PreviousWorld[1] : packoffset(c8);
#	else
	row_major float4x4 WorldViewProj[2] : packoffset(c0);
	row_major float4x4 World[2] : packoffset(c8);
	row_major float4x4 PreviousWorld[2] : packoffset(c16);
#	endif  // !VR
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;
	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif  // VR
	);

	float3 scaledModelPosition = input.InstanceData1.www * input.Position.xyz;
	float3 adjustedModelPosition = 0.0.xxx;
	adjustedModelPosition.x = dot(float2(1, -1) * input.InstanceData2.xy, scaledModelPosition.xy);
	adjustedModelPosition.y = dot(input.InstanceData2.yx, scaledModelPosition.xy);
	adjustedModelPosition.z = scaledModelPosition.z;
	float4 finalModelPosition = float4(input.InstanceData1.xyz + adjustedModelPosition.xyz, 1.0);
	float4 viewPosition = mul(WorldViewProj[eyeIndex], finalModelPosition);

#	ifdef RENDER_DEPTH
	vsout.Depth.xy = viewPosition.zw;
	vsout.Depth.zw = input.InstanceData2.zw;
#	else
	vsout.WorldPosition = mul(World[eyeIndex], finalModelPosition);
	vsout.PreviousWorldPosition = mul(PreviousWorld[eyeIndex], finalModelPosition);
	vsout.ViewPosition = viewPosition;
#	endif  // RENDER_DEPTH

	vsout.Position = viewPosition;
	vsout.TexCoord = float3(input.TexCoord0.xy, FogParam.z);
#	if defined(DYNAMIC_SNOW) && !defined(RENDER_DEPTH)
	// Billboard vertices sit at the base (z <= 0) or at the top: a 0/1 per vertex that the
	// rasteriser turns into the height fraction.
	vsout.SnowHeight = saturate(input.Position.z * 1000.0);
#	endif

#	ifdef VR
	vsout.EyeIndex = eyeIndex;
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.Position, eyeIndex);
	vsout.Position = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#	endif  // VR

	return vsout;
}
#endif  // VSHADER

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;

#if !defined(RENDER_DEPTH)
#	if defined(DEFERRED)
	float2 MotionVector : SV_Target1;
	float4 Normal : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Masks : SV_Target6;
#	endif  // DEFERRED
#endif      // !RENDER_DEPTH
};

#ifdef PSHADER
SamplerState SampDiffuse : register(s0);

SamplerState SampShadowMaskSampler : register(s14);

Texture2D<float4> TexDiffuse : register(t0);

#	if !defined(VR)
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif  // !VR

cbuffer PerFrame : register(b12)
{
	float4 UnknownPerFrame1[12] : packoffset(c0);
	row_major float4x4 ScreenProj : packoffset(c12);
	row_major float4x4 PreviousScreenProj : packoffset(c16);
}

cbuffer PerTechnique : register(b0)
{
	float4 DiffuseColor : packoffset(c0);
	float4 AmbientColor : packoffset(c1);
};

const static float DepthOffsets[16] = {
	0.003921568,
	0.533333361,
	0.133333340,
	0.666666687,
	0.800000000,
	0.266666681,
	0.933333337,
	0.400000000,
	0.200000000,
	0.733333349,
	0.066666670,
	0.600000000,
	0.996078432,
	0.466666669,
	0.866666675,
	0.333333343
};

#	if defined(SCREEN_SPACE_SHADOWS)
#		include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	endif

#	if defined(TERRAIN_SHADOWS)
#		include "TerrainShadows/TerrainShadows.hlsli"
#	endif

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

// (F10) This file used to pass a literal 1.0 as GetIBLColor's skylighting weight, i.e. every
// distant tree in the game claimed unobstructed sky whatever it stood under. Sampling the probe
// needs the array, so the include is new here; t50/t51 are bound once per frame by
// Skylighting::Prepass, which Deferred::Prepasses runs before any object pass, so this shader
// reads the same live binding Lighting.hlsl and RunGrass.hlsl already do.
#	if defined(SKYLIGHTING)
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if defined(PHYSICAL_SKY)
#		include "PhysicalSky/Common.hlsli"
#	endif

#	if defined(DYNAMIC_SNOW)
#		include "DynamicSnow/DynamicSnow.hlsli"
#	endif

#	define LinearSampler SampDiffuse

#	include "Common/ShadowSampling.hlsli"

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if !defined(VR)
	uint eyeIndex = 0;
#	else
	uint eyeIndex = input.EyeIndex;
#	endif  // !VR

#	if defined(RENDER_DEPTH)
	uint2 temp = uint2(input.Position.xy);
	uint index = ((temp.x << 2) & 12) | (temp.y & 3);

	// (batch 39, item 3) Per-frame noise instead of the fixed 4x4 pattern when Temporal LOD dither is on.
	float depthOffset = 0.5 - SharedData::LODStippleThreshold(temp, DepthOffsets[index], SharedData::FrameCount);
	float depthModifier = (input.Depth.w * depthOffset) + input.Depth.z - 0.5;

	if (depthModifier < 0) {
		discard;
	}

	float alpha = TexDiffuse.SampleBias(SampDiffuse, input.TexCoord.xy, SharedData::MipBias).w;

	if ((alpha - AlphaTestRefRS) < 0) {
		discard;
	}

	psout.Diffuse.xyz = input.Depth.xxx / input.Depth.yyy;
	psout.Diffuse.w = 0;
#	else
	float4 baseColor = TexDiffuse.SampleBias(SampDiffuse, input.TexCoord.xy, SharedData::MipBias);
	baseColor.xyz = Color::Diffuse(baseColor.xyz);

	if ((baseColor.w - AlphaTestRefRS) < 0) {
		discard;
	}

#		if defined(DYNAMIC_SNOW)
	// (batch 39c) Snow on distant trees, matching the near trees' average cover (DynamicSnow::
	// GetLodTreeCoverage). Only the colour changes; the alpha test above used the original alpha.
	[branch] if (SharedData::dynamicSnowSettings.Flags & DynamicSnow::FlagLodTrees)
		baseColor.xyz = lerp(baseColor.xyz, Color::ColorToLinear(SharedData::dynamicSnowSettings.SnowColor), DynamicSnow::GetLodTreeCoverage(input.SnowHeight, input.TexCoord.xy));
#		endif

#		if defined(DEFERRED)
	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

	float dirShadow = 1;

#			if defined(SCREEN_SPACE_SHADOWS)
	dirShadow = lerp(1.0, ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, screenUV, screenNoise, eyeIndex), 0.8);
#			endif

	if (dirShadow != 0.0)
		dirShadow *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

	float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	float3 diffuseColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * dirShadow * 0.5 * llDirLightMult * Color::VanillaDiffuseMult();

#			if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		diffuseColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
#			endif

#			if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		diffuseColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
#			endif

#			if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		diffuseColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
#			endif

	float3 ddx = ddx_coarse(input.WorldPosition.xyz);
	float3 ddy = ddy_coarse(input.WorldPosition.xyz);
	float3 normal = -normalize(cross(ddx, ddy));

	float3 directionalAmbientColor = max(0, Color::Ambient(mul(SharedData::DirectionalAmbient, float4(normal, 1.0))));
#			if defined(SKYLIGHTING)
	// (F10) The real probe visibility, replacing the literal 1.0 this call used to pass. Same
	// chain and same order as Lighting.hlsl:3233-3237 -- cosine-lobe product integral, saturate,
	// fade towards 1 outside the probe volume, then the MinDiffuseVisibility floor. sampleNoBias
	// rather than sample: the normal here is a ddx/ddy plane normal for a billboard rather than a
	// real surface normal, so a receiver bias along it would push the lookup off the tree, and
	// this shader has no blue-noise texture bound to jitter with.
	float treeSkylighting = SharedData::skylightingSettings.MinDiffuseVisibility;
	{
#				if defined(VR)
		float3 positionMSSkylight = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#				else
		float3 positionMSSkylight = input.WorldPosition.xyz;
#				endif
		sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, positionMSSkylight);
		float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(normal)) / Math::PI;
		skylightingDiffuse = saturate(skylightingDiffuse);
		skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(input.WorldPosition.xyz));
		treeSkylighting = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);
	}
#			endif
#			if defined(IBL)
	float3 iblColor = 0;
	if (SharedData::iblSettings.EnableDiffuseIBL) {
		directionalAmbientColor = Color::IrradianceToLinear(directionalAmbientColor);
		directionalAmbientColor *= SharedData::iblSettings.DALCAmount;
#					if defined(SKYLIGHTING)
		iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-normal, treeSkylighting), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#					else
		iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-normal), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#					endif
		directionalAmbientColor += Color::IrradianceToGamma(iblColor);
	}
#			endif
#			if defined(SSRT) && defined(DEFERRED)
	if (SharedData::ssrtSettings.DiffuseMult > 0.0) {
		directionalAmbientColor *= SharedData::ssrtSettings.AmbientMult;
		iblColor *= SharedData::ssrtSettings.AmbientMult;
	}
#			endif
	diffuseColor += directionalAmbientColor;

	psout.Diffuse.xyz = diffuseColor * baseColor.xyz;
	psout.Diffuse.w = 1;

#			if !defined(DEFERRED) && defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		const float4 apSample = PhysSky::SampleAp(normalize(input.WorldPosition.xyz), input.Position.xy, length(input.WorldPosition.xyz), SampColorSampler);
		psout.Diffuse.xyz = psout.Diffuse.xyz * apSample.w + apSample.xyz;
	}
#			endif

	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);

	psout.Normal.xy = GBuffer::EncodeNormal(FrameBuffer::WorldToView(normal, false, eyeIndex));
	psout.Normal.zw = 0;

	psout.Albedo = float4(baseColor.xyz, 1);
	// Masks.z carries the ambient luminance that the deferred composite subtracts back out of
	// MAIN (see DeferredCompositeCS.hlsl). It must match the ambient contribution actually
	// written into psout.Diffuse, i.e. directionalAmbientColor * baseColor, in the same YCoCg
	// convention Lighting.hlsl / RunGrass.hlsl use. Hardwiring 1.0 made the composite treat the
	// entire distant-tree diffuse as ambient.
	psout.Masks = float4(0, 0, Color::RGBToYCoCg(directionalAmbientColor * baseColor.xyz).x, 0);
#		else
	float dirShadow = ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

	float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	float3 diffuseColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * dirShadow * 0.5 * llDirLightMult * Color::VanillaDiffuseMult();

#			if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		diffuseColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
#			endif

#			if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		diffuseColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
#			endif

#			if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		diffuseColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
#			endif

	float3 ddx = ddx_coarse(input.WorldPosition.xyz);
	float3 ddy = ddy_coarse(input.WorldPosition.xyz);
	float3 normal = normalize(cross(ddx, ddy));

	float3 directionalAmbientColor = Color::Ambient(mul(SharedData::DirectionalAmbient, float4(normal, 1.0)));
#			if defined(SKYLIGHTING)
	// (F10) Same fix as the branch above, which this block mirrors apart from the normal's sign.
	float treeSkylighting = SharedData::skylightingSettings.MinDiffuseVisibility;
	{
#				if defined(VR)
		float3 positionMSSkylight = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#				else
		float3 positionMSSkylight = input.WorldPosition.xyz;
#				endif
		sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, positionMSSkylight);
		float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(normal)) / Math::PI;
		skylightingDiffuse = saturate(skylightingDiffuse);
		skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(input.WorldPosition.xyz));
		treeSkylighting = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);
	}
#			endif
#			if defined(IBL)
	float3 iblColor = 0;
	if (SharedData::iblSettings.EnableDiffuseIBL) {
		directionalAmbientColor = Color::IrradianceToLinear(directionalAmbientColor);
		directionalAmbientColor *= SharedData::iblSettings.DALCAmount;
#					if defined(SKYLIGHTING)
		iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-normal, treeSkylighting), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#					else
		iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-normal), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#					endif
		directionalAmbientColor += Color::IrradianceToGamma(iblColor);
	}
#			endif
#			if defined(SSRT) && defined(DEFERRED)
	if (SharedData::ssrtSettings.DiffuseMult > 0.0) {
		directionalAmbientColor *= SharedData::ssrtSettings.AmbientMult;
		iblColor *= SharedData::ssrtSettings.AmbientMult;
	}
#			endif
	diffuseColor += directionalAmbientColor;

	float3 color = diffuseColor * baseColor.xyz;
	psout.Diffuse = float4(color, 1.0);
#		endif  // DEFERRED
#	endif      // RENDER_DEPTH

	return psout;
}
#endif  // PSHADER
