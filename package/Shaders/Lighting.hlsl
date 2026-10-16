#define LIGHTING

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/LodLandscape.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"

#if defined(FACEGEN) || defined(FACEGEN_RGB_TINT)
#	define SKIN
#	if defined(CS_SKIN)
#		define DYNAMIC_CUBEMAPS
#	endif
#endif

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if (defined(TREE_ANIM) || defined(LANDSCAPE)) && !defined(VC)
#	define VC
#endif  // TREE_ANIM || LANDSCAPE || !VC

#if defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(LODLANDNOISE) || defined(WORLD_MAP)
#	define LOD
#endif

struct VS_INPUT
{
	float4 Position : POSITION0;
	float2 TexCoord0 : TEXCOORD0;
#if !defined(MODELSPACENORMALS)
	float4 Normal : NORMAL0;
	float4 Bitangent : BINORMAL0;
#endif  // !MODELSPACENORMALS

#if defined(VC)
	float4 Color : COLOR0;
#	if defined(LANDSCAPE)
	float4 LandBlendWeights1 : TEXCOORD2;
	float4 LandBlendWeights2 : TEXCOORD3;
#	endif  // LANDSCAPE
#endif      // VC
#if defined(SKINNED)
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
#endif  // SKINNED
#if defined(EYE)
	float EyeParameter : TEXCOORD2;
#endif  // EYE
#if defined(VR)
	uint InstanceID : SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
#if (defined(PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
	float4
#else
	float2
#endif  // (defined (PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
		TexCoord0 : TEXCOORD0;

#if defined(WORLD_MAP)
		float3 InputPosition : TEXCOORD4;
#endif

	// centroid: under Variable Rate Shading one pixel-shader invocation covers a 2x2 / 4x4 block
	// and is evaluated at the block centre, which on thin (distant, grazing) triangles lies
	// well outside the triangle. Plain interpolation then extrapolates these attributes far out
	// of range (vertex colour / AO below 0 or above 1, normals flipped, blend weights > 1) and
	// the result blows up. centroid moves the evaluation to a covered position. Without VRS
	// (no MSAA) every covered pixel's centroid is its centre, so nothing changes. Texture
	// coordinates and positions stay at the centre: their screen derivatives drive mip
	// selection and the flat-normal / parallax maths and must stay regular.
#if defined(SKINNED) || !defined(MODELSPACENORMALS)
	centroid float3 TBN0 : TEXCOORD1;
	centroid float3 TBN1 : TEXCOORD2;
	centroid float3 TBN2 : TEXCOORD3;
#endif  // defined(SKINNED) || !defined(MODELSPACENORMALS)
#if defined(EYE)
	float3 EyeNormal : TEXCOORD6;
#elif defined(LANDSCAPE)
	centroid float4 LandBlendWeights1 : TEXCOORD6;
	centroid float4 LandBlendWeights2 : TEXCOORD7;
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	float3 TexProj : TEXCOORD7;
#endif  // EYE

	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
	centroid float4 Color : COLOR0;
	float4 FogParam : COLOR1;

#if defined(VR)
	float ClipDistance : SV_ClipDistance0;  // o11
	float CullDistance : SV_CullDistance0;  // p11
#endif

	float3 ModelPosition : TEXCOORD12;

	// (40d) Dynamic Snow on characters: the smooth vertex normal in the mesh's own space, before
	// skinning (= the bind pose for skinned meshes), so the snow cover does not follow the limbs'
	// motion and is not faceted. In every permutation: the engine strips SKINNED from pixel
	// shader descriptors, so a pixel shader cannot know whether its vertex shader skinned.
	centroid float3 SnowBindNormal : TEXCOORD13;
};
#ifdef VSHADER

cbuffer PerTechnique : register(b0)
{
#	if !defined(VR)
	float4 HighDetailRange[1] : packoffset(c0);  // loaded cells center in xy, size in zw
	float4 FogParam : packoffset(c1);
	float4 FogNearColor : packoffset(c2);
	float4 FogFarColor : packoffset(c3);
#	else
	float4 HighDetailRange[2] : packoffset(c0);  // loaded cells center in xy, size in zw
	float4 FogParam : packoffset(c2);
	float4 FogNearColor : packoffset(c3);
	float4 FogFarColor : packoffset(c4);
#	endif  // VR
};

cbuffer PerMaterial : register(b1)
{
	float4 LeftEyeCenter : packoffset(c0);
	float4 RightEyeCenter : packoffset(c1);
	float4 TexcoordOffset : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	row_major float3x4 World[1] : packoffset(c0);
	row_major float3x4 PreviousWorld[1] : packoffset(c3);
	float4 EyePosition[1] : packoffset(c6);
	float4 LandBlendParams : packoffset(c7);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c8);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c9);
	row_major float3x4 TextureProj[1] : packoffset(c10);
	float IndexScale : packoffset(c13);
	float4 WorldMapOverlayParameters : packoffset(c14);
#	else   // VR has 49 vs 30 entries
	row_major float3x4 World[2] : packoffset(c0);
	row_major float3x4 PreviousWorld[2] : packoffset(c6);
	float4 EyePosition[2] : packoffset(c12);
	float4 LandBlendParams : packoffset(c14);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c15);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c16);
	row_major float3x4 TextureProj[2] : packoffset(c17);
	float IndexScale : packoffset(c23);
	float4 WorldMapOverlayParameters : packoffset(c24);
#	endif  // VR
};

cbuffer VS_PerFrame : register(b12)
{
#	if !defined(VR)
	row_major float3x3 ScreenProj[1] : packoffset(c0);
	row_major float4x4 ViewProj[1] : packoffset(c8);
#		if defined(SKINNED)
	float3 BonesPivot[1] : packoffset(c40);
	float3 PreviousBonesPivot[1] : packoffset(c41);
#		endif  // SKINNED
#	else
	row_major float3x3 ScreenProj[2] : packoffset(c0);
	row_major float4x4 ViewProj[2] : packoffset(c16);
#		if defined(SKINNED)
	float3 BonesPivot[2] : packoffset(c80);
	float3 PreviousBonesPivot[2] : packoffset(c82);
#		endif  // SKINNED
#	endif      // VR
};

#	if defined(TREE_ANIM)
float2 GetTreeShiftVector(float4 position, float4 color)
{
	precise float4 tmp1 = (TreeParams.w * TreeParams.y).xxxx * WindTimers.xxyy;
	precise float4 tmp2 = float4(0.1, 0.25, 0.1, 0.25) * tmp1 + dot(position.xyz, 1.0.xxx).xxxx;
	precise float4 tmp3 = abs(-1.0.xxxx + 2.0.xxxx * frac(0.5.xxxx + tmp2.xyzw));
	precise float4 tmp4 = (tmp3 * tmp3) * (3.0.xxxx - 2.0.xxxx * tmp3);
	return (tmp4.xz + 0.1.xx * tmp4.yw) * (TreeParams.z * color.w).xx;
}
#	endif  // TREE_ANIM

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	precise float4 inputPosition = float4(input.Position.xyz, 1.0);

	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif
	);
#	if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	inputPosition = LodLandscape::AdjustLodLandscapeVertexPositionMS(inputPosition, float4x4(World[eyeIndex], float4(0, 0, 0, 1)), HighDetailRange[eyeIndex]);
#	endif  // defined(LODLANDNOISE) || defined(LODLANDSCAPE)                                                                   \

	precise float4 previousInputPosition = inputPosition;

#	if defined(TREE_ANIM)
	precise float2 treeShiftVector = GetTreeShiftVector(input.Position, input.Color);
	float3 normal = -1.0.xxx + 2.0.xxx * input.Normal.xyz;

	inputPosition.xyz += normal.xyz * treeShiftVector.x;
	previousInputPosition.xyz += normal.xyz * treeShiftVector.y;
#	endif

#	if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;

	float3x4 previousWorldMatrix =
		Skinned::GetBoneTransformMatrix(PreviousBones, actualIndices, PreviousBonesPivot[eyeIndex], input.BoneWeights);
	precise float4 previousWorldPosition =
		float4(mul(inputPosition, transpose(previousWorldMatrix)), 1);

	float3x4 worldMatrix = Skinned::GetBoneTransformMatrix(Bones, actualIndices, BonesPivot[eyeIndex], input.BoneWeights);
	precise float4 worldPosition = float4(mul(inputPosition, transpose(worldMatrix)), 1);

	float4 viewPos = mul(ViewProj[eyeIndex], worldPosition);
#	else   // !SKINNED
	precise float4 previousWorldPosition = float4(mul(PreviousWorld[eyeIndex], inputPosition), 1);
	precise float4 worldPosition = float4(mul(World[eyeIndex], inputPosition), 1);
	precise float4x4 world4x4 = float4x4(World[eyeIndex][0], World[eyeIndex][1], World[eyeIndex][2], float4(0, 0, 0, 1));
	precise float4x4 modelView = mul(ViewProj[eyeIndex], world4x4);
	float4 viewPos = mul(modelView, inputPosition);
#	endif  // SKINNED

	vsout.Position = viewPos;

#	if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	vsout.Position.z += min(1, 1e-4 * max(0, viewPos.z - 70000)) * 0.5;
#	endif

	float2 uv = input.TexCoord0.xy * TexcoordOffset.zw + TexcoordOffset.xy;
#	if defined(LANDSCAPE)
	vsout.TexCoord0.zw = (uv * 0.010416667.xx + LandBlendParams.xy) * float2(1, -1) + float2(0, 1);
#	elif defined(PROJECTED_UV) && !defined(SKINNED)
	vsout.TexCoord0.z = mul(TextureProj[eyeIndex][0], inputPosition);
	vsout.TexCoord0.w = mul(TextureProj[eyeIndex][1], inputPosition);
#	endif
	vsout.TexCoord0.xy = uv;

#	if defined(WORLD_MAP)
	vsout.InputPosition.xyz = WorldMapOverlayParameters.xyz + worldPosition.xyz;
#	endif

#	if defined(SKINNED)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
#	endif

#	if !defined(MODELSPACENORMALS)
	float3x3 tbn = float3x3(
		float3(input.Position.w, input.Normal.w * 2 - 1, input.Bitangent.w * 2 - 1),
		input.Bitangent.xyz * 2.0.xxx + -1.0.xxx,
		input.Normal.xyz * 2.0.xxx + -1.0.xxx);
	float3x3 tbnTr = transpose(tbn);

#		if defined(SKINNED)
	float3x3 worldTbnTr = transpose(mul(transpose(tbnTr), transpose(boneRSMatrix)));
	float3x3 worldTbnTrTr = transpose(worldTbnTr);
	worldTbnTrTr[0] = normalize(worldTbnTrTr[0]);
	worldTbnTrTr[1] = normalize(worldTbnTrTr[1]);
	worldTbnTrTr[2] = normalize(worldTbnTrTr[2]);
	worldTbnTr = transpose(worldTbnTrTr);
	vsout.TBN0.xyz = worldTbnTr[0];
	vsout.TBN1.xyz = worldTbnTr[1];
	vsout.TBN2.xyz = worldTbnTr[2];
#		else
	vsout.TBN0.xyz = mul(tbn, World[eyeIndex][0].xyz);
	vsout.TBN1.xyz = mul(tbn, World[eyeIndex][1].xyz);
	vsout.TBN2.xyz = mul(tbn, World[eyeIndex][2].xyz);
	float3x3 tempTbnTr = transpose(float3x3(vsout.TBN0.xyz, vsout.TBN1.xyz, vsout.TBN2.xyz));
	tempTbnTr[0] = normalize(tempTbnTr[0]);
	tempTbnTr[1] = normalize(tempTbnTr[1]);
	tempTbnTr[2] = normalize(tempTbnTr[2]);
	tempTbnTr = transpose(tempTbnTr);
	vsout.TBN0.xyz = tempTbnTr[0];
	vsout.TBN1.xyz = tempTbnTr[1];
	vsout.TBN2.xyz = tempTbnTr[2];
#		endif
#	elif defined(SKINNED)
	float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
	float3x3 worldTbnTr = transpose(float3x3(normalize(boneRSMatrixTr[0]),
		normalize(boneRSMatrixTr[1]), normalize(boneRSMatrixTr[2])));

	vsout.TBN0.xyz = worldTbnTr[0];
	vsout.TBN1.xyz = worldTbnTr[1];
	vsout.TBN2.xyz = worldTbnTr[2];
#	endif

#	if defined(LANDSCAPE)
	vsout.LandBlendWeights1 = input.LandBlendWeights1;

	float2 gridOffset = LandBlendParams.zw - input.Position.xy;
	vsout.LandBlendWeights2.w = 1 - saturate(0.000375600968 * (9625.59961 - length(gridOffset)));
	vsout.LandBlendWeights2.xyz = input.LandBlendWeights2.xyz;
#	elif defined(PROJECTED_UV) && !defined(SKINNED)
	vsout.TexProj = TextureProj[eyeIndex][2].xyz;
#	endif

#	if defined(EYE)
	precise float4 modelEyeCenter = float4(LeftEyeCenter.xyz + input.EyeParameter.xxx * (RightEyeCenter.xyz - LeftEyeCenter.xyz), 1);
	vsout.EyeNormal.xyz = normalize(worldPosition.xyz - mul(modelEyeCenter, transpose(worldMatrix)));
#	endif  // EYE

	vsout.WorldPosition = worldPosition;
	vsout.PreviousWorldPosition = previousWorldPosition;

#	if defined(VC)
	vsout.Color = input.Color;
#	else
	vsout.Color = 1.0.xxxx;
#	endif  // VC

	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(viewPos.xyz) * FogParam.y - FogParam.x))));

	vsout.FogParam.xyz = lerp(FogNearColor.xyz, FogFarColor.xyz, fogColorParam);
	vsout.FogParam.w = fogColorParam;

#	if defined(VR)
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.Position, eyeIndex);
	vsout.Position = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#	endif  // VR

	vsout.ModelPosition = input.Position.xyz;

#	if !defined(MODELSPACENORMALS)
	// Pre-skinning (bind-pose) normal for skinned meshes, the model-space normal otherwise.
	vsout.SnowBindNormal = input.Normal.xyz * 2.0.xxx - 1.0.xxx;
#	else
	// No vertex normal: the pixel shader reads the model-space normal map instead.
	vsout.SnowBindNormal = float3(0, 0, 1);
#	endif

	return vsout;
}
#endif  // VSHADER

typedef VS_OUTPUT PS_INPUT;

#if !defined(LANDSCAPE)
#	undef TERRAIN_BLENDING
#endif

#if defined(DEFERRED)
struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
	float4 MotionVectors : SV_Target1;
	float4 NormalGlossiness : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
	float4 Masks : SV_Target6;
#	if defined(SNOW)
	float4 Parameters : SV_Target7;
#	endif
};
#else
struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
	float4 MotionVectors : SV_Target1;
};
#endif

#ifdef PSHADER

SamplerState SampTerrainParallaxSampler : register(s1);

#	if defined(LANDSCAPE)

SamplerState SampColorSampler : register(s0);

#		define SampLandColor2Sampler SampColorSampler
#		define SampLandColor3Sampler SampColorSampler
#		define SampLandColor4Sampler SampColorSampler
#		define SampLandColor5Sampler SampColorSampler
#		define SampLandColor6Sampler SampColorSampler
#		define SampNormalSampler SampColorSampler
#		define SampLandNormal2Sampler SampColorSampler
#		define SampLandNormal3Sampler SampColorSampler
#		define SampLandNormal4Sampler SampColorSampler
#		define SampLandNormal5Sampler SampColorSampler
#		define SampLandNormal6Sampler SampColorSampler
#		define SampRMAOSSampler SampColorSampler
#		define SampLandRMAOS2Sampler SampColorSampler
#		define SampLandRMAOS3Sampler SampColorSampler
#		define SampLandRMAOS4Sampler SampColorSampler
#		define SampLandRMAOS5Sampler SampColorSampler
#		define SampLandRMAOS6Sampler SampColorSampler

#	else

SamplerState SampColorSampler : register(s0);

#		define SampNormalSampler SampColorSampler

#		if defined(MODELSPACENORMALS) && !defined(LODLANDNOISE)
SamplerState SampSpecularSampler : register(s2);
#		endif
#		if defined(FACEGEN)
SamplerState SampTintSampler : register(s3);
SamplerState SampDetailSampler : register(s4);
#		elif defined(PARALLAX)
SamplerState SampParallaxSampler : register(s3);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
SamplerState SampProjDiffuseSampler : register(s3);
#		endif

#		if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(SNOW_FLAG) || defined(EYE)) && !defined(FACEGEN)
SamplerState SampEnvSampler : register(s4);
SamplerState SampEnvMaskSampler : register(s5);
#		endif

#		if defined(TRUE_PBR)
SamplerState SampParallaxSampler : register(s4);
SamplerState SampRMAOSSampler : register(s5);
#		endif

SamplerState SampGlowSampler : register(s6);

#		if defined(MULTI_LAYER_PARALLAX)
SamplerState SampLayerSampler : register(s8);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
SamplerState SampProjNormalSampler : register(s8);
#		endif

SamplerState SampBackLightSampler : register(s9);

#		if defined(PROJECTED_UV)
SamplerState SampProjDetailSampler : register(s10);
#		endif

SamplerState SampCharacterLightProjNoiseSampler : register(s11);
SamplerState SampRimSoftLightWorldMapOverlaySampler : register(s12);

#		if defined(WORLD_MAP) && (defined(LODLANDSCAPE) || defined(LODLANDNOISE))
SamplerState SampWorldMapOverlaySnowSampler : register(s13);
#		endif

#	endif

#	if defined(LOD_LAND_BLEND)
SamplerState SampLandLodBlend1Sampler : register(s13);
SamplerState SampLandLodBlend2Sampler : register(s15);
#	elif defined(LODLANDNOISE)
SamplerState SampLandLodNoiseSampler : register(s15);
#	endif

SamplerState SampShadowMaskSampler : register(s14);

#	if defined(LANDSCAPE)

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexLandColor2Sampler : register(t1);
Texture2D<float4> TexLandColor3Sampler : register(t2);
Texture2D<float4> TexLandColor4Sampler : register(t3);
Texture2D<float4> TexLandColor5Sampler : register(t4);
Texture2D<float4> TexLandColor6Sampler : register(t5);
Texture2D<float4> TexNormalSampler : register(t7);
Texture2D<float4> TexLandNormal2Sampler : register(t8);
Texture2D<float4> TexLandNormal3Sampler : register(t9);
Texture2D<float4> TexLandNormal4Sampler : register(t10);
Texture2D<float4> TexLandNormal5Sampler : register(t11);
Texture2D<float4> TexLandNormal6Sampler : register(t12);

Texture2D<float4> TexLandTHDisp0Sampler : register(t92);
Texture2D<float4> TexLandTHDisp1Sampler : register(t93);
Texture2D<float4> TexLandTHDisp2Sampler : register(t94);
Texture2D<float4> TexLandTHDisp3Sampler : register(t95);
Texture2D<float4> TexLandTHDisp4Sampler : register(t96);
Texture2D<float4> TexLandTHDisp5Sampler : register(t97);

#		if defined(TRUE_PBR)

Texture2D<float4> TexLandDisplacement0Sampler : register(t80);
Texture2D<float4> TexLandDisplacement1Sampler : register(t81);
Texture2D<float4> TexLandDisplacement2Sampler : register(t82);
Texture2D<float4> TexLandDisplacement3Sampler : register(t83);
Texture2D<float4> TexLandDisplacement4Sampler : register(t84);
Texture2D<float4> TexLandDisplacement5Sampler : register(t85);

Texture2D<float4> TexRMAOSSampler : register(t86);
Texture2D<float4> TexLandRMAOS2Sampler : register(t87);
Texture2D<float4> TexLandRMAOS3Sampler : register(t88);
Texture2D<float4> TexLandRMAOS4Sampler : register(t89);
Texture2D<float4> TexLandRMAOS5Sampler : register(t90);
Texture2D<float4> TexLandRMAOS6Sampler : register(t91);

#		endif

#	else

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexNormalSampler : register(t1);  // normal in xyz, glossiness in w if not modelspacenormal

#		if defined(MODELSPACENORMALS) && !defined(LODLANDNOISE)
Texture2D<float4> TexSpecularSampler : register(t2);
#		endif
#		if defined(FACEGEN)
Texture2D<float4> TexTintSampler : register(t3);
Texture2D<float4> TexDetailSampler : register(t4);
#		elif defined(PARALLAX)
Texture2D<float4> TexParallaxSampler : register(t3);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
Texture2D<float4> TexProjDiffuseSampler : register(t3);
#		endif

#		if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(SNOW_FLAG) || defined(EYE)) && !defined(FACEGEN)
TextureCube<float4> TexEnvSampler : register(t4);
Texture2D<float4> TexEnvMaskSampler : register(t5);
#		endif

#		if defined(TRUE_PBR)
Texture2D<float4> TexParallaxSampler : register(t4);
Texture2D<float4> TexRMAOSSampler : register(t5);
#		endif

Texture2D<float4> TexGlowSampler : register(t6);

#		if defined(MULTI_LAYER_PARALLAX)
Texture2D<float4> TexLayerSampler : register(t8);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
Texture2D<float4> TexProjNormalSampler : register(t8);
#		endif

Texture2D<float4> TexBackLightSampler : register(t9);

#		if defined(PROJECTED_UV)
Texture2D<float4> TexProjDetail : register(t10);
#		endif

Texture2D<float4> TexCharacterLightProjNoiseSampler : register(t11);
Texture2D<float4> TexRimSoftLightWorldMapOverlaySampler : register(t12);

#		if defined(WORLD_MAP) && (defined(LODLANDSCAPE) || defined(LODLANDNOISE))
Texture2D<float4> TexWorldMapOverlaySnowSampler : register(t13);
#		endif

#	endif

#	if defined(LOD_LAND_BLEND)
Texture2D<float4> TexLandLodBlend1Sampler : register(t13);
Texture2D<float4> TexLandLodBlend2Sampler : register(t15);
#	elif defined(LODLANDNOISE)
Texture2D<float4> TexLandLodNoiseSampler : register(t15);
#	endif

Texture2D<float4> TexShadowMaskSampler : register(t14);

#	if defined(SKIN) && defined(CS_SKIN)
Texture2D<float4> TexSkinExtraSampler : register(t71);
Texture2D<float4> TexSkinWetnessSampler : register(t74);
Texture2D<float4> TexSkinWetnessNormalSampler : register(t75);
#	endif

cbuffer PerTechnique : register(b0)
{
	float4 FogColor : packoffset(c0);           // Color in xyz, invFrameBufferRange in w
	float4 ColourOutputClamp : packoffset(c1);  // fLightingOutputColourClampPostLit in x, fLightingOutputColourClampPostEnv in y, fLightingOutputColourClampPostSpec in z
	float4 VPOSOffset : packoffset(c2);         // ???
};

cbuffer PerMaterial : register(b1)
{
	float4 LODTexParams : packoffset(c0);  // TerrainTexOffset in xy, LodBlendingEnabled in z
#	if !(defined(LANDSCAPE) && defined(TRUE_PBR))
	float4 TintColor : packoffset(c1);
	float4 EnvmapData : packoffset(c2);  // fEnvmapScale in x, 1 or 0 in y depending of if has envmask
	float4 ParallaxOccData : packoffset(c3);
	float4 SpecularColor : packoffset(c4);  // Shininess in w, color in xyz
	float4 SparkleParams : packoffset(c5);
	float4 MultiLayerParallaxData : packoffset(c6);  // Layer thickness in x, refraction scale in y, uv scale in zw
#	else
	float4 LandscapeTexture1GlintParameters : packoffset(c1);
	float4 LandscapeTexture2GlintParameters : packoffset(c2);
	float4 LandscapeTexture3GlintParameters : packoffset(c3);
	float4 LandscapeTexture4GlintParameters : packoffset(c4);
	float4 LandscapeTexture5GlintParameters : packoffset(c5);
	float4 LandscapeTexture6GlintParameters : packoffset(c6);
#	endif
	float4 LightingEffectParams : packoffset(c7);  // fSubSurfaceLightRolloff in x, fRimLightPower in y
	float4 IBLParams : packoffset(c8);

#	if !defined(TRUE_PBR)
	float4 LandscapeTexture1to4IsSnow : packoffset(c9);
	float4 LandscapeTexture5to6IsSnow : packoffset(c10);  // bEnableSnowMask in z, inverse iLandscapeMultiNormalTilingFactor in w
	float4 LandscapeTexture1to4IsSpecPower : packoffset(c11);
	float4 LandscapeTexture5to6IsSpecPower : packoffset(c12);
	float4 SnowRimLightParameters : packoffset(c13);  // fSnowRimLightIntensity in x, fSnowGeometrySpecPower in y, fSnowNormalSpecPower in z, bEnableSnowRimLighting in w
#	endif

#	if defined(TRUE_PBR) && defined(LANDSCAPE)
	float3 LandscapeTexture2PBRParams : packoffset(c9);
	float3 LandscapeTexture3PBRParams : packoffset(c10);
	float3 LandscapeTexture4PBRParams : packoffset(c11);
	float3 LandscapeTexture5PBRParams : packoffset(c12);
	float3 LandscapeTexture6PBRParams : packoffset(c13);
#	endif

	float4 CharacterLightParams : packoffset(c14);
	// VR is [9] instead of [15]

	uint PBRFlags : packoffset(c15.x);
	float3 PBRParams1 : packoffset(c15.y);  // roughness scale, displacement scale, specular level
	float4 PBRParams2 : packoffset(c16);    // subsurface color, subsurface opacity
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	float3 DirLightDirection : packoffset(c0);
	float3 DirLightColor : packoffset(c1);
	float4 ShadowLightMaskSelect : packoffset(c2);
	float4 MaterialData : packoffset(c3);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c4);
	float3 EmitColor : packoffset(c4.y);
	float4 ProjectedUVParams : packoffset(c6);
	float4 SSRParams : packoffset(c7);
	float4 WorldMapOverlayParametersPS : packoffset(c8);
	float4 ProjectedUVParams2 : packoffset(c9);
	float4 ProjectedUVParams3 : packoffset(c10);  // fProjectedUVDiffuseNormalTilingScale in x, fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c11);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c14);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[7] : packoffset(c15);               // point light radius in w
	float4 PointLightColor[7] : packoffset(c22);
	float2 NumLightNumShadowLight : packoffset(c29);
#	else
	// VR is [49] instead of [30]
	float3 DirLightDirection : packoffset(c0);
	float4 UnknownPerGeometry[12] : packoffset(c1);
	float3 DirLightColor : packoffset(c13);
	float4 ShadowLightMaskSelect : packoffset(c14);
	float4 MaterialData : packoffset(c15);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c16);
	float3 EmitColor : packoffset(c16.y);
	float4 ProjectedUVParams : packoffset(c18);
	float4 SSRParams : packoffset(c19);
	float4 WorldMapOverlayParametersPS : packoffset(c20);
	float4 ProjectedUVParams2 : packoffset(c21);
	float4 ProjectedUVParams3 : packoffset(c22);  // fProjectedUVDiffuseNormalTilingScale in x,	fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c23);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c26);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[14] : packoffset(c27);              // point light radius in w
	float4 PointLightColor[7] : packoffset(c41);
	float2 NumLightNumShadowLight : packoffset(c48);
#	endif  // VR
};

#	if !defined(VR)
cbuffer AlphaTestRefBuffer : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif

float GetSoftLightMultiplier(float angle)
{
	float softLightParam = saturate((LightingEffectParams.x + angle) / (1 + LightingEffectParams.x));
	float arg1 = (softLightParam * softLightParam) * (3 - 2 * softLightParam);
	float clampedAngle = saturate(angle);
	float arg2 = (clampedAngle * clampedAngle) * (3 - 2 * clampedAngle);
	float softLigtMul = saturate(arg1 - arg2);
	return softLigtMul;
}

float GetRimLightMultiplier(float3 L, float3 V, float3 N)
{
	float NdotV = saturate(dot(N, V));
	return exp2(LightingEffectParams.y * log2(1 - NdotV)) * saturate(dot(V, -L));
}

#	if !defined(TRUE_PBR)
float ProcessSparkleColor(float color)
{
	return exp2(SparkleParams.y * log2(min(1, abs(color))));
}
#	endif

// (batch 15) a_forceVanillaLobe is an extra veto on the GGX branch below, false for every
// material except an eye in EyeDirectSpecularMode = VanillaPhong. The GGX test itself is
// untouched, so when the caller's flag is a compile-time false the whole term folds away and
// the codegen is exactly what it was before this batch. See the flag block near
// `directSpecularRoughness` in the pixel shader body. With VANILLA_FRESNEL undefined the
// parameter is unused and fxc drops it.
float3 GetLightSpecularInput(PS_INPUT input, float3 L, float3 V, float3 N, float3 lightColor, float3 F0, float roughness, float shininess, float2 uv, bool a_forceVanillaLobe)
{
	float3 H = normalize(V + L);
	float HdotN = 1.0;
#	if defined(ANISO_LIGHTING)
	float3 AN = normalize(N * 0.5 + float3(input.TBN0.z, input.TBN1.z, input.TBN2.z));
	float LdotAN = dot(AN, L);
	float HdotAN = dot(AN, H);
	HdotN = 1 - min(1, abs(LdotAN - HdotAN));
#	else
	HdotN = saturate(dot(H, N));
#	endif

#	if defined(SPECULAR)
	float3 lightColorMultiplier = exp2(shininess * log2(HdotN));
#	elif defined(SPARKLE)
	float3 lightColorMultiplier = 0;
#	else
	float3 lightColorMultiplier = HdotN;
#	endif

#	if defined(VANILLA_FRESNEL)
	if (SharedData::vanillaFresnelSettings.Enable && SharedData::vanillaFresnelSettings.EnableGGX && !a_forceVanillaLobe) {
		float NdotV = saturate(dot(N, V));
		float NdotL = saturate(dot(N, L));
		float NdotH = saturate(dot(N, H));
		float HdotV = saturate(dot(H, V));
		float D = BRDF::D_GGX(roughness, HdotN);
		float3 F = BRDF::F_Schlick(F0, HdotV);
		float G = BRDF::Vis_SmithJointApprox(roughness, NdotV, NdotL);
		lightColorMultiplier = max(D * G * F * NdotL, 0.0) * Color::PBRLightingCompensation;
	}
#	endif

#	if defined(ANISO_LIGHTING)
	lightColorMultiplier *= 0.7 * max(0, L.z);
#	endif

#	if defined(SPARKLE) && !defined(SNOW)
	float3 sparkleUvScale = exp2(float3(1.3, 1.6, 1.9) * log2(abs(SparkleParams.x)).xxx);

	float sparkleColor1 = TexProjDetail.Sample(SampProjDetailSampler, uv * sparkleUvScale.xx).z;
	float sparkleColor2 = TexProjDetail.Sample(SampProjDetailSampler, uv * sparkleUvScale.yy).z;
	float sparkleColor3 = TexProjDetail.Sample(SampProjDetailSampler, uv * sparkleUvScale.zz).z;
	float sparkleColor = ProcessSparkleColor(sparkleColor1) + ProcessSparkleColor(sparkleColor2) + ProcessSparkleColor(sparkleColor3);
	float VdotN = dot(V, N);
	V += N * -(2 * VdotN);
	float sparkleMultiplier = exp2(SparkleParams.w * log2(saturate(dot(V, -L)))) * (SparkleParams.z * sparkleColor);
	sparkleMultiplier = sparkleMultiplier >= 0.5 ? 1 : 0;
	lightColorMultiplier += sparkleMultiplier * HdotN;
#	endif
	return lightColor * lightColorMultiplier;
}

float3 TransformNormal(float3 normal)
{
	return normal * 2 + -1.0.xxx;
}

float GetLodLandBlendParameter(float3 color)
{
	float result = saturate(1.6666666 * (dot(color, 0.55.xxx) - 0.4));
	result = ((result * result) * (3 - result * 2));
#	if !defined(WORLD_MAP)
	result *= 0.8;
#	endif
	return result;
}

float GetLodLandBlendMultiplier(float parameter, float mask)
{
	return 0.8333333 * (parameter * (0.37 - mask) + mask) + 0.37;
}

float GetLandSnowMaskValue(float alpha)
{
#	if !defined(TRUE_PBR)
	return alpha * LandscapeTexture5to6IsSnow.z + (1 + -LandscapeTexture5to6IsSnow.z);
#	else
	return 0;
#	endif
}

float3 GetLandNormal(float landSnowMask, float3 normal, float2 uv, SamplerState sampNormal, Texture2D<float4> texNormal)
{
	float3 landNormal = TransformNormal(normal);
#	if defined(SNOW) && !defined(TRUE_PBR)
	if (landSnowMask > 1e-5 && LandscapeTexture5to6IsSnow.w != 1.0) {
		float3 snowNormal =
			float3(-1, -1, 1) *
			TransformNormal(texNormal.Sample(sampNormal, LandscapeTexture5to6IsSnow.ww * uv).xyz);
		landNormal.z += 1;
		float normalProjection = dot(landNormal, snowNormal);
		snowNormal = landNormal * normalProjection.xxx - snowNormal * landNormal.z;
		return normalize(snowNormal);
	} else {
		return landNormal;
	}
#	else
	return landNormal;
#	endif
}

#	if defined(SNOW) && !defined(TRUE_PBR)
float3 GetSnowSpecularColor(PS_INPUT input, float3 worldNormal, float3 viewDirection)
{
	if (SnowRimLightParameters.w > 1e-5) {
#		if defined(MODELSPACENORMALS) && !defined(SKINNED)
		float3 modelGeometryNormal = float3(0, 0, 1);
#		else
		float3 modelGeometryNormal = normalize(float3(input.TBN0.z, input.TBN1.z, input.TBN2.z));
#		endif
		float normalFactor = 1 - saturate(dot(worldNormal, viewDirection));
		float geometryNormalFactor = 1 - saturate(dot(modelGeometryNormal, viewDirection));
		return (SnowRimLightParameters.x * (exp2(SnowRimLightParameters.y * log2(geometryNormalFactor)) * exp2(SnowRimLightParameters.z * log2(normalFactor)))).xxx;
	} else {
		return 0.0.xxx;
	}
}
#	endif

#	if defined(FACEGEN)
float3 GetFacegenBaseColor(float3 rawBaseColor, float2 uv)
{
	float3 detailColor = TexDetailSampler.SampleBias(SampDetailSampler, uv, SharedData::MaterialMipBias()).xyz;
	detailColor = float3(3.984375, 3.984375, 3.984375) * (float3(0.00392156886, 0, 0.00392156886) + detailColor);
	float3 tintColor = TexTintSampler.SampleBias(SampTintSampler, uv, SharedData::MaterialMipBias()).xyz;
	tintColor = tintColor * rawBaseColor * 2.0.xxx;
	tintColor = tintColor - tintColor * rawBaseColor;
	return (rawBaseColor * rawBaseColor + tintColor) * detailColor;
}
#	endif

#	if defined(FACEGEN_RGB_TINT)
float3 GetFacegenRGBTintBaseColor(float3 rawBaseColor, float2 uv)
{
	float3 tintColor = TintColor.xyz * rawBaseColor * 2.0.xxx;
	tintColor = tintColor - tintColor * rawBaseColor;
	return float3(1.01171875, 0.99609375, 1.01171875) * (rawBaseColor * rawBaseColor + tintColor);
}
#	endif

#	if defined(WORLD_MAP)
float3 GetWorldMapNormal(PS_INPUT input, float3 rawNormal, float3 baseColor)
{
	float3 normal = normalize(rawNormal);
#		if defined(MODELSPACENORMALS)
	float3 worldMapNormalSrc = normal.xyz;
#		else
	float3 worldMapNormalSrc = float3(input.TBN0.z, input.TBN1.z, input.TBN2.z);
#		endif
	float3 worldMapNormal = 7.0.xxx * (-0.2.xxx + abs(normalize(worldMapNormalSrc)));
	worldMapNormal = max(0.01.xxx, worldMapNormal * worldMapNormal * worldMapNormal);
	worldMapNormal /= dot(worldMapNormal, 1.0.xxx);
	float3 worldMapColor1 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.yz).xyz;
	float3 worldMapColor2 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.xz).xyz;
	float3 worldMapColor3 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.xy).xyz;
#		if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float3 worldMapSnowColor1 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.yz).xyz;
	float3 worldMapSnowColor2 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.xz).xyz;
	float3 worldMapSnowColor3 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.xy).xyz;
#		endif
	float3 worldMapColor = worldMapNormal.xxx * worldMapColor1 + worldMapNormal.yyy * worldMapColor2 + worldMapNormal.zzz * worldMapColor3;
#		if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float3 worldMapSnowColor = worldMapSnowColor1 * worldMapNormal.xxx + worldMapSnowColor2 * worldMapNormal.yyy + worldMapSnowColor3 * worldMapNormal.zzz;
	float snowMultiplier = GetLodLandBlendParameter(baseColor);
	worldMapColor = snowMultiplier * (worldMapSnowColor - worldMapColor) + worldMapColor;
#		endif
	worldMapColor = normalize(2.0.xxx * (-0.5.xxx + (worldMapColor)));
#		if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float worldMapLandTmp = saturate(19.9999962 * (rawNormal.z - 0.95));
	worldMapLandTmp = saturate(-(worldMapLandTmp * worldMapLandTmp) * (worldMapLandTmp * -2 + 3) + 1.5);
	float3 worldMapLandTmp1 = normalize(normal.zxy * float3(1, 0, 0) - normal.yzx * float3(0, 0, 1));
	float3 worldMapLandTmp2 = normalize(worldMapLandTmp1.yzx * normal.zxy - worldMapLandTmp1.zxy * normal.yzx);
	float3 worldMapLandTmp3 = normalize(worldMapColor.xxx * worldMapLandTmp1 + worldMapColor.yyy * worldMapLandTmp2 + worldMapColor.zzz * normal.xyz);
	float worldMapLandTmp4 = dot(worldMapLandTmp3, worldMapLandTmp3);
	if (worldMapLandTmp4 > 0.999 && worldMapLandTmp4 < 1.001) {
		normal.xyz = worldMapLandTmp * (worldMapLandTmp3 - normal.xyz) + normal.xyz;
	}
#		else
	normal.xyz = normalize(
		WorldMapOverlayParametersPS.zzz * (rawNormal.xyz - worldMapColor.xyz) + worldMapColor.xyz);
#		endif
	return normal;
}

float3 GetWorldMapBaseColor(float3 originalBaseColor, float3 rawBaseColor, float texProjTmp)
{
#		if defined(LODOBJECTS) && !defined(PROJECTED_UV)
	return rawBaseColor;
#		endif
#		if defined(LODLANDSCAPE) || defined(LODOBJECTSHD) || defined(LODLANDNOISE)
	float lodMultiplier = GetLodLandBlendParameter(originalBaseColor.xyz);
#		elif defined(LODOBJECTS) && defined(PROJECTED_UV)
	float lodMultiplier = saturate(10 * texProjTmp);
#		else
	float lodMultiplier = 1;
#		endif
#		if defined(LODOBJECTS)
	float4 lodColorMul = lodMultiplier.xxxx * float4(0.269999981, 0.281000018, 0.441000015, 0.441000015) + float4(0.0780000091, 0.09799999, -0.0349999964, 0.465000004);
	float4 lodColor = lodColorMul.xyzw * 2.0.xxxx;
	lodColor.xyz = Color::Diffuse(lodColor.xyz);
	bool useLodColorZ = lodColorMul.w > 0.5;
	lodColor.xyz = max(lodColor.xyz, rawBaseColor.xyz);
	lodColor.w = useLodColorZ ? lodColor.z : min(lodColor.w, rawBaseColor.z);
	return (0.5 * lodMultiplier).xxx * (lodColor.xyw - rawBaseColor.xyz) + rawBaseColor;
#		else
	float4 lodColorMul = lodMultiplier.xxxx * float4(0.199999988, 0.441000015, 0.269999981, 0.281000018) + float4(0.300000012, 0.465000004, 0.0780000091, 0.09799999);
	float3 lodColor = lodColorMul.zwy * 2.0.xxx;
	lodColor.xyz = Color::Diffuse(lodColor.xyz);
	lodColor.xy = max(lodColor.xy, rawBaseColor.xy);
	lodColor.z = lodColorMul.y > 0.5 ? max((lodMultiplier * 0.441 + -0.0349999964) * 2, rawBaseColor.z) : min(lodColor.z, rawBaseColor.z);
	return lodColorMul.xxx * (lodColor - rawBaseColor.xyz) + rawBaseColor;
#		endif
}
#	endif

float GetSnowParameterY(float texProjTmp, float alpha)
{
	if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::BaseObjectIsSnow) {
		return min(1, texProjTmp + alpha);
	}
	return texProjTmp;
}

#	if defined(LOD)
#		undef EXTENDED_MATERIALS
#		undef WATER_BLENDING
#		undef LIGHT_LIMIT_FIX
#		undef WETNESS_EFFECTS
#		undef DYNAMIC_CUBEMAPS
#		undef WATER_EFFECTS
#	endif

#	if defined(WORLD_MAP)
#		undef CLOUD_SHADOWS
#		undef SKYLIGHTING
#	endif

#	if defined(WATER_EFFECTS)
#		include "WaterEffects/WaterCaustics.hlsli"
#	endif

#	if defined(EYE)
#		undef WETNESS_EFFECTS
#	endif

#	if defined(EXTENDED_MATERIALS) && !defined(LOD) && (defined(PARALLAX) || defined(LANDSCAPE) || defined(ENVMAP) || defined(TRUE_PBR))
#		define EMAT
#	endif

#	if defined(EMAT) && (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#		define EMAT_ENVMAP
#	endif

#	if defined(DYNAMIC_CUBEMAPS)
#		include "DynamicCubemaps/DynamicCubemaps.hlsli"
#	endif

#	if defined(TRUE_PBR)
#		include "Common/PBR.hlsli"
#	endif

#	if defined(EMAT)
#		include "ExtendedMaterials/ExtendedMaterials.hlsli"
#	endif

#	if defined(SCREEN_SPACE_SHADOWS)
#		include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	endif

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#		include "InverseSquareLighting/InverseSquareLighting.hlsli"
#	endif

#	if defined(TREE_ANIM)
#		undef WETNESS_EFFECTS
#	endif

#	if defined(WETNESS_EFFECTS)
#		include "WetnessEffects/WetnessEffects.hlsli"

// (42c) Character wetness (Wetness Effects > Characters). Permutations that can draw a
// character: skin and hair by technique, anything else only when Hooks.cpp flags the draw as
// actor geometry (SKINNED never reaches a pixel shader). Eyes, terrain and LOD never do, so
// their code is untouched.
#		if !defined(EYE) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE) && !defined(LOD) && !defined(LOD_LAND_BLEND)
#			define WET_CHARACTER_PERM
#		endif

// (44b) Anti-mirror for characters: wetness never pushes the G-buffer gloss (what SSRT / NRD / IBL
// read) above 0.8, i.e. roughness 0.2, unless the dry surface was already glossier.
float CapCharacterWetGloss(float wetGloss, float dryGloss, bool character)
{
	return character ? min(wetGloss, max(dryGloss, 0.8)) : wetGloss;
}

// (44c) G-buffer gloss of a wet character pixel. Gear in the Character Wetness New model
// (gearTarget >= 0): toward its film's gloss by the film's reflection energy, never above
// max(dry gloss, class cap) (cloth 0.55, leather 0.7, metal 0.75): wet never turns gear into a
// mirror, and metal that was already glossy keeps its own value. Otherwise the 42c / 44b rule.
float CharacterWetGBufferGloss(float dryGloss, float wetWeight, bool character, float gearTarget, float gearCap)
{
	[branch] if (gearTarget >= 0.0)
		return min(lerp(dryGloss, max(dryGloss, gearTarget), saturate(wetWeight)), max(dryGloss, gearCap));
	return CapCharacterWetGloss(lerp(dryGloss, max(dryGloss, wetWeight), wetWeight), dryGloss, character);
}

// Clear-coat style water film: GGX with water's F0 and a fixed roughness. Caller scales by the
// film weight; the base layer is attenuated separately.
float3 GetWetFilmSpecular(float3 N, float3 V, float3 L, float3 lightColor, float roughness)
{
	float3 H = normalize(V + L);
	float NdotL = saturate(dot(N, L));
	float NdotV = saturate(abs(dot(N, V)) + 1e-5);
	float NdotH = saturate(dot(N, H));
	float VdotH = saturate(dot(V, H));
	float D = BRDF::D_GGX(roughness, NdotH);
	float Vis = BRDF::Vis_SmithJointApprox(roughness, NdotV, max(NdotL, 1e-5));
	float F = BRDF::F_Schlick(0.02.xxx, VdotH).x;
	return (D * Vis * F * NdotL) * lightColor;
}
#	endif

// (batch 39, items 5-6) Surfaces that take accumulated snow and footprints: world geometry
// only. Skin, hair and eyes, the LOD noise overlay and the world map are left alone; actors and
// their gear are excluded per draw (IsActorGeometry). (batch 39c) Animated trees and foliage
// (TREE_ANIM) take snow too, through their own coverage (DynamicSnow::GetTreeCoverage, switch
// FlagTrees), and get no footprints.
#	if defined(DYNAMIC_SNOW) && !defined(SKINNED) && !defined(SKIN) && !defined(HAIR) && !defined(EYE) && !defined(LODLANDNOISE) && !defined(WORLD_MAP)
#		define DYNAMIC_SNOW_SURFACE
#		include "DynamicSnow/DynamicSnow.hlsli"
#	endif

#	if defined(TERRAIN_BLENDING)
#		include "TerrainBlending/TerrainBlending.hlsli"
#	endif

#	if defined(SSS) && defined(SKIN) && defined(DEFERRED)
#		undef SOFT_LIGHTING
#	endif

#	if defined(TERRAIN_SHADOWS)
#		include "TerrainShadows/TerrainShadows.hlsli"
#	endif

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	if defined(SKYLIGHTING)
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if defined(HAIR) && defined(CS_HAIR)
#		include "Hair/Hair.hlsli"
#	endif

#	if defined(TERRAIN_VARIATION)
#		include "TerrainVariation/TerrainVariation.hlsli"
#	endif

#	if defined(EXTENDED_TRANSLUCENCY) && !(defined(LOD) || defined(SKIN) || defined(HAIR) || defined(EYE) || defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS) || defined(DEPTH_WRITE_DECALS))
#		include "ExtendedTranslucency/ExtendedTranslucency.hlsli"
#		define ANISOTROPIC_ALPHA
#	endif

#	if defined(CS_SKIN)
#		include "Common/PBR.hlsli"
#		include "Skin/Skin.hlsli"
#	endif

// (46y) The point-light trace first: SSPLS.hlsli reads its traced-caster mark (Screen-Space Shadows for Non-RT Casters).
#	if defined(RT_POINT_SHADOWS) && defined(LIGHT_LIMIT_FIX)
#		include "RayTracing/PointLightShadows.hlsli"
#	endif

#	if defined(SSPLS)
#		include "ScreenSpacePointLightShadows/SSPLS.hlsli"
#	endif

#	if defined(PHYSICAL_SKY)
#		include "PhysicalSky/Common.hlsli"
#	endif

#	define LinearSampler SampColorSampler

#	include "Common/ShadowSampling.hlsli"

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout;
	uint eyeIndex = Stereo::GetEyeIndexPS(input.Position, VPOSOffset);

	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(input.WorldPosition.xyz, 1)).xyz;
	float3 viewDirection = -normalize(input.WorldPosition.xyz);

	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

#	if defined(DEFERRED)
	const bool inWorld = true;
#	else
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);
#	endif
	const bool inReflection = Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection;

	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewPosition.z);

#	if defined(SKINNED) || !defined(MODELSPACENORMALS)
	float3x3 tbn = float3x3(input.TBN0.xyz, input.TBN1.xyz, input.TBN2.xyz);

#		if !defined(TREE_ANIM) && !defined(LOD)
	// Fix incorrect vertex normals on double-sided meshes
	if (!frontFace)
		tbn = lerp(tbn, -tbn, nearFactor);
#		endif

	float3x3 tbnTr = transpose(tbn);

	tbnTr[0] = normalize(tbnTr[0]);
	tbnTr[1] = normalize(tbnTr[1]);
	tbnTr[2] = normalize(tbnTr[2]);

	tbn = transpose(tbnTr);

#	endif  // defined (SKINNED) || !defined (MODELSPACENORMALS)

#	if !defined(TRUE_PBR)
#		if defined(LANDSCAPE)
	float shininess = dot(input.LandBlendWeights1, LandscapeTexture1to4IsSpecPower) + input.LandBlendWeights2.x * LandscapeTexture5to6IsSpecPower.x + input.LandBlendWeights2.y * LandscapeTexture5to6IsSpecPower.y;
#		elif defined(SPECULAR)
	float shininess = SpecularColor.w;
#		else
	float shininess = 0.0;
#		endif  // defined (LANDSCAPE)
#	endif

#	if defined(TERRAIN_BLENDING)
	float depthSampled = TerrainBlending::TerrainBlendingMaskTexture[input.Position.xy].x;

	float depthSampledLinear = SharedData::GetScreenDepth(depthSampled);
	float depthPixelLinear = SharedData::GetScreenDepth(input.Position.z);

	float blendFactorTerrain = saturate((depthSampledLinear - depthPixelLinear) / 10.0);

	if (input.Position.z == depthSampled)
		blendFactorTerrain = 1;

	blendFactorTerrain = saturate(blendFactorTerrain);
#	endif

	float2 uv = input.TexCoord0.xy;
	float2 uvOriginal = uv;

#	if defined(EMAT)
	float parallaxShadowQuality = sqrt(1.0 - saturate(viewPosition.z / 2048.0));
#	endif

#	if defined(LANDSCAPE)
	float mipLevels[6];
#	else
	float mipLevel = 0;
#	endif  // LANDSCAPE
	float sh0 = 0;
	float pixelOffset = 0;

#	if defined(EMAT)
#		if defined(LANDSCAPE)
	DisplacementParams displacementParams[6];
	displacementParams[0].DisplacementScale = 1.f;
	displacementParams[0].DisplacementOffset = 0.f;
	displacementParams[0].HeightScale = 1;
	displacementParams[0].FlattenAmount = 0;
#		else
	DisplacementParams displacementParams;
	displacementParams.DisplacementScale = 1.f;
	displacementParams.DisplacementOffset = 0.f;
	displacementParams.HeightScale = 1;
	displacementParams.FlattenAmount = 0;
#		endif

#	endif

	float curvature = 0;
	float normalSmoothness = 0;

#	if !defined(MODELSPACENORMALS)
	float3 vertexNormal = tbnTr[2];
#		if defined(EMAT)

	if (SharedData::extendedMaterialSettings.EnableParallaxWarpingFix) {
		float3 ndx = ddx(vertexNormal);
		float3 ndy = ddy(vertexNormal);
		float3 fdx = ddx(input.WorldPosition.xyz);
		float3 fdy = ddy(input.WorldPosition.xyz);
		float fragSize = rcp(length(max(abs(fdx), abs(fdy))));
		curvature = pow(length(max(abs(ndx), abs(ndy))) * fragSize, 0.5);
		float3 flatWorldNormal = normalize(-cross(ddx(input.WorldPosition.xyz), ddy(input.WorldPosition.xyz)));
		normalSmoothness = (1 - dot(vertexNormal, flatWorldNormal));
#			if defined(LANDSCAPE)
		displacementParams[0].HeightScale = saturate(1 - curvature);
		displacementParams[0].FlattenAmount = (normalSmoothness + curvature);
#			else
		displacementParams.HeightScale = saturate(1 - curvature);
		displacementParams.FlattenAmount = (normalSmoothness + curvature);
#			endif
	}
#		endif
#	endif

	float3 entryNormal = 0;
	float3 entryNormalTS = 0;
	float eta = 1;
	float3 refractedViewDirection = viewDirection;
	float4 sampledCoatColor = PBRParams2;
	float3 complexSpecular = 1.0;  // Declare complexSpecular at a higher scope so it's available throughout the shader (NEEDED FOR STOCH. FIX)
#	if defined(EMAT)
#		if defined(PARALLAX)
	if (SharedData::extendedMaterialSettings.EnableParallax) {
		mipLevel = ExtendedMaterials::GetMipLevel(uv, TexParallaxSampler);
		uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, viewDirection, tbnTr, screenNoise, TexParallaxSampler, SampParallaxSampler, 0, displacementParams, pixelOffset);
		if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows))
			sh0 = TexParallaxSampler.SampleLevel(SampParallaxSampler, uv, mipLevel).x;
	}
#		endif  // PARALLAX

	bool complexMaterial = false;
	bool complexMaterialParallax = false;
	float4 complexMaterialColor = 1.0;

#		if defined(EMAT_ENVMAP)

	if (SharedData::extendedMaterialSettings.EnableComplexMaterial) {
		float envMaskTest = TexEnvMaskSampler.SampleLevel(SampEnvMaskSampler, uv, 15).w;
		complexMaterial = envMaskTest < (1.0 - (4.0 / 255.0));

		if (complexMaterial) {
			if (envMaskTest > (4.0 / 255.0)) {
				complexMaterialParallax = true;
				mipLevel = ExtendedMaterials::GetMipLevel(uv, TexEnvMaskSampler);
				uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, viewDirection, tbnTr, screenNoise, TexEnvMaskSampler, SampTerrainParallaxSampler, 3, displacementParams, pixelOffset);
				if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows))
					sh0 = TexEnvMaskSampler.SampleLevel(SampEnvMaskSampler, uv, mipLevel).w;
			}

			complexMaterialColor = TexEnvMaskSampler.SampleBias(SampEnvMaskSampler, uv, SharedData::SpecularMipBias());
		}
	}

#		endif  // ENVMAP

#		if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	bool PBRParallax = false;
	[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture0) != 0)
	{
		float4 sampledCoatProperties = TexRimSoftLightWorldMapOverlaySampler.SampleBias(SampRimSoftLightWorldMapOverlaySampler, uv, SharedData::MaterialMipBias());
		sampledCoatColor.rgb *= Color::Diffuse(sampledCoatProperties.rgb);
		sampledCoatColor.a *= sampledCoatProperties.a;
	}
	[branch] if (SharedData::extendedMaterialSettings.EnableParallax && (PBRFlags & PBR::Flags::HasDisplacement) != 0)
	{
		PBRParallax = true;
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			displacementParams.HeightScale = PBRParams1.y;
			displacementParams.DisplacementScale = 0.5;
			displacementParams.DisplacementOffset = -0.25;

			eta = lerp(1.0, (1 - sqrt(MultiLayerParallaxData.y)) / (1 + sqrt(MultiLayerParallaxData.y)), sampledCoatColor.w);
			[branch] if ((PBRFlags & PBR::Flags::CoatNormal) != 0)
			{
				entryNormalTS = normalize(TransformNormal(TexBackLightSampler.SampleBias(SampBackLightSampler, uvOriginal, SharedData::MaterialMipBias()).xyz));
			}
			else
			{
				entryNormalTS = normalize(TransformNormal(TexNormalSampler.SampleBias(SampNormalSampler, uvOriginal, SharedData::MaterialMipBias()).xyz));
			}
			entryNormal = normalize(mul(tbn, entryNormalTS));
			refractedViewDirection = -refract(-viewDirection, entryNormal, eta);
		}
		else
		{
			displacementParams.HeightScale *= PBRParams1.y;
		}
		mipLevel = ExtendedMaterials::GetMipLevel(uv, TexParallaxSampler);
		uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, refractedViewDirection, tbnTr, screenNoise, TexParallaxSampler, SampParallaxSampler, 0, displacementParams, pixelOffset);
		if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows))
			sh0 = TexParallaxSampler.SampleLevel(SampParallaxSampler, uv, mipLevel).x;
	}
#		endif  // TRUE_PBR

#	endif  // EMAT

#	if defined(SNOW)
	bool useSnowSpecular = true;
#	else
	bool useSnowSpecular = false;
#	endif  // SNOW

#	if defined(SPARKLE) || !defined(PROJECTED_UV)
	bool useSnowDecalSpecular = true;
#	else
	bool useSnowDecalSpecular = false;
#	endif  // defined(SPARKLE) || !defined(PROJECTED_UV)

	float2 diffuseUv = uv;

#	if defined(SPARKLE)
	diffuseUv = ProjectedUVParams2.yy * input.TexCoord0.zw;
#	endif  // SPARKLE

#	if defined(LANDSCAPE)
	// Normalise blend weights
	float totalWeight = input.LandBlendWeights1.x + input.LandBlendWeights1.y + input.LandBlendWeights1.z +
	                    input.LandBlendWeights1.w + input.LandBlendWeights2.x + input.LandBlendWeights2.y;
	if (totalWeight > 0.0) {
		input.LandBlendWeights1 /= totalWeight;
		input.LandBlendWeights2.xy /= totalWeight;
	}
	float3 blendedRGB = 0;
	float blendedAlpha = 0;
	float3 blendedNormalRGB = 0;
	float blendedNormalAlpha = 0;

#		if defined(TRUE_PBR)
	float4 blendedRMAOS = 0;
#		endif

	// Compute stochastic offsets and derivatives once for all layers (only when terrain variation is enabled)
#		if defined(TERRAIN_VARIATION)
			bool useTerrainVariation = SharedData::terrainVariationSettings.enableTilingFix;
			// Initialise dx, dy, and sharedOffset for when Terrain Variation is disabled via enableTilingFix but still #defined
			float2 dx = 0, dy = 0;
			StochasticOffsets sharedOffset;
			sharedOffset.offset1 = float2(0, 0);
			sharedOffset.offset2 = float2(0, 0);
			sharedOffset.offset3 = float2(0, 0);
			sharedOffset.weights = float3(0, 0, 0);
			[branch] if (useTerrainVariation)
			{
				dx = ddx(input.TexCoord0.zw);
				dy = ddy(input.TexCoord0.zw);
				sharedOffset = ComputeStochasticOffsets(input.TexCoord0.zw);
			}
#		endif

#		if defined(EMAT)
#			if defined(TRUE_PBR)
	if (SharedData::extendedMaterialSettings.EnableParallax) {
#			else
	if (SharedData::extendedMaterialSettings.EnableTerrainParallax || (SharedData::extendedMaterialSettings.EnableParallax && Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement)) {
#			endif
		mipLevels[0] = ExtendedMaterials::GetMipLevel(uv, TexColorSampler);
		mipLevels[1] = ExtendedMaterials::GetMipLevel(uv, TexLandColor2Sampler);
		mipLevels[2] = ExtendedMaterials::GetMipLevel(uv, TexLandColor3Sampler);
		mipLevels[3] = ExtendedMaterials::GetMipLevel(uv, TexLandColor4Sampler);
		mipLevels[4] = ExtendedMaterials::GetMipLevel(uv, TexLandColor5Sampler);
		mipLevels[5] = ExtendedMaterials::GetMipLevel(uv, TexLandColor6Sampler);

		displacementParams[1] = displacementParams[0];
		displacementParams[2] = displacementParams[0];
		displacementParams[3] = displacementParams[0];
		displacementParams[4] = displacementParams[0];
		displacementParams[5] = displacementParams[0];
#			if defined(TRUE_PBR)
		displacementParams[0].HeightScale *= PBRParams1.y;
		displacementParams[1].HeightScale *= LandscapeTexture2PBRParams.y;
		displacementParams[2].HeightScale *= LandscapeTexture3PBRParams.y;
		displacementParams[3].HeightScale *= LandscapeTexture4PBRParams.y;
		displacementParams[4].HeightScale *= LandscapeTexture5PBRParams.y;
		displacementParams[5].HeightScale *= LandscapeTexture6PBRParams.y;
#			endif

		float weights[6];
		// Initialize weights array
		weights[0] = weights[1] = weights[2] = weights[3] = weights[4] = weights[5] = 0.0;
#			if defined(TERRAIN_VARIATION)
		uv = ExtendedMaterials::GetParallaxCoords(input, viewPosition.z, uv, mipLevels, viewDirection, tbnTr, screenNoise, displacementParams, sharedOffset, dx, dy, pixelOffset, weights);
#			else
		uv = ExtendedMaterials::GetParallaxCoords(input, viewPosition.z, uv, mipLevels, viewDirection, tbnTr, screenNoise, displacementParams, pixelOffset, weights);
#			endif
		if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
			input.LandBlendWeights1.x = weights[0];
			input.LandBlendWeights1.y = weights[1];
			input.LandBlendWeights1.z = weights[2];
			input.LandBlendWeights1.w = weights[3];
			input.LandBlendWeights2.x = weights[4];
			input.LandBlendWeights2.y = weights[5];
		}
		if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows)) {
#			if defined(TERRAIN_VARIATION)
			sh0 = ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, displacementParams, parallaxShadowQuality, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, dx, dy, weights);
			float shadowMultiplier = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, DirLightDirection, sh0, parallaxShadowQuality, screenNoise, displacementParams, sharedOffset, dx, dy);
#			else
			sh0 = ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, displacementParams, parallaxShadowQuality, input.LandBlendWeights1, input.LandBlendWeights2.xy, weights);
#			endif
		}
	}

#		if defined(TERRAIN_VARIATION)
	else if (useTerrainVariation) {
		// Calculate proper mip levels for terrain variation when parallax is disabled but EMAT is available
		mipLevels[0] = ExtendedMaterials::GetMipLevel(uv, TexColorSampler);
		mipLevels[1] = ExtendedMaterials::GetMipLevel(uv, TexLandColor2Sampler);
		mipLevels[2] = ExtendedMaterials::GetMipLevel(uv, TexLandColor3Sampler);
		mipLevels[3] = ExtendedMaterials::GetMipLevel(uv, TexLandColor4Sampler);
		mipLevels[4] = ExtendedMaterials::GetMipLevel(uv, TexLandColor5Sampler);
		mipLevels[5] = ExtendedMaterials::GetMipLevel(uv, TexLandColor6Sampler);
	}
#		endif
#		else
	// Initialize mip levels for non-EMAT case
	mipLevels[0] = mipLevels[1] = mipLevels[2] = mipLevels[3] = mipLevels[4] = mipLevels[5] = 0.0;
#		endif  // EMAT
#	endif      // LANDSCAPE

#	if defined(SPARKLE)
	diffuseUv = ProjectedUVParams2.yy * (input.TexCoord0.zw + (uv - uvOriginal));
#	else
	diffuseUv = uv;
#	endif  // SPARKLE

	float4 baseColor = 0;
	float4 normal = 0;
	float glossiness = 0;
#	if defined(CS_SKIN)
	const bool skinEnabled = SharedData::skinData.skinParams.w > 0.0f;
#		if defined(SKIN)
	float skinRoughness = 0;
	float skinSpecular = 0;
	float skinFuzzMask = 1;
	float skinWetMask = 1;
	float skinAO = 1;
	bool skinRoughnessSet = false;
#		endif
#	endif

	float4 rawRMAOS = 0;

	float4 glintParameters = 0;

#	if defined(SNOW)  // Earlier snow definition for Terrain Variation rework.
#		if !defined(TRUE_PBR)
	float landSnowMask = 0.0;
#			if defined(LANDSCAPE)
	landSnowMask = GetLandSnowMaskValue(baseColor.w);
#			endif
#		endif
#	endif

#	if defined(LANDSCAPE)
	// Layer 1 (LandBlendWeights1.x)
	if (input.LandBlendWeights1.x > 0.01) {
		float weight = input.LandBlendWeights1.x;

		// Sample diffuse texture for layer 1
#		if defined(TERRAIN_VARIATION)
		float4 landColor1;
		[branch] if (useTerrainVariation)
		{
			landColor1 = StochasticEffect(TexColorSampler, SampColorSampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landColor1 = TexColorSampler.SampleBias(SampColorSampler, uv, SharedData::MipBias);
		}
#		else
		float4 landColor1 = TexColorSampler.SampleBias(SampColorSampler, uv, SharedData::MipBias);
#		endif
		float3 landColorRGB1 = landColor1.rgb;
#		if defined(TRUE_PBR)
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile0PBR) == 0)
		{
			landColorRGB1 = Color::GammaToTrueLinear(landColorRGB1 / Color::PBRLightingScale);
		}
#		endif
		float landAlpha1 = landColor1.a;
		float landSnowMask1 = GetLandSnowMaskValue(landColor1.w);

		// Sample normal texture for layer 1
#		if defined(TERRAIN_VARIATION)
		float4 landNormal1;
		[branch] if (useTerrainVariation)
		{
			landNormal1 = StochasticEffect(TexNormalSampler, SampNormalSampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landNormal1 = TexNormalSampler.SampleBias(SampNormalSampler, uv, SharedData::MipBias);
		}
#		else
		float4 landNormal1 = TexNormalSampler.SampleBias(SampNormalSampler, uv, SharedData::MipBias);
#		endif
		float3 landNormalRGB1 = landNormal1.rgb;
		float landNormalAlpha1 = landNormal1.a;
#		if defined(SNOW) && !defined(TRUE_PBR)
		landSnowMask += LandscapeTexture1to4IsSnow.x * input.LandBlendWeights1.x * landSnowMask1;
#		endif  // SNOW

		// Sample RMAOS texture for layer 1
#		if defined(TRUE_PBR)
		float4 landRMAOS1;
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile0PBR) != 0)
		{
#			if defined(TERRAIN_VARIATION)
			[branch] if (useTerrainVariation)
			{
				landRMAOS1 = StochasticEffect(TexRMAOSSampler, SampRMAOSSampler, uv, sharedOffset, dx, dy) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
			}
			else
			{
				landRMAOS1 = TexRMAOSSampler.SampleBias(SampRMAOSSampler, uv, SharedData::MipBias) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
			}
#			else
			landRMAOS1 = TexRMAOSSampler.SampleBias(SampRMAOSSampler, uv, SharedData::MipBias) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
#			endif
			if ((PBRFlags & PBR::TerrainFlags::LandTile0HasGlint) != 0) {
				glintParameters += weight * LandscapeTexture1GlintParameters;
			}
		}
		else
		{
			landRMAOS1 = input.LandBlendWeights1.x * float4(1 - glossiness.x, 0, 1, 0);
		}
		blendedRMAOS += landRMAOS1 * weight;
#		endif
		blendedRGB += landColorRGB1 * weight;
		blendedAlpha += landAlpha1 * weight;
		blendedNormalRGB += landNormalRGB1 * weight;
		blendedNormalAlpha += landNormalAlpha1 * weight;
	}

	// Layer 2 (LandBlendWeights1.y)
	if (input.LandBlendWeights1.y > 0.01) {
		float weight = input.LandBlendWeights1.y;

		// Sample diffuse texture for layer 2
#		if defined(TERRAIN_VARIATION)
		float4 landColor2;
		[branch] if (useTerrainVariation)
		{
			landColor2 = StochasticEffect(TexLandColor2Sampler, SampLandColor2Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landColor2 = TexLandColor2Sampler.SampleBias(SampLandColor2Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landColor2 = TexLandColor2Sampler.SampleBias(SampLandColor2Sampler, uv, SharedData::MipBias);
#		endif
		float3 landColorRGB2 = landColor2.rgb;
#		if defined(TRUE_PBR)
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile1PBR) == 0)
		{
			landColorRGB2 = Color::GammaToTrueLinear(landColorRGB2 / Color::PBRLightingScale);
		}
#		endif
		float landAlpha2 = landColor2.a;
		float landSnowMask2 = GetLandSnowMaskValue(landColor2.w);

		// Sample normal texture for layer 2
#		if defined(TERRAIN_VARIATION)
		float4 landNormal2;
		[branch] if (useTerrainVariation)
		{
			landNormal2 = StochasticEffect(TexLandNormal2Sampler, SampLandNormal2Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landNormal2 = TexLandNormal2Sampler.SampleBias(SampLandNormal2Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landNormal2 = TexLandNormal2Sampler.SampleBias(SampLandNormal2Sampler, uv, SharedData::MipBias);
#		endif
		float3 landNormalRGB2 = landNormal2.rgb;
		float landNormalAlpha2 = landNormal2.a;
#		if defined(SNOW) && !defined(TRUE_PBR)
		landSnowMask += LandscapeTexture1to4IsSnow.y * input.LandBlendWeights1.y * landSnowMask2;
#		endif  // SNOW

		// Sample RMAOS texture for layer 2
#		if defined(TRUE_PBR)
		float4 landRMAOS2;
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile1PBR) != 0)
		{
#			if defined(TERRAIN_VARIATION)
			[branch] if (useTerrainVariation)
			{
				landRMAOS2 = StochasticEffect(TexLandRMAOS2Sampler, SampLandRMAOS2Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture2PBRParams.x, 1, 1, LandscapeTexture2PBRParams.z);
			}
			else
			{
				landRMAOS2 = TexLandRMAOS2Sampler.SampleBias(SampLandRMAOS2Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture2PBRParams.x, 1, 1, LandscapeTexture2PBRParams.z);
			}
#			else
			landRMAOS2 = TexLandRMAOS2Sampler.SampleBias(SampLandRMAOS2Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture2PBRParams.x, 1, 1, LandscapeTexture2PBRParams.z);
#			endif
			if ((PBRFlags & PBR::TerrainFlags::LandTile1HasGlint) != 0) {
				glintParameters += weight * LandscapeTexture2GlintParameters;
			}
		}
		else
		{
			landRMAOS2 = input.LandBlendWeights1.y * float4(1 - glossiness.x, 0, 1, 0);
		}
		blendedRMAOS += landRMAOS2 * weight;
#		endif
		blendedRGB += landColorRGB2 * weight;
		blendedAlpha += landAlpha2 * weight;
		blendedNormalRGB += landNormalRGB2 * weight;
		blendedNormalAlpha += landNormalAlpha2 * weight;
	}

	// Layer 3 (LandBlendWeights1.z)
	if (input.LandBlendWeights1.z > 0.01) {
		float weight = input.LandBlendWeights1.z;
		// Sample diffuse texture for layer 3
#		if defined(TERRAIN_VARIATION)
		float4 landColor3;
		[branch] if (useTerrainVariation)
		{
			landColor3 = StochasticEffect(TexLandColor3Sampler, SampLandColor3Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landColor3 = TexLandColor3Sampler.SampleBias(SampLandColor3Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landColor3 = TexLandColor3Sampler.SampleBias(SampLandColor3Sampler, uv, SharedData::MipBias);
#		endif
		float3 landColorRGB3 = landColor3.rgb;
#		if defined(TRUE_PBR)
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile2PBR) == 0)
		{
			landColorRGB3 = Color::GammaToTrueLinear(landColorRGB3 / Color::PBRLightingScale);
		}
#		endif
		float landAlpha3 = landColor3.a;
		float landSnowMask3 = GetLandSnowMaskValue(landColor3.w);

		// Sample normal texture for layer 3
#		if defined(TERRAIN_VARIATION)
		float4 landNormal3;
		[branch] if (useTerrainVariation)
		{
			landNormal3 = StochasticEffect(TexLandNormal3Sampler, SampLandNormal3Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landNormal3 = TexLandNormal3Sampler.SampleBias(SampLandNormal3Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landNormal3 = TexLandNormal3Sampler.SampleBias(SampLandNormal3Sampler, uv, SharedData::MipBias);
#		endif
		float3 landNormalRGB3 = landNormal3.rgb;
		float landNormalAlpha3 = landNormal3.a;
#		if defined(SNOW) && !defined(TRUE_PBR)
		landSnowMask += LandscapeTexture1to4IsSnow.z * input.LandBlendWeights1.z * landSnowMask3;
#		endif  // SNOW

		// Sample RMAOS texture for layer 3
#		if defined(TRUE_PBR)
		float4 landRMAOS3;
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile2PBR) != 0)
		{
#			if defined(TERRAIN_VARIATION)
			[branch] if (useTerrainVariation)
			{
				landRMAOS3 = StochasticEffect(TexLandRMAOS3Sampler, SampLandRMAOS3Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture3PBRParams.x, 1, 1, LandscapeTexture3PBRParams.z);
			}
			else
			{
				landRMAOS3 = TexLandRMAOS3Sampler.SampleBias(SampLandRMAOS3Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture3PBRParams.x, 1, 1, LandscapeTexture3PBRParams.z);
			}
#			else
			landRMAOS3 = TexLandRMAOS3Sampler.SampleBias(SampLandRMAOS3Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture3PBRParams.x, 1, 1, LandscapeTexture3PBRParams.z);
#			endif
			if ((PBRFlags & PBR::TerrainFlags::LandTile2HasGlint) != 0) {
				glintParameters += weight * LandscapeTexture3GlintParameters;
			}
		}
		else
		{
			landRMAOS3 = input.LandBlendWeights1.z * float4(1 - glossiness.x, 0, 1, 0);
		}
		blendedRMAOS += landRMAOS3 * weight;
#		endif
		blendedRGB += landColorRGB3 * weight;
		blendedAlpha += landAlpha3 * weight;
		blendedNormalRGB += landNormalRGB3 * weight;
		blendedNormalAlpha += landNormalAlpha3 * weight;
	}
	// Layer 4 (LandBlendWeights1.w)
	if (input.LandBlendWeights1.w > 0.01) {
		float weight = input.LandBlendWeights1.w;

		// Sample diffuse texture for layer 4
#		if defined(TERRAIN_VARIATION)
		float4 landColor4;
		[branch] if (useTerrainVariation)
		{
			landColor4 = StochasticEffect(TexLandColor4Sampler, SampLandColor4Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landColor4 = TexLandColor4Sampler.SampleBias(SampLandColor4Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landColor4 = TexLandColor4Sampler.SampleBias(SampLandColor4Sampler, uv, SharedData::MipBias);
#		endif
		float3 landColorRGB4 = landColor4.rgb;
#		if defined(TRUE_PBR)
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile3PBR) == 0)
		{
			landColorRGB4 = Color::GammaToTrueLinear(landColorRGB4 / Color::PBRLightingScale);
		}
#		endif
		float landAlpha4 = landColor4.a;
		float landSnowMask4 = GetLandSnowMaskValue(landColor4.w);

		// Sample normal texture for layer 4
#		if defined(TERRAIN_VARIATION)
		float4 landNormal4;
		[branch] if (useTerrainVariation)
		{
			landNormal4 = StochasticEffect(TexLandNormal4Sampler, SampLandNormal4Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landNormal4 = TexLandNormal4Sampler.SampleBias(SampLandNormal4Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landNormal4 = TexLandNormal4Sampler.SampleBias(SampLandNormal4Sampler, uv, SharedData::MipBias);
#		endif
		float3 landNormalRGB4 = landNormal4.rgb;
		float landNormalAlpha4 = landNormal4.a;
#		if defined(SNOW) && !defined(TRUE_PBR)
		landSnowMask += LandscapeTexture1to4IsSnow.w * input.LandBlendWeights1.w * landSnowMask4;
#		endif  // SNOW

		// Sample RMAOS texture for layer 4
#		if defined(TRUE_PBR)
		float4 landRMAOS4;
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile3PBR) != 0)
		{
#			if defined(TERRAIN_VARIATION)
			[branch] if (useTerrainVariation)
			{
				landRMAOS4 = StochasticEffect(TexLandRMAOS4Sampler, SampLandRMAOS4Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture4PBRParams.x, 1, 1, LandscapeTexture4PBRParams.z);
			}
			else
			{
				landRMAOS4 = TexLandRMAOS4Sampler.SampleBias(SampLandRMAOS4Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture4PBRParams.x, 1, 1, LandscapeTexture4PBRParams.z);
			}
#			else
			landRMAOS4 = TexLandRMAOS4Sampler.SampleBias(SampLandRMAOS4Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture4PBRParams.x, 1, 1, LandscapeTexture4PBRParams.z);
#			endif
			if ((PBRFlags & PBR::TerrainFlags::LandTile3HasGlint) != 0) {\
				glintParameters += weight * LandscapeTexture4GlintParameters;
			}
		}
		else
		{
			landRMAOS4 = input.LandBlendWeights1.w * float4(1 - glossiness.x, 0, 1, 0);
		}
		blendedRMAOS += landRMAOS4 * weight;
#		endif
		blendedRGB += landColorRGB4 * weight;
		blendedAlpha += landAlpha4 * weight;
		blendedNormalRGB += landNormalRGB4 * weight;
		blendedNormalAlpha += landNormalAlpha4 * weight;
	}

	// Layer 5 (LandBlendWeights2.x)
	if (input.LandBlendWeights2.x > 0.01) {
		float weight = input.LandBlendWeights2.x;
		// Sample diffuse texture for layer 5
#		if defined(TERRAIN_VARIATION)
		float4 landColor5;
		[branch] if (useTerrainVariation)
		{
			landColor5 = StochasticEffect(TexLandColor5Sampler, SampLandColor5Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landColor5 = TexLandColor5Sampler.SampleBias(SampLandColor5Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landColor5 = TexLandColor5Sampler.SampleBias(SampLandColor5Sampler, uv, SharedData::MipBias);
#		endif
		float3 landColorRGB5 = landColor5.rgb;
#		if defined(TRUE_PBR)
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile4PBR) == 0)
		{
			landColorRGB5 = Color::GammaToTrueLinear(landColorRGB5 / Color::PBRLightingScale);
		}
#		endif
		float landAlpha5 = landColor5.a;
		float landSnowMask5 = GetLandSnowMaskValue(landColor5.w);

		// Sample normal texture for layer 5
#		if defined(TERRAIN_VARIATION)
		float4 landNormal5;
		[branch] if (useTerrainVariation)
		{
			landNormal5 = StochasticEffect(TexLandNormal5Sampler, SampLandNormal5Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landNormal5 = TexLandNormal5Sampler.SampleBias(SampLandNormal5Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landNormal5 = TexLandNormal5Sampler.SampleBias(SampLandNormal5Sampler, uv, SharedData::MipBias);
#		endif
		float3 landNormalRGB5 = landNormal5.rgb;
		float landNormalAlpha5 = landNormal5.a;

#		if defined(SNOW) && !defined(TRUE_PBR)
		landSnowMask += LandscapeTexture5to6IsSnow.x * input.LandBlendWeights2.x * landSnowMask5;
#		endif  // SNOW

		// Sample RMAOS texture for layer 5
#		if defined(TRUE_PBR)
		float4 landRMAOS5;
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile4PBR) != 0)
		{
#			if defined(TERRAIN_VARIATION)
			[branch] if (useTerrainVariation)
			{
				landRMAOS5 = StochasticEffect(TexLandRMAOS5Sampler, SampLandRMAOS5Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture5PBRParams.x, 1, 1, LandscapeTexture5PBRParams.z);
			}
			else
			{
				landRMAOS5 = TexLandRMAOS5Sampler.SampleBias(SampLandRMAOS5Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture5PBRParams.x, 1, 1, LandscapeTexture5PBRParams.z);
			}
#			else
			landRMAOS5 = TexLandRMAOS5Sampler.SampleBias(SampLandRMAOS5Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture5PBRParams.x, 1, 1, LandscapeTexture5PBRParams.z);
#			endif
			if ((PBRFlags & PBR::TerrainFlags::LandTile4HasGlint) != 0) {
				glintParameters += weight * LandscapeTexture5GlintParameters;
			}
		}
		else
		{
			landRMAOS5 = input.LandBlendWeights2.x * float4(1 - glossiness.x, 0, 1, 0);
		}
		blendedRMAOS += landRMAOS5 * weight;
#		endif
		blendedRGB += landColorRGB5 * weight;
		blendedAlpha += landAlpha5 * weight;
		blendedNormalRGB += landNormalRGB5 * weight;
		blendedNormalAlpha += landNormalAlpha5 * weight;
	}
	// Layer 6 (LandBlendWeights2.y)
	if (input.LandBlendWeights2.y > 0.01) {
		float weight = input.LandBlendWeights2.y;

		// Sample layer 6 textures
#		if defined(TERRAIN_VARIATION)
		float4 landColor6;
		[branch] if (useTerrainVariation)
		{
			landColor6 = StochasticEffect(TexLandColor6Sampler, SampLandColor6Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landColor6 = TexLandColor6Sampler.SampleBias(SampLandColor6Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landColor6 = TexLandColor6Sampler.SampleBias(SampLandColor6Sampler, uv, SharedData::MipBias);
#		endif
		float3 landColorRGB6 = landColor6.rgb;
#		if defined(TRUE_PBR)
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile5PBR) == 0)
		{
			landColorRGB6 = Color::GammaToTrueLinear(landColorRGB6 / Color::PBRLightingScale);
		}
#		endif
		float landAlpha6 = landColor6.a;
		float landSnowMask6 = GetLandSnowMaskValue(landColor6.w);

		// Sample normal texture for layer 6
#		if defined(TERRAIN_VARIATION)
		float4 landNormal6;
		[branch] if (useTerrainVariation)
		{
			landNormal6 = StochasticEffect(TexLandNormal6Sampler, SampLandNormal6Sampler, uv, sharedOffset, dx, dy);
		}
		else
		{
			landNormal6 = TexLandNormal6Sampler.SampleBias(SampLandNormal6Sampler, uv, SharedData::MipBias);
		}
#		else
		float4 landNormal6 = TexLandNormal6Sampler.SampleBias(SampLandNormal6Sampler, uv, SharedData::MipBias);
#		endif
		float3 landNormalRGB6 = landNormal6.rgb;
		float landNormalAlpha6 = landNormal6.a;
#		if defined(SNOW) && !defined(TRUE_PBR)
		landSnowMask += LandscapeTexture5to6IsSnow.y * 	input.LandBlendWeights2.y * landSnowMask6;
#		endif  // SNOW

		// Sample RMAOS texture for layer 6
#		if defined(TRUE_PBR)
		float4 landRMAOS6;
		[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile5PBR) != 0)
		{
#			if defined(TERRAIN_VARIATION)
			[branch] if (useTerrainVariation)
			{
				landRMAOS6 = StochasticEffect(TexLandRMAOS6Sampler, SampLandRMAOS6Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture6PBRParams.x, 1, 1, LandscapeTexture6PBRParams.z);
			}
			else
			{
				landRMAOS6 = TexLandRMAOS6Sampler.SampleBias(SampLandRMAOS6Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture6PBRParams.x, 1, 1, LandscapeTexture6PBRParams.z);
			}
#			else
			landRMAOS6 = TexLandRMAOS6Sampler.SampleBias(SampLandRMAOS6Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture6PBRParams.x, 1, 1, LandscapeTexture6PBRParams.z);
#			endif
			if ((PBRFlags & PBR::TerrainFlags::LandTile5HasGlint) != 0) {
				glintParameters += weight * LandscapeTexture6GlintParameters;
			}
		}
		else
		{
			landRMAOS6 = input.LandBlendWeights2.y * float4(1 - glossiness.x, 0, 1, 0);
		}
		blendedRMAOS += landRMAOS6 * weight;
#		endif
		blendedRGB += landColorRGB6 * weight;
		blendedAlpha += landAlpha6 * weight;
		blendedNormalRGB += landNormalRGB6 * weight;
		blendedNormalAlpha += landNormalAlpha6 * weight;
	}

	float4 rawBaseColor = float4(blendedRGB, blendedAlpha);
	baseColor = float4(Color::Diffuse(blendedRGB), blendedAlpha);
	normal = float4(blendedNormalRGB, blendedNormalAlpha);
#		if defined(TRUE_PBR)
	rawRMAOS = blendedRMAOS;
#		endif
#	else  // Non-landscape code
	float4 rawBaseColor = TexColorSampler.SampleBias(SampColorSampler, diffuseUv, SharedData::MipBias);
	baseColor = float4(Color::Diffuse(rawBaseColor.rgb), rawBaseColor.a);
	float4 normalColor = TexNormalSampler.SampleBias(SampNormalSampler, uv, SharedData::MipBias);
	normal = normalColor;
#		if defined(TRUE_PBR)
	rawRMAOS = TexRMAOSSampler.SampleBias(SampRMAOSSampler, diffuseUv, SharedData::MipBias) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
		if ((PBRFlags & PBR::Flags::Glint) != 0) {
				glintParameters = MultiLayerParallaxData;
			}
#		endif
#	endif


#	if defined(LOD_BLENDING)
#		if defined(LODOBJECTS) || defined(LODOBJECTSHD)
	baseColor.xyz = pow(abs(baseColor.xyz), SharedData::lodBlendingSettings.LODObjectGamma) * SharedData::lodBlendingSettings.LODObjectBrightness;
#		elif defined(LODLANDSCAPE)
	// First apply terrain variation if enabled
	#			if defined(TERRAIN_VARIATION)
		if (SharedData::terrainVariationSettings.enableLODTerrainTilingFix) {
			float2 dx = ddx(uv);
			float2 dy = ddy(uv);
			StochasticOffsets lodOffset = ComputeStochasticOffsetsLOD(uv);
			float4 lodStochasticColor = StochasticSampleLOD(screenNoise, TexColorSampler, SampColorSampler, uv, lodOffset, dx, dy);

			// Apply the stochastic result directly
			baseColor.xyz = Color::Diffuse(lodStochasticColor.rgb);
		}
#			endif
	baseColor.xyz = pow(abs(baseColor.xyz), SharedData::lodBlendingSettings.LODTerrainGamma) * SharedData::lodBlendingSettings.LODTerrainBrightness;
#		elif defined(BACK_LIGHTING)
	// Grass LOD. It reaches none of the branches above: the engine hands out the LOD defines
	// from the geometry's own LOD property flags and merged grass LOD carries none of them, so
	// the class has to be identified per draw instead (Hooks.cpp, IsLODGrass).
	//
	// Gated on BACK_LIGHTING to keep this out of the permutations that cannot be grass LOD.
	// Grass LOD billboards are back-lit, and of the 553 Lighting permutations observed in a
	// real playthrough only 88 carry that flag -- the other 465 stay byte-identical.
	//
	// (44h) Gamma, then a brightness multiplier, in the same order as the LOD object branch above.
	// The value also lands in the deferred albedo that screen-space GI reads back, so brightness
	// above 1 can push that albedo past 1, exactly as LOD Object Brightness already can.
	if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsLODGrass) {
		baseColor.xyz = pow(abs(baseColor.xyz), SharedData::lodBlendingSettings.LODGrassGamma) * SharedData::lodBlendingSettings.LODGrassBrightness;
	}
#		endif
#	endif  // LOD_BLENDING

#	if defined(SKIN) && defined(CS_SKIN)
	float4 skinsk = 0;
	float4 skinExtra = 0;
	float4 skinWetnessSample = 0;
	uint2 skinExtraDimensions = uint2(0, 0);
	uint2 wetnessDimensions = uint2(0, 0);
	bool hasSkinExtra = false;
	bool hasSkinWetness = false;
	if (skinEnabled) {
		skinsk = TexRimSoftLightWorldMapOverlaySampler.SampleBias(SampRimSoftLightWorldMapOverlaySampler, uv, SharedData::MaterialMipBias());
		TexSkinExtraSampler.GetDimensions(skinExtraDimensions.x, skinExtraDimensions.y);
		TexSkinWetnessSampler.GetDimensions(wetnessDimensions.x, wetnessDimensions.y);
		hasSkinExtra = skinExtraDimensions.x > 32 && skinExtraDimensions.y > 32;
		hasSkinWetness = wetnessDimensions.x > 32 && wetnessDimensions.y > 32;
	}
	float4 skinWetnessNormal = float4(0.f, 0.f, 0.f, 1.f);

	if (hasSkinExtra && SharedData::skinData.skinParams.x > 0.0f) {
		skinExtra = TexSkinExtraSampler.SampleBias(SampColorSampler, uv, SharedData::MaterialMipBias());
		skinRoughness = skinExtra.x;
		skinFuzzMask = skinExtra.y;
		skinAO = skinExtra.z;
		skinSpecular = skinExtra.w;
		skinRoughnessSet = true;
	} else {
		skinRoughnessSet = false;
	}
	if (hasSkinWetness && skinEnabled) {
		skinWetnessSample = TexSkinWetnessSampler.SampleBias(SampColorSampler, uv, SharedData::MaterialMipBias());
		if ((skinWetnessSample.y == 0 && skinWetnessSample.z == 0) || (skinWetnessSample.x == skinWetnessSample.y && skinWetnessSample.y == skinWetnessSample.z && skinWetnessSample.w >= 0.99f)) {
			skinWetMask = skinWetnessSample.x;
			skinWetnessNormal.xyz = Skin::CalculateNormalFromHeight(skinWetMask, SharedData::skinData.wetParams.w * 0.0001, uv) * 0.5 + 0.5;
		} else {
			skinWetnessNormal.xyz = skinWetnessSample.xyz;
			skinWetMask = skinWetnessSample.w;
		}
	} else {
		skinWetMask = 1.0;
	}
#	endif

	float landSnowMask1 = GetLandSnowMaskValue(baseColor.w);

#	if defined(MODELSPACENORMALS)
#		if defined(LODLANDNOISE)
	normal.xyz = normal.xzy - 0.5.xxx;
	float lodLandNoiseParameter = GetLodLandBlendParameter(baseColor.xyz);
	float noise = TexLandLodNoiseSampler.Sample(SampLandLodNoiseSampler, uv * 3.0.xx).x;
	float lodLandNoiseMultiplier = GetLodLandBlendMultiplier(lodLandNoiseParameter, noise);
	baseColor.xyz *= lodLandNoiseMultiplier;
	normal.xyz *= 2;
	normal.w = 1;
	glossiness = 0;
#		elif defined(LODLANDSCAPE)
	normal.xyz = 2.0.xxx * (-0.5.xxx + normal.xzy);
	normal.w = 1;
	glossiness = 0;
#		else
	normal.xyz = normal.xzy * 2.0.xxx + -1.0.xxx;
	normal.w = 1;
	glossiness = TexSpecularSampler.SampleBias(SampSpecularSampler, uv, SharedData::SpecularMipBias()).x;
#		endif  // LODLANDNOISE
#	elif (defined(SNOW) && defined(LANDSCAPE))
	normal.xyz = GetLandNormal(landSnowMask1, normal.xyz, uv, SampNormalSampler, TexNormalSampler);
	glossiness = normal.w;
#	else
	normal.xyz = TransformNormal(normal.xyz);
	glossiness = normal.w;
#	endif  // MODELSPACENORMALS

#	if defined(WORLD_MAP)
	normal.xyz = GetWorldMapNormal(input, normal.xyz, rawBaseColor.xyz);
#	endif  // WORLD_MAP

#	if defined(LANDSCAPE)
#		if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask = LandscapeTexture1to4IsSnow.x * input.LandBlendWeights1.x;
#		endif  // SNOW
#	endif  // LANDSCAPE

#	if defined(EMAT_ENVMAP)
	complexMaterial = complexMaterial && complexMaterialColor.y > (4.0 / 255.0);
	shininess = lerp(shininess, shininess * complexMaterialColor.y, complexMaterial);
	if (complexMaterial) {
		complexSpecular = lerp(1.0, baseColor.xyz, complexMaterialColor.z);
#		if defined(VANILLA_FRESNEL)
		complexSpecular = saturate(complexSpecular * (SharedData::vanillaFresnelSettings.Enable ? SharedData::vanillaFresnelSettings.ComplexMaterialF0Multiplier / Math::PI : 1.0));
#		endif
		baseColor.xyz = lerp(baseColor.xyz, 0.0, complexMaterialColor.z);

	}
#	endif  // defined (EMAT) && defined(ENVMAP)

#	if defined(FACEGEN)
	if (!SharedData::linearLightingSettings.enableLinearLighting) {
		baseColor.xyz = GetFacegenBaseColor(baseColor.xyz, uv);
	} else {
		baseColor.xyz = Color::GammaToLinear(GetFacegenBaseColor(Color::LinearToGamma(baseColor.xyz), uv));
	}
#	elif defined(FACEGEN_RGB_TINT)
	if (!SharedData::linearLightingSettings.enableLinearLighting) {
		baseColor.xyz = GetFacegenRGBTintBaseColor(baseColor.xyz, uv);
	} else {
		baseColor.xyz = Color::GammaToLinear(GetFacegenRGBTintBaseColor(Color::LinearToGamma(baseColor.xyz), uv));
	}
#	endif  // FACEGEN

#	if defined(SKIN) && defined(CS_SKIN)
	if (skinEnabled) {
		baseColor.xyz = baseColor.xyz * SharedData::skinData.skinParams2.w;
	}
#	endif  // CS_SKIN

#	if defined(HAIR) && defined(CS_HAIR)
	float3 hairTint = 0;

	if (SharedData::hairSpecularSettings.Enabled) {
		hairTint = lerp(1, Color::Diffuse(TintColor.xyz), Color::ColorToLinear(input.Color.y));
		baseColor.xyz *= hairTint;
		baseColor.xyz = Hair::Saturation(baseColor.xyz, SharedData::hairSpecularSettings.HairSaturation);
		baseColor.xyz *= SharedData::hairSpecularSettings.BaseColorMult;
		baseColor.xyz = SharedData::hairSpecularSettings.HairMode == 1 ? baseColor.xyz * baseColor.xyz : baseColor.xyz;  // To match color for Marschner
	}

	float3 sampledHairFlow = 0;
	bool useHairFlowMap = false;
#		if defined(BACK_LIGHTING)
	if (SharedData::hairSpecularSettings.Enabled) {
		uint2 hairFlowDimensions = uint2(0, 0);
		sampledHairFlow = float3(TexBackLightSampler.SampleBias(SampBackLightSampler, uv, SharedData::MaterialMipBias()).xy, 0.5f);
		TexBackLightSampler.GetDimensions(hairFlowDimensions.x, hairFlowDimensions.y);
		useHairFlowMap = (sampledHairFlow.x > 0.0 || sampledHairFlow.y > 0.0) && hairFlowDimensions.x > 32 && hairFlowDimensions.y > 32;
		sampledHairFlow = useHairFlowMap ? sampledHairFlow * 2.0f - 1.0f : float3(0.5f, 0.5f, 0.5f);
	}
#		endif
#	endif

#	if defined(LOD_LAND_BLEND)
	float4 lodLandColor;

#		if defined(TERRAIN_VARIATION)
	if (SharedData::terrainVariationSettings.enableLODTerrainTilingFix) {
		// Apply stochastic sampling to LOD_LAND_BLEND color texture
		float2 blendColorUV = input.TexCoord0.zw;
		float2 dx = ddx(blendColorUV);
		float2 dy = ddy(blendColorUV);
		StochasticOffsets lodBlendColorOffset = ComputeStochasticOffsetsLOD(blendColorUV);
		lodLandColor = StochasticSampleLOD(screenNoise, TexLandLodBlend1Sampler, SampLandLodBlend1Sampler, blendColorUV, lodBlendColorOffset, dx, dy);
	} else {
		lodLandColor = TexLandLodBlend1Sampler.Sample(SampLandLodBlend1Sampler, input.TexCoord0.zw);
	}
#		else
	lodLandColor = TexLandLodBlend1Sampler.Sample(SampLandLodBlend1Sampler, input.TexCoord0.zw);
#		endif

	lodLandColor.xyz = Color::ColorToLinear(lodLandColor.xyz) * Color::VanillaDiffuseColorMult();
#		if defined(LOD_BLENDING)
	lodLandColor.xyz = pow(abs(lodLandColor.xyz), SharedData::lodBlendingSettings.LODTerrainGamma) * SharedData::lodBlendingSettings.LODTerrainBrightness;
#		endif  // LOD_BLENDING
	float lodBlendParameter = GetLodLandBlendParameter(lodLandColor.xyz);
	float lodBlendMask = TexLandLodBlend2Sampler.Sample(SampLandLodBlend2Sampler, 3.0.xx * input.TexCoord0.zw).x;
	float lodLandFadeFactor = GetLodLandBlendMultiplier(lodBlendParameter, lodBlendMask);
	float lodLandBlendFactor = LODTexParams.z * input.LandBlendWeights2.w;
	normal.xyz = lerp(normal.xyz, float3(0, 0, 1), lodLandBlendFactor);

#		if !defined(TRUE_PBR)
	baseColor.w = 0;
	baseColor = lerp(baseColor, lodLandColor * lodLandFadeFactor, lodLandBlendFactor);
	glossiness = lerp(glossiness, 0, lodLandBlendFactor);
#		endif
#	endif  // LOD_LAND_BLEND

#	if defined(SNOW) && !defined(TRUE_PBR)
	useSnowSpecular = landSnowMask != 0.0;
#	endif  // SNOW

#	if defined(BACK_LIGHTING)
	float4 backLightColor = TexBackLightSampler.SampleBias(SampBackLightSampler, uv, SharedData::MaterialMipBias());
#		if defined(HAIR) && defined(CS_HAIR)
	if (useHairFlowMap) {
		backLightColor = 0.0f;
	}
#		endif
#	endif  // BACK_LIGHTING

#	if (defined(RIM_LIGHTING) || defined(SOFT_LIGHTING) || defined(LOAD_SOFT_LIGHTING))
	float4 rimSoftLightColor = TexRimSoftLightWorldMapOverlaySampler.SampleBias(SampRimSoftLightWorldMapOverlaySampler, uv, SharedData::MaterialMipBias());
#	endif  // RIM_LIGHTING || SOFT_LIGHTING

	uint numLights = min(7, uint(NumLightNumShadowLight.x));
	uint numShadowLights = min(4, uint(NumLightNumShadowLight.y));

#	if defined(MODELSPACENORMALS) && !defined(SKINNED)
	float3 worldNormal = normal.xyz;
#	else
	float3 worldNormal = normalize(mul(tbn, normal.xyz));

#		if defined(SPARKLE)
	float3 projectedNormal = normalize(mul(tbn, float3(ProjectedUVParams2.xx * normal.xy, normal.z)));
#		endif  // SPARKLE
#	endif      // defined (MODELSPACENORMALS) && !defined (SKINNED)

#	if defined(SKIN) && defined(CS_SKIN)
	float3 wetWorldNormal = worldNormal.xyz;
	float3 originalWorldNormal = worldNormal.xyz;
#		if defined(FACEGEN)
	float2 wetUV = uv;
#		else
	float2 wetUV = uv * SharedData::skinData.skinDetailParams.y;
#		endif
	float2 dynamicWet = Skin::GetWetness(input.WorldPosition.z + FrameBuffer::CameraPosAdjust[eyeIndex].z, worldNormal.xyz);
	// (44a) Wet Skin model (Characters > Wet Skin). "Original (pre-42c)": the 41b Advanced Skin
	// film exactly (Perlin coverage, _wet mask, half strength for skins without one); the CPU
	// also turns the 42c character switches off. "New": one wetness per character from the
	// manual slider, swimming (hold + dry-off), rain and sweat; soft patches; a broad sheen;
	// droplets and rivulets on the film only.
	const bool skinWetNew = Skin::WetSkinNewModel();
	float skinFilmStrength = clamp(dynamicWet.x + dynamicWet.y + SharedData::skinData.skinParams2.y, 0.f, 2.f) * (hasSkinWetness ? 1.0 : 0.5);
	// (42c, 4.3) Rain feeds the same film as swimming and sweat: its share of the strength is
	// remembered so the sky occlusion can be applied to it later (the probes are sampled after
	// the normal is final) and so it never raises the G-buffer gloss. (44a) New model: always;
	// Original: the switch is forced off on the CPU, as before 42c.
	float skinRainStrength = 0.0;
#		if defined(WETNESS_EFFECTS)
	if ((skinWetNew || SharedData::wetnessEffectsSettings.EnableRainSkinFilm) && inWorld)
		skinRainStrength = saturate(SharedData::wetnessEffectsSettings.SkinWetness * SharedData::wetnessEffectsSettings.Wetness);
#		endif
	float skinRainShare = 0.0;
	float skinWetness = 0.0;
	float skinWetDropDriver = 0.0;   // (44a) how many droplets (0..1)
	float skinWetDropRainOnly = 0.0; // (44a) 1 when rain is what makes them (sky occlusion applies)
	float skinWetStreakDriver = 0.0; // (44a) rivulets
	float skinWetDropMask = 0.0;
	float skinWetDropRim = 0.0;
	float skinWetStreak = 0.0;
	// (44b) Screen derivatives for the droplets and rivulets, taken here in uniform control flow:
	// inside the per-pixel wetness branch below they were undefined wherever a 2x2 quad was split.
	const float3 wetdPdx = ddx(input.WorldPosition.xyz);
	const float3 wetdPdy = ddy(input.WorldPosition.xyz);
	const float2 wetdUVdx = ddx(wetUV);
	const float2 wetdUVdy = ddy(wetUV);
	// (47c) The world size of one pixel at this distance (from the view ray, so not foreshortened on
	// slanted skin), for the droplet / rivulet distance LOD.
	const float3 wetViewRay = normalize(input.WorldPosition.xyz);
	const float wetPixelWorld = length(input.WorldPosition.xyz) * max(length(ddx(wetViewRay)), length(ddy(wetViewRay)));
	// (47c) Droplets and rivulets: set up in the block below, evaluated after the sky occlusion
	// (WETNESS_EFFECTS) or right there (without it).
	Skin::SkinWetDetailSetup skinWetDetail = (Skin::SkinWetDetailSetup)0;
	bool skinWetDetailPending = false;
	bool skinWetNoOtherWet = false;  // (47c) no swimming, manual or sweat wetness: only rain
	[branch] if (skinWetNew)
	{
		const float worldZ = input.WorldPosition.z + FrameBuffer::CameraPosAdjust[eyeIndex].z;
		// (44b) The same per-character function hair and gear use.
		float wSwim, wManual, wSweat;
		const float wDry = Skin::CharDryWetness(worldZ, dynamicWet.x, wSwim, wManual, wSweat);
		skinWetNoOtherWet = max(max(wSwim, wManual), wSweat) <= 0.0;
		const float wRain = saturate(skinRainStrength * Skin::wetSkinParams[0].w);
		const float wAll = 1.0 - (1.0 - wDry) * (1.0 - wRain);
		skinRainShare = wAll > 0.0 ? saturate((wAll - wDry) / wAll) : 0.0;
		skinWetness = Skin::WetCoverage(wetUV, wAll, SharedData::skinData.wetParams.x, SharedData::skinData.wetParams.y, SharedData::skinData.wetParams.z);
		skinWetMask = 1.0;
		const float dropOthers = max(max(wSwim, wManual), wSweat * 0.5);
		const float dropRain = wRain * saturate(Skin::wetSkinParams[5].w);
		skinWetDropDriver = smoothstep(0.35, 0.9, max(dropOthers, dropRain));
		skinWetDropRainOnly = dropRain > dropOthers ? 1.0 : 0.0;
		skinWetStreakDriver = max(smoothstep(0.2, 0.85, max(wSwim, wManual)), wRain);  // (44b) still running while drying off
	} else {
		skinRainShare = skinRainStrength / max(skinFilmStrength + skinRainStrength, 1e-4);
		skinWetness = Skin::PerlinNoise(wetUV, SharedData::skinData.wetParams.x, SharedData::skinData.wetParams.y, SharedData::skinData.wetParams.z, min(skinFilmStrength + skinRainStrength, 2.f));
	}
	if ((SharedData::skinData.skinDetailParams.w > 0.0f || skinWetness > 0.0f) && skinEnabled) {
#		if defined(FACEGEN)
		float2 detailUV = input.TexCoord0.xy * SharedData::skinData.skinDetailParams.x;
#		else
		float2 detailUV = input.TexCoord0.xy * SharedData::skinData.skinDetailParams.x * SharedData::skinData.skinDetailParams.y;
#		endif  // FACEGEN
#		if defined(MODELSPACENORMALS)
		const float3x3 tbnTr = Skin::ReconstructTBN(input.WorldPosition.xyz, worldNormal, screenUV);
		const float3x3 tbn = transpose(tbnTr);
		const float3 tangentNormal = mul(tbnTr, worldNormal.xyz);
#		else
		const float3 tangentNormal = normal.xyz;
#		endif  // MODELSPACENORMALS
		float3 detailNormal = float3(Skin::TexSkinDetailNormal.SampleBias(SampNormalSampler, detailUV, SharedData::MipBias).xy, 0.5f);
		skinAO *= Skin::TexSkinDetailNormal.SampleBias(SampNormalSampler, detailUV, SharedData::MaterialMipBias()).w;
		detailNormal = (detailNormal * 2.0 - 1.0) * SharedData::skinData.skinDetailParams.z;
		float3 combinedTangentNormal = normalize(float3(Skin::ReorientNormal(detailNormal, tangentNormal).xy, tangentNormal.z));
		float3 combinedNormal = normalize(mul(tbn, combinedTangentNormal));
		if (SharedData::skinData.skinDetailParams.w > 0.0f)
			worldNormal.xyz = combinedNormal;
		originalWorldNormal = worldNormal.xyz;
		wetWorldNormal = worldNormal.xyz;
		if (skinWetness > 0.0f && skinWetNew) {
			// (44a) New model: the skin keeps its own normal; only the water film gets the droplet
			// domes (inside each bead only). Droplets and rivulets live in uv cells, so they stay
			// put on a moving character; gravity is world down projected on the surface.
			const float3 dPdx = wetdPdx;
			const float3 dPdy = wetdPdy;
			const float2 dUVdx = wetdUVdx;
			const float2 dUVdy = wetdUVdy;
			const float uvDet = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
			const float3 baseN = worldNormal.xyz;
			float3 dPdu = (dPdx * dUVdy.y - dPdy * dUVdx.y) / (abs(uvDet) > 1e-12 ? uvDet : 1e-12);
			float3 dPdv = (dPdy * dUVdx.x - dPdx * dUVdy.x) / (abs(uvDet) > 1e-12 ? uvDet : 1e-12);
			dPdu -= baseN * dot(baseN, dPdu);
			dPdv -= baseN * dot(baseN, dPdv);
			const float3 down = float3(0.0, 0.0, -1.0);
			const float alignment = saturate(length(down - baseN * dot(baseN, down)));
			float2 gravityUV = float2(dot(down, dPdu) / max(dot(dPdu, dPdu), 1e-12), dot(down, dPdv) / max(dot(dPdv, dPdv), 1e-12));
			gravityUV = dot(gravityUV, gravityUV) > 1e-20 ? normalize(gravityUV) : float2(0.0, 1.0);

#		if defined(FACEGEN)
			const float cellsPerUV = 140.0 / clamp(Skin::wetSkinParams[3].x, 0.25, 4.0);
			const float faceDroplets = saturate(Skin::wetSkinParams[3].z);
			const float faceRivulets = saturate(Skin::wetCharParams[6].z);  // (44e) Rivulets on Face
			const float rivuletColumnsPerUV = 11.0;  // (44b) the face uv covers the head: same size in cm
#		else
			const float cellsPerUV = 320.0 / clamp(Skin::wetSkinParams[3].x, 0.25, 4.0);
			const float faceDroplets = 1.0;
			const float faceRivulets = 1.0;
			const float rivuletColumnsPerUV = 25.0;  // (44b) about 3 cm per rivulet column on the body
#		endif
			// (47c) Distance LOD (Faster Noise & Distance LOD; 1 when off): droplets and rivulets fade
			// out on characters far away, the wet sheen and darkening stay.
			const float detailLOD = Skin::WetDetailLOD(wetPixelWorld);
			const float footprint = max(max(length(dUVdx), length(dUVdy)) * cellsPerUV, 1e-6);
			// Beads are about 0.3 cell in radius: fade them out below ~1.5 pixels (no DLSS sparkle).
			const float dropVisibility = smoothstep(1.0, 2.5, 0.3 / footprint) * detailLOD;
			// More beads where water rests (shoulders, chest), fewer on undersides.
			const float restFactor = lerp(0.7, 1.2, saturate(baseN.z * 0.5 + 0.5));
			skinWetDetail.wetUV = wetUV;
			skinWetDetail.cellUV = wetUV * cellsPerUV;
			skinWetDetail.footprint = footprint;
			skinWetDetail.uvFootprint = max(length(dUVdx), length(dUVdy));
			skinWetDetail.gravityUV = gravityUV;
			skinWetDetail.alignment = alignment;
			skinWetDetail.presence = saturate(Skin::wetSkinParams[2].w) * saturate(Skin::wetSkinParams[2].z) * skinWetDropDriver * restFactor * faceDroplets * (dropVisibility > 0.0 ? 1.0 : 0.0);
			skinWetDetail.dropVisibility = dropVisibility;
			skinWetDetail.streakDriver = skinWetStreakDriver;
			skinWetDetail.rivuletColumnsPerUV = rivuletColumnsPerUV;
			skinWetDetail.rivuletDriver = skinWetStreakDriver * faceRivulets;
			skinWetDetail.lod = detailLOD;
			skinWetDetail.baseN = baseN;
			skinWetDetail.tu = dot(dPdu, dPdu) > 1e-20 ? normalize(dPdu) : float3(1.0, 0.0, 0.0);
			skinWetDetail.tv = dot(dPdv, dPdv) > 1e-20 ? normalize(dPdv) : float3(0.0, 1.0, 0.0);
			skinWetDetail.domeStrength = clamp(Skin::wetSkinParams[5].z, 0.0, 2.0);
			skinWetDetailPending = true;
#		if !defined(WETNESS_EFFECTS)
			// No sky occlusion without Wetness Effects: the droplets and rivulets right here.
			Skin::SkinWetDetail(skinWetDetail, skinWetDropMask, skinWetDropRim, skinWetStreak, wetWorldNormal);
			skinWetDetailPending = false;
#		endif
		} else if (skinWetness > 0.0f) {
			float3 wetNormal = Skin::CalculateNormalFromHeight(skinWetness, SharedData::skinData.wetParams.w * 0.0005, uv);
			if (hasSkinWetness) {
				// float3 wetMaskNormal = Skin::CalculateNormalFromHeight(skinWetMask, SharedData::skinData.wetParams.w * 0.00005, uv);
				float3 wetMaskNormal = (skinWetnessNormal.xyz * 2.0 - 1.0);
				wetNormal = Skin::ReorientNormal(wetMaskNormal, wetNormal);
			}
			if (SharedData::skinData.skinParams2.y > 1.0f) {
				wetNormal = lerp(wetNormal, tangentNormal, saturate(SharedData::skinData.skinParams2.y - 1.0f));
			}
			// Texture sampling already supplies filtered mask coverage. Applying a
			// second screen-space fwidth here made the artist mask vary with distance;
			// procedural wet/dry coverage is filtered at its threshold instead.
			const float wetMaskCoverage = saturate(skinWetMask);
			const float wetNormalWeight = Skin::GetWetnessResponse(skinWetness * wetMaskCoverage);
			float3 combinedWetNormal = normalize(lerp(combinedTangentNormal, wetNormal, wetNormalWeight));
			wetWorldNormal = normalize(mul(tbn, combinedWetNormal));
			worldNormal.xyz = wetWorldNormal;
		}
	}
#	endif  // CS_SKIN

	float2 baseShadowUV = 1.0.xx;
	float4 shadowColor = 1.0;
	if ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) && ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::ShadowDir) || inWorld) || numShadowLights > 0) {
		baseShadowUV = input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy;
		float2 adjustedShadowUV = baseShadowUV * VPOSOffset.xy + VPOSOffset.zw;
		float2 shadowUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(adjustedShadowUV);
		shadowColor = TexShadowMaskSampler.Sample(SampShadowMaskSampler, shadowUV);
	}

	float projectedMaterialWeight = 0;

	float projWeight = 0;

#	if defined(PROJECTED_UV)
	float2 projNoiseUv = ProjectedUVParams.zz * input.TexCoord0.zw;
	float projNoise = TexCharacterLightProjNoiseSampler.Sample(SampCharacterLightProjNoiseSampler, projNoiseUv).x;
	float3 texProj = normalize(input.TexProj);
#		if defined(TREE_ANIM) || defined(LODOBJECTSHD)
	float vertexAlpha = 1;
#		else
	float vertexAlpha = input.Color.w;
#		endif  // defined (TREE_ANIM) || defined (LODOBJECTSHD)
	projWeight = -ProjectedUVParams.x * projNoise + (dot(worldNormal.xyz, texProj) * vertexAlpha - ProjectedUVParams.w);
#		if defined(LODOBJECTSHD)
	projWeight += (-0.5 + input.Color.w) * 2.5;
#		endif  // LODOBJECTSHD
#		if defined(SPARKLE)
	if (projWeight < 0)
		discard;

	worldNormal.xyz = projectedNormal;
#			if defined(SNOW)
	psout.Parameters.y = 1;
#			endif  // SNOW
#		elif !defined(FACEGEN) && !defined(MULTI_LAYER_PARALLAX) && !defined(PARALLAX) && !defined(SPARKLE)
	if (ProjectedUVParams3.w > 0.5) {
		float2 projNormalDiffuseUv = ProjectedUVParams3.x * projNoiseUv;
		float3 projNormal = TransformNormal(TexProjNormalSampler.SampleBias(SampProjNormalSampler, projNormalDiffuseUv, SharedData::MaterialMipBias()).xyz);
		float2 projDetailNormalUv = ProjectedUVParams3.y * projNoiseUv;
		float3 projDetailNormal = TexProjDetail.SampleBias(SampProjDetailSampler, projDetailNormalUv, SharedData::MaterialMipBias()).xyz;
		float3 finalProjNormal = normalize(TransformNormal(projDetailNormal) * float3(1, 1, projNormal.z) + float3(projNormal.xy, 0));
		float3 projBaseColor = Color::ColorToLinear(TexProjDiffuseSampler.SampleBias(SampProjDiffuseSampler, projNormalDiffuseUv, SharedData::MaterialMipBias()).xyz) * ProjectedUVParams2.xyz;
		projectedMaterialWeight = smoothstep(0, 1, 5 * (0.1 + projWeight));
#			if defined(TRUE_PBR)
		projBaseColor = saturate(Color::ColorToLinear(EnvmapData.xyz) * projBaseColor);
		rawRMAOS.xyw = lerp(rawRMAOS.xyw, float3(ParallaxOccData.x, 0, ParallaxOccData.y), projectedMaterialWeight);
		float4 projectedGlintParameters = 0;
		if ((PBRFlags & PBR::Flags::ProjectedGlint) != 0) {
			projectedGlintParameters = SparkleParams;
		}
		glintParameters = lerp(glintParameters, projectedGlintParameters, projectedMaterialWeight);
#			elif defined(LOD_BLENDING) && (defined(LODOBJECTS) || defined(LODOBJECTSHD))
		projBaseColor.xyz = pow(abs(projBaseColor.xyz), SharedData::lodBlendingSettings.LODObjectSnowGamma) * SharedData::lodBlendingSettings.LODObjectSnowBrightness;
#			endif  // TRUE_PBR
		normal.xyz = lerp(normal.xyz, finalProjNormal, projectedMaterialWeight);
		baseColor.xyz = lerp(baseColor.xyz, projBaseColor, projectedMaterialWeight);

#			if defined(SNOW)
		useSnowDecalSpecular = true;
		psout.Parameters.y = GetSnowParameterY(projectedMaterialWeight, baseColor.w);
#			endif  // SNOW
	} else {
		if (projWeight > 0) {
			baseColor.xyz = Color::ColorToLinear(ProjectedUVParams2.xyz);
#			if defined(SNOW)
			useSnowDecalSpecular = true;
			psout.Parameters.y = GetSnowParameterY(projWeight, baseColor.w);
#			endif  // SNOW
		} else {
#			if defined(SNOW)
			psout.Parameters.y = 0;
#			endif  // SNOW
		}
	}

#			if defined(SPECULAR)
	useSnowSpecular = useSnowDecalSpecular;
#			endif  // SPECULAR
#		endif      // SPARKLE

#	elif defined(SNOW)
#		if defined(LANDSCAPE)
	psout.Parameters.y = landSnowMask;
#		else
	psout.Parameters.y = baseColor.w;
#		endif  // LANDSCAPE
#	endif      // SNOW

#	if defined(WORLD_MAP)
	baseColor.xyz = GetWorldMapBaseColor(rawBaseColor.xyz, baseColor.xyz, projWeight);
#	endif  // WORLD_MAP

#	if defined(MODELSPACENORMALS)
	float3 vertexNormal = worldNormal;
#	endif

#	if defined(DYNAMIC_SNOW_SURFACE)
	// (batch 39, items 5-6) Accumulated snow and snow/mud footprints. Runs after the material's
	// own snow is known (psout.Parameters.y: landscape snow textures, directional snow
	// projection, snow-flagged materials) and before anything reads the albedo, normal or
	// roughness, so the lit colour, the G-buffer and everything downstream of it (SSRT, NRD,
	// the deferred composite) all see one material. Flags == 0 (feature off, Batch 39 master
	// off, interiors) skips the whole block: the 38b path.
	float dynamicSnowCoverage = 0.0;
	[branch] if (SharedData::dynamicSnowSettings.Flags != 0 && inWorld && !SharedData::InInterior)
	{
		const uint snowFlags = SharedData::dynamicSnowSettings.Flags;
		float3 snowPositionWS = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
#		if defined(MODELSPACENORMALS) && !defined(SKINNED)
		float3 snowGeometryNormal = worldNormal;
#		else
		float3 snowGeometryNormal = normalize(mul(tbn, float3(0, 0, 1)));
#		endif

#		if defined(SNOW) && defined(DEFERRED)
		float snowExisting = saturate(psout.Parameters.y);
#		else
		float snowExisting = 0.0;
#		endif

		// (batch 39b) Characters, creatures and everything attached to them or worn by them
		// (Hooks.cpp sets IsActorGeometry per draw; the SKINNED macro this block used to rely on
		// never reaches a pixel shader, the engine strips it from pixel descriptors).
		const bool snowOnActor = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsActorGeometry) != 0;

		// (44h) Local accumulation amount: the global amount x the cold-region mask (FlagColdMap) x
		// the fade towards the edge of the loaded cells, 0 on distant LOD (FlagNoLodSnow). (44i) The mask
		// includes the snow line (world height), so the absolute position goes in.
#		if defined(LODLANDSCAPE) || defined(LODOBJECTS) || defined(LODOBJECTSHD)
		const bool snowOnLod = true;
#		else
		const bool snowOnLod = false;
#		endif
		float snowAmountScale = 1.0;
		[branch] if (snowFlags & (DynamicSnow::FlagAccumulation | DynamicSnow::FlagShowColdMap))
			snowAmountScale = DynamicSnow::AmountScale(snowPositionWS, snowOnLod);
		float snowAmount = SharedData::dynamicSnowSettings.Amount * snowAmountScale;
#		if defined(CS_SKIN)
		// (46a) Characters keep their own snow (none indoors, building up again outside, washed off
		// below the water line): Advanced Skin's per-character state in b7.
		[branch] if (snowOnActor && (snowFlags & DynamicSnow::FlagCharacterSnowState))
			snowAmount = Skin::CharacterSnowAmount(snowPositionWS.z, SharedData::dynamicSnowSettings.Amount) * snowAmountScale;
#		endif

		// (batch 39b) Authored snow the material path above does not report. 39a only had
		// psout.Parameters.y, which exists only in SNOW permutations: True PBR terrain (every
		// landscape with a PBR texture set, e.g. Vanaheimr Landscapes) has no SNOW permutation and
		// no per-layer snow constants, so High Hrothgar's snow counted as bare ground (mud).
		[branch] if (!snowOnActor)
		{
			float authoredSnow = 0.0;
#		if defined(LANDSCAPE)
			float4 snowLayerW1 = input.LandBlendWeights1;
			float2 snowLayerW2 = input.LandBlendWeights2.xy;
#			if defined(TRUE_PBR)
			// TruePBR.cpp puts each tile's "is snow" (LTEX flag / snow material / "snow" texture
			// set) into PBRFlags bits 24..29.
			const uint snowTiles = PBRFlags >> 24;
			float4 snowLayer1 = float4((snowTiles >> uint4(0, 1, 2, 3)) & 1);
			float2 snowLayer2 = float2((snowTiles >> uint2(4, 5)) & 1);
#			else
			// The engine's own per-layer flags (LTEX "is snow"), also outside SNOW permutations.
			float4 snowLayer1 = saturate(LandscapeTexture1to4IsSnow);
			float2 snowLayer2 = saturate(LandscapeTexture5to6IsSnow.xy);
#			endif
			float snowLayerTotal = dot(snowLayerW1, float4(1, 1, 1, 1)) + snowLayerW2.x + snowLayerW2.y;
			authoredSnow = (dot(snowLayerW1, snowLayer1) + dot(snowLayerW2, snowLayer2)) / max(snowLayerTotal, 1e-3);
#		endif
#		if defined(PROJECTED_UV)
			// Directional snow on rocks and mountains. The same projection also lays moss, so only
			// a white projected layer counts.
			float projectedSnow = saturate(max(projectedMaterialWeight, projWeight));
			authoredSnow = max(authoredSnow, projectedSnow * DynamicSnow::AlbedoSnowGuess(baseColor.xyz));
#		endif
			snowExisting = max(snowExisting, saturate(authoredSnow));
		}

#		if defined(TREE_ANIM)
		[branch] if ((snowFlags & DynamicSnow::FlagAccumulation) && (snowFlags & DynamicSnow::FlagTrees) && !snowOnActor && snowAmount > 0.0)
		{
			float snowCoverage = DynamicSnow::GetTreeCoverage(snowPositionWS, snowGeometryNormal, uv, snowAmount);
#		else
		[branch] if ((snowFlags & DynamicSnow::FlagAccumulation) && (!snowOnActor || (snowFlags & DynamicSnow::FlagSnowOnCharacters)) && snowAmount > 0.0)
		{
			float snowCoverage;
			[branch] if (snowOnActor)
			{
				// (40d) Characters: coverage in the mesh's bind pose, so snow stays put while limbs
				// move. The normal is the interpolated vertex normal from before skinning (smooth
				// across the mesh, unlike a per-triangle derivative normal, which gave faceted
				// diamonds); model Z is up. Model-space normal-map meshes have no vertex normal and
				// use their normal map at a fixed low mip (no derivatives in this branch), so only the
				// broad shape remains.
#			if defined(MODELSPACENORMALS)
				float3 snowBindNormal = TexNormalSampler.SampleLevel(SampNormalSampler, uv, 4.0).xzy * 2.0 - 1.0;
#			else
				float3 snowBindNormal = input.SnowBindNormal;
#			endif
				// (40e) Rigid gear (shield, weapon, quiver, helmet; IsActorRigid) has a model space of
				// its own: a shield's face is "up" in it wherever it is worn, so a shield on the back
				// turned fully white. Its slope comes from the world vertex normal instead: an upright
				// actor only turns about Z, which leaves the up component of a normal unchanged, so
				// this equals the normal in the actor's own frame (inverse yaw). The noise stays in
				// model space and so stays on the gear. Back faces (the inside of a double-sided
				// surface) take no snow, on skinned meshes too.
				[branch] if (Permutation::ExtraShaderDescriptor & DynamicSnow::ExtraIsActorRigid)
					snowBindNormal = snowGeometryNormal;
				snowBindNormal = snowBindNormal * rsqrt(max(dot(snowBindNormal, snowBindNormal), 1e-8));
				snowCoverage = frontFace ? DynamicSnow::GetActorCoverage(input.ModelPosition.xyz, snowBindNormal, length(input.WorldPosition.xyz), snowAmount) : 0.0;
			}
			else
			{
				snowCoverage = DynamicSnow::GetCoverage(snowPositionWS, worldNormal, snowGeometryNormal, 1.0, length(input.WorldPosition.xyz), snowAmount);
			}
#		endif
			[branch] if (snowCoverage > 0.0)
			{
#		if defined(SKYLIGHTING)
				// Same sky occlusion as Wetness Effects (the skylighting probes' up-facing
				// visibility, squared), but sampled without the per-frame blue-noise jitter so the
				// albedo and roughness written to the G-buffer do not shimmer under a roof edge.
#			if defined(VR)
				float3 snowPositionMS = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#			else
				float3 snowPositionMS = input.WorldPosition.xyz;
#			endif
				// (46d) Characters whose snow the CPU keeps with its roof test: no per-pixel sky test (it erased
				// their snow under eaves; under a roof their snow now just stops building up).
#			if defined(CS_SKIN)
				const bool snowRoofsByCpu = snowOnActor && (snowFlags & DynamicSnow::FlagCharacterRoofs);
#			else
				const bool snowRoofsByCpu = false;
#			endif
				[branch] if (!snowRoofsByCpu)
				{
					sh2 snowSkySH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, snowPositionMS + snowGeometryNormal * Skylighting::CELL_SIZE);
					snowCoverage *= pow(saturate(SphericalHarmonics::Unproject(snowSkySH, float3(0, 0, 1))), 2);
				}
#		endif
				// The material's own snow stays as it is; only the rest of the surface is covered.
				dynamicSnowCoverage = snowCoverage * (1.0 - snowExisting);
			}

			[branch] if (dynamicSnowCoverage > 0.0)
			{
				baseColor.xyz = lerp(baseColor.xyz, Color::ColorToLinear(SharedData::dynamicSnowSettings.SnowColor), dynamicSnowCoverage);
#		if defined(TREE_ANIM)
				// Leaf cards: keep most of their shading normal (it carries the canopy's volume).
				worldNormal = normalize(lerp(worldNormal, snowGeometryNormal, dynamicSnowCoverage * 0.5));
#		else
				// (batch 39c) Thick snow hides the material's relief completely: its own normal is the
				// geometry's (39b kept 20% of the normal map under full cover). (40e) Under Even Cover
				// only SnowSmoothing of it (default 0.2): snow follows the bumps below it.
				worldNormal = normalize(lerp(worldNormal, snowGeometryNormal, dynamicSnowCoverage * ((snowFlags & DynamicSnow::FlagCoverageSlope) ? DynamicSnow::SnowSmoothing() : 0.8)));
#		endif
				// Snow is never smoother than SnowRoughness, and never makes a matte surface
				// shinier: SSRT/NRD only ever see roughness go up on snow.
#		if defined(TRUE_PBR)
				rawRMAOS.x = lerp(rawRMAOS.x, max(rawRMAOS.x, SharedData::dynamicSnowSettings.SnowRoughness), dynamicSnowCoverage);
				rawRMAOS.y = lerp(rawRMAOS.y, 0.0, dynamicSnowCoverage);
				rawRMAOS.w = lerp(rawRMAOS.w, 0.02, dynamicSnowCoverage);
#		else
				glossiness = lerp(glossiness, min(glossiness, saturate(1.0 - SharedData::dynamicSnowSettings.SnowRoughness)), dynamicSnowCoverage);
#		endif
#		if defined(SNOW) && defined(DEFERRED)
				psout.Parameters.y = saturate(snowExisting + dynamicSnowCoverage);
#		endif
			}
		}

		// (44h) Debug: where accumulation can build (blue) and where it cannot (red).
		[branch] if ((snowFlags & DynamicSnow::FlagShowColdMap) && !snowOnActor)
			baseColor.xyz = lerp(baseColor.xyz, DynamicSnow::ColdMapDebugColor(snowAmountScale), 0.6);

#		if !defined(TREE_ANIM)
		[branch] if ((snowFlags & DynamicSnow::FlagTrails) && !snowOnActor)
		{
			float trailSnow = 0.0;
			if (snowFlags & DynamicSnow::FlagTrailsOnSnow)
				trailSnow += snowExisting;
			if (snowFlags & DynamicSnow::FlagTrailsOnAccumulated)
				trailSnow += dynamicSnowCoverage;
			trailSnow = saturate(trailSnow);
			float trailMud = 0.0;
#		if defined(LANDSCAPE)
			if (snowFlags & DynamicSnow::FlagMudTrails)
				trailMud = (1.0 - trailSnow) * SharedData::dynamicSnowSettings.TrailMudStrength;
#		endif
			float trailWeight = trailSnow + trailMud;
			[branch] if (trailWeight > 0.01)
			{
				DynamicSnow::TrailSample trail = DynamicSnow::SampleTrail(snowPositionWS, viewDirection);
				[branch] if (abs(trail.pressed) > 1e-3)
				{
					float trailDarken = SharedData::dynamicSnowSettings.TrailDarken * trailSnow + 0.35 * trailMud;
					baseColor.xyz *= saturate(1.0 - trailDarken * saturate(trail.pressed));  // dents darken, rims do not
					worldNormal = DynamicSnow::ApplyTrailNormal(worldNormal, snowGeometryNormal, trail, trailWeight);
				}
			}
		}
#		endif  // !TREE_ANIM
	}
#	endif  // DYNAMIC_SNOW_SURFACE

	float3 screenSpaceNormal = normalize(FrameBuffer::WorldToView(worldNormal, false, eyeIndex));

#	if defined(HAIR) && defined(CS_HAIR)
	float3 Bitangent = normalize(float3(input.TBN0.y, input.TBN1.y, input.TBN2.y));
	float3 hairT = 0;
#		if defined(BACK_LIGHTING)
	hairT = useHairFlowMap ? normalize(mul(tbn, sampledHairFlow)) : Bitangent;
#		else
	hairT = Bitangent;
#		endif
	hairT = Hair::ReorientTangent(hairT, worldNormal);

	// (44c) Wet hair clumps and specular AA, from screen derivatives taken here (uniform control
	// flow; the per-pixel wetness comes later). Strand space: the hair tangent projected into uv
	// gives "along" the strands; clumps are bands across them, slightly bent by a value noise
	// along the strand. All fixed in uv (no time dependence), so DLSS accumulates them normally.
	float hairClumpX = 0.5;     // 0..1, position across the clump
	float hairClumpR = 0.0;     // -0.5..0.5, the clump's random highlight offset
	float hairClumpFade = 0.0;  // 0 where a clump is narrower than ~2.5 px, 1 above ~5 px
	float hairWetSpecAA = 0.0;  // squared widening of the highlight lobes
	float hairClumpL = 0.0;     // (47c) clump coordinates, for Skip Invisible Work (clumps only where wet)
	float hairClumpS = 0.0;
#		if defined(CS_SKIN) && defined(WETNESS_EFFECTS)
	[branch] if (skinEnabled && SharedData::hairSpecularSettings.Enabled && Skin::WetSkinNewModel() && Skin::wetCharParams[1].x > 0.5)
	{
		const float3 dPdx = ddx(input.WorldPosition.xyz);
		const float3 dPdy = ddy(input.WorldPosition.xyz);
		const float2 dUVdx = ddx(uv);
		const float2 dUVdy = ddy(uv);
		const float3 dTdx = ddx(hairT);
		const float3 dTdy = ddy(hairT);
		hairWetSpecAA = min(0.5 * (dot(dTdx, dTdx) + dot(dTdy, dTdy)), 0.3);
		const float uvDet = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
		const float uvDetSafe = abs(uvDet) > 1e-12 ? uvDet : 1e-12;
		const float3 dPdu = (dPdx * dUVdy.y - dPdy * dUVdx.y) / uvDetSafe;
		const float3 dPdv = (dPdy * dUVdx.x - dPdx * dUVdy.x) / uvDetSafe;
		float2 tUV = float2(dot(hairT, dPdu) / max(dot(dPdu, dPdu), 1e-12), dot(hairT, dPdv) / max(dot(dPdv, dPdv), 1e-12));
		tUV = dot(tUV, tUV) > 1e-20 ? normalize(tUV) : float2(0.0, 1.0);
		const float2 aUV = float2(-tUV.y, tUV.x);
		const float clumpCount = clamp(Skin::wetCharParams[2].w, 16.0, 128.0);
		hairClumpL = dot(uv, tUV);
		hairClumpS = dot(uv, aUV) * clumpCount;
		// (47c) Skip Invisible Work: computed below, only where the hair is wet (hairWetK > 0).
		[branch] if (!Skin::WetSkipInvisible())
			Skin::WetHairClump(hairClumpL, hairClumpS, hairClumpX, hairClumpR);
		const float sPerPixel = max(abs(dot(dUVdx, aUV)), abs(dot(dUVdy, aUV))) * clumpCount;
		hairClumpFade = smoothstep(2.5, 5.0, 1.0 / max(sPerPixel, 1e-6));
	}
#		endif

	if (SharedData::hairSpecularSettings.Enabled) {
		if (SharedData::hairSpecularSettings.EnableTangentShift && SharedData::hairSpecularSettings.HairMode != 1) {
			float3 shiftedNormal = Hair::ShiftWorldNormal(hairT, worldNormal, 0, uv);
			screenSpaceNormal = normalize(FrameBuffer::WorldToView(shiftedNormal, false, eyeIndex));
		}
	}

	float3 transmissionColor = 0;
#	endif

#	if defined(TRUE_PBR)
	PBR::SurfaceProperties pbrSurfaceProperties = PBR::InitSurfaceProperties();

	pbrSurfaceProperties.Noise = screenNoise;

	pbrSurfaceProperties.Roughness = clamp(rawRMAOS.x, PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
	pbrSurfaceProperties.Metallic = saturate(rawRMAOS.y);
	pbrSurfaceProperties.AO = rawRMAOS.z;
	if (!SharedData::linearLightingSettings.enableLinearLighting) {
		pbrSurfaceProperties.F0 = lerp(saturate(rawRMAOS.w), Color::GammaToTrueLinear(baseColor.xyz), pbrSurfaceProperties.Metallic);
	} else {
		pbrSurfaceProperties.F0 = lerp(saturate(rawRMAOS.w), baseColor.xyz, pbrSurfaceProperties.Metallic);
	}

	pbrSurfaceProperties.GlintScreenSpaceScale = max(1, glintParameters.x);
	pbrSurfaceProperties.GlintLogMicrofacetDensity = clamp(PBR::Constants::MaxGlintDensity - glintParameters.y, PBR::Constants::MinGlintDensity, PBR::Constants::MaxGlintDensity);
	pbrSurfaceProperties.GlintMicrofacetRoughness = clamp(glintParameters.z, PBR::Constants::MinGlintRoughness, PBR::Constants::MaxGlintRoughness);
	pbrSurfaceProperties.GlintDensityRandomization = clamp(glintParameters.w, PBR::Constants::MinGlintDensityRandomization, PBR::Constants::MaxGlintDensityRandomization);

#		if defined(GLINT)
	float glintNoise = Random::R1Modified(float(SharedData::FrameCount), (Random::pcg2d(uint2(input.Position.xy)) / 4294967296.0).x);
	PBR::Glints::PrecomputeGlints(glintNoise, uvOriginal, ddx(uvOriginal), ddy(uvOriginal), pbrSurfaceProperties.GlintScreenSpaceScale, pbrSurfaceProperties.GlintCache);
#		endif

	baseColor.xyz *= 1 - pbrSurfaceProperties.Metallic;

	pbrSurfaceProperties.BaseColor = baseColor.xyz;

	float3 coatWorldNormal = worldNormal;

#		if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::Subsurface) != 0)
	{
		pbrSurfaceProperties.SubsurfaceColor = PBRParams2.xyz;
		pbrSurfaceProperties.Thickness = PBRParams2.w;
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture0) != 0)
		{
			float4 sampledSubsurfaceProperties = TexRimSoftLightWorldMapOverlaySampler.SampleBias(SampRimSoftLightWorldMapOverlaySampler, uv, SharedData::MaterialMipBias());
			pbrSurfaceProperties.SubsurfaceColor *= Color::Diffuse(sampledSubsurfaceProperties.xyz);
			pbrSurfaceProperties.Thickness *= sampledSubsurfaceProperties.w;
		}
		pbrSurfaceProperties.Thickness = lerp(pbrSurfaceProperties.Thickness, 1, projectedMaterialWeight);
	}
	else if ((PBRFlags & PBR::Flags::TwoLayer) != 0)
	{
		pbrSurfaceProperties.CoatColor = sampledCoatColor.xyz;
		pbrSurfaceProperties.CoatStrength = sampledCoatColor.w;
		pbrSurfaceProperties.CoatRoughness = MultiLayerParallaxData.x;
		pbrSurfaceProperties.CoatF0 = MultiLayerParallaxData.y;

		float2 coatUv = uv;
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			coatUv = uvOriginal;
		}
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture1) != 0)
		{
			float4 sampledCoatProperties = TexBackLightSampler.SampleBias(SampBackLightSampler, coatUv, SharedData::MaterialMipBias());
			pbrSurfaceProperties.CoatRoughness *= sampledCoatProperties.w;
			[branch] if ((PBRFlags & PBR::Flags::CoatNormal) != 0)
			{
				coatWorldNormal = normalize(mul(tbn, TransformNormal(sampledCoatProperties.xyz)));
			}
		}
		pbrSurfaceProperties.CoatStrength = lerp(pbrSurfaceProperties.CoatStrength, 0, projectedMaterialWeight);
	}

	[branch] if ((PBRFlags & PBR::Flags::Fuzz) != 0)
	{
		pbrSurfaceProperties.FuzzColor = MultiLayerParallaxData.xyz;
		pbrSurfaceProperties.FuzzWeight = MultiLayerParallaxData.w;
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture1) != 0)
		{
			float4 sampledFuzzProperties = TexBackLightSampler.SampleBias(SampBackLightSampler, uv, SharedData::MaterialMipBias());
			pbrSurfaceProperties.FuzzColor *= Color::Diffuse(sampledFuzzProperties.xyz);
			pbrSurfaceProperties.FuzzWeight *= sampledFuzzProperties.w;
		}
		pbrSurfaceProperties.FuzzWeight = lerp(pbrSurfaceProperties.FuzzWeight, 0, projectedMaterialWeight);
	}
#		endif

	float3 specularColorPBR = 0;
	float3 transmissionColor = 0;

	float pbrGlossiness = 1 - pbrSurfaceProperties.Roughness;
#	endif  // TRUE_PBR

#	if defined(VANILLA_FRESNEL)
	const bool enableVanillaFresnel = SharedData::vanillaFresnelSettings.Enable;
	float3 F0 = enableVanillaFresnel ? SharedData::vanillaFresnelSettings.MinF0 : 0.0;
#	else
	float3 F0 = 0.0;
#	endif
	float roughness = 1.0;

#	if defined(VANILLA_FRESNEL)
	// (batch 9) Eyes are identified per draw now, not per permutation. A compile-time EYE
	// macro misses the eye meshes that ship as ordinary kEnvironmentMap materials -- those
	// used to fall through to the generic ENVMAP branch below and take the cubemap's average
	// colour as F0, which the x pi at the conversion site saturates to ~1 on any bright
	// cubemap: a mirror-metal eyeball. The compile-time macro stays as an OR term, since a
	// permutation that carries it is an eye by definition and needs no runtime confirmation.
	const bool isEyeMaterial =
#		if defined(EYE)
		true;
#		else
		(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsEye) != 0;
#		endif
	const bool applyEyeHandling = isEyeMaterial && SharedData::vanillaFresnelSettings.EnableEyeSpecialHandling;

	// (batch 9) This used to be `#if SPECULAR ... #elif EYE`, which meant the SPECULAR+EYE
	// permutations -- and they exist: Lighting:Pixel:10000201/10000211/10100201/10100211 all
	// appear in the shipping permutation set -- could never reach the eye branch at all. The
	// specular branch now excludes eyes and the eye handling is its own statement.
#		if defined(SPECULAR) && !defined(TRUE_PBR)
	if (enableVanillaFresnel && !isEyeMaterial) {
		F0 = saturate(glossiness * SpecularColor.xyz / Math::PI);
		// (batch 9) Two derivations of roughness from the vanilla specular inputs, mixed by
		// how un-glossy the pixel is. Blend 0 reduces to the old specular-power-only form.
		// roughness selects the cubemap mip, i.e. how sharp the reflection is, so this is
		// the knob behind metals reading flat. abs() is what the old form was missing --
		// it silenced a standing X3571 on this line.
		float roughnessFromSpecular = (1.0 - glossiness) * (1.0 - glossiness);
		float roughnessFromShininess = pow(abs(2.0 / (shininess + 2.0)), 0.25);
		roughness = lerp(roughnessFromShininess, roughnessFromSpecular,
			SharedData::vanillaFresnelSettings.SpecularRoughnessBlend * (1.0 - glossiness));
	}
#		endif
	if (enableVanillaFresnel && applyEyeHandling) {
		F0 = 0.027;
		// (batch 10b) An absolute roughness now, and deliberately not scaled by
		// RoughnessMultiplier -- see the twin of this at the end of the env-map block for why.
		// F0 stays hardcoded at 0.027: that is the cornea's real reflectance and it was never
		// the problem. The old 0.1 lives on as the setting's "previous behaviour" value.
		roughness = clamp(SharedData::vanillaFresnelSettings.EyeRoughness, 0.04, 1.0);
	}
	F0 = max((enableVanillaFresnel ? SharedData::vanillaFresnelSettings.MinF0 : 0.0), F0 * SharedData::vanillaFresnelSettings.BaseF0Multiplier);
	float3 baseF0 = F0;
#	endif

#	if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	float envMask = EnvmapData.x * MaterialData.x;
#		if defined(DYNAMIC_SNOW_SURFACE)
	envMask *= 1.0 - dynamicSnowCoverage;  // (batch 39) snow hides the environment reflection
#		endif

	float viewNormalAngle = dot(worldNormal.xyz, viewDirection);
	float3 envSamplingPoint = (viewNormalAngle * 2) * worldNormal.xyz - viewDirection;

	if (envMask > 0.0) {
		if (EnvmapData.y) {
			envMask *= TexEnvMaskSampler.SampleBias(SampEnvMaskSampler, uv, SharedData::SpecularMipBias()).x;
		} else {
			envMask *= glossiness;
		}
	}

	float3 envColor = 0.0;
	bool dynamicCubemap = false;

	if (envMask > 0.0) {
#		if defined(DYNAMIC_CUBEMAPS)
		uint2 envSize;
		TexEnvSampler.GetDimensions(envSize.x, envSize.y);

#			if defined(EMAT) && !defined(VANILLA_FRESNEL)
		if (envSize.x == 1 && envSize.y == 1 || complexMaterial) {
#			elif defined(VANILLA_FRESNEL)
		bool vfStartDynamicCubemapTest = (enableVanillaFresnel && SharedData::vanillaFresnelSettings.EnableDynamicCubemapsConversion);
#				if defined(EMAT)
		vfStartDynamicCubemapTest = complexMaterial || vfStartDynamicCubemapTest;
#				endif
		if (vfStartDynamicCubemapTest || (envSize.x == 1 && envSize.y == 1)) {
#			else
		if (envSize.x == 1 && envSize.y == 1) {
#			endif

			dynamicCubemap = true;

#			if defined(EMAT)
			if (!complexMaterial)
#			endif
			{
				// Dynamic Cubemap Creator sets this value to black, if it is anything but black it is wrong
				float3 envColorTest = TexEnvSampler.SampleLevel(SampEnvSampler, float3(0.0, 1.0, 0.0), 15).xyz;
				dynamicCubemap = all(envColorTest == 0.0);
			}

#			if defined(CREATOR)
			if (SharedData::cubemapCreatorSettings.Enabled) {
				dynamicCubemap = true;
			}
#			endif

#			if defined(VANILLA_FRESNEL)
			dynamicCubemap = dynamicCubemap || (SharedData::vanillaFresnelSettings.EnableDynamicCubemapsConversion && enableVanillaFresnel);
#			endif

			if (dynamicCubemap) {
				float4 envColorBase = TexEnvSampler.SampleLevel(SampEnvSampler, float3(1.0, 0.0, 0.0), 15);
				float envRoughness = 1.0;

				if (envColorBase.a < 1.0) {
					F0 = Color::GammaToLinear(envColorBase.rgb);
					envRoughness = envColorBase.a;
				} else {
#			if defined(VANILLA_FRESNEL)
					F0 = saturate(Color::GammaToLinear(envColorBase.rgb) * Math::PI * SharedData::vanillaFresnelSettings.CubemapToF0Multiplier);
					envRoughness = roughness;
#			else
					F0 = 1.0;
					envRoughness = 1.0 / 7.0;
#			endif
				}

#			if defined(CREATOR)
				if (SharedData::cubemapCreatorSettings.Enabled) {
					F0 = SharedData::cubemapCreatorSettings.CubemapColor.rgb;
					envRoughness = SharedData::cubemapCreatorSettings.CubemapColor.a;
				}
#			endif

#			if defined(EMAT)
				float complexMaterialRoughness = 1.0 - complexMaterialColor.y;
				envRoughness = lerp(envRoughness, complexMaterialRoughness, complexMaterial);
				F0 = lerp(F0, complexSpecular, complexMaterial);
#			endif

				envMask = saturate(envMask);
#			if defined(VANILLA_FRESNEL)
				if (enableVanillaFresnel) {
#				if defined(SPECULAR)
					F0 = max(lerp(baseF0, F0, envMask), SharedData::vanillaFresnelSettings.MinF0);
#				else
					F0 = max(lerp(0, F0, envMask), SharedData::vanillaFresnelSettings.MinF0);
#				endif
					roughness = lerp(roughness, envRoughness, envMask);
				} else {
					roughness = envRoughness;
				}
#			else
				roughness = envRoughness;
#			endif
			}
		}
#		endif

#		if defined(VANILLA_FRESNEL)
		if (enableVanillaFresnel) {
			// (batch 9) Runtime, not `#if defined(EYE)`. This is the override that puts an eye
			// back to 0.027 after the conversion branch above has handed it a cubemap-derived
			// F0; before, it did not compile into the permutations the mis-identified eye
			// materials actually use, which is why those eyes came out chrome.
			//
			// (batch 10b) The eye roughness is an absolute value and skips RoughnessMultiplier on
			// purpose. Otherwise the two knobs fight: the only way to unfocus an eye used to be to
			// raise the global multiplier (5x-10x, measured), and that multiplies every ordinary
			// material's roughness by the same factor and flattens all of them at once. Both
			// branches still land in [0.04, 1.0].
			if (applyEyeHandling) {
				F0 = 0.027;
				roughness = clamp(SharedData::vanillaFresnelSettings.EyeRoughness, 0.04, 1.0);
			} else {
				roughness = clamp(roughness * SharedData::vanillaFresnelSettings.RoughnessMultiplier, 0.04, 1.0);
			}
		}
#		endif

		if (!dynamicCubemap) {
			float3 envColorBase = Color::GammaToLinear(TexEnvSampler.Sample(SampEnvSampler, envSamplingPoint).xyz);
			envColor = envColorBase.xyz * envMask;
		}
	}

#	endif  // defined (ENVMAP) || defined (MULTI_LAYER_PARALLAX) || defined(EYE)

	// (batch 13) The roughness handed to the *direct* light lobes, split off from the one that
	// drives the environment reflection.
	//
	// GetLightSpecularInput feeds its roughness straight into BRDF::D_GGX (:701), and every
	// direct light in this shader calls it -- the sun at :2880/:2922/:2926 and each point light
	// below. Both eye overrides above (:2413 and :2537) run before any of those calls, so
	// EyeRoughness went into the sun's GGX lobe as well as into the cubemap fetch. At 0.7 that
	// lobe is wide enough that side-on sunlight stops reading as a glint on the cornea and
	// becomes a white sheet across the whole eyeball. 0.7 exists to blur an environment
	// reflection that is too *bright*; the direct lobe never needed that compensation.
	//
	// Everything else keeps using `roughness`, deliberately: GetDynamicCubemap (:3362) is the
	// environment reflection, and psout.NormalGlossiness writes 1 - roughness, which
	// DeferredCompositeCS.hlsl turns back into `roughness * 7` to pick a cubemap mip
	// (:485-486) -- also the environment. Only the direct lobes move.
	//
	// Non-eye pixels take the `roughness` value unchanged, so nothing outside eye materials
	// changes; with VANILLA_FRESNEL off this is a plain copy that fxc folds away.
	float directSpecularRoughness = roughness;

	// (batch 15) Which lighting model the direct lobes use, and whether the vanilla per-pixel
	// specular gates in the SPECULAR block near the end of this shader still run.
	//
	// Batches 10b and 13 both tried to fix the glowing eye by moving a roughness, and both
	// failed for the same reason: the GGX lobe in GetLightSpecularInput is energy-normalised,
	// so roughness only decides how wide the same total energy spreads. What actually changed
	// in batch 9 was EnableGGX's *default*, false -> true, and that did two things at once:
	//
	//   1. It turned on the GGX block at :701, which for eyes runs on a hardcoded F0 (0.027)
	//      and a hardcoded roughness -- the only surface in the shader whose GGX inputs are
	//      not read from the material.
	//   2. It made the assignment in the SPECULAR block skip itself, and that assignment is
	//      where the two per-pixel gates lived: `specularColor * glossiness` (glossiness is
	//      normal.w, the gloss map, set at :1969/:1972) times either `SpecularColor.xyz` or
	//      `lerp(SpecularColor.xyz, F0, envMask)` -- the environment mask. `MaterialData.yyy`
	//      is a separate statement and was never skipped.
	//
	// Every other material feeds its own gloss/specular numbers into GGX, so for them (2) is
	// a deliberate model swap. Eyes had nothing else holding the lobe down, so for eyes it is
	// just a missing brake -- D_GGX peaks at 3183 at roughness 0.1 against phong's 1.
	//
	// All three flags below are *additional* terms that start at false and are only ever
	// raised inside `applyEyeHandling`. They are deliberately not folded into the existing
	// conditions they modify: the original expressions stay spelled out verbatim at their
	// original sites and gain one `&& !flag` / `|| flag`. That is what keeps non-eye output
	// bit-identical -- fold the eye test to a compile-time false and fxc constant-propagates
	// each flag to false, the extra term disappears, and the conditions revert to the exact
	// text they had before this batch. An earlier draft hoisted `Enable && EnableGGX` into a
	// variable instead, which is semantically the same but made fxc materialise the predicate
	// once outside the light loop rather than at each inlined call site -- one extra ine/and
	// pair and a register-allocation cascade through every SPECULAR permutation.
#	if defined(VANILLA_FRESNEL)
	bool eyeUseVanillaDirectLobe = false;    // VanillaPhong: phong lobe instead of the GGX one
	bool eyeForceVanillaGates = false;       // VanillaPhong + GGXGated: run the two gates
	bool eyeSuppressConvertedF0 = false;     // VanillaPhong: gate on plain SpecularColor.xyz
	if (enableVanillaFresnel && applyEyeHandling) {
		directSpecularRoughness = clamp(SharedData::vanillaFresnelSettings.EyeDirectRoughness, 0.04, 1.0);

		const uint eyeDirectSpecularMode = SharedData::vanillaFresnelSettings.EyeDirectSpecularMode;
		if (eyeDirectSpecularMode == SharedData::EyeDirectSpecularModeVanillaPhong) {
			// Phong lobe plus both gates, and the gate uses plain SpecularColor.xyz with no
			// converted-cubemap F0 mixed in. That is exactly what bedec8379 compiled to for an
			// eye, because EnableGGX and EnableDynamicCubemapsConversion both defaulted off
			// there. The one branch with a known-correct reference.
			eyeUseVanillaDirectLobe = true;
			eyeForceVanillaGates = true;
			eyeSuppressConvertedF0 = true;
		} else if (eyeDirectSpecularMode == SharedData::EyeDirectSpecularModeGGXGated) {
			// GGX lobe, but put the two gates back. Theoretically the better answer --
			// energy-conserving lobe, and the author's gloss map and env mask respected --
			// and unverified, which is why it is not the default.
			eyeForceVanillaGates = true;
		}
		// EyeDirectSpecularModeGGXRaw leaves all three false: GGX lobe, gates skipped, i.e.
		// the batch 9 - batch 14 behaviour, kept for A/B.
	}
#	else
	const bool eyeUseVanillaDirectLobe = false;
#	endif

	// (batch 17) Two knobs on the vanilla soft-lighting / rim / back-lighting fill terms.
	// Both are spelled as preprocessor macros rather than local variables, for the same
	// reason batch 15's flags are spelled out verbatim at their original sites: hoisting a
	// predicate into a named variable makes fxc materialise it somewhere else and cascades
	// register allocation through the whole light loop. Batch 12 (const locals) and batch 15
	// (a hoisted `Enable && EnableGGX`) each lost bit-identical non-eye codegen that way. A
	// macro is pure text, so the original expression is still the original expression with one
	// factor appended.
	//
	// EYE_SOFT_LIGHTING_SCALE -- item 1, the eye fix. Scales only the soft-lighting fill on
	// eye materials; the Lambert diffuse and every specular term are untouched. Substitute the
	// literal 1.0 for this macro and the multiply folds away entirely, which is how "non-eye
	// materials are bit-identical" is proved: a non-eye pixel multiplies by exactly 1.0 and no
	// other instruction in the shader changes. On permutations compiled without
	// VANILLA_FRESNEL there is no eye handling at all, so the macro *is* the literal.
	//
	// SUN_SOFT_SHADOW -- item 2, the consistency fix, off by default. dirDetailShadow has
	// already absorbed parallaxShadow by the time these terms run (`dirDetailShadow *=
	// parallaxShadow` a few lines above each use site), so this single factor supplies both
	// occluders. Folded to a compile-time false the ternary becomes 1.0 and the multiply
	// disappears, so the default is a zero-instruction no-op rather than a value that happens
	// to be one.
	//
	// (44h) EYE_BACK_LIGHT_GATE(L) -- same pattern. Fades the eye's soft and rim lighting out as
	// the light moves behind the head: 1 for side light (L.V ~ 0), 0 once L.V <= -cut. A
	// non-eye pixel (or cut 0) multiplies by exactly 1.0.
#	if defined(VANILLA_FRESNEL)
#		define EYE_SOFT_LIGHTING_SCALE ((enableVanillaFresnel && applyEyeHandling) ? SharedData::vanillaFresnelSettings.EyeSoftLightingScale : 1.0)
#		define EYE_BACK_LIGHT_GATE(L) ((enableVanillaFresnel && applyEyeHandling && SharedData::vanillaFresnelSettings.EyeBackLightCut > 0.0) ? saturate((dot((L), viewDirection) + SharedData::vanillaFresnelSettings.EyeBackLightCut) / SharedData::vanillaFresnelSettings.EyeBackLightCut) : 1.0)
#	else
#		define EYE_SOFT_LIGHTING_SCALE 1.0
#		define EYE_BACK_LIGHT_GATE(L) 1.0
#	endif
#	define SUN_SOFT_SHADOW (SharedData::vanillaFresnelSettings.ShadowSoftLighting ? dirDetailShadow : 1.0)

#	if defined(SKIN) && defined(CS_SKIN)
	Skin::SkinSurfaceProperties skinSurfaceProperties = Skin::InitSkinSurfaceProperties();

	skinSurfaceProperties.Albedo = baseColor.xyz;

	const float ExtraRoughness = BRDF::F_Schlick(0.04, saturate(dot(worldNormal.xyz, viewDirection))) * SharedData::skinData.fuzzParams.w;
	skinSurfaceProperties.RoughnessPrimary = SharedData::skinData.skinParams.x;
	skinSurfaceProperties.RoughnessPrimary = saturate(SharedData::skinData.skinParams.x - SharedData::skinData.skinParams.z * glossiness);
	skinSurfaceProperties.RoughnessSecondary = SharedData::skinData.skinParams.y;
	if (skinRoughnessSet) {
		skinSurfaceProperties.RoughnessPrimary = skinRoughness * SharedData::skinData.physicalParams.x;
		skinSurfaceProperties.RoughnessSecondary = skinRoughness * SharedData::skinData.physicalParams.y;
	}
	skinSurfaceProperties.RoughnessPrimary = min(1.0, skinSurfaceProperties.RoughnessPrimary + ExtraRoughness);
	skinSurfaceProperties.RoughnessSecondary = min(1.0, skinSurfaceProperties.RoughnessSecondary + ExtraRoughness);
	skinSurfaceProperties.SecondarySpecIntensity = SharedData::skinData.skinParams2.x;
	skinSurfaceProperties.Thickness = 1 - skinsk.x;
	skinSurfaceProperties.SubsurfaceColor = skinsk.xyz;
	skinSurfaceProperties.F0 = SharedData::skinData.skinParams2.zzz;
	skinSurfaceProperties.AO = skinAO;
	skinSurfaceProperties.Curvature = Skin::CalculateCurvature(worldNormal.xyz);

	skinSurfaceProperties.FuzzWeight = SharedData::skinData.fuzzParams.x;
	skinSurfaceProperties.FuzzRoughness = SharedData::skinData.fuzzParams.y;
	skinSurfaceProperties.FuzzColor = SharedData::skinData.fuzzParams.zzz;

	const float skinWetMaskCoverage = saturate(skinWetMask);
	skinSurfaceProperties.Wetness = saturate(skinWetness * skinWetMaskCoverage);
	skinSurfaceProperties.WetnessFilmStrength = max(SharedData::skinData.physicalParams.w, 0.0f);

	if (skinRoughnessSet) {
		skinSurfaceProperties.F0 = 0.08f * skinSpecular * SharedData::skinData.physicalParams.z;
		skinSurfaceProperties.FuzzWeight *= skinFuzzMask;
	}

	float3 specularColorPBR = 0;
	float3 transmissionColor = 0;

	float averageRoughness = lerp(skinSurfaceProperties.RoughnessPrimary, skinSurfaceProperties.RoughnessSecondary, skinSurfaceProperties.SecondarySpecIntensity);

	float pbrWeight = 1;
	// (42c) The rain part of the film (skinRainShare, 0 unless "Rain Wets Advanced Skin") is
	// left out of the G-buffer gloss: SSRT/NRD see the same roughness as before 42c.
	float pbrGlossiness = lerp(1 - averageRoughness, 1 - WATER_ROUGHNESS, saturate(Skin::GetWetnessOpticalResponse(skinSurfaceProperties.Wetness) * skinSurfaceProperties.WetnessFilmStrength) * (1.0 - skinRainShare));
	// (44a) New model: the film's own roughness (>= 0.2) and response. Either model: the gloss the
	// reflections (SSRT / IBL) see on skin never exceeds 0.8, i.e. roughness >= 0.2 (anti-mirror).
	[branch] if (skinWetNew)
	{
		const float skinWetResponse = pow(saturate(skinSurfaceProperties.Wetness), 1.3);
		const float skinWetFilmRoughness = max(lerp(Skin::wetSkinParams[1].y, Skin::wetSkinParams[1].z, skinWetResponse), 0.2);
		pbrGlossiness = lerp(1 - averageRoughness, 1 - skinWetFilmRoughness, saturate(skinWetResponse * Skin::wetSkinParams[1].w));
	}
	pbrGlossiness = min(pbrGlossiness, max(1 - averageRoughness, 0.8));  // dry skin keeps its own value
#	endif  // CS_SKIN

	float porosity = 1.0;

#	if defined(SKYLIGHTING)
#		if defined(VR)
	float3 positionMSSkylight = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		else
	float3 positionMSSkylight = input.WorldPosition.xyz;
#		endif
#		if defined(DEFERRED)
	sh2 skylightingSH = Skylighting::sample(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, Skylighting::stbn_vec3_2Dx1D_128x128x64, input.Position.xy, positionMSSkylight, worldNormal);
#		else
	sh2 skylightingSH = inWorld ? Skylighting::sample(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, Skylighting::stbn_vec3_2Dx1D_128x128x64, input.Position.xy, positionMSSkylight, worldNormal) : float4(sqrt(4.0 * Math::PI), 0, 0, 0);
#		endif

#	endif

	float4 waterData = SharedData::GetWaterData(input.WorldPosition.xyz);
	float waterHeight = waterData.w;

	float waterRoughnessSpecular = 1;

#	if defined(WETNESS_EFFECTS)
	// Initialize wetness parameters
	float wetness = 0.0;
	float3 wetnessSpecular = 0.0;
	float3 wetnessNormal = worldNormal;

	// (42c) Is this pixel a character (skin, hair, or anything an actor wears or carries)?
	// Every 42c change below is gated on it, so world surfaces keep the old wetness exactly.
#		if defined(WET_CHARACTER_PERM)
#			if defined(SKIN) || defined(HAIR)
	const bool wetCharacter = true;
#			else
	const bool wetCharacter = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsActorGeometry) != 0;
#			endif
#		else
	const bool wetCharacter = false;
#		endif

	// Calculate shore wetness factors
	float wetnessDistToWater = abs(input.WorldPosition.z - waterHeight);
	float shoreFactor = saturate(1.0 - (wetnessDistToWater / SharedData::wetnessEffectsSettings.ShoreRange));
	float shoreFactorAlbedo = (input.WorldPosition.z < waterHeight) ? 1.0 : shoreFactor;

	// Calculate wetness angle and occlusion
	float minWetnessValue = SharedData::wetnessEffectsSettings.MinRainWetness;
	float minWetnessAngle = saturate(max(minWetnessValue, worldNormal.z));
#		if defined(SKYLIGHTING)
	float wetnessOcclusion = inWorld ? pow(saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1))), 2) : 0.0;
#		else
	float wetnessOcclusion = inWorld;
#		endif
	float flatnessAmount = smoothstep(SharedData::wetnessEffectsSettings.PuddleMaxAngle, 1.0, minWetnessAngle);
	// Calculate raindrop effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	bool shouldCalculateRaindrops = (worldNormal.z > 0.0) &&
	                                (SharedData::wetnessEffectsSettings.Raining > 0.0) &&
	                                (SharedData::wetnessEffectsSettings.EnableRaindropFx) &&
	                                (wetnessOcclusion > 0.5);

	// (46y) Raindrops: Fast Path (Wetness Effects > Raindrops). Raindrop cells smaller than a pixel: the world size of one
	// pixel here (the smaller of the two screen axes, so grazing ground is not cut early) against the grid; full effect up
	// to half a cell per pixel, none from one cell per pixel on (the drops are sub-pixel noise there). Top level: derivatives.
	const uint raindropFlags = SharedData::wetnessEffectsSettings.EnableVanillaRipples;
	const float3 raindropDdx = ddx(input.WorldPosition.xyz), raindropDdy = ddy(input.WorldPosition.xyz);
	const float raindropCellsPerPixel = sqrt(min(dot(raindropDdx, raindropDdx), dot(raindropDdy, raindropDdy))) * SharedData::wetnessEffectsSettings.RaindropGridSizeRcp;
	const float raindropFade = (raindropFlags & WetnessEffects::kRaindropSubPixelSkip) ? saturate(2.0 - 2.0 * raindropCellsPerPixel) : 1.0;

	if (shouldCalculateRaindrops) {
#		if defined(SKINNED)
		float3 ripplePosition = input.ModelPosition.xyz;
#		elif defined(DEFERRED)
		float3 ripplePosition = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
#		else
		float3 ripplePosition = !FrameBuffer::FrameParams.y ? input.ModelPosition.xyz : input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
#		endif
		[branch] if (raindropFlags & WetnessEffects::kRaindropFastPath)
		{
			// Splashes only matter where they can raise the rain wetness below (max(rainWetness, splash), splash <= Strength);
			// skin and hair replace their rain wetness right after, so they never show. Ripples only with a strength above 0
			// (flatnessAmount is their strength: below Puddle Max Angle a ripple is the identity rotation).
#		if defined(SKIN) || defined(HAIR)
			const bool raindropSplashes = false;
#		else
			const float rainWetnessBefore = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
			const bool raindropSplashes = rainWetnessBefore < SharedData::wetnessEffectsSettings.SplashesStrength * raindropFade;
#		endif
			const float rippleModifier = flatnessAmount * raindropFade;
			const bool raindropRipples = rippleModifier > 0.0 && SharedData::wetnessEffectsSettings.RippleStrength != 0.0;
			[branch] if (raindropSplashes || raindropRipples)
			{
				raindropInfo = WetnessEffects::GetRainDropsFast(ripplePosition, SharedData::wetnessEffectsSettings.Time, worldNormal, rippleModifier, raindropSplashes, raindropRipples);
				raindropInfo.w *= raindropFade;
			}
		}
		else
		{
			raindropInfo = WetnessEffects::GetRainDrops(ripplePosition, SharedData::wetnessEffectsSettings.Time, worldNormal, flatnessAmount);
		}
	}

	// Calculate different wetness types
	float rainWetness = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
	rainWetness = max(rainWetness, raindropInfo.w);

#		if defined(SKIN) || defined(HAIR)
		rainWetness = SharedData::wetnessEffectsSettings.SkinWetness * SharedData::wetnessEffectsSettings.Wetness;
#		endif

	// (44b) Wet Skin New model, hair and gear: the character's wetness without the rain (swimming
	// with the same hold and dry-off as the skin, the manual slider, sweat). Added after the sky
	// occlusion below: only the rain stays dry under a roof.
	float charDryWetness = 0.0;
	// (44c) Character Wetness New model: hair and gear (per-class) state, filled below.
	bool charWetNew = false;    // the New model owns this character pixel's wetness (hair or gear)
	bool hairWetActive = false; // HAIR: wet hair in the New model
	bool gearWetNew = false;    // gear: the New per-class gear model
	float4 gearClass = float4(1.0, 0.0, 0.0, 0.0);  // cloth, leather, metal weights; w = fur
	float4 gearParams = 0.0;    // blended class darkening, sheen, roughness floor, dry multiplier
	float gearSwimProgress = 0.0;
	float charWetSwim = 0.0, charWetManual = 0.0, charWetSweat = 0.0;
	float3 geardPdx = 0.0, geardPdy = 0.0;
	float2 gearUVdx = 0.0, gearUVdy = 0.0;
	float gearNormalVariance = 0.0;
	float gearPixelWorld = 0.0;  // (47c) world size of a pixel here, for the droplet / rivulet distance LOD
#		if defined(CS_SKIN) && !defined(SKIN)
	if (skinEnabled) {
		const float charWorldZ = input.WorldPosition.z + FrameBuffer::CameraPosAdjust[eyeIndex].z;
		float2 dynamicWetness = Skin::GetWetness(charWorldZ, worldNormal.xyz);
#			if defined(TRUE_PBR)
		dynamicWetness.x = lerp(dynamicWetness.x, 0.0f, pbrSurfaceProperties.Metallic);
#			endif
		[branch] if (Skin::WetSkinNewModel())
		{
			float wSwim = 0.0, wManual = 0.0, wSweat = 0.0;
#			if defined(HAIR)
			// (44c) Hair: its own drying time (x Hair Dry Time) and sweat share; rain weighted like skin.
			charWetNew = wetCharacter;
			[branch] if (wetCharacter && Skin::wetCharParams[1].x > 0.5)
			{
				hairWetActive = true;
				float progress;
				const float swimHair = Skin::CharSwimClass(clamp(Skin::wetCharParams[0].z, 0.25, 4.0), progress);
				charDryWetness = Skin::CharDryWetnessClass(charWorldZ, dynamicWetness.x, swimHair, 0.0, saturate(Skin::wetCharParams[0].w), wSwim, wManual, wSweat);
				rainWetness *= saturate(Skin::wetSkinParams[0].w);
			}
			else
				rainWetness = 0.0;  // Wet Hair off: not wet at all
#			else
			const float gearMode = Skin::wetCharParams[4].x;  // 0 off, 1 New, 2 42c (44b)
			if (wetCharacter && gearMode > 1.5) {
				charDryWetness = Skin::CharDryWetness(charWorldZ, dynamicWetness.x, wSwim, wManual, wSweat);
			} else if (wetCharacter && gearMode > 0.5) {
				// (44c) Gear: a soft per-pixel material class (cloth / leather / metal, fur), each with
				// its own darkening, sheen, roughness floor and drying time.
				charWetNew = true;
				gearWetNew = true;
				float m0 = 0.0, g0 = 0.0, l0 = 0.0;
#				if defined(TRUE_PBR)
				m0 = ((PBRFlags & PBR::Flags::TwoLayer) != 0) ? 0.7 : saturate(sqrt(pbrSurfaceProperties.Metallic));
				g0 = 1.0 - pbrSurfaceProperties.Roughness;
				l0 = smoothstep(0.25, 0.55, g0);
#				else
#					if defined(SPECULAR)
				g0 = glossiness * Color::RGBToLuminance(SpecularColor.xyz);
#					endif
#					if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX)
				m0 = saturate(sqrt(envMask));
#					else
				m0 = 0.6 * smoothstep(0.55, 0.85, g0);
#					endif
				l0 = smoothstep(0.08, 0.35, g0);
#				endif
#				if defined(BACK_LIGHTING) || defined(SOFT_LIGHTING)
				const bool gearFur = true;
#				else
				const bool gearFur = false;
#				endif
				gearClass = Skin::GearClassWeights(m0, g0, l0, gearFur);
				gearParams = Skin::GearClassParams(gearClass);
				// Swimming per class (cloth stays wet longest), blended like the other parameters.
				float pC, pL, pM, pF;
				const float sC = Skin::CharSwimClass(Skin::wetCharParams[7].w, pC);
				const float sL = Skin::CharSwimClass(Skin::wetCharParams[8].w, pL);
				const float sM = Skin::CharSwimClass(Skin::wetCharParams[9].w, pM);
				const float sF = Skin::CharSwimClass(Skin::wetCharParams[10].w, pF);
				const float swimGear = lerp(gearClass.x * sC + gearClass.y * sL + gearClass.z * sM, sF, gearClass.w);
				gearSwimProgress = lerp(gearClass.x * pC + gearClass.y * pL + gearClass.z * pM, pF, gearClass.w);
				// Cloth wicks water a few units above the water line (irregular tide mark), and the
				// line recedes while drying: the top dries first, the hem last.
				const float clothW = gearClass.x * (1.0 - gearClass.w);
				const float wick = max(Skin::wetCharParams[4].w, 0.0) * clothW;
				const float tide = (Skin::WetValueNoise(uv * 40.0) - 0.5) * 4.0 * clothW;
				const float recede = 0.6 * Skin::wetSkinPerGeometry.y * gearSwimProgress;
				charDryWetness = Skin::CharDryWetnessClass(charWorldZ, dynamicWetness.x, swimGear, wick + tide - recede, saturate(Skin::wetCharParams[5].x), wSwim, wManual, wSweat);
				// Screen derivatives for droplets, rivulets and the specular AA, here in uniform
				// control flow (everything above is per draw).
				geardPdx = ddx(input.WorldPosition.xyz);
				geardPdy = ddy(input.WorldPosition.xyz);
				gearUVdx = ddx(uv);
				gearUVdy = ddy(uv);
				const float3 dNdx = ddx(worldNormal.xyz);
				const float3 dNdy = ddy(worldNormal.xyz);
				gearNormalVariance = 0.5 * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
				const float3 viewRay = normalize(input.WorldPosition.xyz);
				gearPixelWorld = length(input.WorldPosition.xyz) * max(length(ddx(viewRay)), length(ddy(viewRay)));
			} else if (wetCharacter) {
				rainWetness = 0.0;  // Wet Gear off: no character wetness (world surfaces untouched)
			}
#			endif
			charWetSwim = wSwim;
			charWetManual = wManual;
			charWetSweat = wSweat;
		}
		else
		{
			float dynamicWetnessValue = clamp(dynamicWetness.x + dynamicWetness.y, 0.f, 2.f);
#			if defined(HAIR)
			dynamicWetnessValue = min(SharedData::skinData.skinParams2.y + dynamicWetnessValue, 2.0f);
#			endif
			rainWetness += min(dynamicWetnessValue, 1.f);
		}
	}
#		endif
	float shoreWetness = shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness;
	wetness = max(shoreWetness, rainWetness);

	// Calculate puddle effects
	float puddleWetness = SharedData::wetnessEffectsSettings.PuddleWetness * minWetnessAngle;
	float puddle = wetness;

#		if !defined(SKINNED) && !(defined(SKIN) && defined(CS_SKIN))
	if (wetness > 0.0 || puddleWetness > 0.0) {
		float3 puddleCoords = ((input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz) * 0.5 + 0.5) * 0.01 / SharedData::wetnessEffectsSettings.PuddleRadius;
		puddle = Random::perlinNoise(puddleCoords) * 0.5 + 0.5;
		puddle = puddle * ((minWetnessAngle / SharedData::wetnessEffectsSettings.PuddleMaxAngle) * SharedData::wetnessEffectsSettings.MaxPuddleWetness * 0.25) + 0.5;
		puddle *= lerp(wetness, puddleWetness, saturate(puddle - 0.25));
	}
#		endif

	// Apply occlusion and distance factors
	puddle *= wetnessOcclusion * nearFactor;
	const float charRainPart = saturate(puddle);  // (44c) the sky-occluded rain part (gear droplets, rivulets)
	// (44b) Wet Skin New model, hair and gear: swimming, manual and sweat are not sky-occluded.
	[branch] if (charDryWetness > 0.0)
		puddle = max(puddle, 1.0 - (1.0 - saturate(puddle)) * (1.0 - saturate(charDryWetness)));

	// Calculate wetness glossiness factors
	float wetnessGlossinessAlbedo = max(puddle, shoreFactorAlbedo * SharedData::wetnessEffectsSettings.MaxShoreWetness);
	wetnessGlossinessAlbedo *= wetnessGlossinessAlbedo;

	float wetnessGlossinessSpecular = puddle;
	if (input.WorldPosition.z < waterHeight) {
		wetnessGlossinessSpecular *= shoreFactor;
	}
	// (42c, 4.4, always on) Characters: wetness above 1 (up-facing gear reaches ~1.05) made the
	// highlight stronger than the light (x w > 1) and pushed the G-buffer gloss past 1.
	if (wetCharacter)
		wetnessGlossinessSpecular = saturate(wetnessGlossinessSpecular);

	// Update flatness and normal calculations
	flatnessAmount *= smoothstep(SharedData::wetnessEffectsSettings.PuddleMinWetness, 1.0, wetnessGlossinessSpecular);

	// Apply ripple normal effects. (44c) Not on characters in the Character Wetness New model: rain
	// ripples are a puddle effect (they looked like puddle rings on shoulders); the G-buffer keeps the
	// base normal.
#		if defined(SKIN) && defined(CS_SKIN)
	const bool charNoRipple = skinWetNew && skinEnabled;
#		else
	const bool charNoRipple = charWetNew;
#		endif
	if (!charNoRipple) {
		float3 rippleNormal = normalize(lerp(float3(0, 0, 1), raindropInfo.xyz, lerp(flatnessAmount, 1.0, 0.5)));
		wetnessNormal = WetnessEffects::ReorientNormal(rippleNormal, wetnessNormal);
	}

	waterRoughnessSpecular = 1.0 - wetnessGlossinessSpecular;
	// (42c, 4.4, always on) Characters: a fully wet surface had roughness 0, a highlight too
	// small to see (and a 0/0 at its centre). Same 0.05 floor as upstream.
	if (wetCharacter)
		waterRoughnessSpecular = max(waterRoughnessSpecular, 0.05);
	// (44b) Anti-mirror for every character path: what the reflections (SSRT, IBL, cubemaps) see of
	// the water on a character is never smoother than roughness 0.2 (44a did this for skin only).
	float wetIndirectRoughness = wetCharacter ? max(waterRoughnessSpecular, 0.2) : waterRoughnessSpecular;
	// (44b) Wet Skin New model owns hair wetness: hair writes its dry G-buffer values (gloss,
	// reflectance), so SSRT / NRD see wet hair exactly as dry hair.
#		if defined(HAIR) && defined(CS_SKIN)
	const bool hairWetNew = skinEnabled && Skin::WetSkinNewModel();
#		else
	const bool hairWetNew = false;
#		endif

	// (44c) Character Wetness New model, hair and gear. Both read the character's wetness (rain
	// part sky-occluded, the rest not). Hair: its own highlight model gets wet (darker, sharper and
	// brighter primary highlight, weaker secondary, clumps); no water film, dry G-buffer. Gear: soft
	// material classes, each with its own darkening, sheen, roughness floor and G-buffer gloss cap;
	// droplets and rivulets on metal and leather (direct light only).
	float3 wetFilmNormal = wetnessNormal;  // the water film's normal (gear droplet domes); direct light only
	float charWetGlossTarget = -1.0;       // gear New: G-buffer gloss target (1 - film roughness), < 0 = 42c rule
	float charWetGlossCap = 0.8;           // gear New: the class cap
	float charWetDarken = 0.0;             // hair / gear New: how far toward the soaked colour (0..2)
	float gearFilmRoughness = 0.2;
	float gearFilmEnergy = 0.0;
	float gearIndirectEnergy = 0.0;
	float hairWetK = 0.0;
#		if defined(CS_SKIN) && !defined(SKIN)
	[branch] if (charWetNew)
	{
		const float wChar = saturate(puddle);
#			if defined(HAIR)
		const float hairWetW = hairWetActive ? wChar : 0.0;
		hairWetK = smoothstep(0.05, 0.85, hairWetW);
		charWetDarken = clamp(Skin::wetCharParams[1].y, 0.0, 1.5) * hairWetK;
#				if defined(CS_HAIR)
		[branch] if (hairWetK > 0.0 && SharedData::hairSpecularSettings.Enabled)
		{
			Hair::WetK = hairWetK;
			Hair::WetSharpness = lerp(1.0, clamp(Skin::wetCharParams[1].z, 0.4, 1.0), hairWetK);
			Hair::WetGain = lerp(1.0, clamp(Skin::wetCharParams[1].w, 0.5, 1.6), hairWetK);
			Hair::WetTRT = lerp(1.0, 1.0 - saturate(Skin::wetCharParams[2].x), hairWetK);
			Hair::WetScatter = 1.0 - saturate(Skin::wetCharParams[2].y) * hairWetK;
			Hair::WetSpecAA = hairWetSpecAA;
			// (47c) Skip Invisible Work: the clump noise left out above for dry hair, here instead.
			[branch] if (Skin::WetSkipInvisible())
				Skin::WetHairClump(hairClumpL, hairClumpS, hairClumpX, hairClumpR);
			// Clumps: strength from the wetness, faded out where a clump is narrower than ~3 px.
			const float clump = saturate(Skin::wetCharParams[2].z) * smoothstep(0.3, 0.9, hairWetW) * hairClumpFade;
			const float x = hairClumpX;
			const float window = smoothstep(0.0, 0.25, x) * smoothstep(1.0, 0.75, x);
			const float edge = smoothstep(0.0, 0.15, x) * smoothstep(1.0, 0.85, x);
			const float gapWidth = 0.12;
			const float gap = 1.0 - smoothstep(0.0, gapWidth, x) * smoothstep(1.0, 1.0 - gapWidth, x);
			Hair::WetShift = hairClumpR * clamp(Skin::wetCharParams[3].x, 0.0, 0.4) * clump * window;
			Hair::WetCylinder = (x - 0.5) * edge * clump;  // up to +-0.5 rad at the clump's sides
			Hair::WetGap = 1.0 - clamp(Skin::wetCharParams[3].y, 0.0, 0.7) * gap * clump;
		}
		const bool hairVanillaWet = !SharedData::hairSpecularSettings.Enabled;
#				else
		const bool hairVanillaWet = true;
#				endif
		// Hair Specular off: the vanilla highlight, tighter and a little stronger (below).
		if (hairVanillaWet)
			shininess *= lerp(1.0, 2.5, hairWetK);
#			else
		[branch] if (gearWetNew)
		{
			const float clothW = gearClass.x * (1.0 - gearClass.w);
			const float hardW = (gearClass.y + gearClass.z) * (1.0 - gearClass.w);  // leather + metal
			// Soft wet patches (lower contrast than skin: cloth soaks in sheets); cloth spreads sooner.
			float cov = Skin::WetCoverageEx(uv, max(length(gearUVdx), length(gearUVdy)), wChar, SharedData::skinData.wetParams.x, SharedData::skinData.wetParams.y, SharedData::skinData.wetParams.z, Skin::wetSkinParams[2].x, Skin::wetCharParams[4].z);
			cov = lerp(cov, saturate(cov * 1.25), clothW);
			const float resp = pow(saturate(cov), 1.3);

			// Droplets (metal, leather) and rivulets, in mesh uv like skin: they stay put on a moving
			// character; gravity is world down projected on the surface.
			float dropMask = 0.0, dropRim = 0.0, streak = 0.0;
			float2 dropSlope = 0.0;
			float3 tu = float3(1.0, 0.0, 0.0), tv = float3(0.0, 1.0, 0.0);
			const float dropDensity = saturate(Skin::wetSkinParams[2].w) * (gearClass.z * saturate(Skin::wetCharParams[6].x) + gearClass.y * saturate(Skin::wetCharParams[5].z)) * (1.0 - gearClass.w);
			const float rivAmount = (gearClass.z * saturate(Skin::wetCharParams[6].y) + gearClass.y * saturate(Skin::wetCharParams[5].w)) * (1.0 - gearClass.w);
			// (47c) Distance LOD (Faster Noise & Distance LOD; 1 when off): no droplets or rivulets on gear far away.
			const float gearDetailLOD = Skin::WetDetailLOD(gearPixelWorld);
			[branch] if ((dropDensity > 0.0 || rivAmount > 0.0) && wChar > 0.0 && gearDetailLOD > 0.0)
			{
				const float3 baseN = worldNormal.xyz;
				const float uvDet = gearUVdx.x * gearUVdy.y - gearUVdx.y * gearUVdy.x;
				const float uvDetSafe = abs(uvDet) > 1e-12 ? uvDet : 1e-12;
				float3 dPdu = (geardPdx * gearUVdy.y - geardPdy * gearUVdx.y) / uvDetSafe;
				float3 dPdv = (geardPdy * gearUVdx.x - geardPdx * gearUVdy.x) / uvDetSafe;
				dPdu -= baseN * dot(baseN, dPdu);
				dPdv -= baseN * dot(baseN, dPdv);
				const float3 down = float3(0.0, 0.0, -1.0);
				const float alignment = saturate(length(down - baseN * dot(baseN, down)));
				float2 gravityUV = float2(dot(down, dPdu) / max(dot(dPdu, dPdu), 1e-12), dot(down, dPdv) / max(dot(dPdv, dPdv), 1e-12));
				gravityUV = dot(gravityUV, gravityUV) > 1e-20 ? normalize(gravityUV) : float2(0.0, 1.0);
				const float uvFootprint = max(length(gearUVdx), length(gearUVdy));
				tu = dot(dPdu, dPdu) > 1e-20 ? normalize(dPdu) : tu;
				tv = dot(dPdv, dPdv) > 1e-20 ? normalize(dPdv) : tv;

				const float dropOthers = max(charWetSwim, charWetManual);
				const float dropRain = charRainPart * saturate(Skin::wetSkinParams[5].w);
				const float dropDriver = smoothstep(0.35, 0.9, max(dropOthers, dropRain));
				const float cellsPerUV = 230.0 / clamp(Skin::wetSkinParams[3].x, 0.25, 4.0);
				const float footprint = max(uvFootprint * cellsPerUV, 1e-6);
				// Beads are about 0.3 cell in radius: fade them out below ~1.5 pixels (no DLSS sparkle).
				const float dropVisibility = smoothstep(1.0, 2.5, 0.3 / footprint) * gearDetailLOD;
				const float restFactor = lerp(0.7, 1.2, saturate(baseN.z * 0.5 + 0.5));
				const float streakDriver = max(smoothstep(0.2, 0.85, dropOthers), charRainPart);
				const float presence = dropDensity * dropDriver * restFactor * (dropVisibility > 0.0 ? 1.0 : 0.0);
				const Skin::WetSkinDetail detail = Skin::WetSkinDroplets(uv * cellsPerUV, footprint, gravityUV, alignment, presence, streakDriver);
				dropMask = detail.dropMask * dropVisibility;
				dropRim = detail.dropRim * dropMask;
				dropSlope = detail.dropSlope;
				const float rivulets = rivAmount > 0.0 ? Skin::WetSkinRivulets(uv, 25.0, uvFootprint, gravityUV, alignment, streakDriver * rivAmount, gearDetailLOD) : 0.0;
				streak = saturate(max(rivulets, detail.trail * 0.8 * dropVisibility * hardW));
			}

			// Darkening: per class (metal none), more along leather rivulets and at bead rims on leather.
			charWetDarken = gearParams.x * pow(saturate(cov), 0.8) + (0.5 * streak + saturate(Skin::wetSkinParams[3].w) * dropRim) * gearClass.y * (1.0 - gearClass.w);
#				if defined(DYNAMIC_SNOW_SURFACE)
			charWetDarken *= 1.0 - dynamicSnowCoverage;  // snow lies on top of the wet surface
#				endif

			// Sheen: the 42c film, its roughness and energy per class. Specular AA: curved or distant
			// gear (normal varying across the pixel) gets a wider lobe, never a single bright pixel.
			const float overall = clamp(Skin::wetCharParams[4].y, 0.0, 3.0);
			const float sheen = max(gearParams.y, 0.0) * overall;
			const float fullStrength = clamp(Skin::wetSkinParams[1].w, 0.0, 2.5);
			float rough = max(lerp(gearParams.z + 0.15, gearParams.z, resp) - 0.04 * streak, 0.2);
			const float aaVar = min(2.0 * gearNormalVariance, 0.18);
			rough = sqrt(sqrt(rough * rough * rough * rough + aaVar));
			const float dropRough = sqrt(sqrt(0.0001 + aaVar));
			float energy = sheen * resp * fullStrength * (1.0 + 1.5 * streak);
			energy += 1.5 * dropMask * saturate(wChar * 2.0) * min(overall, 1.0);
			gearFilmRoughness = lerp(rough, max(dropRough, 0.1), saturate(dropMask));
			gearFilmEnergy = min(max(energy, 0.0), 2.5);
			gearIndirectEnergy = saturate(resp * sheen * 0.6);
			charWetGlossTarget = 1.0 - rough;
			charWetGlossCap = Skin::GearClassGlossCap(gearClass);
			const float domeStrength = clamp(Skin::wetSkinParams[5].z, 0.0, 2.0);
			[branch] if (dropMask > 0.0)
				wetFilmNormal = normalize(wetnessNormal + (dropSlope.x * tu + dropSlope.y * tv) * domeStrength * dropMask);
		}
#			endif
		// The New model replaces the 42c generic lobes on hair and gear: hair has no water film and
		// reflects as when dry; gear reflects its own film (energy <= 1, roughness >= 0.2).
		wetnessGlossinessSpecular = gearWetNew ? gearIndirectEnergy : 0.0;
		waterRoughnessSpecular = (gearWetNew && gearIndirectEnergy > 0.0) ? max(1.0 - charWetGlossTarget, 0.2) : 1.0;
		wetIndirectRoughness = max(waterRoughnessSpecular, 0.2);
	}
#		endif

	// (42c) Character wetness: 4.3 occlusion for the rain-fed skin film, 4.2 darkening, 4.1 film.
#		if defined(SKIN) && defined(CS_SKIN)
	// (47c) The skin's droplets and rivulets (set up with the coverage above). Skip Invisible Work:
	// left out where the sky is fully occluded (factor 0) and rain is the only water on this
	// character: the occlusion below multiplies the rain droplets by exactly 0 (drop-only-from-rain
	// = 1) and the rivulets by 1 - rainShare, which is 0 up to one division rounding (rainShare =
	// wAll / wAll); the film then has no energy, so the bead domes in wetWorldNormal are unused.
	[branch] if (skinWetDetailPending)
	{
		const bool rainDetailHidden = Skin::WetSkipInvisible() && skinWetNoOtherWet && saturate(wetnessOcclusion * nearFactor) <= 0.0;
		[branch] if (!rainDetailHidden)
			Skin::SkinWetDetail(skinWetDetail, skinWetDropMask, skinWetDropRim, skinWetStreak, wetWorldNormal);
		else
			wetWorldNormal = normalize(skinWetDetail.baseN);
	}
	// 4.3: the rain share of the Advanced Skin film follows the same sky occlusion as the rest
	// of Wetness Effects (a character under a roof is not rained on).
	skinSurfaceProperties.Wetness *= lerp(1.0, saturate(wetnessOcclusion * nearFactor), skinRainShare);
	// (44a) New model: droplets made by rain, and the rain's rivulets, follow it too.
	skinWetDropMask *= lerp(1.0, saturate(wetnessOcclusion * nearFactor), skinWetDropRainOnly);
	skinWetDropRim *= lerp(1.0, saturate(wetnessOcclusion * nearFactor), skinWetDropRainOnly);
	skinWetStreak *= lerp(1.0, saturate(wetnessOcclusion * nearFactor), skinRainShare);
#		endif

#		if defined(WET_CHARACTER_PERM)
	// 4.2: darken here, before any light is evaluated and before the skin takes its albedo,
	// so the sun, ambient, SSGI (G-buffer albedo) and SSS all see the wet colour. The face
	// (FACEGEN) is included, so face and body match. Wet albedo of a fully soaked porous
	// material: A / (n^2 - (n^2 - 1) A) with n = 1.33 (Twomey et al. 1986), per channel, which
	// darkens dark colours relatively more and saturates slightly. Porosity scales the step
	// (1 = fully absorbent, about a third darker at A = 0.4).
#			if defined(SKIN) && defined(CS_SKIN)
	const bool skinWetNewActive = skinWetNew && skinEnabled;  // (44a) the Wet Skin New model owns skin wetness
#			else
	const bool skinWetNewActive = false;
#			endif
	// (44c) Character Wetness New: hair and gear darken by their own amount (charWetDarken), the 42c
	// switch has no say there.
	const bool wetDarkenNew = wetCharacter && (SharedData::wetnessEffectsSettings.EnableWetDarkening || skinWetNewActive || charWetNew);
	[branch] if (wetDarkenNew)
	{
		float wetDarkenWetness = saturate(max(puddle, shoreFactorAlbedo * SharedData::wetnessEffectsSettings.MaxShoreWetness));
#			if defined(SKIN) && defined(CS_SKIN)
		if (skinEnabled)
			wetDarkenWetness = max(wetDarkenWetness, skinSurfaceProperties.Wetness);
#			endif
#			if defined(SKIN)
		float wetPorosity = SharedData::wetnessEffectsSettings.PorositySkin;
#			elif defined(HAIR)
		float wetPorosity = SharedData::wetnessEffectsSettings.PorosityOther;
#			else
		float wetMetalness = 0.0;
#				if defined(TRUE_PBR)
		wetMetalness = ((PBRFlags & PBR::Flags::TwoLayer) != 0) ? 1.0 : saturate(sqrt(pbrSurfaceProperties.Metallic));
#				elif defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX)
		wetMetalness = saturate(sqrt(envMask));
#				endif
		float wetPorosity = lerp(SharedData::wetnessEffectsSettings.PorosityCloth, SharedData::wetnessEffectsSettings.PorosityMetal, wetMetalness);
#			endif
#			if defined(DYNAMIC_SNOW_SURFACE)
		wetPorosity *= 1.0 - dynamicSnowCoverage;  // snow lies on top of the wet surface
#			endif
		// Darkening shows as soon as the surface layer is damp, well before it is soaked.
		float wetDarkenAmount = wetPorosity * sqrt(wetDarkenWetness);
#			if defined(SKIN) && defined(CS_SKIN)
		if (skinWetNewActive)  // (44a) the soft coverage, rivulets and bead rims
			wetDarkenAmount = Skin::WetSkinDarkening(skinSurfaceProperties.Wetness, skinWetStreak, skinWetDropRim);
#			endif
		const float3 wetAlbedo = baseColor.xyz / (1.77 - 0.77 * saturate(baseColor.xyz));
#			if defined(CS_SKIN)
		[branch] if (charWetNew)
			baseColor.xyz = Skin::WetDarkenAlbedo(baseColor.xyz, charWetDarken);
		else
#			endif
			baseColor.xyz = max(lerp(baseColor.xyz, wetAlbedo, wetDarkenAmount), 0.0);
#			if defined(SKIN) && defined(CS_SKIN)
		skinSurfaceProperties.Albedo = baseColor.xyz;
#			endif
	}
#		endif

	// 4.1: film weight = wetness (once, clamped), times the strength slider. On skin with
	// Advanced Skin the film replaces the Advanced Skin direct film (rain and swimming merged
	// with max, so they never stack); elsewhere it replaces the old "rougher when drier,
	// then x wetness" lobe.
	// (44c) Character Wetness New: gear draws its own per-class film here (hair none); the 42c generic
	// lobes are off for both.
	const bool wetFilm = wetCharacter && (SharedData::wetnessEffectsSettings.EnableWetFilm || charWetNew);
	const float wetFilmRoughness = gearWetNew ? gearFilmRoughness : SharedData::wetnessEffectsSettings.WetFilmRoughness;
	const float wetFilmWeight = saturate(wetnessGlossinessSpecular);
	float3 wetFilmSpecular = 0.0;
#		if defined(SKIN) && defined(CS_SKIN)
	// (44a) The New model always draws the skin film itself (WetSkinFilm below).
	const bool wetFilmInSkin = (wetFilm || skinWetNew) && skinEnabled;
	if (wetFilmInSkin && !skinWetNew) {
		skinSurfaceProperties.WetFilmRoughness = wetFilmRoughness;
		skinSurfaceProperties.WetFilmEnergy = saturate(max(wetFilmWeight, Skin::GetWetnessOpticalResponse(skinSurfaceProperties.Wetness) * max(skinSurfaceProperties.WetnessFilmStrength, 0.0))) * SharedData::wetnessEffectsSettings.WetFilmStrength;
	}
#		else
	const bool wetFilmInSkin = false;
#		endif
	// Film handled here (not inside the Advanced Skin BRDF), and its weight.
	const bool wetFilmGeneric = charWetNew ? (gearWetNew && gearFilmEnergy > 0.0) : (wetFilm && !wetFilmInSkin && wetFilmWeight > 0.0);
	const float wetFilmEnergy = gearWetNew ? gearFilmEnergy : wetFilmWeight * SharedData::wetnessEffectsSettings.WetFilmStrength;
#	endif

#	if defined(SKIN) && defined(CS_SKIN)
	// (44a) Wet Skin New model: the film this pixel shows (with or without Wetness Effects).
	[branch] if (skinWetNew && skinEnabled)
	{
#		if !defined(WETNESS_EFFECTS)
		// Without Wetness Effects there is no 4.2 pass above: darken here.
		const float3 skinWetAlbedo = baseColor.xyz / (1.77 - 0.77 * saturate(baseColor.xyz));
		baseColor.xyz = max(lerp(baseColor.xyz, skinWetAlbedo, Skin::WetSkinDarkening(skinSurfaceProperties.Wetness, skinWetStreak, skinWetDropRim)), 0.0);
		skinSurfaceProperties.Albedo = baseColor.xyz;
#		endif
		Skin::WetSkinFilm(skinSurfaceProperties, skinWetStreak, skinWetStreakDriver, skinWetDropMask);
	}
#	endif

	float llDirLightMult = SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear && (inWorld || inReflection) && !SharedData::InInterior ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	float3 dirLightColor = Color::DirectionalLight(DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
	float3 dirLightColorMultiplier = 1;

#	if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		dirLightColorMultiplier *= PhysSky::SampleTr(normalize(DirLightDirection.xyz), SampShadowMaskSampler);
#	endif

#	if defined(WATER_EFFECTS)
	dirLightColorMultiplier *= WaterEffects::ComputeCaustics(waterData, input.WorldPosition.xyz, eyeIndex);
#	endif

	float dirLightAngle = dot(worldNormal.xyz, DirLightDirection.xyz);

	if ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) && (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::ShadowDir)) {
		dirLightColorMultiplier *= shadowColor.x;
	}
#	if !defined(DEFERRED)
	else if (!SharedData::InInterior && inWorld) {
#		if defined(VOLUMETRIC_SHADOWS)
		// (batch 38, A2, upstream 0f79d567a) soft VSM sun shadow instead of the 16-tap PCF
		float vsmDetailedShadow;
		[branch] if (VolumetricShadows::ForwardSoftShadowsEnabled() && !inReflection)
		{
			VolumetricShadows::GetVSMShadow2D(input.WorldPosition.xyz, eyeIndex, vsmDetailedShadow);
			dirLightColorMultiplier *= vsmDetailedShadow;
		}
		else
#		endif
			dirLightColorMultiplier *= ShadowSampling::GetLightingShadow(screenNoise, input.WorldPosition.xyz, eyeIndex);
	}
#	endif

#	if defined(SOFT_LIGHTING) || defined(BACK_LIGHTING) || defined(RIM_LIGHTING)
	bool inDirShadow = ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) && (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::ShadowDir) && shadowColor.x == 0);
#	else
	bool inDirShadow = ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) && (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::ShadowDir) && shadowColor.x == 0) && dirLightAngle > 0.0;
#	endif

	float3 refractedDirLightDirection = DirLightDirection;
#	if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
	{
		if (dot(DirLightDirection, coatWorldNormal) > 0)
			refractedDirLightDirection = -refract(-DirLightDirection, coatWorldNormal, eta);
	}
#	endif

	float dirDetailShadow = 1.0;
	float dirShadow = 1.0;
	float parallaxShadow = 1;

#	if defined(SCREEN_SPACE_SHADOWS) && defined(DEFERRED)
	if (!SharedData::InInterior)
		dirDetailShadow = ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, screenUV, screenNoise, eyeIndex);
#	endif

#	if defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
	[branch] if (inWorld && SharedData::extendedMaterialSettings.EnableShadows)
	{
		float3 dirLightDirectionTS = mul(refractedDirLightDirection, tbn).xyz;
#		if defined(LANDSCAPE)
#			if defined(TRUE_PBR)
	if (SharedData::extendedMaterialSettings.EnableParallax){
#			else
	if (SharedData::extendedMaterialSettings.EnableTerrainParallax || (SharedData::extendedMaterialSettings.EnableParallax && Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement)){
#			endif
#			if defined(TERRAIN_VARIATION)
			float weights[6];
			// Initialize weights array
			weights[0] = weights[1] = weights[2] = weights[3] = weights[4] = weights[5] = 0.0;

			float sh0 = ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, displacementParams, parallaxShadowQuality, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, dx, dy, weights);

			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, dirLightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams, sharedOffset, dx, dy);
#			else
			// Standard terrain parallax shadow without stochastic sampling
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, dirLightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams);
#			endif
		}
#		elif defined(PARALLAX)
		[branch] if (SharedData::extendedMaterialSettings.EnableParallax)
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, lerp(parallaxShadowQuality, 1.0, SharedData::extendedMaterialSettings.ExtendShadows), screenNoise, displacementParams);
#		elif defined(EMAT_ENVMAP)
		[branch] if (complexMaterialParallax)
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexEnvMaskSampler, SampEnvMaskSampler, 3, lerp(parallaxShadowQuality, 1.0, SharedData::extendedMaterialSettings.ExtendShadows), screenNoise, displacementParams);
#		elif defined(TRUE_PBR) && !defined(LODLANDSCAPE)
		[branch] if (PBRParallax)
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, lerp(parallaxShadowQuality, 1.0, SharedData::extendedMaterialSettings.ExtendShadows), screenNoise, displacementParams);
#		endif  // LANDSCAPE
	}
#	endif  // defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))

#	if defined(DYNAMIC_SNOW_SURFACE)
	// (batch 39c) Snow fills the material's relief, so its parallax self-shadow fades with the cover.
	// (40e) Only by SnowSmoothing (default 0.2): the relief still shades the snow on it.
	if (SharedData::dynamicSnowSettings.Flags & DynamicSnow::FlagCoverageSlope)
		parallaxShadow = lerp(parallaxShadow, 1.0, dynamicSnowCoverage * DynamicSnow::SnowSmoothing());
#	endif

	if (dirShadow != 0.0 && (inWorld || inReflection))
		dirShadow *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

	dirLightColorMultiplier *= dirShadow;

	float3 diffuseColor = 0.0.xxx;
	float3 specularColor = 0.0.xxx;

	float3 lightsDiffuseColor = 0.0.xxx;
	float3 coatLightsDiffuseColor = 0.0.xxx;
	float3 lightsSpecularColor = 0.0.xxx;

	float3 lodLandDiffuseColor = 0;

#	if defined(TRUE_PBR)
	{
		PBR::LightProperties lightProperties = PBR::InitLightProperties(dirLightColor, dirLightColorMultiplier * dirDetailShadow, parallaxShadow);
		float3 dirDiffuseColor, coatDirDiffuseColor, dirTransmissionColor, dirSpecularColor;
		PBR::GetDirectLightInput(dirDiffuseColor, coatDirDiffuseColor, dirTransmissionColor, dirSpecularColor, worldNormal.xyz, coatWorldNormal, refractedViewDirection, viewDirection, refractedDirLightDirection, DirLightDirection, lightProperties, pbrSurfaceProperties, tbnTr, uvOriginal);
		lightsDiffuseColor += dirDiffuseColor;
		coatLightsDiffuseColor += coatDirDiffuseColor;
		transmissionColor += dirTransmissionColor;
		specularColorPBR += dirSpecularColor * !SharedData::InInterior;
#		if defined(LOD_LAND_BLEND)
		lodLandDiffuseColor += dirLightColor / Math::PI * saturate(dirLightAngle) * dirLightColorMultiplier * dirDetailShadow * parallaxShadow;
#		endif
#		if defined(WETNESS_EFFECTS)
		if (wetFilmGeneric)
			wetFilmSpecular += GetWetFilmSpecular(wetFilmNormal, viewDirection, DirLightDirection, lightProperties.CoatLightColor, wetFilmRoughness);
		else if (waterRoughnessSpecular < 1.0 && !wetFilm)
			specularColorPBR += PBR::GetWetnessDirectLightSpecularInput(wetnessNormal, viewDirection, DirLightDirection, lightProperties.CoatLightColor, waterRoughnessSpecular) * wetnessGlossinessSpecular;
#		endif
	}
#	elif defined(SKIN) && defined(CS_SKIN)
	if (skinEnabled) {
		PBR::LightProperties lightProperties = PBR::InitLightProperties(dirLightColor, dirLightColorMultiplier * dirDetailShadow, parallaxShadow);
		float3 dirDiffuseColor, dirTransmissionColor, dirSpecularColor;
		Skin::SkinDirectLightInput(dirDiffuseColor, dirTransmissionColor, dirSpecularColor, lightProperties, skinSurfaceProperties, originalWorldNormal, viewDirection, DirLightDirection, wetWorldNormal);
		lightsDiffuseColor += dirDiffuseColor;
		transmissionColor += dirTransmissionColor;
		float3 sssTransmittance = Skin::SSSSTransmittance(
									  SharedData::skinData.sssParams.x,
									  SharedData::skinData.sssParams.y,
									  originalWorldNormal,
									  DirLightDirection,
									  skinSurfaceProperties.Thickness) *
		                          SharedData::skinData.sssParams.w;
		transmissionColor += min(sssTransmittance * lightProperties.LightColor * skinSurfaceProperties.Albedo, dirLightColor * dirLightColorMultiplier);
		specularColorPBR += dirSpecularColor * !SharedData::InInterior;
#		if defined(WETNESS_EFFECTS)
		// (42c) With the film on, the Advanced Skin BRDF above already drew it.
		if (waterRoughnessSpecular < 1.0 && !wetFilmInSkin && skinRainStrength <= 0.0)
			specularColorPBR += PBR::GetWetnessDirectLightSpecularInput(wetnessNormal, viewDirection, DirLightDirection, lightProperties.CoatLightColor, max(waterRoughnessSpecular, 0.2)) * wetnessGlossinessSpecular;
#		endif
	} else {
		dirDetailShadow *= parallaxShadow;
		dirLightColor *= dirLightColorMultiplier;
		float3 dirDiffuseColor = dirLightColor * saturate(dirLightAngle) * dirDetailShadow * Color::VanillaDiffuseMult();

#		if defined(SPECULAR) || defined(SPARKLE)
		lightsSpecularColor = GetLightSpecularInput(input, DirLightDirection, viewDirection, worldNormal.xyz, dirLightColor.xyz * dirDetailShadow, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#		endif

		lightsDiffuseColor += dirDiffuseColor;

#		if defined(WETNESS_EFFECTS)
		if (wetFilmGeneric)
			wetFilmSpecular += GetWetFilmSpecular(wetFilmNormal, viewDirection, DirLightDirection, dirLightColor * dirDetailShadow, wetFilmRoughness);
		else if (waterRoughnessSpecular < 1.0 && !wetFilm)
			wetnessSpecular += WetnessEffects::GetWetnessSpecular(wetnessNormal, DirLightDirection, viewDirection, dirLightColor * dirDetailShadow, waterRoughnessSpecular);
#		endif
	}
#	else
	dirDetailShadow *= parallaxShadow;
	dirLightColor *= dirLightColorMultiplier;

	float3 dirDiffuseColor = dirLightColor * saturate(dirLightAngle) * dirDetailShadow * Color::VanillaDiffuseMult();

#		if defined(SOFT_LIGHTING)
	lightsDiffuseColor += dirLightColor * GetSoftLightMultiplier(dirLightAngle) * rimSoftLightColor.xyz * Color::VanillaDiffuseMult() * EYE_SOFT_LIGHTING_SCALE * EYE_BACK_LIGHT_GATE(DirLightDirection) * SUN_SOFT_SHADOW;
#		endif

#		if defined(RIM_LIGHTING)
	lightsDiffuseColor += dirLightColor * GetRimLightMultiplier(DirLightDirection, viewDirection, worldNormal.xyz) * rimSoftLightColor.xyz * Color::VanillaDiffuseMult() * EYE_BACK_LIGHT_GATE(DirLightDirection) * SUN_SOFT_SHADOW;
#		endif

#		if defined(BACK_LIGHTING)
	lightsDiffuseColor += dirLightColor * saturate(-dirLightAngle) * backLightColor.xyz * Color::VanillaDiffuseMult() * SUN_SOFT_SHADOW;
#		endif

	if (useSnowSpecular && useSnowDecalSpecular) {
#		if defined(SNOW)
		lightsSpecularColor += GetSnowSpecularColor(input, worldNormal.xyz, viewDirection);
#		endif
	} else {
#		if defined(HAIR) && defined(CS_HAIR)
		if (SharedData::hairSpecularSettings.Enabled) {
			float3 dirTransmissionColor = 0.0;
			float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, DirLightDirection, screenNoise, eyeIndex);
			Hair::GetHairDirectLight(dirDiffuseColor, lightsSpecularColor, dirTransmissionColor, hairT, DirLightDirection, viewDirection, worldNormal.xyz, vertexNormal.xyz, dirLightColor.xyz * dirDetailShadow, SharedData::hairSpecularSettings.HairGlossiness, hairShadow, uv, baseColor.xyz);
			transmissionColor += dirTransmissionColor;
		}
		else {
#			if defined(SPECULAR)
			lightsSpecularColor = GetLightSpecularInput(input, DirLightDirection, viewDirection, worldNormal.xyz, dirLightColor.xyz * dirDetailShadow, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#			endif
		}
#		elif defined(SPECULAR) || defined(SPARKLE)
		lightsSpecularColor = GetLightSpecularInput(input, DirLightDirection, viewDirection, worldNormal.xyz, dirLightColor.xyz * dirDetailShadow, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#		endif
	}

	lightsDiffuseColor += dirDiffuseColor;

#		if defined(WETNESS_EFFECTS)
	if (wetFilmGeneric)
		wetFilmSpecular += GetWetFilmSpecular(wetFilmNormal, viewDirection, DirLightDirection, dirLightColor * dirDetailShadow, wetFilmRoughness);
	else if (waterRoughnessSpecular < 1.0 && !wetFilm)
		wetnessSpecular += WetnessEffects::GetWetnessSpecular(wetnessNormal, DirLightDirection, viewDirection, dirLightColor * dirDetailShadow, waterRoughnessSpecular);
#		endif
#	endif

#	if !defined(LOD)
#		if !defined(LIGHT_LIMIT_FIX)
	[loop] for (uint lightIndex = 0; lightIndex < numLights; lightIndex++)
	{
		float3 lightDirection = PointLightPosition[eyeIndex * numLights + lightIndex].xyz - input.WorldPosition.xyz;
		float lightDist = length(lightDirection);
		float intensityFactor = saturate(lightDist / PointLightPosition[lightIndex].w);
		if (intensityFactor == 1)
			continue;

		float intensityMultiplier = 1 - intensityFactor * intensityFactor;
		float3 lightColor = Color::PointLight(PointLightColor[lightIndex].xyz) * intensityMultiplier;
		float lightShadow = 1.f;
		if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) {
			if (lightIndex < numShadowLights) {
				lightShadow *= shadowColor[ShadowLightMaskSelect[lightIndex]];
			}
		}

		float3 normalizedLightDirection = normalize(lightDirection);

#			if defined(TRUE_PBR)
		{
			float3 pointDiffuseColor, coatPointDiffuseColor, pointTransmissionColor, pointSpecularColor;
			float3 refractedLightDirection = normalizedLightDirection;
#				if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
			[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
			{
				if (dot(normalizedLightDirection, coatWorldNormal) > 0)
					refractedLightDirection = -refract(-normalizedLightDirection, coatWorldNormal, eta);
			}
#				endif
			PBR::LightProperties lightProperties = PBR::InitLightProperties(lightColor, lightShadow, 1);
			PBR::GetDirectLightInput(pointDiffuseColor, coatPointDiffuseColor, pointTransmissionColor, pointSpecularColor, worldNormal.xyz, coatWorldNormal, refractedViewDirection, viewDirection, refractedLightDirection, normalizedLightDirection, lightProperties, pbrSurfaceProperties, tbnTr, uvOriginal);
			lightsDiffuseColor += pointDiffuseColor;
			coatLightsDiffuseColor += coatPointDiffuseColor;
			transmissionColor += pointTransmissionColor;
			specularColorPBR += pointSpecularColor;
		}
#			elif defined(SKIN) && defined(CS_SKIN)
		if (skinEnabled) {
			float3 pointDiffuseColor, pointTransmissionColor, pointSpecularColor;
			PBR::LightProperties lightProperties = PBR::InitLightProperties(lightColor, lightShadow, 1);
			Skin::SkinDirectLightInput(pointDiffuseColor, pointTransmissionColor, pointSpecularColor, lightProperties, skinSurfaceProperties, originalWorldNormal, viewDirection, normalizedLightDirection, wetWorldNormal);
			lightsDiffuseColor += pointDiffuseColor;
			transmissionColor += pointTransmissionColor;
			float3 sssTransmittance = Skin::SSSSTransmittance(
										  SharedData::skinData.sssParams.x,
										  SharedData::skinData.sssParams.y,
										  originalWorldNormal,
										  normalizedLightDirection,
										  skinSurfaceProperties.Thickness) *
			                          SharedData::skinData.sssParams.w;
			transmissionColor += min(sssTransmittance * lightProperties.LightColor * skinSurfaceProperties.Albedo, lightProperties.LightColor);
			specularColorPBR += pointSpecularColor;
		} else {
			lightColor *= lightShadow;
			float lightAngle = dot(worldNormal.xyz, normalizedLightDirection.xyz);
			float3 lightDiffuseColor = lightColor * saturate(lightAngle.xxx) * Color::VanillaDiffuseMult();

#				if defined(SPECULAR) || (defined(SPARKLE) && !defined(SNOW))
			lightsSpecularColor += GetLightSpecularInput(input, normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#				endif  // defined (SPECULAR) || (defined (SPARKLE) && !defined(SNOW))

			lightsDiffuseColor += lightDiffuseColor;
		}
#			else
		lightColor *= lightShadow;
		float lightAngle = dot(worldNormal.xyz, normalizedLightDirection.xyz);
		float3 lightDiffuseColor = lightColor * saturate(lightAngle.xxx) * Color::VanillaDiffuseMult();

#				if defined(SOFT_LIGHTING)
		lightDiffuseColor += lightColor * GetSoftLightMultiplier(lightAngle) * rimSoftLightColor.xyz * Color::VanillaDiffuseMult() * EYE_SOFT_LIGHTING_SCALE * EYE_BACK_LIGHT_GATE(normalizedLightDirection);
#				endif  // SOFT_LIGHTING

#				if defined(RIM_LIGHTING)
		lightDiffuseColor += lightColor * GetRimLightMultiplier(normalizedLightDirection, viewDirection, worldNormal.xyz) * rimSoftLightColor.xyz * Color::VanillaDiffuseMult() * EYE_BACK_LIGHT_GATE(normalizedLightDirection);
#				endif  // RIM_LIGHTING

#				if defined(BACK_LIGHTING)
		lightDiffuseColor += lightColor * saturate(-lightAngle) * backLightColor.xyz * Color::VanillaDiffuseMult();
#				endif  // BACK_LIGHTING
#				if defined(HAIR) && defined(CS_HAIR)
		if (SharedData::hairSpecularSettings.Enabled) {
			float3 lightSpecularColor = 0;
			float3 lightTransmissionColor = 0;
			float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, normalizedLightDirection, screenNoise, eyeIndex);
			Hair::GetHairDirectLight(lightDiffuseColor, lightSpecularColor, lightTransmissionColor, hairT, normalizedLightDirection, viewDirection, worldNormal.xyz, vertexNormal.xyz, lightColor, SharedData::hairSpecularSettings.HairGlossiness, hairShadow, uv, baseColor.xyz);
			lightsSpecularColor += lightSpecularColor;
			transmissionColor += lightTransmissionColor;
		} else {
#					if defined(SPECULAR)
			lightsSpecularColor += GetLightSpecularInput(input, normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#					endif
		}
#				elif defined(SPECULAR) || (defined(SPARKLE) && !defined(SNOW))
		lightsSpecularColor += GetLightSpecularInput(input, normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#				endif  // defined (SPECULAR) || (defined (SPARKLE) && !defined(SNOW))

		lightsDiffuseColor += lightDiffuseColor;
#			endif
	}

#		else

#			if defined(SSPLS)
	// (46y) Debug view SSPLS Coverage: per pixel the point light reaching it, by kind of shadow (x RT only, y RT + screen-space for
	// non-RT casters, z not ray traced: screen-space and / or shadow map, w no shadow), weighted by each light's brightness here.
	const bool ssplsCoverageView = (SharedData::ssplsSettings.Enable & ScreenSpacePointLightShadows::SettingFlags::CoverageView) != 0;
	float4 ssplsCoverage = 0.0;
#			endif
	uint numClusteredLights = 0;
	uint totalLightCount = LightLimitFix::NumStrictLights;
	uint clusterIndex = 0;
	uint lightOffset = 0;
	if (inWorld && LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		numClusteredLights = LightLimitFix::lightGrid[clusterIndex].lightCount;
		totalLightCount += numClusteredLights;
		lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
	}

	[loop] for (uint lightIndex = 0; lightIndex < totalLightCount; lightIndex++)
	{
		LightLimitFix::Light light;
		if (lightIndex < LightLimitFix::NumStrictLights) {
			light = LightLimitFix::StrictLights[lightIndex];
		} else {
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + (lightIndex - LightLimitFix::NumStrictLights)];
			light = LightLimitFix::lights[clusteredLightIndex];

			if (LightLimitFix::IsLightIgnored(light))
				continue;
		}

		float3 lightDirection = light.positionWS[eyeIndex].xyz - input.WorldPosition.xyz;
		float lightDist = length(lightDirection);

#			if defined(ISL)
		float intensityMultiplier = InverseSquareLighting::GetAttenuation(lightDist, light);
		if (intensityMultiplier < 1e-5)
			continue;
#			else
		float intensityFactor = saturate(lightDist / light.radius);
		if (intensityFactor == 1)
			continue;
		float intensityMultiplier = 1 - intensityFactor * intensityFactor;
#			endif

		const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
		float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * intensityMultiplier * light.fade;
		float lightShadow = 1.0;

		float shadowComponent = 1.0;
#			if defined(RT_POINT_SHADOWS)
		// (46b) Ray-traced visibility of this light at this pixel (false: the old path below, unchanged).
		float rtVisibility = 1.0;
		const bool rtShadow = RTPointShadows::Get(light.rtShadow, input.Position.xy, screenUV, viewPosition.z, eyeIndex, rtVisibility);
#			else
		const bool rtShadow = false;
		const float rtVisibility = 1.0;
#			endif
		bool rtApplied = false;
		if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) {
			if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
				// (46b) An engine shadow-map light with a ray-traced source: the traced visibility instead of its shadow map.
				shadowComponent = rtShadow ? rtVisibility : shadowColor[light.shadowLightIndex];
				lightShadow *= shadowComponent;
				rtApplied = rtShadow;
			}
		}
		// (46b) Every other case: the traced visibility replaces Screen Space Point Light Shadows (below).
		if (rtShadow && !rtApplied)
			lightShadow *= rtVisibility;

		float3 normalizedLightDirection = normalize(lightDirection);
		float lightAngle = dot(worldNormal.xyz, normalizedLightDirection.xyz);

#			if defined(SSPLS)
		uint ssplsSteps = round(SharedData::ssplsSettings.StepLimit * (1.0 - saturate(viewPosition.z / SharedData::ssplsSettings.MaxDistance)));
		// (46y) Screen-Space Shadows for Non-RT Casters (the light's tag): beside RT, only occluders the ray tracer does not see.
#				if defined(RT_POINT_SHADOWS)
		const bool ssplsNonRT = rtShadow && (light.rtShadow & RTPointShadows::kSSPLSNonRT) != 0;
		const bool ssplsBesideRT = ssplsNonRT || (light.rtShadow & RTPointShadows::kKeepSSPLS) != 0;
#				else
		const bool ssplsNonRT = false;
		const bool ssplsBesideRT = false;
#				endif
		const bool ssplsRun = SharedData::ssplsSettings.Enable && inWorld &&
		                      !(light.lightFlags & LightLimitFix::LightFlags::Simple) &&
		                      shadowComponent != 0.0 &&
		                      lightAngle > 0.0 &&
		                      (!rtShadow || ssplsBesideRT);
		[branch] if (ssplsRun)
		{
			float3 lightDirectionVS = FrameBuffer::WorldToView(light.positionWS[eyeIndex].xyz, true, eyeIndex) - viewPosition.xyz;
			// (46y) Contact length for traced occluders: the tag's eighths x the trace's start offset (RayTracing.cpp kRayBiasConstant
			// + kRayBiasPerUnit x the distance to the camera). -1: every occluder counts.
			const float ssplsRtContact = ssplsNonRT ? float((light.rtShadow >> 16) & 0xFFu) * 0.125 * (2.0 + 0.0015 * length(viewPosition.xyz)) : -1.0;
			float SSPLSShadow = lerp(1.0, ScreenSpacePointLightShadows::GetShadow(LinearSampler, viewPosition, screenNoise, lightDirectionVS, ssplsSteps, light.radius, eyeIndex, light.lightFlags & LightLimitFix::LightFlags::Shadow, ssplsRtContact), SharedData::ssplsSettings.Strength);
			// (46b) Kept beside a ray-traced shadow: the darker of both (a shadow both see is not doubled).
			lightShadow = rtShadow ? min(lightShadow, SSPLSShadow) : lightShadow * SSPLSShadow;
		}
		[branch] if (ssplsCoverageView)
		{
			const float coverageWeight = Color::RGBToLuminance(max(lightColor, 0.0)) * saturate(lightAngle);
			const bool shadowMap = (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) && (light.lightFlags & LightLimitFix::LightFlags::Shadow) && !rtApplied;
			if (rtShadow)
				ssplsCoverage += ssplsRun ? float4(0.0, coverageWeight, 0.0, 0.0) : float4(coverageWeight, 0.0, 0.0, 0.0);
			else
				ssplsCoverage += (ssplsRun || shadowMap) ? float4(0.0, 0.0, coverageWeight, 0.0) : float4(0.0, 0.0, 0.0, coverageWeight);
		}
#			endif

		float3 refractedLightDirection = normalizedLightDirection;
#			if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			if (dot(normalizedLightDirection, coatWorldNormal) > 0)
				refractedLightDirection = -refract(-normalizedLightDirection, coatWorldNormal, eta);
		}
#			endif

		float parallaxShadow = 1;

#			if defined(EMAT)
		[branch] if (
			SharedData::extendedMaterialSettings.EnableShadows &&
			!(light.lightFlags & LightLimitFix::LightFlags::Simple) &&
			lightAngle > 0.0 &&
			shadowComponent != 0.0)
		{
			float3 lightDirectionTS = normalize(mul(refractedLightDirection, tbn).xyz);
#				if defined(PARALLAX)
			[branch] if (SharedData::extendedMaterialSettings.EnableParallax)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams);
#				elif defined(LANDSCAPE)
#					if defined(TRUE_PBR)
					if (SharedData::extendedMaterialSettings.EnableParallax)
#					else
					if (SharedData::extendedMaterialSettings.EnableTerrainParallax || (SharedData::extendedMaterialSettings.EnableParallax && Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement))
#					endif
#					if defined(TERRAIN_VARIATION)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, lightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams, sharedOffset, dx, dy);
#					else
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, lightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams);
#					endif
#				elif defined(EMAT_ENVMAP)
			[branch] if (complexMaterialParallax)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexEnvMaskSampler, SampEnvMaskSampler, 3, parallaxShadowQuality, screenNoise, displacementParams);
#				elif defined(TRUE_PBR) && !defined(LODLANDSCAPE)
			[branch] if (PBRParallax)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams);
#				endif
		}
#			endif

#			if defined(DYNAMIC_SNOW_SURFACE)
		// (batch 39c) As for the sun: snow fills the relief.
		if (SharedData::dynamicSnowSettings.Flags & DynamicSnow::FlagCoverageSlope)
			parallaxShadow = lerp(parallaxShadow, 1.0, dynamicSnowCoverage * DynamicSnow::SnowSmoothing());
#			endif

#			if defined(TRUE_PBR)
		{
			PBR::LightProperties lightProperties = PBR::InitLightProperties(lightColor, lightShadow, parallaxShadow);
			float3 pointDiffuseColor, coatPointDiffuseColor, pointTransmissionColor, pointSpecularColor;
			PBR::GetDirectLightInput(pointDiffuseColor, coatPointDiffuseColor, pointTransmissionColor, pointSpecularColor, worldNormal.xyz, coatWorldNormal, refractedViewDirection, viewDirection, refractedLightDirection, normalizedLightDirection, lightProperties, pbrSurfaceProperties, tbnTr, uvOriginal);
			lightsDiffuseColor += pointDiffuseColor;
			coatLightsDiffuseColor += coatPointDiffuseColor;
			transmissionColor += pointTransmissionColor;
			specularColorPBR += pointSpecularColor;
#				if defined(WETNESS_EFFECTS)
			if (wetFilmGeneric)
				wetFilmSpecular += GetWetFilmSpecular(wetFilmNormal, viewDirection, normalizedLightDirection, lightProperties.CoatLightColor, wetFilmRoughness);
			else if (waterRoughnessSpecular < 1.0 && !wetFilm)
				specularColorPBR += PBR::GetWetnessDirectLightSpecularInput(wetnessNormal, viewDirection, normalizedLightDirection, lightProperties.CoatLightColor, waterRoughnessSpecular) * wetnessGlossinessSpecular;
#				endif
		}
#			elif defined(SKIN) && defined(CS_SKIN)
		if (skinEnabled) {
			PBR::LightProperties lightProperties = PBR::InitLightProperties(lightColor, lightShadow, parallaxShadow);
			float3 pointDiffuseColor, pointTransmissionColor, pointSpecularColor;
			Skin::SkinDirectLightInput(pointDiffuseColor, pointTransmissionColor, pointSpecularColor, lightProperties, skinSurfaceProperties, originalWorldNormal, viewDirection, normalizedLightDirection, wetWorldNormal);
			lightsDiffuseColor += pointDiffuseColor;
			transmissionColor += pointTransmissionColor;
			float3 sssTransmittance = Skin::SSSSTransmittance(
										  SharedData::skinData.sssParams.x,
										  SharedData::skinData.sssParams.y,
										  originalWorldNormal,
										  normalizedLightDirection,
										  skinSurfaceProperties.Thickness) *
			                          SharedData::skinData.sssParams.w;
			transmissionColor += min(sssTransmittance * lightProperties.LightColor * skinSurfaceProperties.Albedo, lightProperties.LightColor);
			specularColorPBR += pointSpecularColor;
#				if defined(WETNESS_EFFECTS)
			if (waterRoughnessSpecular < 1.0 && !wetFilmInSkin && skinRainStrength <= 0.0)
				specularColorPBR += PBR::GetWetnessDirectLightSpecularInput(wetnessNormal, viewDirection, normalizedLightDirection, lightProperties.CoatLightColor, max(waterRoughnessSpecular, 0.2)) * wetnessGlossinessSpecular;
#				endif
		} else {
			lightColor *= lightShadow;

			float3 lightDiffuseColor = lightColor * parallaxShadow * saturate(lightAngle.xxx) * Color::VanillaDiffuseMult();

#				if defined(SPECULAR) || (defined(SPARKLE) && !defined(SNOW))
			lightsSpecularColor += GetLightSpecularInput(input, normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#				endif

			lightsDiffuseColor += lightDiffuseColor;
		}
#			else
		lightColor *= lightShadow;

		float3 lightDiffuseColor = lightColor * parallaxShadow * saturate(lightAngle.xxx) * Color::VanillaDiffuseMult();
		float lightBacklighting = 1.0 + saturate(dot(normalizedLightDirection.xyz, viewDirection));

#				if defined(SOFT_LIGHTING)
		lightDiffuseColor += lightBacklighting * lightColor * GetSoftLightMultiplier(lightAngle) * rimSoftLightColor.xyz * Color::VanillaDiffuseMult() * EYE_SOFT_LIGHTING_SCALE * EYE_BACK_LIGHT_GATE(normalizedLightDirection);
#				endif

#				if defined(RIM_LIGHTING)
		lightDiffuseColor += lightBacklighting * lightColor * GetRimLightMultiplier(normalizedLightDirection, viewDirection, worldNormal.xyz) * rimSoftLightColor.xyz * Color::VanillaDiffuseMult() * EYE_BACK_LIGHT_GATE(normalizedLightDirection);
#				endif

#				if defined(BACK_LIGHTING)
		lightDiffuseColor += lightBacklighting * lightColor * saturate(-lightAngle) * backLightColor.xyz * Color::VanillaDiffuseMult();
#				endif

#				if defined(HAIR) && defined(CS_HAIR) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
		if (SharedData::hairSpecularSettings.Enabled) {
			float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, normalizedLightDirection, screenNoise, eyeIndex);
			float3 lightSpecularColor = 0;
			float3 lightTransmissionColor = 0;
			Hair::GetHairDirectLight(lightDiffuseColor, lightSpecularColor, lightTransmissionColor, hairT, normalizedLightDirection, viewDirection, worldNormal.xyz, vertexNormal.xyz, lightColor, SharedData::hairSpecularSettings.HairGlossiness, hairShadow, uv, baseColor.xyz);
			lightsSpecularColor += lightSpecularColor;
			transmissionColor += lightTransmissionColor;
		} else {
#					if defined(SPECULAR)
			lightsSpecularColor += GetLightSpecularInput(input, normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#					endif
		}
#				elif defined(SPECULAR) || (defined(SPARKLE) && !defined(SNOW))
		lightsSpecularColor += GetLightSpecularInput(input, normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, F0, directSpecularRoughness, shininess, uv, eyeUseVanillaDirectLobe) * Color::VanillaSpecularMult();
#				endif

		lightsDiffuseColor += lightDiffuseColor;
#			endif

#			if defined(WETNESS_EFFECTS)
#				if !defined(TRUE_PBR)
		if (wetFilmGeneric)
			wetFilmSpecular += GetWetFilmSpecular(wetFilmNormal, viewDirection, normalizedLightDirection, lightColor, wetFilmRoughness);
		else
#				endif
			if (waterRoughnessSpecular < 1.0 && !wetFilm)
				wetnessSpecular += WetnessEffects::GetWetnessSpecular(wetnessNormal, normalizedLightDirection, viewDirection, lightColor, waterRoughnessSpecular);
#			endif
	}
#		endif
#	endif

#	if defined(WETNESS_EFFECTS)
	// (42c, 4.1) Energy conservation for the film: what the water reflects never reaches the
	// surface below. One view-angle Fresnel for all direct light (F0 = 0.02, so ~2% facing the
	// camera, more at the silhouette). Advanced Skin does this per light inside its BRDF.
	[branch] if (wetFilmGeneric)
	{
		const float wetFilmBaseScale = 1.0 - saturate(wetFilmEnergy) * BRDF::F_Schlick(0.02.xxx, saturate(abs(dot(wetFilmNormal, viewDirection)) + 1e-5)).x;
		lightsDiffuseColor *= wetFilmBaseScale;
		lightsSpecularColor *= wetFilmBaseScale;
#		if defined(TRUE_PBR)
		coatLightsDiffuseColor *= wetFilmBaseScale;
		specularColorPBR *= wetFilmBaseScale;
		specularColorPBR += wetFilmSpecular * wetFilmEnergy;
#		endif
	}
#	endif

#	if defined(HAIR) && defined(WETNESS_EFFECTS)
	// (44c) Wet hair with Hair Specular off: the vanilla highlight a little stronger (shininess x2.5 above).
#		if defined(CS_HAIR)
	if (!SharedData::hairSpecularSettings.Enabled)
#		endif
		lightsSpecularColor *= 1.0 + 0.3 * hairWetK;
#	endif
	diffuseColor += lightsDiffuseColor;
	specularColor += lightsSpecularColor;

#	if !defined(LANDSCAPE)
	if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::CharacterLight) {
		float charLightMul = saturate(dot(viewDirection, worldNormal.xyz)) * CharacterLightParams.x + CharacterLightParams.y * saturate(dot(float2(0.164398998, -0.986393988), worldNormal.yz));
		float charLightColor = min(CharacterLightParams.w, max(0, CharacterLightParams.z * TexCharacterLightProjNoiseSampler.Sample(SampCharacterLightProjNoiseSampler, baseShadowUV).x));
		diffuseColor += (charLightMul * charLightColor).xxx;
	}
#	endif

#	if defined(EYE)
	worldNormal.xyz = input.EyeNormal;
#	endif  // EYE

	float3 emitColor = Color::EmitColor(EmitColor);
#	if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	bool hasEmissive = (0x3F & (Permutation::PixelShaderDescriptor >> 24)) == Permutation::LightingTechnique::Glowmap;
#		if defined(TRUE_PBR)
	hasEmissive = hasEmissive || (PBRFlags & PBR::Flags::HasEmissive != 0);
#		endif
	[branch] if (hasEmissive)
	{
		float3 glowColor = Color::Glowmap(TexGlowSampler.SampleBias(SampGlowSampler, uv, SharedData::MaterialMipBias()).xyz);
		emitColor *= glowColor;
	}
#	endif

#	if !defined(TRUE_PBR)
	diffuseColor += emitColor.xyz;
#	endif

	float3 ambientNormal = worldNormal;
#	if defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled && SharedData::hairSpecularSettings.HairMode == 1)
		ambientNormal = normalize(viewDirection - hairT * dot(viewDirection, hairT));
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(ambientNormal, false, eyeIndex));
#	endif

	float3 directionalAmbientColor = Color::Ambient(max(0, mul(DirectionalAmbient, float4(ambientNormal, 1.0))));

#	if defined(IBL)
	if (SharedData::iblSettings.EnableDiffuseIBL) {
		if (SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection) {
			directionalAmbientColor = ImageBasedLighting::GetStaticDiffuseIBL(ambientNormal, SampColorSampler);
		} else if (!SharedData::InInterior || SharedData::iblSettings.EnableInterior || SharedData::iblSettings.EnableInterior) {
			directionalAmbientColor *= SharedData::iblSettings.DALCAmount;
		}
	}
#	endif

#	if defined(SKYLIGHTING)
	float skylightingDiffuse = 1;
	float skylightingFadeOutFactor = 1.0;
	if (!SharedData::InInterior) {
		skylightingFadeOutFactor = Skylighting::getFadeOutFactor(input.WorldPosition.xyz);
		skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(ambientNormal)) / Math::PI;
		skylightingDiffuse = saturate(skylightingDiffuse);
		skylightingDiffuse = lerp(1.0, skylightingDiffuse, skylightingFadeOutFactor);
		skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);
	}
#	endif

#	if defined(IBL)
	float3 iblColor = 0;
	if (SharedData::iblSettings.EnableDiffuseIBL) {
		if ((!SharedData::InInterior || SharedData::iblSettings.EnableInterior) && !(SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection))
		{
#		if defined(SKYLIGHTING)
			iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-ambientNormal, skylightingDiffuse), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#		else
			iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-ambientNormal), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
#		endif
			iblColor = Color::IrradianceToGamma(iblColor);
			directionalAmbientColor += iblColor;
		}
	}
#	endif

#	if defined(SSRT) && defined(DEFERRED)
	if (SharedData::ssrtSettings.DiffuseMult > 0.0) {
		directionalAmbientColor *= SharedData::ssrtSettings.AmbientMult;
		iblColor *= SharedData::ssrtSettings.AmbientMult;
	}
#	endif

	float3 reflectionDiffuseColor = diffuseColor + directionalAmbientColor;

#	if defined(TRUE_PBR) && defined(LOD_LAND_BLEND) && !defined(DEFERRED)
	lodLandDiffuseColor += directionalAmbientColor;
#	endif

#	if !defined(TRUE_PBR) && !(defined(SKIN) && defined(CS_SKIN))
#		if defined(HAIR) && defined(CS_HAIR)
	if (!SharedData::hairSpecularSettings.Enabled)
		diffuseColor += directionalAmbientColor;
#		else
	diffuseColor += directionalAmbientColor;
#		endif
#	elif defined(SKIN) && defined(CS_SKIN)
	if (!skinEnabled) {
#		if defined(DEFERRED) && defined(SSGI)
#		else
		diffuseColor += directionalAmbientColor;
#		endif
	}
#	endif

	float3 reflectance = 0;
#	if defined(DYNAMIC_CUBEMAPS)
	if (any(F0 > 0.0)) {
#		if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
#			if defined(VANILLA_FRESNEL)
		if (dynamicCubemap || enableVanillaFresnel)
#			else
		if (dynamicCubemap)
#			endif
#		endif
#		if defined(SKYLIGHTING)
			reflectance = DynamicCubemaps::GetDynamicCubemap(worldNormal, vertexNormal, viewDirection, roughness, F0, skylightingSH);
#		else
			reflectance = DynamicCubemaps::GetDynamicCubemap(worldNormal, vertexNormal, viewDirection, roughness, F0);
#		endif

#		if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#			if defined(VANILLA_FRESNEL)
		// (batch 10) Deliberate divergence from upstream, which skips this multiply for every
		// material once VanillaFresnel is on. VanillaFresnel exists to invent reflections for
		// vanilla materials that never had any; those have no author env mask (EnvmapData.y == 0,
		// see :563 and :2426) and `envMask` is only `glossiness` there, so skipping is right.
		// A material that ships its own env mask already had reflections in vanilla and the
		// author said where and how strong they are, so honour the mask instead of overriding it.
		// This is what unflattens eyes whose cubemap is near-black: the conversion at :2471
		// forces `dynamicCubemap`, so :2544 never samples that near-black cubemap, and without
		// this multiply the substituted real-time reflection also escaped the author's mask.
		//
		// (batch 10b) `EnvMaskStrength` fades that mask back out, because honouring it in full
		// is expensive elsewhere: across all 270 shipping `_m.dds` masks the median per-texture
		// mean is 0.14, the median texel is 0.06, 47.7% of texels are near black and only 0.8%
		// reach 0.8, so full strength cuts metal armour reflections to roughly a tenth. Eyes are
		// the exception that made batch 10 work at all -- the vanilla eye mask is 73% near black
		// but 12.4% of its texels are >= 0.8 (p90 = 1.0), a bright iris ring on a black field.
		// Linear, not thresholded: a "only when the mask is dark" rule would band on gradients.
		//
		// The strength only applies while VanillaFresnel is actually on. With the feature
		// compiled in but switched off at runtime this line is still reachable (the guard above
		// admits `dynamicCubemap` on its own), and a disabled feature must not move the mask, so
		// that case keeps the plain multiply. Which pixels get any multiply at all is unchanged
		// from batch 10: `!enableVanillaFresnel || EnvmapData.y`, just spelled as two branches.
		if (!enableVanillaFresnel)
			reflectance *= envMask;
		else if (EnvmapData.y)
			reflectance *= lerp(1.0, envMask, SharedData::vanillaFresnelSettings.EnvMaskStrength);
#			else
		reflectance *= envMask;
#			endif
#		endif

#		if defined(VANILLA_FRESNEL)
		// (44h) Eye night-glow controls, eye-only. EyeGrazingLimit re-evaluates the env BRDF
		// GetDynamicCubemap applied with N.V floored at the limit (ratio new/old, so horizon and
		// the env mask above are kept and the forward path is covered too); EyeReflectionStrength
		// then scales the whole reflection. Limit 0 and strength 1 reproduce the old result.
		if (enableVanillaFresnel && applyEyeHandling) {
			float eyeGrazingLimit = SharedData::vanillaFresnelSettings.EyeGrazingLimit;
			float eyeNoV = saturate(dot(worldNormal, viewDirection));
			if (eyeGrazingLimit > 0.0 && eyeNoV < eyeGrazingLimit) {
				float2 eyeBRDFOld = BRDF::EnvBRDF(roughness, eyeNoV);
				float2 eyeBRDFNew = BRDF::EnvBRDF(roughness, eyeGrazingLimit);
				reflectance *= (F0 * eyeBRDFNew.x + eyeBRDFNew.y) / max(F0 * eyeBRDFOld.x + eyeBRDFOld.y, 1e-4);
			}
			reflectance *= SharedData::vanillaFresnelSettings.EyeReflectionStrength;
		}
#		endif
	}
#	endif

#	if defined(SPECULAR)
	reflectance *= MaterialData.yyy;
#	endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);

#	if defined(WETNESS_EFFECTS)
#		if !(defined(FACEGEN) || defined(FACEGEN_RGB_TINT) || defined(EYE)) || defined(TREE_ANIM)
#			if defined(WET_CHARACTER_PERM)
	// (42c) Characters with "Wet Darkening" on were darkened before lighting (4.2).
	[branch] if (!wetDarkenNew)
#			endif
	{
#			if defined(TRUE_PBR)
#				if !defined(LANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::TwoLayer) != 0)
	{
		porosity = 0;
	}
	else
#				endif
	{
		porosity = lerp(porosity, 0.0, saturate(sqrt(pbrSurfaceProperties.Metallic)));
	}
#			elif defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX)
	porosity = lerp(porosity, 0.0, saturate(sqrt(envMask)));
#			endif
	float wetnessDarkeningAmount = porosity * wetnessGlossinessAlbedo;
	baseColor.xyz = lerp(baseColor.xyz, pow(abs(baseColor.xyz), 1.0 + wetnessDarkeningAmount), 0.5);
	}
#		endif

#		if defined(DYNAMIC_CUBEMAPS)
#			if defined(SKYLIGHTING)
	float3 wetnessReflectance = DynamicCubemaps::GetDynamicCubemap(wetnessNormal, vertexNormal, viewDirection, wetIndirectRoughness, 0.02 * wetnessGlossinessSpecular, skylightingSH);
#			else
	float3 wetnessReflectance = DynamicCubemaps::GetDynamicCubemap(wetnessNormal, vertexNormal, viewDirection, wetIndirectRoughness, 0.02 * wetnessGlossinessSpecular);
#			endif
#		else
	float3 wetnessReflectance = 0.0;
#		endif

#		if !defined(DEFERRED)
	wetnessSpecular += wetnessReflectance;
#		endif
#	endif

#	if defined(HAIR)
	float3 vertexColor = lerp(1, Color::ColorToLinear(TintColor.xyz), Color::ColorToLinear(input.Color.y));
#		if defined(CS_HAIR)
	float3 indirectDiffuseLobeWeight, indirectSpecularLobeWeightPrim, indirectSpecularLobeWeightSec;
	if (SharedData::hairSpecularSettings.Enabled)
		vertexColor = 1;
	Hair::GetHairIndirectSpecularLobeWeights(indirectDiffuseLobeWeight, indirectSpecularLobeWeightPrim, indirectSpecularLobeWeightSec, hairT, worldNormal.xyz, viewDirection, vertexNormal, SharedData::hairSpecularSettings.HairGlossiness, uv, baseColor.xyz);
	indirectDiffuseLobeWeight *= SharedData::hairSpecularSettings.DiffuseIndirectMult;
	indirectSpecularLobeWeightPrim *= SharedData::hairSpecularSettings.SpecularIndirectMult;
	indirectSpecularLobeWeightSec *= SharedData::hairSpecularSettings.SpecularIndirectMult;
#			if defined(WETNESS_EFFECTS)
	{
		// (44c) Wet clumps: the gaps between them are darker for ambient light too, at half strength.
		const float hairAmbientGap = 1.0 - 0.5 * (1.0 - Hair::WetGap);
		indirectDiffuseLobeWeight *= hairAmbientGap;
		indirectSpecularLobeWeightPrim *= hairAmbientGap;
		indirectSpecularLobeWeightSec *= hairAmbientGap;
	}
#			endif
#		endif  // CS_HAIR
#	elif defined(SKYLIGHTING)
	float3 vertexColor = input.Color.xyz;
	float vertexAO = max(max(vertexColor.r, vertexColor.g), vertexColor.b);

	if (!SharedData::InInterior) {
#		if defined(LANDSCAPE)
		// Remove AO
		vertexColor = vertexColor / vertexAO;
#			if defined(LOD_BLENDING)
		vertexColor = lerp(vertexColor, 1, SharedData::lodBlendingSettings.DisableTerrainVertexColors);
#			endif  // LOD_BLENDING
#		else

		if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsTree) {
			// Remove AO
			float3 originalVertexColor = vertexColor;
			vertexColor = lerp(vertexColor, vertexColor / vertexAO, sqrt(vertexAO));
			vertexColor = lerp(input.Color.xyz, vertexColor, skylightingFadeOutFactor);

			// Apply AO to direct lighting only
			float3 originalDiffuseColor = diffuseColor;
			diffuseColor -= lightsDiffuseColor;
			diffuseColor += lerp(lightsDiffuseColor, lightsDiffuseColor * vertexAO, skylightingFadeOutFactor);

			vertexColor = lerp(vertexColor, originalVertexColor, SharedData::skylightingSettings.MinDiffuseVisibility);
			diffuseColor = lerp(diffuseColor, originalDiffuseColor, SharedData::skylightingSettings.MinDiffuseVisibility);
		}

		// Brighten skylighting on vertex AO
		vertexColor *= 1.0 + (1.0 - vertexAO) * (1.0 - skylightingDiffuse);
#		endif
	}
#	else
	float3 vertexColor = input.Color.xyz;
#		if defined(LANDSCAPE) && defined(LOD_BLENDING)
		vertexColor = lerp(vertexColor, 1, SharedData::lodBlendingSettings.DisableTerrainVertexColors);
#		endif  // LOD_BLENDING
#	endif  // defined (HAIR)

	float4 color = 0;

#	if defined(TRUE_PBR)
	{
		float3 directLightsDiffuseInput = diffuseColor * baseColor.xyz;
		[branch] if ((PBRFlags & PBR::Flags::ColoredCoat) != 0)
		{
			directLightsDiffuseInput = lerp(directLightsDiffuseInput, pbrSurfaceProperties.CoatColor * coatLightsDiffuseColor, pbrSurfaceProperties.CoatStrength);
		}

		color.xyz += directLightsDiffuseInput;
	}

	float3 indirectDiffuseLobeWeight, indirectSpecularLobeWeight;
	PBR::GetIndirectLobeWeights(indirectDiffuseLobeWeight, indirectSpecularLobeWeight, worldNormal.xyz, viewDirection, vertexNormal, baseColor.xyz, pbrSurfaceProperties);
#		if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1.0)
		indirectSpecularLobeWeight = max(indirectSpecularLobeWeight, PBR::GetWetnessIndirectSpecularLobeWeight(wetnessNormal, viewDirection, vertexNormal, wetIndirectRoughness) * (gearWetNew ? wetnessGlossinessSpecular : 1.0));  // (44c) gear New: x its film energy (<= 1)
#		endif

	color.xyz += indirectDiffuseLobeWeight * directionalAmbientColor;

#		if !defined(DEFERRED)
#			if defined(DYNAMIC_CUBEMAPS)
#				if defined(SKYLIGHTING)
	specularColorPBR += indirectSpecularLobeWeight * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(screenUV, worldNormal, vertexNormal, viewDirection, pbrSurfaceProperties.Roughness, skylightingSH);
#				else
	specularColorPBR += indirectSpecularLobeWeight * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(screenUV, worldNormal, vertexNormal, viewDirection, pbrSurfaceProperties.Roughness);
#				endif
#			else
	specularColorPBR += indirectSpecularLobeWeight * directionalAmbientColor;
#			endif
#		else
	indirectDiffuseLobeWeight *= vertexColor;
#		endif

	// Fixes white items in UI for VR
	[branch] if ((PBRFlags & PBR::Flags::HasEmissive) != 0)
	{
		color.xyz += emitColor.xyz;
	}
	color.xyz += transmissionColor;
#	elif defined(SKIN) && defined(CS_SKIN)
	float3 indirectDiffuseLobeWeight, indirectSpecularLobeWeight;
	if (skinEnabled) {
		float3 directLightsDiffuseInput = diffuseColor * baseColor.xyz;
		color.xyz += directLightsDiffuseInput;

		Skin::SkinIndirectLobeWeights(indirectDiffuseLobeWeight, indirectSpecularLobeWeight, skinSurfaceProperties, originalWorldNormal, viewDirection, vertexNormal, wetWorldNormal);

#		if defined(WETNESS_EFFECTS)
		// (42c, 4.3) While rain feeds the Advanced Skin film, its indirect lobe (above) already
		// carries the rain; adding this one too would count the same water twice.
		if (waterRoughnessSpecular < 1.0 && skinRainStrength <= 0.0 && !skinWetNew)  // (44a) anti-mirror: >= 0.2 on skin
			indirectSpecularLobeWeight += PBR::GetWetnessIndirectSpecularLobeWeight(wetnessNormal, viewDirection, vertexNormal, max(waterRoughnessSpecular, 0.2)) * wetnessGlossinessSpecular;
#		endif

#		if !(defined(DEFERRED) && defined(SSGI))
		color.xyz += indirectDiffuseLobeWeight * directionalAmbientColor;
#		endif

#		if !defined(DEFERRED)
#			if defined(DYNAMIC_CUBEMAPS)
#				if defined(SKYLIGHTING)
		specularColorPBR += indirectSpecularLobeWeight * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(screenUV, worldNormal, vertexNormal, viewDirection, averageRoughness, skylightingSH);
#				else
		specularColorPBR += indirectSpecularLobeWeight * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(screenUV, worldNormal, vertexNormal, viewDirection, averageRoughness);
#				endif
#			else
		specularColorPBR += indirectSpecularLobeWeight * directionalAmbientColor;
#			endif
#		else
		indirectDiffuseLobeWeight *= vertexColor;
#		endif

		color.xyz += transmissionColor;
	} else {
		color.xyz += diffuseColor * baseColor.xyz;
	}
#	elif defined(HAIR) && defined(CS_HAIR)
	color.xyz += diffuseColor * baseColor.xyz;
	if (SharedData::hairSpecularSettings.Enabled) {
		color.xyz += indirectDiffuseLobeWeight * directionalAmbientColor;
		color.xyz += transmissionColor;
	}
#	else
	color.xyz += diffuseColor * baseColor.xyz;
#		if !defined(DEFERRED) && (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)) && defined(DYNAMIC_CUBEMAPS)
	if (dynamicCubemap)
		envColor += reflectance;
#		endif
#	endif

#	if defined(HAIR) && defined(CS_HAIR)
#		if !defined(DEFERRED)
#			if defined(DYNAMIC_CUBEMAPS)
	if (SharedData::hairSpecularSettings.Enabled && SharedData::hairSpecularSettings.HairMode != 1)
#				if defined(SKYLIGHTING)
	{
		float3 indirectSpecular = Hair::GetHairDynamicCubemapSpecularIrradiance(uv, screenUV, hairT, worldNormal, vertexNormal, viewDirection, SharedData::hairSpecularSettings.HairGlossiness, indirectSpecularLobeWeightPrim, indirectSpecularLobeWeightSec, skylightingSH);
		color.xyz += indirectSpecular;
	}
#				else
	{
		float3 indirectSpecular = Hair::GetHairDynamicCubemapSpecularIrradiance(uv, screenUV, hairT, worldNormal, vertexNormal, viewDirection, SharedData::hairSpecularSettings.HairGlossiness, indirectSpecularLobeWeightPrim, indirectSpecularLobeWeightSec);
		color.xyz += indirectSpecular;
	}
#				endif
#			endif
#		endif
#	endif

	color.xyz *= vertexColor;

#	if defined(MULTI_LAYER_PARALLAX)
	float layerValue = MultiLayerParallaxData.x * TexLayerSampler.SampleBias(SampLayerSampler, uv, SharedData::MaterialMipBias()).w;
	float3 tangentViewDirection = mul(viewDirection, tbn);
	float3 layerNormal = MultiLayerParallaxData.yyy * (normalColor.xyz * 2.0.xxx + float3(-1, -1, -2)) + float3(0, 0, 1);
	float layerViewAngle = dot(-tangentViewDirection.xyz, layerNormal.xyz) * 2;
	float3 layerViewProjection = -layerNormal.xyz * layerViewAngle.xxx - tangentViewDirection.xyz;
	float2 layerUv = uv * MultiLayerParallaxData.zw + (0.0009765625 * (layerValue / abs(layerViewProjection.z))).xx * layerViewProjection.xy;

	float3 layerColor = TexLayerSampler.SampleBias(SampLayerSampler, layerUv, SharedData::MaterialMipBias()).xyz;

	float mlpBlendFactor = saturate(viewNormalAngle) * (1.0 - baseColor.w);

	color.xyz = lerp(color.xyz, diffuseColor * vertexColor * layerColor, mlpBlendFactor);

#		if defined(DEFERRED)
	baseColor.xyz *= 1.0 - mlpBlendFactor;
#		endif
#	endif  // MULTI_LAYER_PARALLAX

#	if defined(SPECULAR)
# 		if defined(HAIR) && defined(CS_HAIR)
	if (!SharedData::hairSpecularSettings.Enabled)
#		elif defined(SKIN) && defined(CS_SKIN)
	if (!skinEnabled)
#		endif
		// (batch 15) This assignment is the *only* place the two per-pixel gates on the direct
		// specular live: `specularColor * glossiness` (glossiness is normal.w, the gloss map,
		// set at :1969/:1972) and the environment mask inside the ternary. Turning EnableGGX on
		// makes the whole statement skip itself, which is a deliberate model swap for materials
		// that feed their own gloss/specular numbers into GGX -- and a missing brake for eyes,
		// whose GGX F0 and roughness are hardcoded and have nothing else restraining them.
		// `specularColor *= MaterialData.yyy` below is a separate statement and was never part
		// of the skip.
		//
		// The two original conditions are unchanged; each gained one extra term that is false
		// for everything except an eye in the matching EyeDirectSpecularMode.
		// eyeForceVanillaGates runs the gates in VanillaPhong and GGXGated;
		// eyeSuppressConvertedF0 drops the converted-cubemap F0 mix in VanillaPhong so the gate
		// is plain SpecularColor.xyz, which is the shape bedec8379 had (conversion defaulted
		// off there).
#		if defined(VANILLA_FRESNEL)
	if (!(enableVanillaFresnel && SharedData::vanillaFresnelSettings.EnableGGX) || eyeForceVanillaGates)
#		endif
#		if defined(VANILLA_FRESNEL) && (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
		specularColor = (specularColor * glossiness) * ((enableVanillaFresnel && SharedData::vanillaFresnelSettings.EnableDynamicCubemapsConversion && !eyeSuppressConvertedF0) ? lerp(SpecularColor.xyz, F0, envMask) : SpecularColor.xyz);
#		else
		specularColor = (specularColor * glossiness) * SpecularColor.xyz;
#		endif
	specularColor *= MaterialData.yyy;
#	elif defined(SPARKLE)
	specularColor *= glossiness;
#	endif  // SPECULAR

#	if defined(SNOW)
	if (useSnowSpecular)
		specularColor = 0;
#	endif

	specularColor = Color::IrradianceToLinear(specularColor);

	diffuseColor = reflectionDiffuseColor;

#	if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#		if defined(DYNAMIC_CUBEMAPS)
	if (!dynamicCubemap)
#		endif
#		if defined(VANILLA_FRESNEL)
	if (!(enableVanillaFresnel && SharedData::vanillaFresnelSettings.EnableDynamicCubemapsConversion))
#		endif
		specularColor += envColor * Color::IrradianceToLinear(diffuseColor);
#	endif

#	if defined(EMAT_ENVMAP)
#		if defined(VANILLA_FRESNEL)
	if (!(enableVanillaFresnel && SharedData::vanillaFresnelSettings.EnableGGX))
#		endif
	specularColor *= complexSpecular;
#	endif  // defined (EMAT) && defined(ENVMAP)

#	if !defined(DEFERRED) && defined(DYNAMIC_CUBEMAPS) && (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
	if (dynamicCubemap)
		specularColor += envColor;
#	endif

#	if defined(WETNESS_EFFECTS) && !defined(TRUE_PBR) && !(defined(SKIN) && defined(CS_SKIN))
	specularColor += wetnessSpecular * wetnessGlossinessSpecular;
#	endif

#	if defined(LOD_LAND_BLEND) && defined(TRUE_PBR)
	{
		lodLandDiffuseColor += directionalAmbientColor;
		float3 litLodLandColor = vertexColor * lodLandColor.xyz * lodLandFadeFactor * lodLandDiffuseColor;
		color.xyz = lerp(color.xyz * Color::PBRLightingScale, litLodLandColor, lodLandBlendFactor);

		specularColor = lerp(specularColorPBR * Color::PBRLightingScale, 0, lodLandBlendFactor);
		indirectDiffuseLobeWeight = lerp(indirectDiffuseLobeWeight * Color::PBRLightingScale, vertexColor * lodLandColor.xyz * lodLandFadeFactor, lodLandBlendFactor);
		indirectSpecularLobeWeight = lerp(indirectSpecularLobeWeight, 0, lodLandBlendFactor);
		pbrGlossiness = lerp(pbrGlossiness, 0, lodLandBlendFactor);
	}
#	elif defined(TRUE_PBR)
	color.xyz *= Color::PBRLightingScale;
	specularColorPBR *= Color::PBRLightingScale;
	indirectDiffuseLobeWeight *= Color::PBRLightingScale;
	specularColor = specularColorPBR;
#	elif defined(SKIN) && defined(CS_SKIN)
	if (skinEnabled) {
		specularColor = specularColorPBR;
	}
#	endif

#	if defined(WETNESS_EFFECTS) && !defined(TRUE_PBR)
	// (42c, 4.1) The water film on top of everything else (True PBR added it to its own
	// specular before the PBR scale). Not gated by the material's gloss or specular maps.
	if (wetFilmGeneric)
		specularColor += wetFilmSpecular * wetFilmEnergy;
#	endif

	float3 outputAlbedo = baseColor.xyz * vertexColor;

#		if defined(TRUE_PBR)
	outputAlbedo = indirectDiffuseLobeWeight;
#		elif defined(VANILLA_FRESNEL)
	if (enableVanillaFresnel)
	{
		outputAlbedo *= 1 - reflectance;
	}
#		endif

#		if defined(SKIN) && defined(CS_SKIN)
	if (skinEnabled) {
		outputAlbedo = indirectDiffuseLobeWeight;
	}
#		endif

#		if defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		outputAlbedo = indirectDiffuseLobeWeight;
	}
#		endif

#	if defined(IBL) && defined(SKYLIGHTING)
	directionalAmbientColor -= iblColor;
#	endif

	directionalAmbientColor *= outputAlbedo;

#	if defined(SKYLIGHTING)
	Skylighting::applySkylighting(color.xyz, directionalAmbientColor, outputAlbedo, skylightingDiffuse);
#	endif

#	if defined(IBL) && defined(SKYLIGHTING)
	directionalAmbientColor += iblColor * outputAlbedo;
#	endif

#	if !defined(DEFERRED)
	color.xyz = Color::IrradianceToGamma(Color::IrradianceToLinear(color.xyz) + specularColor);
	float3 fogColor = Color::Fog(input.FogParam.xyz);
	float fogFactor = Color::FogAlpha(input.FogParam.w);
	float weatherFogAmount = 0.0;  // (47b) share of the view the game's fog covers under the height fog
#		if defined(IBL)
	if (SharedData::iblSettings.EnableDiffuseIBL && !SharedData::InInterior) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#		endif
#		if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		// (batch 38, A1) the froxel volumes belong to the main camera: not in reflections
		float4 exponentialHeightFog;
		[branch] if (inWorld && !inReflection)
			exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor, eyeIndex, input.Position.xy);
		else
			exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor);
		// (47b) the game's own fog (weather / room) under the height fog; no-op while off
		exponentialHeightFog = ExponentialHeightFog::ComposeVanillaFog(exponentialHeightFog, fogColor, fogFactor, 1.0, weatherFogAmount);
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
	}
#		endif
	if (FrameBuffer::FrameParams.y && FrameBuffer::FrameParams.z)
		color.xyz = lerp(color.xyz, fogColor, fogFactor);
	else
		weatherFogAmount = 0.0;

#		if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		float4 apSample = PhysSky::SampleAp(normalize(input.WorldPosition.xyz), input.Position.xy, length(input.WorldPosition.xyz), SampColorSampler);
#			if defined(EXP_HEIGHT_FOG)
		// (47b) the game's fog sits in front of the haze here (forward objects are fogged first): what it covers loses its haze
		[branch] if (weatherFogAmount > 0.0)
			apSample = ExponentialHeightFog::ScaleAerialPerspective(apSample, weatherFogAmount);
#			endif
		color.xyz = color.xyz * apSample.w + apSample.xyz;
	}
#		endif
#	endif

#	if defined(TESTCUBEMAP) && defined(ENVMAP) && defined(DYNAMIC_CUBEMAPS)
	baseColor.xyz = 0.0;
	specularColor = 0.0;
	diffuseColor = 0.0;
	reflectance = 1.0;
	roughness = 0.0;
	color.xyz = 0;
#	endif

#	if defined(LANDSCAPE) && !defined(LOD_LAND_BLEND)
	psout.Diffuse.w = 0;
#	else
	float alpha = baseColor.w;
#		if defined(EMAT) && !defined(LANDSCAPE)
#			if defined(PARALLAX)
	alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
#			elif defined(TRUE_PBR)
	[branch] if (PBRParallax)
	{
		alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
	}
#			endif
#		endif
#		if defined(DO_ALPHA_TEST)
	[branch] if ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::AdditionalAlphaMask) != 0)
	{
		uint2 alphaMask = input.Position.xy;
		alphaMask.x = ((alphaMask.x << 2) & 12);
		alphaMask.x = (alphaMask.y & 3) | (alphaMask.x & ~3);
		const float maskValues[16] = {
			0.003922,
			0.533333,
			0.133333,
			0.666667,
			0.800000,
			0.266667,
			0.933333,
			0.400000,
			0.200000,
			0.733333,
			0.066667,
			0.600000,
			0.996078,
			0.466667,
			0.866667,
			0.333333,
		};

		// (batch 39, item 3) Per-frame noise instead of the fixed pattern when Temporal LOD dither
		// is on; same pixel and frame as the depth prepass (Utility.hlsl), so the same pixels stay.
		// The reflection cubemap is not accumulated over frames: it keeps a still pattern.
		const uint stippleFrame = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) ? 0 : SharedData::FrameCount;
		const float stippleThreshold = SharedData::LODStippleThreshold(uint2(input.Position.xy), maskValues[alphaMask.x], stippleFrame);
		if (MaterialData.z - stippleThreshold < 0) {
			discard;
		}
	}
	else
#		endif  // defined(DO_ALPHA_TEST)
	{
		alpha *= MaterialData.z;
	}
#		if !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
	alpha *= input.Color.w;
#		endif  // !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
#		if defined(DO_ALPHA_TEST)
#			if defined(DEPTH_WRITE_DECALS)
	if (alpha - 0.0156862754 < 0) {
		discard;
	}
	alpha = saturate(1.05 * alpha);
#			endif  // DEPTH_WRITE_DECALS
	if (alpha - AlphaTestRefRS < 0) {
		discard;
	}
#		endif      // DO_ALPHA_TEST

#		if defined(ANISOTROPIC_ALPHA)
	// Uniform alpha material settings
	uint AlphaMaterialModel = ExtendedTranslucency::GetMaterialModelFromDescriptor(Permutation::ExtraFeatureDescriptor);
	float AlphaMaterialReduction = 0.f;
	float AlphaMaterialSoftness = 0.f;
	float AlphaMaterialStrength = 0.f;
	[branch] if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::Default) {
		AlphaMaterialModel = SharedData::extendedTranslucencySettings.MaterialModel;
		AlphaMaterialReduction = SharedData::extendedTranslucencySettings.Reduction;
		AlphaMaterialSoftness = SharedData::extendedTranslucencySettings.Softness;
		AlphaMaterialStrength = SharedData::extendedTranslucencySettings.Strength;
	}

	[branch] if (ExtendedTranslucency::IsValidMaterial(AlphaMaterialModel))
	{
		if (alpha >= 0.0156862754 && alpha < 1.0) {
			float originalAlpha = alpha;
			alpha = alpha * (1.0 - AlphaMaterialReduction);
			[branch] if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::AnisotropicFabric)
			{
#			if defined(SKINNED) || !defined(MODELSPACENORMALS)
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric2D(alpha, viewDirection, tbnTr);
#			else
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric1D(alpha, viewDirection, worldNormal.xyz);
#			endif
			}
			else if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::IsotropicFabric)
			{
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric1D(alpha, viewDirection, worldNormal.xyz);
			}
			else
			{
				alpha = ExtendedTranslucency::GetViewDependentAlphaNaive(alpha, viewDirection, worldNormal.xyz);
			}
			alpha = saturate(ExtendedTranslucency::SoftClamp(alpha, 2.0f - AlphaMaterialSoftness));
			alpha = lerp(alpha, originalAlpha, AlphaMaterialStrength);
		}
	}
#		endif  // ANISOTROPIC_ALPHA

	psout.Diffuse.w = alpha;
#	endif

#	if defined(WETNESS_EFFECTS) && defined(CS_SKIN) && !defined(SKIN)
	// (44c) Character Wetness debug view: tint hair and gear by material class (red cloth, yellow
	// leather, blue metal, green fur / other, purple hair), shaded by the base colour.
	[branch] if (charWetNew && Skin::wetCharParams[3].z > 0.5)
	{
#		if defined(HAIR)
		const float3 classTint = float3(0.7, 0.3, 1.0);
#		else
		const float3 classTint = lerp(gearClass.x * float3(1.0, 0.15, 0.1) + gearClass.y * float3(1.0, 0.8, 0.1) + gearClass.z * float3(0.15, 0.4, 1.0), float3(0.2, 0.9, 0.2), gearClass.w);
#		endif
		color.xyz = classTint * (0.3 + 0.7 * saturate(Color::RGBToLuminance(baseColor.xyz) * 2.0));
		outputAlbedo = 0.0;
		specularColor = 0.0;
	}
#	endif

#	if defined(SSPLS) && defined(LIGHT_LIMIT_FIX) && !defined(LOD)
	// (46y) Debug view SSPLS Coverage (Ray Tracing > Diagnostics): green = ray traced only, cyan = ray traced + screen-space for the
	// non-RT casters, orange = not ray traced (screen-space and / or shadow map), red = no shadow, dark grey = no point light.
	[branch] if (ssplsCoverageView)
	{
		const float coverageTotal = dot(ssplsCoverage, 1.0.xxxx);
		const float3 coverageTint = coverageTotal > 1e-5 ?
		                                (ssplsCoverage.x * float3(0.1, 0.9, 0.2) + ssplsCoverage.y * float3(0.1, 0.8, 0.9) + ssplsCoverage.z * float3(1.0, 0.5, 0.05) +
		                                    ssplsCoverage.w * float3(0.9, 0.05, 0.05)) /
		                                    coverageTotal :
		                                float3(0.06, 0.06, 0.06);
		color.xyz = coverageTint * (0.3 + 0.7 * saturate(Color::RGBToLuminance(baseColor.xyz) * 2.0));
		outputAlbedo = 0.0;
		specularColor = 0.0;
	}
#	endif

#	if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap(LightLimitFix::NumStrictLights >= 7.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap((float)LightLimitFix::NumStrictLights / 15.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 2) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap((float)numClusteredLights / MAX_CLUSTER_LIGHTS);
		} else {
			psout.Diffuse.xyz = shadowColor.xyz;
		}
		baseColor.xyz = 0.0;
	} else {
		psout.Diffuse.xyz = color.xyz;
	}
#	else
	psout.Diffuse.xyz = color.xyz;
#	endif  // defined(LIGHT_LIMIT_FIX)

#	if defined(CS_SKIN) && !defined(DEFERRED)
	// (46a) See-through clothes of a character in water (Character Wetness > See-through clothes in
	// water): this draw is tested against the depth from before the water, so its part below the surface
	// is drawn over the water; tint and fade it as if seen through the water.
	[branch] if (Skin::waterPerGeometry.y > 0.5)
	{
		const float underWaterDepth = Skin::waterPerGeometry.x - (input.WorldPosition.z + FrameBuffer::CameraPosAdjust[eyeIndex].z);
		[branch] if (underWaterDepth > 0.0)
		{
			const float underWater = saturate(underWaterDepth / 48.0) * saturate(Skin::waterPerGeometry.z);
			psout.Diffuse.xyz *= lerp(float3(1.0, 1.0, 1.0), float3(0.45, 0.62, 0.66), underWater);
			psout.Diffuse.w *= 1.0 - 0.65 * underWater;
		}
	}
#	endif

	psout.MotionVectors.xy = screenMotionVector.xy;
	psout.MotionVectors.zw = float2(0, psout.Diffuse.w);

#	if defined(DEFERRED)

#		if defined(TERRAIN_BLENDING)
	psout.Diffuse.w = blendFactorTerrain;
#		endif

	psout.MotionVectors.zw = float2(0.0, psout.Diffuse.w);
	psout.Specular = float4(specularColor, psout.Diffuse.w);
	psout.Albedo = float4(outputAlbedo, psout.Diffuse.w);

#		if defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		roughness = (SharedData::hairSpecularSettings.HairMode == 1 ? 1.0 : pow(abs(2.0 / (glossiness * 0.5 + 2.0)), 0.25));
	}
#		endif

#		if defined(WETNESS_EFFECTS)
	screenSpaceNormal = normalize(FrameBuffer::WorldToView(wetnessNormal, false, eyeIndex));
#		endif

#		if defined(TRUE_PBR)
	psout.Reflectance = float4(indirectSpecularLobeWeight, psout.Diffuse.w);
#			if defined(WETNESS_EFFECTS)
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), CharacterWetGBufferGloss(pbrGlossiness, wetnessGlossinessSpecular, wetCharacter, charWetGlossTarget, charWetGlossCap), psout.Diffuse.w);
#			else
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), pbrGlossiness, psout.Diffuse.w);
#			endif
#		elif defined(SKIN) && defined(CS_SKIN)
	if (skinEnabled) {
		psout.Reflectance = float4(indirectSpecularLobeWeight, psout.Diffuse.w);
#			if defined(WETNESS_EFFECTS)
		psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), skinWetNew ? pbrGlossiness : min(lerp(pbrGlossiness, max(pbrGlossiness, wetnessGlossinessSpecular), wetnessGlossinessSpecular), max(pbrGlossiness, 0.8)), psout.Diffuse.w);  // (44a) anti-mirror; New: rain is in pbrGlossiness
#			else
		psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), pbrGlossiness, psout.Diffuse.w);
#			endif
	} else {
#			if defined(WETNESS_EFFECTS)
	psout.Reflectance = float4(max(reflectance, wetnessReflectance), psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), CapCharacterWetGloss(lerp(1.0 - roughness, max(1 - roughness, wetnessGlossinessSpecular), wetnessGlossinessSpecular), 1.0 - roughness, wetCharacter), psout.Diffuse.w);
#			else
	psout.Reflectance = float4(reflectance, psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), 1.0 - roughness, psout.Diffuse.w);
#			endif
	}
#		elif defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
#			if defined(WETNESS_EFFECTS)
		psout.Reflectance = float4(indirectSpecularLobeWeightPrim + indirectSpecularLobeWeightSec + (hairWetNew ? 0.0.xxx : wetnessReflectance), psout.Diffuse.w);
		psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), hairWetNew ? 1.0 - roughness : CapCharacterWetGloss(lerp(1.0 - roughness, max(1 - roughness, wetnessGlossinessSpecular), wetnessGlossinessSpecular), 1.0 - roughness, true), psout.Diffuse.w);
#			else
		psout.Reflectance = float4(indirectSpecularLobeWeightPrim + indirectSpecularLobeWeightSec, psout.Diffuse.w);
		psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), 1.0 - roughness, psout.Diffuse.w);
#			endif
	} else {
#			if defined(WETNESS_EFFECTS)
	psout.Reflectance = float4(hairWetNew ? reflectance : max(reflectance, wetnessReflectance), psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), hairWetNew ? 1.0 - roughness : CapCharacterWetGloss(lerp(1.0 - roughness, max(1 - roughness, wetnessGlossinessSpecular), wetnessGlossinessSpecular), 1.0 - roughness, true), psout.Diffuse.w);
#			else
	psout.Reflectance = float4(reflectance, psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), 1.0 - roughness, psout.Diffuse.w);
#			endif
	}
#		elif defined(WETNESS_EFFECTS)
	psout.Reflectance = float4(max(reflectance, wetnessReflectance), psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), CharacterWetGBufferGloss(1.0 - roughness, wetnessGlossinessSpecular, wetCharacter, charWetGlossTarget, charWetGlossCap), psout.Diffuse.w);
#		else
	psout.Reflectance = float4(reflectance, psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), 1.0 - roughness, psout.Diffuse.w);
#		endif

#		if defined(SNOW)
#			if defined(TRUE_PBR)
	psout.Parameters.x = Color::RGBToLuminanceAlternative(specularColor);
	psout.Parameters.y = 0;
#			else
	psout.Parameters.x = Color::RGBToLuminanceAlternative(lightsSpecularColor);
#			endif
	psout.Parameters.w = psout.Diffuse.w;
#		endif

#		if defined(SSS) && defined(SKIN)
	psout.Masks = float4(saturate(baseColor.a), !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsBeastRace), Color::RGBToYCoCg(directionalAmbientColor).x, psout.Diffuse.w);
#		else
	psout.Masks = float4(0, 0, Color::RGBToYCoCg(directionalAmbientColor).x, psout.Diffuse.w);
#		endif

	float stochasticBlend = (screenNoise * screenNoise) < psout.Diffuse.w ? 1.0 : 0.0;
	psout.NormalGlossiness.w = stochasticBlend;
#	endif

	if ((!inWorld && !inReflection) && SharedData::linearLightingSettings.enableLinearLighting && !(Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow)) {
		psout.Diffuse.xyz = Color::TrueLinearToGamma(psout.Diffuse.xyz);
	}

	return psout;
}
#endif  // PSHADER
