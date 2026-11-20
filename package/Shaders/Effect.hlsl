#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/PhysicalLighting.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"
#include "Common/VR.hlsli"

#define EFFECT

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

struct VS_INPUT
{
	float4 Position : POSITION0;
#if defined(TEXCOORD)
#	if defined(STRIP_PARTICLES)
	float3
#	else
	float2
#	endif
		TexCoord0 : TEXCOORD0;
#endif
#if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float4 Normal : NORMAL0;
#endif
#if defined(BINORMAL_TANGENT)
	float4 Bitangent : BINORMAL0;
#endif
#if defined(VC)
	float4 Color : COLOR0;
#endif
#if defined(SKINNED)
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
#endif
#if defined(VR)
	uint InstanceID : SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
	float4 TexCoord0 : TEXCOORD0;
	float4 WorldPosition : POSITION1;
#if defined(VC)
	float4 Color : COLOR0;
#endif
#if !defined(MOTIONVECTORS_NORMALS)
	float4 FogParam : COLOR1;
#endif
#if defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS)
	float3 ScreenSpaceNormal : TEXCOORD1;
#elif (defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))) || (defined(PROJECTED_UV) && defined(NORMALS))
	float3 TBN0 : TEXCOORD1;
#endif
#if defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))
	float FogAlpha : TEXCOORD5;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS)) || (defined(PROJECTED_UV) && defined(NORMALS) && !defined(MEMBRANE))
	float3 TBN1 : TEXCOORD2;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS))
	float3 TBN2 : TEXCOORD3;
#endif
#if defined(MEMBRANE)
	float4 ViewVector : TEXCOORD4;
#endif
#if defined(LIGHTING)
	float3 MSPosition : TEXCOORD6;
#endif
#if !(defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS)))
	float FogAlpha : TEXCOORD5;
#endif
#if defined(MOTIONVECTORS_NORMALS)
#	if !defined(LIGHTING) && !(defined(MEMBRANE) && defined(SKINNED)) && !(defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS))
	float3 ScreenSpaceNormal : TEXCOORD7;
#	endif
	float4 PreviousWorldPosition : POSITION2;
#	if (defined(LIGHTING) || (defined(MEMBRANE) && defined(SKINNED))) && !(defined(MEMBRANE) && defined(NORMALS))
	float3 ScreenSpaceNormal : TEXCOORD7;
#	endif
#endif
#if defined(VR)
	float ClipDistance : SV_ClipDistance0;  // o11
	float CullDistance : SV_CullDistance0;  // p11
	uint EyeIndex : EYEIDX0;
#endif  // VR
};

#ifdef VSHADER
cbuffer VS_PerFrame : register(b12)
{
#	if !defined(VR)
	row_major float4x4 ScreenProj[1] : packoffset(c0);
	row_major float4x4 ViewProj[1] : packoffset(c8);
#		if defined(SKINNED)
	float3 BonesPivot[1] : packoffset(c40);
#			if defined(MOTIONVECTORS_NORMALS)
	float3 PreviousBonesPivot[1] : packoffset(c41);
#			endif  // MOTIONVECTORS_NORMALS
#		endif      // SKINNED
#	else
	row_major float4x4 ScreenProj[2] : packoffset(c0);
	row_major float4x4 ViewProj[2] : packoffset(c16);
#		if defined(SKINNED)
	float3 BonesPivot[2] : packoffset(c80);
#			if defined(MOTIONVECTORS_NORMALS)
	float3 PreviousBonesPivot[2] : packoffset(c82);
#			endif  // MOTIONVECTORS_NORMALS
#		endif      // SKINNED
#	endif          // VR
};

cbuffer PerTechnique : register(b0)
{
	float4 FogParam : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
	float4 FogFarColor : packoffset(c2);
};

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
	float4 SoftMateralVSParams : packoffset(c1);
	float4 FalloffData : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	row_major float3x4 World[1] : packoffset(c0);
	row_major float3x4 PreviousWorld[1] : packoffset(c3);
	float4 MatProj[3] : packoffset(c6);
	float4 EyePosition[1] : packoffset(c12);
	float4 PosAdjust[1] : packoffset(c13);
	float4 TexcoordOffsetMembrane : packoffset(c14);
#	else
	row_major float3x4 World[2] : packoffset(c0);
	row_major float3x4 PreviousWorld[2] : packoffset(c6);
	float4 MatProj[3] : packoffset(c12);
	float4 EyePosition[2] : packoffset(c21);
	float4 PosAdjust[2] : packoffset(c23);
	float4 TexcoordOffsetMembrane : packoffset(c25);
#	endif  // VR
}

cbuffer IndexedTexcoordBuffer : register(b11)
{
	float4 IndexedTexCoord[128] : packoffset(c0);
}

#	if defined(PROJECTED_UV)
float GetProjectedU(float3 worldPosition, float4 texCoordOffset)
{
	float projUvTmp = min(abs(worldPosition.x), abs(worldPosition.y)) *
	                  (1 / max(abs(worldPosition.x), abs(worldPosition.y)));
	float projUvTmpSqr = projUvTmp * projUvTmp;
	float projUvTmp2 =
		projUvTmpSqr *
			(projUvTmpSqr *
					(projUvTmpSqr * (projUvTmpSqr * 0.0208350997 + -0.0851330012) + 0.180141002) +
				-0.330299497) +
		0.999866009;
	float projUvTmp5;
	if (abs(worldPosition.x) > abs(worldPosition.y)) {
		projUvTmp5 = projUvTmp * projUvTmp2 * -2 + Math::HALF_PI;
	} else {
		projUvTmp5 = 0;
	}
	float projUvTmp6 = projUvTmp * projUvTmp2 + projUvTmp5;
	float projUvTmp7;
	if (worldPosition.y < -worldPosition.y) {
		projUvTmp7 = -Math::PI;
	} else {
		projUvTmp7 = 0;
	}
	float projUvTmp4 = projUvTmp6 + projUvTmp7;
	float minCoord = min(worldPosition.x, worldPosition.y);
	float maxCoord = max(worldPosition.x, worldPosition.y);
	if (minCoord < -minCoord && maxCoord >= -maxCoord) {
		projUvTmp4 = -projUvTmp4;
	}
	return abs(0.318309158 * projUvTmp4) * texCoordOffset.w + texCoordOffset.y;
}

float GetProjectedV(float3 worldPosition, uint a_eyeIndex = 0)
{
	return (-PosAdjust[a_eyeIndex].x + (PosAdjust[a_eyeIndex].z + worldPosition.z)) / PosAdjust[a_eyeIndex].y;
}
#	endif

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;
	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif  // VR
	);
	precise float4 inputPosition = float4(input.Position.xyz, 1.0);

	precise row_major float4x4 world4x4 = float4x4(World[eyeIndex][0], World[eyeIndex][1], World[eyeIndex][2], float4(0, 0, 0, 1));
	precise float3x3 world3x3 =
		transpose(float3x3(transpose(World[eyeIndex])[0], transpose(World[eyeIndex])[1], transpose(World[eyeIndex])[2]));

#	if defined(SKY_OBJECT)
	float4x4 viewProj = float4x4(ViewProj[eyeIndex][0], ViewProj[eyeIndex][1], ViewProj[eyeIndex][3], ViewProj[eyeIndex][3]);
#	else
	row_major float4x4 viewProj = ViewProj[eyeIndex];
#	endif

#	if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;
#		if defined(MOTIONVECTORS_NORMALS)
	float3x4 previousBoneTransformMatrix =
		Skinned::GetBoneTransformMatrix(PreviousBones, actualIndices, PreviousBonesPivot[eyeIndex], input.BoneWeights);
	precise float4 previousWorldPosition =
		float4(mul(inputPosition, transpose(previousBoneTransformMatrix)), 1);
#		endif
	float3x4 boneTransformMatrix =
		Skinned::GetBoneTransformMatrix(Bones, actualIndices, BonesPivot[eyeIndex], input.BoneWeights);
	precise float4 worldPosition = float4(mul(inputPosition, transpose(boneTransformMatrix)), 1);
	float4 viewPos = mul(viewProj, worldPosition);
#	else
	precise float4 worldPosition = float4(mul(World[eyeIndex], inputPosition), 1);
	precise float4 previousWorldPosition = float4(mul(PreviousWorld[eyeIndex], inputPosition), 1);
	precise row_major float4x4 modelView = mul(viewProj, world4x4);
	float4 viewPos = mul(modelView, inputPosition);
#	endif

	vsout.Position = viewPos;

#	if defined(SKINNED)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
	float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
#	endif

#	if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float3 normal = input.Normal.xyz * 2 - 1;

#		if defined(SKINNED)
	float3 worldNormal = normalize(mul(normal, boneRSMatrixTr));
#		else
	float3 worldNormal = normalize(mul(world3x3, normal));
#		endif
#	endif

#	if defined(VC)
	vsout.Color = input.Color;
#	endif

#	if !defined(MOTIONVECTORS_NORMALS)
	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(viewPos.xyz) * FogParam.y - FogParam.x))));

	vsout.FogParam.xyz = lerp(FogNearColor.xyz, FogFarColor.xyz, fogColorParam);
	vsout.FogParam.w = fogColorParam;
#	endif

	float4 texCoord = float4(0, 0, 1, 0);

#	if defined(MEMBRANE)
	float4 texCoordOffset = TexcoordOffsetMembrane;
#	else
	float4 texCoordOffset = TexcoordOffset;
#	endif

#	if defined(TEXCOORD_INDEX)
#		if defined(NORMALS)
	uint index = input.TexCoord0.z;
#		else
	uint index = input.Position.w;
#		endif
#	endif

#	if defined(PROJECTED_UV)
#		if defined(NORMALS) && !defined(MEMBRANE)
	texCoord.x = dot(MatProj[0].xyz, inputPosition.xyz);
#		else
	texCoord.x = GetProjectedU(worldPosition.xyz, texCoordOffset);
#		endif
#	else
#		if defined(TEXTURE)
	float u = input.TexCoord0.x;
#			if defined(TEXCOORD_INDEX)
	u = IndexedTexCoord[index].y * u + IndexedTexCoord[index].x;
#			endif
	texCoord.x = u * texCoordOffset.z + texCoordOffset.x;
#		endif
#	endif
#	if defined(PROJECTED_UV)
#		if defined(NORMALS) && !defined(MEMBRANE)
	texCoord.y = dot(MatProj[1].xyz, inputPosition.xyz);
#		else
	texCoord.y = GetProjectedV(worldPosition.xyz, eyeIndex);
#		endif
#	else
#		if defined(TEXTURE)
	float v = input.TexCoord0.y;
#			if defined(TEXCOORD_INDEX)
	v = IndexedTexCoord[index].w * v + IndexedTexCoord[index].z;
#			endif
	texCoord.y = v * texCoordOffset.w + texCoordOffset.y;
#		endif
#	endif
#	if defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.w = input.TexCoord0.y;
#	elif defined(SOFT)
	texCoord.w = viewPos.w / SoftMateralVSParams.x;
#	elif defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.w = input.TexCoord0.y;
#	endif
#	if defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.z = input.TexCoord0.x;
#	elif defined(FALLOFF)
	float3 inverseWorldDirection = normalize(-worldPosition.xyz);
	float WdotN = dot(worldNormal, inverseWorldDirection);
	float falloff = saturate((-FalloffData.x + abs(WdotN)) / (FalloffData.y - FalloffData.x));
	float falloffParam = (falloff * falloff) * (3 - falloff * 2);
	texCoord.z = lerp(FalloffData.z, FalloffData.w, falloffParam);
#	elif defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.z = input.TexCoord0.x;
#	endif
	vsout.TexCoord0 = texCoord;

	float3 eyePosition = 0.0.xxx;
#	if defined(MEMBRANE) && defined(TEXTURE) && !defined(SKINNED)
	eyePosition = EyePosition[eyeIndex].xyz;
#	endif

	float3 viewPosition = inputPosition.xyz;
#	if defined(SKINNED)
	viewPosition = worldPosition.xyz;
#	endif

#	if defined(MEMBRANE)
#		if defined(SKINNED)
#			if defined(NORMALS)
	vsout.TBN0.xyz = worldNormal;
#			else
	float3x3 tbnTr = float3x3(normalize(boneRSMatrixTr[0]), normalize(boneRSMatrixTr[1]),
		normalize(boneRSMatrixTr[2]));
#				if defined(MOTIONVECTORS_NORMALS)
	tbnTr[2] = worldNormal;
#				endif
	float3x3 tbn = transpose(tbnTr);
	vsout.TBN0.xyz = tbn[0];
	vsout.TBN1.xyz = tbn[1];
	vsout.TBN2.xyz = tbn[2];
#			endif
#		endif

	vsout.ViewVector.xyz = normalize(eyePosition - viewPosition);
	vsout.ViewVector.w = 1;
#	endif

#	if !defined(SKINNED) && defined(NORMALS) && !(defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS))
#		if defined(MEMBRANE)
	vsout.TBN0.xyz = normal;
#		elif defined(PROJECTED_UV)
	vsout.TBN0.xyz = input.Normal.xyz;
#		endif
#	endif

#	if defined(MOTIONVECTORS_NORMALS) && !(defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
#		if defined(SKINNED) && !defined(MEMBRANE)
	float3 screenSpaceNormal = normal;
#		elif defined(FALLOFF) || (defined(SKINNED) && defined(MEMBRANE))
	float3 screenSpaceNormal = worldNormal;
#		else
	float4x4 modelScreen = mul(ScreenProj[eyeIndex], world4x4);
	float3 screenSpaceNormal = normalize(mul(modelScreen, float4(normal, 0))).xyz;
#		endif

	vsout.ScreenSpaceNormal = screenSpaceNormal;
#	endif

#	if defined(LIGHTING)
	vsout.MSPosition = viewPosition;
#	endif

	vsout.FogAlpha.x = FogNearColor.w;

#	if defined(PROJECTED_UV) && defined(NORMALS) && !defined(MEMBRANE)
	vsout.TBN1.xyz = MatProj[2].xyz;
#	endif

	vsout.WorldPosition = worldPosition;
#	if defined(MOTIONVECTORS_NORMALS)
	vsout.PreviousWorldPosition = previousWorldPosition;
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
#endif

typedef VS_OUTPUT PS_INPUT;
SamplerState SampBaseSampler : register(s0);
SamplerState SampNormalSampler : register(s1);
SamplerState SampNoiseSampler : register(s2);
SamplerState SampDepthSampler : register(s3);
SamplerState SampGrayscaleSampler : register(s4);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexNormalSampler : register(t1);
Texture2D<float4> TexNoiseSampler : register(t2);
Texture2D<float4> TexDepthSamplerEffect : register(t3);
Texture2D<float4> TexGrayscaleSampler : register(t4);

#if defined(DEFERRED)
struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
#	if defined(MOTIONVECTORS_NORMALS)
	float4 MotionVectors : SV_Target1;
	float4 NormalGlossiness : SV_Target2;
#	elif defined(NORMALS)
	float4 NormalGlossiness : SV_Target2;
#	endif
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
	float4 Masks : SV_Target6;
};
#else
struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
#	if defined(MOTIONVECTORS_NORMALS)
	float2 MotionVectors : SV_Target1;
	float4 ScreenSpaceNormals : SV_Target2;
#	else
	float4 Normal : SV_Target1;
	float4 Color2 : SV_Target2;
#	endif
};
#endif

#ifdef PSHADER

#	if !defined(VR)
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif  // !VR

cbuffer PerTechnique : register(b0)
{
	float4 CameraDataEffect : packoffset(c0);
	float2 VPOSOffset : packoffset(c1);
	float2 FilteringParam : packoffset(c1.z);
};

cbuffer PerMaterial : register(b1)
{
	float4 BaseColor : packoffset(c0);
	float4 BaseColorScale : packoffset(c1);
	float4 LightingInfluence : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	float4 PLightPositionX[1] : packoffset(c0);
	float4 PLightPositionY[1] : packoffset(c1);
	float4 PLightPositionZ[1] : packoffset(c2);
	float4 PLightingRadiusInverseSquared : packoffset(c3);
	float4 PLightColorR : packoffset(c4);
	float4 PLightColorG : packoffset(c5);
	float4 PLightColorB : packoffset(c6);
	float4 DLightColor : packoffset(c7);
	float4 PropertyColor : packoffset(c8);
	float4 AlphaTestRef : packoffset(c9);
	float4 MembraneRimColor : packoffset(c10);
	float4 MembraneVars : packoffset(c11);
#	else
	float4 PLightPositionX[2] : packoffset(c0);
	float4 PLightPositionY[2] : packoffset(c2);
	float4 PLightPositionZ[2] : packoffset(c4);
	float4 PLightingRadiusInverseSquared : packoffset(c6);
	float4 PLightColorR : packoffset(c7);
	float4 PLightColorG : packoffset(c8);
	float4 PLightColorB : packoffset(c9);
	float4 DLightColor : packoffset(c10);
	float4 PropertyColor : packoffset(c11);  // VR should be 11; this could start earlier though
	float4 AlphaTestRef : packoffset(c12);
	float4 MembraneRimColor : packoffset(c13);
	float4 MembraneVars : packoffset(c14);
#	endif
};

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#		include "InverseSquareLighting/InverseSquareLighting.hlsli"
#	endif

#	define LinearSampler SampBaseSampler

#	if defined(TERRAIN_SHADOWS)
#		include "TerrainShadows/TerrainShadows.hlsli"
#	endif

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	if defined(SKYLIGHTING)
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		define SampColorSampler SampBaseSampler
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
// (49b) "Particles Use RT Shadows" (Ray Tracing): the volumetric fog's ray-traced froxel visibility, temporally filtered
// (ExponentialHeightFog/RTFogVisibility.hlsli; only while the near fog grid is built). Off / unbound: every factor is 1 and no light is added.
// (49g) In every effect permutation: lit smoke without the LIGHTING flag uses it too when it follows the scene light.
#		include "ExponentialHeightFog/RTFogVisibility.hlsli"
namespace RTFogVis
{
	/// Near-grid volume uvw (texel centres) of a camera-relative position; false outside the near grid or without the textures.
	bool ParticleCoords(float3 a_positionWS, uint a_eyeIndex, out float3 a_uvw)
	{
		a_uvw = 0.0.xxx;
		// The grid belongs to the main camera: not for menus or the water reflection's camera (as the volumetric fog lookup).
		if (!(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) || (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))
			return false;
		uint w, h, d;
		RTFogSun.GetDimensions(w, h, d);
		const float4 clip = mul(FrameBuffer::CameraViewProj[a_eyeIndex], float4(a_positionWS, 1.0));
		if (clip.w <= 0.0 || w == 0u || h == 0u || d == 0u)
			return false;
		float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
		if (any(uv < 0.0) || any(uv > 1.0))
			return false;
		uv = Stereo::ConvertToStereoUV(uv, a_eyeIndex);
		const float z = ExponentialHeightFog::VolumetricDepthToNormalizedSlice(max(clip.w, SharedData::CameraData.y), SharedData::volumetricFogSettings.NearGridZParams.xyz,
			SharedData::volumetricFogSettings.NearGridZParams.w);
		if (z < 0.0 || z >= 1.0)
			return false;
		const float3 size = float3(w, h, d);
		a_uvw = clamp(float3(uv, z), 0.5 / size, 1.0 - 0.5 / size);
		return true;
	}

	/// The ray-traced sun factor of a lit effect at a camera-relative position (1 = no data or off).
	float ParticleSun(float3 a_positionWS, uint a_eyeIndex)
	{
		float result = 1.0;
		float3 uvw;
		[branch] if ((Flags() & kParticleSun) && ParticleCoords(a_positionWS, a_eyeIndex, uvw))
			result = SunFactor(RTFogSun.SampleLevel(SampColorSampler, uvw, 0));
		return result;
	}
}
#	endif

#	if defined(PHYSICAL_SKY)
#		include "PhysicalSky/Common.hlsli"
#	endif

#	include "Common/ShadowSampling.hlsli"

float ComputeShadowVariance(float shadow)
{
    // Measure local gradient magnitude; classify "no variation" using a small threshold.
    const float2 grad = float2(ddx(shadow), ddy(shadow));
    const float v = abs(grad.x) + abs(grad.y) + fwidth(shadow);
    const float epsilon = 1e-4;
    return (v < epsilon) ? 1.0 : 0.0;
}

// (49g) Linear Lighting > Effects: "Effects Follow Scene Light" and the three effect multipliers (LinearLighting.cpp; bound for every
// effect draw by Hooks.cpp, unbound again after it). Zero (master off) = the old effect lighting and the old Other Effects Multiplier.
cbuffer EffectSceneCB : register(b7)
{
	uint EffectSceneFlags;        // 1 master, 2 lit smoke and mist, 4 unlit smoke and mist
	float EffectFireMagicMult;    // Fire and Magic Multiplier
	float EffectLitSmokeMult;     // Lit Smoke and Mist Multiplier
	float EffectUnlitSmokeMult;   // Unlit Smoke and Mist Multiplier
	float EffectUnlitSceneScale;  // the scene's ambient brightness now / this weather's midday, clamped (C++)
	float3 EffectScenePad;        // (49j) x: Physical Brightness self-lit scale of fire and magic (calFire x min(P, P_cap)), flag 8
};

namespace EffectScene
{
	static const uint kMaster = 1u;
	static const uint kLit = 2u;
	static const uint kUnlit = 4u;
	static const uint kBrightness = 8u;  // (49j) Physical Brightness: the self-lit part of every effect takes P (SelfLitScale)

	static const uint kFireMagic = 0u;
	static const uint kLitSmoke = 1u;
	static const uint kUnlitSmoke = 2u;

	bool Master()
	{
		return ENABLE_LL && (EffectSceneFlags & kMaster) != 0u;
	}

	/// Which of the three multipliers an effect takes: additive (fire, magic, sparks, light sprites) and unlit palette-coloured ones
	/// (grayscale to colour: flames and magic drawn with alpha) -> Fire and Magic; others lit / unlit by their Lighting Influence.
	uint Category(float a_lightingInfluence)
	{
#	if defined(ADDBLEND)
		return kFireMagic;
#	else
		if (a_lightingInfluence > 0.0)
			return kLitSmoke;
		if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor)
			return kFireMagic;
		return kUnlitSmoke;
#	endif
	}

	/// The effects whose multiplier is one of the three (membrane, blood, projected and deferred effects keep their own).
	bool OwnMultiplier()
	{
#	if defined(MEMBRANE) || defined(BLOOD) || defined(PROJECTED_UV) || defined(DEFERRED)
		return false;
#	else
		return true;
#	endif
	}

	/// (49j, design 3.2 row 12) Physical Brightness in effect: an effect's colour is split into its lit part (propertyColor: the scene's light,
	/// already x P) and its self-lit part, which takes P here (fire and magic: their calibration x min(P, P_cap); smoke and every other
	/// effect: the game-colour factor, the ambient now / the game's ambient, as the game drew them against its ambient).
	bool Brightness()
	{
		return ENABLE_LL && (EffectSceneFlags & kBrightness) != 0u;
	}
	float SelfLitScale(uint a_category)
	{
#	if defined(MULTBLEND) || defined(MULTBLEND_DECAL)
		return 1.0;  // a multiplicative blend's colour is a ratio, not a light
#	else
		if (!Brightness() || !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
			return 1.0;  // menus and item previews are not in the model (design 3.2 row 29)
		return a_category == kFireMagic ? EffectScenePad.x : EffectUnlitSceneScale;
#	endif
	}

	float Multiplier(uint a_category)
	{
		return a_category == kFireMagic ? EffectFireMagicMult : (a_category == kLitSmoke ? EffectLitSmokeMult : EffectUnlitSmokeMult);
	}

	/// In-world effects that may follow the scene light (never additive, membrane, blood, sky, projected or deferred effects).
	bool CanFollow()
	{
#	if defined(ADDBLEND) || defined(MEMBRANE) || defined(BLOOD) || defined(SKY_OBJECT) || defined(PROJECTED_UV) || defined(DEFERRED)
		return false;
#	else
		return (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) != 0u;
#	endif
	}

	/// Unlit effects scaled by the scene brightness: alpha-blended ones only (a multiplicative blend darkens, it does not shine).
	bool CanFollowUnlit()
	{
#	if defined(MULTBLEND) || defined(MULTBLEND_DECAL)
		return false;
#	else
		return CanFollow();
#	endif
	}

	/// The light a matte vanilla surface gets at this spot (Lighting.hlsl's vanilla diffuse terms, without a normal): the ambient
	/// (the directional ambient's isotropic part) with IBL and skylighting as GetLightingColor's own sun path does, plus the sun
	/// (Physical Sky transmittance, the weather fog's sun attenuation) x its shadow (Volumetric Shadows' particle shadows or the shadow
	/// map, x the ray-traced visibility of "Particles Use RT Shadows") x Vanilla Diffuse Multiplier x Effect Lighting Multiplier.
	float3 MatteLight(float3 a_positionWS, float4 a_screenPosition, uint a_eyeIndex, inout float a_shadowVariance)
	{
		float3 ambient = Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(0, 0, 0, 1))));
		float skylightingDiffuse = 1.0;
#	if defined(SKYLIGHTING)
		[branch] if (!SharedData::InInterior)
		{
#		if defined(VR)
			float3 positionMSSkylight = a_positionWS + FrameBuffer::CameraPosAdjust[a_eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		else
			float3 positionMSSkylight = a_positionWS;
#		endif
			sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, positionMSSkylight);
			skylightingDiffuse = saturate(SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(float3(0, 0, 1))) / Math::PI);
			skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(a_positionWS));
			skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);
		}
#	endif
#	if defined(IBL)
		const bool diffuseIBL = SharedData::iblSettings.EnableDiffuseIBL && (!SharedData::InInterior || SharedData::iblSettings.EnableInterior);
		if (diffuseIBL)
			ambient *= SharedData::iblSettings.DALCAmount;
#	endif
		ambient *= skylightingDiffuse;
#	if defined(IBL)
		[branch] if (diffuseIBL)
		{
#		if defined(SKYLIGHTING)
			ambient += Color::Saturation(ImageBasedLighting::GetIBLColor(float3(0, 0, 0), skylightingDiffuse), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#		else
			ambient += Color::Saturation(ImageBasedLighting::GetIBLColor(float3(0, 0, 0)), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#		endif
		}
#	endif

		// The sun as Lighting.hlsl takes it (Linear Lighting's sunlight scale outside the gamma, outdoors only).
		const float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear && !SharedData::InInterior) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
		float3 sun = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
		sun *= Color::VanillaDiffuseMult() * Color::EffectLightingMult();
#	if defined(PHYSICAL_SKY)
		if (SharedData::physSkyData.enabled)
			sun *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampDepthSampler);
#	endif
#	if defined(EXP_HEIGHT_FOG)
		sun *= WeatherFog::SunlightAttenuation(a_positionWS.z);  // 1 unless "CS Fog Takes Over" applies
#	endif
		[branch] if (!SharedData::InInterior)
		{
			bool isWorldShadow = false;
			float shadow;
#	if defined(VOLUMETRIC_SHADOWS)
			[branch] if (VolumetricShadows::ParticleShadowsEnabled())
				shadow = VolumetricShadows::GetEffectShadow(a_positionWS, normalize(a_positionWS), a_screenPosition.xy, a_eyeIndex);
			else
#	endif
				shadow = ShadowSampling::GetEffectShadow(a_positionWS, normalize(a_positionWS), a_screenPosition.xy, a_eyeIndex, isWorldShadow);
#	if defined(EXP_HEIGHT_FOG)
			shadow *= RTFogVis::ParticleSun(a_positionWS, a_eyeIndex);
#	endif
			sun *= shadow;
			if (!isWorldShadow)
				a_shadowVariance = ComputeShadowVariance(shadow);
		}
		return ambient + sun;
	}

#	if defined(LIGHT_LIMIT_FIX)
	/// Light Limit Fix's clustered point lights on an effect (49b: x the ray-traced visibility of "Particles Use RT Shadows"). Each light
	/// adds PointLight x attenuation x a_scaleA x fade x a_scaleB: the old lit-effect loop with a_scaleA = 0.5, a_scaleB = Effect Lighting
	/// Multiplier; following the scene light a_scaleA = Vanilla Diffuse Multiplier.
	void AddPointLights(inout float3 a_color, float3 a_positionWS, uint a_eyeIndex, float a_scaleA, float a_scaleB, inout uint a_lightCount)
	{
		float3 viewPosition = mul(FrameBuffer::CameraView[a_eyeIndex], float4(a_positionWS, 1)).xyz;
		float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex);
		bool inWorld = Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld;

		uint clusterIndex = 0;
		if (inWorld && LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
			a_lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
			uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
#		if defined(EXP_HEIGHT_FOG)
			// (49b) Particles Use RT Shadows: the four point-light sources traced at this spot of the fog grid. A light among them is shadowed by
			// its ray-traced visibility; a shadow-casting light (skipped before) lights the effect only when it is among them.
			bool rtLights = (RTFogVis::Flags() & RTFogVis::kParticleLights) != 0u;
			uint2 rtSlots = 0u.xx;
			[branch] if (rtLights)
			{
				float3 rtUvw;
				rtLights = RTFogVis::ParticleCoords(a_positionWS, a_eyeIndex, rtUvw);
				uint rtW, rtH, rtD;
				RTFogVis::RTFogLightId.GetDimensions(rtW, rtH, rtD);
				const int4 rtCoord = int4(min(uint3(rtUvw * float3(rtW, rtH, rtD)), uint3(rtW, rtH, rtD) - 1u), 0);
				if (rtLights)
					rtSlots = RTFogVis::PackSlots(RTFogVis::RTFogLightId.Load(rtCoord), RTFogVis::RTFogLightVis.Load(rtCoord));
			}
#		endif
			[loop] for (uint i = 0; i < a_lightCount; i++)
			{
				uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
				LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
				if (LightLimitFix::IsLightIgnored(light))
					continue;
				bool skipLight = (light.lightFlags & LightLimitFix::LightFlags::Shadow) != 0u;
				float rtLightVis = 1.0;
#		if defined(EXP_HEIGHT_FOG)
				[branch] if (rtLights)
				{
					const uint rtId = RTFogVis::LightId(clusteredLightIndex);
					bool rtFound;
					rtLightVis = RTFogVis::LightVisibility(rtSlots, rtId, rtFound);
					if (rtFound)
						skipLight = false;
				}
#		endif
				if (skipLight)
					continue;
				float3 lightDirection = light.positionWS[a_eyeIndex].xyz - a_positionWS;
				float lightDist = length(lightDirection);

#		if defined(ISL)
				float intensityMultiplier = InverseSquareLighting::GetAttenuation(lightDist, light);
#		else
				float intensityFactor = saturate(lightDist / light.radius);
				float intensityMultiplier = 1 - intensityFactor * intensityFactor;
#		endif

				const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
				float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * intensityMultiplier * a_scaleA * light.fade * a_scaleB;
				a_color += lightColor * rtLightVis;
			}
		}
	}
#	endif
}

#	if defined(LIGHTING) && (defined(VOLUMETRIC_SHADOWS) || defined(EXP_HEIGHT_FOG))  // (47d) + the sunlight in the weather's fog
// (batch 38, A2) Port of upstream ShadowSampling::ExtractLighting (0f79d567a): split the
// engine's combined effect light colour into sun and ambient by the luminance ratio of the
// sun colour and the ambient (DALC, with IBL where it replaces part of it). Returns the sun
// part; the rest is ambient.
float3 EstimateEffectDirectionalPart(float3 inputColor)
{
	float3 ambientColor = max(0, mul(SharedData::DirectionalAmbient, float4(0, 0, 1, 1)));
	if (PhysLight::On(PhysLight::kSmallFixes))  // (49h stage 1, S8) the weather ambient converted like every other ambient
		ambientColor = Color::Ambient(ambientColor);
#		if defined(IBL)
	if (SharedData::iblSettings.EnableDiffuseIBL && (!SharedData::InInterior || SharedData::iblSettings.EnableInterior)) {
		ambientColor *= SharedData::iblSettings.DALCAmount;
#			if defined(SKYLIGHTING) && !defined(INTERIOR)
		float3 iblColor = ImageBasedLighting::GetIBLColor(float3(0, 0, -1), 1.0);
#			else
		float3 iblColor = ImageBasedLighting::GetIBLColor(float3(0, 0, -1));
#			endif
		ambientColor += Color::IrradianceToGamma(Color::Saturation(iblColor, SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale);
	}
#		endif

	float llDirLightMult = PhysLight::DirLightMultApplies() ? SharedData::linearLightingSettings.dirLightMult /* (49h stage 1, S8) + not indoors */ : 1.0f;
	float3 dirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;

	float inputLuma = Color::RGBToLuminance(inputColor);
	float ambientLuma = Color::RGBToLuminance(ambientColor);
	float dirLightLuma = Color::RGBToLuminance(dirLightColor);
	float totalLuma = ambientLuma + dirLightLuma;

	// Scale the ambient so that ambient + sun luminance matches the input
	if (totalLuma > 0.0 && ambientLuma > 0.0)
		ambientColor *= inputLuma / totalLuma;

	return max(0.0, inputColor - ambientColor);
}
#	endif

#	if defined(LIGHTING)
float3 GetLightingColor(float3 msPosition, float3 worldPosition, float4 screenPosition, uint eyeIndex, inout float shadowVariance, bool a_follow)
{
	float4 lightDistanceSquared = (PLightPositionX[eyeIndex] - msPosition.xxxx) * (PLightPositionX[eyeIndex] - msPosition.xxxx) + (PLightPositionY[eyeIndex] - msPosition.yyyy) * (PLightPositionY[eyeIndex] - msPosition.yyyy) + (PLightPositionZ[eyeIndex] - msPosition.zzzz) * (PLightPositionZ[eyeIndex] - msPosition.zzzz);
	float4 lightFadeMul = 1.0.xxxx - saturate(PLightingRadiusInverseSquared * lightDistanceSquared);

	float3 color = DLightColor.xyz * Color::EffectLightingMult();

	[branch] if (a_follow) {
		// (49g) Effects Follow Scene Light > Lit Smoke and Mist: the light a matte vanilla surface gets here (EffectScene::MatteLight).
		color = EffectScene::MatteLight(worldPosition, screenPosition, eyeIndex, shadowVariance);
	} else if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::EffectShadows)) {
		float llDirLightMult = PhysLight::DirLightMultApplies() ? SharedData::linearLightingSettings.dirLightMult /* (49h stage 1, S8) + not indoors */ : 1.0f;
		float3 dirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult * (PhysLight::On(PhysLight::kSmallFixes) ? Color::VanillaDiffuseMult() : 0.5) * Color::EffectLightingMult();  // (49h stage 1, S8) the ground's diffuse factor (was 0.5)

#		if defined(PHYSICAL_SKY)
		if (SharedData::physSkyData.enabled)
			dirLightColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampDepthSampler);
#		endif
#		if defined(EXP_HEIGHT_FOG)
		dirLightColor *= WeatherFog::SunlightAttenuation(worldPosition.z);  // (47d) 1 unless "CS Fog Takes Over" applies
#		endif

		float3 ambientColor = max(0, mul(SharedData::DirectionalAmbient, float4(0, 0, 1, 1)));
		if (PhysLight::On(PhysLight::kSmallFixes))  // (49h stage 1, S8) converted (was raw)
			ambientColor = Color::Ambient(ambientColor);

#		if defined(IBL)
		if (SharedData::iblSettings.EnableDiffuseIBL && (!SharedData::InInterior || SharedData::iblSettings.EnableInterior)) {
			ambientColor *= SharedData::iblSettings.DALCAmount;
		}
#		endif

		color = ambientColor;

#		if defined(SKYLIGHTING)
#			if defined(VR)
		float3 positionMSSkylight = worldPosition + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#			else
		float3 positionMSSkylight = worldPosition;
#			endif

		sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, positionMSSkylight);
		float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(float3(0, 0, 1))) / Math::PI;
		skylightingDiffuse = saturate(skylightingDiffuse);
		skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(worldPosition));
		skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);

		color = Color::IrradianceToLinear(color);
		color *= skylightingDiffuse;
		color = Color::IrradianceToGamma(color);
#		endif

#		if defined(IBL)
		if (SharedData::iblSettings.EnableDiffuseIBL) {
			if (!SharedData::InInterior || SharedData::iblSettings.EnableInterior)
			{
				color = Color::IrradianceToLinear(color);
#			if defined(SKYLIGHTING)
				color += Color::Saturation(ImageBasedLighting::GetIBLColor(float3(0, 0, -1), skylightingDiffuse), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#			else
				color += Color::Saturation(ImageBasedLighting::GetIBLColor(float3(0, 0, -1)), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#			endif
				color = Color::IrradianceToGamma(color);
			}
		}
#		endif

#		if defined(IBL)
		float3 iblColor = 0;
		if (SharedData::iblSettings.EnableDiffuseIBL && !PhysLight::On(PhysLight::kSmallFixes)) {  // (49h stage 1, S8) the IBL was added a second time here
			if (!SharedData::InInterior || SharedData::iblSettings.EnableInterior)
			{
#			if defined(SKYLIGHTING)
				iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(float3(0, 0, 0), skylightingDiffuse), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#			else
				iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(float3(0, 0, 0)), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#			endif
				color += Color::IrradianceToGamma(iblColor);
			}
		}
#		endif

		if (!SharedData::InInterior){
			bool isWorldShadow = false;
			float shadow;
#		if defined(VOLUMETRIC_SHADOWS)
			// (batch 38, A2) VSM along the view ray instead of the raw-cascade 3D filter
			[branch] if (VolumetricShadows::ParticleShadowsEnabled())
				shadow = VolumetricShadows::GetEffectShadow(worldPosition.xyz, normalize(worldPosition.xyz), screenPosition.xy, eyeIndex);
			else
#		endif
				shadow = ShadowSampling::GetEffectShadow(worldPosition.xyz, normalize(worldPosition.xyz), screenPosition.xy, eyeIndex, isWorldShadow);
#		if defined(EXP_HEIGHT_FOG)
			// (49b) Particles Use RT Shadows: x the ray-traced sun visibility (the casters Ray Tracing took out of the shadow map / VSM).
			shadow *= RTFogVis::ParticleSun(worldPosition.xyz, eyeIndex);
#		endif
			color += dirLightColor * shadow;
			// Do not denoise world shadows
			if (!isWorldShadow)
				shadowVariance = ComputeShadowVariance(shadow);
		} else {
			color += dirLightColor;
		}
	} else {
		// (49b) Particles Use RT Shadows: the ray-traced sun visibility of this effect (1 = off / no data).
		float rtSun = 1.0;
#		if defined(EXP_HEIGHT_FOG)
		[branch] if (!SharedData::InInterior && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
			rtSun = RTFogVis::ParticleSun(worldPosition.xyz, eyeIndex);
#		endif
#		if defined(VOLUMETRIC_SHADOWS)
		// (batch 38, A2, upstream 0f79d567a) Every other lit effect in the world: the engine hands
		// us one colour for sun + ambient. Estimate the sun's share (upstream ExtractLighting)
		// and take the shadowed part of it away. Fully lit = exactly the old colour.
		[branch] if (VolumetricShadows::ParticleShadowsEnabled() && !SharedData::InInterior && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
		{
			float shadow = VolumetricShadows::GetEffectShadow(worldPosition.xyz, normalize(worldPosition.xyz), screenPosition.xy, eyeIndex) * rtSun;
			rtSun = 1.0;
			color -= EstimateEffectDirectionalPart(color) * (1.0 - shadow);
			shadowVariance = ComputeShadowVariance(shadow);
		}
#		endif
#		if defined(EXP_HEIGHT_FOG)
		// (49b) Without the Volumetric Shadows term: the ray-traced visibility alone takes the shadowed part of the sun's share away.
		[branch] if (rtSun < 1.0)
			color -= EstimateEffectDirectionalPart(color) * (1.0 - rtSun);
#		endif
#		if defined(EXP_HEIGHT_FOG)
		// (47d) "CS Fog Takes Over": the sun's share of the engine's colour, dimmed by the weather's fog toward the sun.
		[branch] if (WeatherFog::Active() && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
			color -= EstimateEffectDirectionalPart(color) * (1.0 - WeatherFog::SunTransmittance(worldPosition.z));
#		endif
#		if defined(SKYLIGHTING)
#			if defined(VR)
		float3 positionMSSkylight = worldPosition + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#			else
		float3 positionMSSkylight = worldPosition;
#			endif

		sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, positionMSSkylight);

		if (!SharedData::InInterior) {
			float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(float3(0, 0, 1))) / Math::PI;
			skylightingDiffuse = saturate(skylightingDiffuse);
			skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(worldPosition));
			skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);

			color = Color::IrradianceToLinear(color);
			color *= skylightingDiffuse;
			color = Color::IrradianceToGamma(color);
		}
#		endif
	}

	// (49g) Following the scene light, the engine's point lights x Vanilla Diffuse Multiplier as on surfaces (Effect Lighting Multiplier a trim).
	const float pointLightMult = a_follow ? Color::VanillaDiffuseMult() * Color::EffectLightingMult() : Color::EffectLightingMult();
#		if defined(LIGHT_LIMIT_FIX)
	if (!(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
#		endif
	{
		color.x += dot(Color::PointLight(PLightColorR.xxx).x * lightFadeMul * pointLightMult, 1.0.xxxx);
        color.y += dot(Color::PointLight(PLightColorG.xxx).x * lightFadeMul * pointLightMult, 1.0.xxxx);
        color.z += dot(Color::PointLight(PLightColorB.xxx).x * lightFadeMul * pointLightMult, 1.0.xxxx);
	}

	return color;
}
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout = (PS_OUTPUT)0;

#	if !defined(VR)
	uint eyeIndex = 0;
#	else
	uint eyeIndex = input.EyeIndex;
#	endif  // !VR

	float4 fogMul = 1;
#	if !defined(MULTBLEND)
	fogMul.xyz = input.FogAlpha;
#	endif

#	if defined(MEMBRANE)
#		if !defined(MOTIONVECTORS_NORMALS) && defined(ALPHA_TEST)
	float noiseAlpha = TexNoiseSampler.Sample(SampNoiseSampler, input.TexCoord0.zw).w;
#			if defined(VC)
	noiseAlpha *= input.Color.w;
#			endif
	if (noiseAlpha - AlphaTestRef.x < 0) {
		discard;
	}
#		endif

#		if defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS)
	float3 normal = input.ScreenSpaceNormal;
#		elif defined(NORMALS)
	float3 normal = input.TBN0;
#		else
	float3 normal = TexNormalSampler.Sample(SampNormalSampler, input.TexCoord0.zw).xzy * 2 - 1;
#			if defined(SKINNED)
	normal = mul(normal, transpose(float3x3(input.TBN0, input.TBN1, input.TBN2)));
#			endif
#		endif
	float NdotV = dot(normal, input.ViewVector.xyz);
	float membraneColorMul = pow(saturate(1 - NdotV), MembraneVars.x);
	float4 membraneColor = MembraneRimColor * membraneColorMul;
#	elif defined(PROJECTED_UV) && defined(NORMALS)
	float2 noiseTexCoord = 0.00333333341 * input.TexCoord0.xy;
	float noise = TexNoiseSampler.Sample(SampNoiseSampler, noiseTexCoord).x * 0.2 + 0.4;
	if (dot(input.TBN0, input.TBN1) - noise < 0) {
		discard;
	}
#	endif

	float softMul = 1;
#	if defined(SOFT)
	float depth = TexDepthSamplerEffect.Load(int3(input.Position.xy, 0)).x;
	softMul = saturate(-input.TexCoord0.w + LightingInfluence.y / ((1 - depth) * CameraDataEffect.z + CameraDataEffect.y));
#	endif

	float lightingInfluence = LightingInfluence.x;
	float3 propertyColor = Color::Effect(PropertyColor.xyz);
	float shadowVariance = 1.0;

	float4 baseTexColor = float4(1, 1, 1, 1);
	float4 baseColor = float4(1, 1, 1, 1);
#	if !defined(TEXTURE)
	[branch] if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
#	endif
	{
		baseTexColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
		baseTexColor.xyz = Color::Effect(baseTexColor.xyz);
		baseColor *= baseTexColor;
		if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::IgnoreTexAlpha || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha) {
			baseColor.w = 1;
		}
	}

#	if defined(MEMBRANE)
	float4 baseColorMul = float4(1, 1, 1, 1);
#	else
	float4 baseColorMul = BaseColor;
	baseColorMul.xyz = Color::Effect(baseColorMul.xyz);
#		if defined(VC) && !defined(PROJECTED_UV)
	baseColorMul *= float4(Color::Effect(input.Color.xyz), input.Color.w);
#		endif
#	endif

#	if defined(MEMBRANE)
	baseColor.w *= input.ViewVector.w;
#	else
	baseColor.w *= input.TexCoord0.z;
#	endif

	baseColor = baseColorMul * baseColor;
	baseColor.w *= softMul;

#	if defined(SOFT) && !(defined(FALLOFF) && defined(MULTBLEND))
	if (baseColor.w - 0.003 < 0) {
		discard;
	}
#	endif

	float alpha = baseColor.w;

#	if defined(BLOOD)
	alpha = baseColor.y;
	float deltaY = saturate(baseColor.y - AlphaTestRef.x);
	float bloodMul = baseColor.z;
#		if defined(VC)
	bloodMul *= input.Color.w;
#		endif
	if (deltaY < AlphaTestRef.y) {
		bloodMul *= (deltaY / AlphaTestRef.y);
	}
	baseColor.xyz = saturate(float3(2, 1, 1) - bloodMul.xxx) * (-bloodMul * AlphaTestRef.z + 1);
#	endif

	alpha *= PropertyColor.w;

	float baseColorScale = BaseColorScale.x;

#	if defined(MEMBRANE)
	baseColor.xyz = (PropertyColor.xyz + baseColor.xyz) * alpha + membraneColor.xyz * membraneColor.w;
	alpha += membraneColor.w;
	baseColorScale = MembraneVars.z;
#	endif

	if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
		alpha = TexGrayscaleSampler.Sample(SampGrayscaleSampler, float2(baseTexColor.w, alpha)).w;

	[branch] if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor)
	{
		float2 grayscaleToColorUv = float2(baseTexColor.y, baseColorMul.x);
#	if defined(MEMBRANE)
		grayscaleToColorUv.y = PropertyColor.x;
#	endif
		baseColor.xyz = Color::Effect(baseColorScale * TexGrayscaleSampler.Sample(SampGrayscaleSampler, grayscaleToColorUv).xyz);
	}

#	if !defined(MOTIONVECTORS_NORMALS)
	if (alpha * fogMul.w - AlphaTestRefRS < 0) {
		discard;
	}
#	endif

	// (49g) Effects Follow Scene Light (Linear Lighting > Effects): which multiplier this effect takes, and whether its light follows
	// the scene (lit smoke and mist: the light a matte surface gets here; unlit smoke and mist: scaled by the scene's brightness).
	// Master off: both false, every term below as before.
	const uint sceneCategory = EffectScene::Category(lightingInfluence);
	const bool sceneMaster = EffectScene::Master();
	const bool followLit = sceneMaster && (EffectSceneFlags & EffectScene::kLit) != 0u && sceneCategory == EffectScene::kLitSmoke && EffectScene::CanFollow();
	const bool followUnlit = sceneMaster && (EffectSceneFlags & EffectScene::kUnlit) != 0u && sceneCategory == EffectScene::kUnlitSmoke && EffectScene::CanFollowUnlit();
	// (49i) The 49h S1 cap divided baseColor by BaseColorScale.x here. BaseColorScale is multiplied into the colour only in the
	// GrayscaleToColor branch above (and there inside Color::Effect, pow 1.8 with Linear Lighting, so the division left scale^0.8), and the
	// division switched on above 1.0: an animated emissive multiple flickered the smoke. Replaced by the cap after the lighting below.

	// (48n, official CS #3033) Light only pixels that survive the alpha test.
#	if defined(LIGHTING)
	propertyColor = GetLightingColor(input.MSPosition.xyz, input.WorldPosition.xyz, input.Position.xyzw, eyeIndex, shadowVariance, followLit);

#		if defined(LIGHT_LIMIT_FIX)
	uint lightCount = 0;
	if (LightingInfluence.x > 0.0)
		EffectScene::AddPointLights(propertyColor, input.WorldPosition.xyz, eyeIndex, followLit ? Color::VanillaDiffuseMult() : 0.5, Color::EffectLightingMult(), lightCount);
#		endif
#	elif defined(MEMBRANE)
	propertyColor *= 0;
	lightingInfluence = 0;
#	else
	// (49g) Lit smoke and mist without the LIGHTING flag (the engine hands them one light colour): the same light as the lit ones.
	[branch] if (followLit)
	{
		propertyColor = EffectScene::MatteLight(input.WorldPosition.xyz, input.Position.xyzw, eyeIndex, shadowVariance);
#		if defined(LIGHT_LIMIT_FIX)
		uint followLightCount = 0;
		EffectScene::AddPointLights(propertyColor, input.WorldPosition.xyz, eyeIndex, Color::VanillaDiffuseMult(), Color::EffectLightingMult(), followLightCount);
#		endif
	}
#	endif

	float3 lightColor = lerp(baseColor.xyz * EffectScene::SelfLitScale(sceneCategory), propertyColor * baseColor.xyz, lightingInfluence.xxx);  // (49j) self-lit part x P

	// (49i, S1 "Same Sun and Moon Light Everywhere") Lit smoke and mist that follow the scene light are never brighter than a lit matte
	// surface: at most the light a matte surface gets here (propertyColor: the same light, MatteLight / GetLightingColor, + point lights) x a
	// colour no brighter than white (the effect's own colour, clamped to 1 per channel). Uses no per-frame material constant: the
	// result is only ever lowered, never divided, so an animated emissive multiple cannot be amplified (49h: it flickered smoke near fires
	// and the mountain clouds). Night mountain clouds (emissive multiple 1.5, partly self-lit) end at the moonlit matte brightness.
	[branch] if (followLit && PhysLight::On(PhysLight::kDirect))
		lightColor = min(lightColor, propertyColor * saturate(baseColor.xyz));

	// (49g) Unlit Smoke and Mist: x the scene's ambient brightness relative to this weather's midday (dims at night and indoors).
	// (49j) With Physical Brightness the scale is already in the self-lit part (SelfLitScale).
	[branch] if (followUnlit && !EffectScene::Brightness())
		lightColor *= EffectUnlitSceneScale;

	float effectNormalization = 1.0;
#	if defined(IBL) && !defined(LIGHTING) && !defined(DEFERRED)
	// (49g) Not on effects that follow the scene light (they already have the scene's brightness).
	if (SharedData::iblSettings.EnableDiffuseIBL && SharedData::iblSettings.EffectNormalization && (!SharedData::InInterior || SharedData::iblSettings.EnableInterior) && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) && !followLit && !followUnlit) {
		float directionalAmbientColorLuminance = Color::RGBToLuminance(
            max(0, mul(SharedData::DirectionalAmbient, float4(0, 0, 0, 1.0)))
        );
#		if defined(SKYLIGHTING)
		// (F10) The real probe visibility, replacing the literal 1 this call used to pass, which
		// normalised every effect against unobstructed sky wherever it was standing. Same chain
		// and same order as GetLightingColor above (:587-591), which is this file's own idiom for
		// it, so the two agree; sampleNoBias for the same reason it does.
		float effectSkylighting;
		{
#			if defined(VR)
			float3 positionMSSkylight = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#			else
			float3 positionMSSkylight = input.WorldPosition.xyz;
#			endif
			sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, positionMSSkylight);
			float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(float3(0, 0, 1))) / Math::PI;
			skylightingDiffuse = saturate(skylightingDiffuse);
			skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(input.WorldPosition.xyz));
			effectSkylighting = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);
		}
		float3 iblColor = ImageBasedLighting::GetIBLColor(float3(0, 0, 0), effectSkylighting);
#		else
		float3 iblColor = ImageBasedLighting::GetIBLColor(float3(0, 0, 0));
#		endif
		float iblLuminance = Color::RGBToLuminance(iblColor);
		effectNormalization = iblLuminance * SharedData::iblSettings.DiffuseIBLScale + directionalAmbientColorLuminance * SharedData::iblSettings.DALCAmount;
		effectNormalization *= SharedData::iblSettings.EffectNormalizationMult;
		effectNormalization = max(effectNormalization, SharedData::iblSettings.MinEffectMult);
	}
#	endif
	lightColor *= effectNormalization;

	// (49g) With Effects Follow Scene Light the effect's multiplier (Fire and Magic / Lit / Unlit Smoke and Mist) comes before the fog, so the
	// fog over an effect matches the fog around it. Master off: Color::EffectMult after the fog, as before.
	const bool sceneMultBeforeFog = sceneMaster && EffectScene::OwnMultiplier();
	[branch] if (sceneMultBeforeFog)
		lightColor *= EffectScene::Multiplier(sceneCategory);

	float weatherFogAmount = 0.0;  // (47b) share of the view the game's fog covers under the height fog
#	if !defined(MOTIONVECTORS_NORMALS)
	float fogFactor = Color::FogAlpha(input.FogParam.w);
	float3 fogColor = Color::Fog(input.FogParam.xyz);
	// (49j, design 3.2 row 20) Physical Brightness: the game's fog colour x the game-colour factor (it was drawn against the game's ambient).
	if (EffectScene::Brightness() && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
		fogColor *= EffectUnlitSceneScale;
#		if defined(IBL)
	if (SharedData::iblSettings.EnableDiffuseIBL && !SharedData::InInterior) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#		endif
#		if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		// (batch 38, A1) + volumetric fog for effects drawn by the main camera
		// (47b) the game's own fog (weather / room, with the game's fog fade) under the height fog; no-op while off
		// (47d) or, with "CS Fog Takes Over", the weather's fog as a layer of the height fog
		const bool useVolumetricFog = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection);
		float4 exponentialHeightFog = ExponentialHeightFog::GetFog(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor, fogFactor, fogMul.x, useVolumetricFog, eyeIndex, input.Position.xy, weatherFogAmount);
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
		fogMul = 1;
	}
#		endif
#		if defined(ADDBLEND)
	float3 blendedColor = lightColor * (1 - fogFactor);
#		elif defined(MULTBLEND) || defined(MULTBLEND_DECAL)
	float3 blendedColor = lerp(lightColor, 1.0.xxx, saturate(1.5 * fogFactor).xxx);
#		else
	float3 blendedColor = lerp(lightColor, fogColor, fogFactor.xxx);
#		endif
#	else
	float3 blendedColor = lightColor.xyz;
#	endif

	alpha = Color::EffectAlpha(alpha);

	float4 finalColor = float4(sceneMultBeforeFog ? blendedColor : Color::EffectMult(blendedColor), alpha);
#	if defined(MULTBLEND_DECAL)
	finalColor.xyz *= alpha;
#	else
	finalColor *= fogMul;
#	endif
	psout.Diffuse = finalColor;
#	if defined(LIGHTING) && defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap(0.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap(0.0);
		} else {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap((float)lightCount / MAX_CLUSTER_LIGHTS);
		}
	}
#	endif

#	if defined(DEFERRED)

#		if defined(MOTIONVECTORS_NORMALS)
#			if (defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
	float3 screenSpaceNormal = normalize(input.TBN0);
#			else
	float3 screenSpaceNormal = normalize(input.ScreenSpaceNormal);
#			endif
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), 0.0, psout.Diffuse.w);
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);
	psout.MotionVectors = float4(screenMotionVector, 0.0, psout.Diffuse.w);
#		endif

#if defined(MULTBLEND) || defined(MULTBLEND_DECAL)
	psout.Specular = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Albedo = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Reflectance = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Masks = float4(Color::RGBToLuminance(psout.Diffuse.xyz).xxx, finalColor.w);
#else
	psout.Albedo = float4(0, 0, 0, finalColor.w);
	psout.Specular = float4(0, 0, 0, finalColor.w);
	psout.Reflectance = float4(0, 0, 0, finalColor.w);
	psout.Masks = float4(0, 0, 0, finalColor.w);
#endif

#	elif defined(MOTIONVECTORS_NORMALS)
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);
	psout.MotionVectors = screenMotionVector;

#		if (defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
	float3 screenSpaceNormal = normalize(input.TBN0);
#		else
	float3 screenSpaceNormal = normalize(input.ScreenSpaceNormal);
#		endif

	screenSpaceNormal.z = max(0.001, sqrt(8 + -8 * screenSpaceNormal.z));
	screenSpaceNormal.xy /= screenSpaceNormal.zz;
	psout.ScreenSpaceNormals.xy = screenSpaceNormal.xy + 0.5.xx;
	psout.ScreenSpaceNormals.zw = 0.0.xx;
#	else
	psout.Normal = float4(shadowVariance, 0, 0, finalColor.w);
	psout.Color2 = finalColor;
#	endif

#	if !defined(DEFERRED)
#		if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld)) {
		float4 apSample = PhysSky::SampleAp(normalize(input.WorldPosition.xyz), input.Position.xy, length(input.WorldPosition.xyz), SampBaseSampler);
#			if defined(EXP_HEIGHT_FOG)
		[branch] if (weatherFogAmount > 0.0)  // (47b) what the game's fog covers loses its haze
			apSample = ExponentialHeightFog::ScaleAerialPerspective(apSample, weatherFogAmount);
#			endif
		psout.Diffuse.xyz = psout.Diffuse.xyz * apSample.w + apSample.xyz;
	}
#		endif
#	endif
	if (!(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) && SharedData::linearLightingSettings.enableLinearLighting) {
		psout.Diffuse.xyz = Color::TrueLinearToGamma(psout.Diffuse.xyz);
	}
	return psout;
}
#endif