///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>         (\_/)
// Version:         (see XeGTAO.h)                                                                            (='.'=)
// Details:         https://github.com/GameTechDev/XeGTAO                                                     (")_(")
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// with additional edits by FiveLimbedCat/ProfJack
//
// More references:
//
// Screen Space Indirect Lighting with Visibility Bitmask
//  https://arxiv.org/abs/2301.11376
//
// Exploring Raytraced Future in Metro Exodus
//  https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Common/Color.hlsli"
#include "Common/FastMath.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/VR.hlsli"
#include "ScreenSpaceGI/common.hlsli"
#ifdef SSGI_REBLUR
#	include "NRD/NRDReblurSH.hlsli"
#endif

// (directional env v2) Cross-feature include, same pattern and same justification as
// ssrt_raymarch.hlsl: the include only exists when the C++ side confirmed the Skylighting
// feature is installed (the SKYLIGHTING define is derived from `loaded`), and at runtime all
// installed features share one merged Data/Shaders tree, so the path resolves whenever the
// define is set.
#if defined(DYNAMIC_CUBEMAPS) && defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

#define RCP_PI (0.31830988618)

Texture2D<float> srcWorkingDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);
Texture2D<float3> srcRadiance : register(t2);  // maybe half-res
Texture2D<unorm float2> srcNoise : register(t3);
Texture2D<unorm float> srcAccumFrames : register(t4);  // maybe half-res
Texture2D<float> srcPrevAo : register(t5);             // maybe half-res
Texture2D<float4> srcPrevY : register(t6);             // maybe half-res
Texture2D<float2> srcPrevCoCg : register(t7);          // maybe half-res
Texture2D<float4> srcPrevGISpecular : register(t8);    // maybe half-res
// (directional env) Reprojected bent-normal history, written by radianceDisocc.cs.hlsl the same
// way srcPrevAo is. Encoding: see SSGI_EncodeBentNormal in common.hlsli.
Texture2D<unorm float4> srcPrevBentNormal : register(t9);  // maybe half-res
#if defined(DYNAMIC_CUBEMAPS)
// (directional env v2) Reprojected environment-irradiance history, written by
// radianceDisocc.cs.hlsl the same way srcPrevIlY is. RGB = linear hemisphere irradiance
// premultiplied by the confidence in A (see the write in main()); it is RADIANCE data and is
// filtered exactly like the IL channels, never like the bent-normal vector.
Texture2D<float4> srcPrevEnvIrradiance : register(t10);  // maybe half-res
// The live dynamic-cubemap pair, bound only while the DynamicCubemaps feature is loaded (the
// define is derived from `loaded` on the C++ side). Same semantics as in the SSRT ray march:
// EnvTexture is the sky-free capture, EnvReflectionsTexture includes the sky.
TextureCube<float3> EnvTexture : register(t11);
TextureCube<float3> EnvReflectionsTexture : register(t12);
#	if defined(SKYLIGHTING)
Texture3D<sh2> SkylightingProbeArray : register(t13);
Texture2DArray<float3> stbn_vec3_2Dx1D_128x128x64 : register(t14);
#	endif
#endif

RWTexture2D<unorm float> outAo : register(u0);
RWTexture2D<float4> outY : register(u1);
RWTexture2D<float2> outCoCg : register(u2);
RWTexture2D<float4> outGISpecular : register(u3);
RWTexture2D<half3> outPrevGeo : register(u4);
RWTexture2D<unorm float4> outBentNormal : register(u5);
#if defined(DYNAMIC_CUBEMAPS)
RWTexture2D<float4> outEnvIrradiance : register(u6);
#endif
#ifdef SSGI_REBLUR
// Current-frame diffuse signal, independent of the legacy SH2 temporal history.
RWTexture2D<float4> outNRDDiffuse : register(u7);
#endif

float GetDepthFade(float depth)
{
	return saturate((depth - DepthFadeRange.x) * DepthFadeScaleConst);
}

// Engine-specific screen & temporal noise loader
float2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)  // without TAA, temporalIndex is always 0
{
	// noise texture from https://github.com/electronicarts/fastnoise
	// 128x128x64
	uint2 noiseCoord = (pixCoord % 128) + uint2(0, (temporalIndex % 64) * 128);
	return srcNoise.Load(uint3(noiseCoord, 0));
}

// [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"]
float GetNormalDistributionFunctionGGX(float roughness, float NdotH)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float d = max((NdotH * a2 - NdotH) * NdotH + 1, 1e-5);
	return a2 / (Math::PI * d * d);
}

// [Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"]
float GetVisibilityFunctionSmithJointApprox(float roughness, float NdotV, float NdotL)
{
	float a = roughness * roughness;
	float visSmithV = NdotL * (NdotV * (1 - a) + a);
	float visSmithL = NdotV * (NdotL * (1 - a) + a);
	float vis = visSmithV + visSmithL;
	return vis > 0 ? (0.5 / vis) : 0;
}

// (directional env) The bit-field machinery below used to live behind `#ifdef GI`. The bent-normal
// output integrates the *unoccluded* bits with the same table and the same quadrature in every
// permutation, AO-only included, so the table and the two functions are now unconditional. The GI
// radiance accumulation that consumes them stays behind the define.

///////////////////////////////////////////////////////////////////////////////
// Analytic bit-field integration
//
// Ported from the upstream rewrite (build-0816 diffuseGI.cs.hlsl:95-185) and adapted to this
// lineage's SH transport. It replaces the `giBoost = 4 * PI * (...)` heuristic and the
// `countbits(...) * 0.03125` bit average with a closed-form quadrature over the covered angular
// bins, so a constant-radiance white furnace integrates to exactly 1.
//
// DERIVATION (why the numbers below are what they are)
//
// A slice is the great circle spanned by `directionVec` and `viewVec`. Writing the hemisphere
// integral in that parametrisation,
//
//     E = INT_0^PI dphi INT_slice L * dot(dir, N) * |sin(theta_view)| dtheta
//
// where phi is the azimuth about the view axis (a slice covers phi and phi+PI, i.e. the whole
// great circle, so phi only needs to run over PI) and |sin(theta_view)| is the spherical
// Jacobian. Sampling phi uniformly with NumSlices slices turns the outer integral into
// (PI / NumSlices) * sum, and Lambert's 1/PI then cancels the PI exactly - which is why every
// accumulator below is scaled by nothing but `rcpNumSlices` and still lands on E/PI, i.e. on the
// value that wants exactly one albedo multiplication at composite time.
//
// Inside a slice, bit b covers relative angle theta_b = (b + 0.5)/32 * PI - PI/2 measured from
// the projected receiver normal, and
//
//     sum_b cos(theta_b) * |sin(theta_b)| = 1 / sin(PI/32)   (exact, closed form)
//
// so multiplying each bin by `normalizedBitMeasure = sin(PI/32)` is the analytic finite-bin
// normalisation: full coverage of a unit-radiance hemisphere returns exactly 1.
//
// TWO MEASURES, BECAUSE THIS LINEAGE TRANSPORTS LUMINANCE THROUGH SH (the H3 re-derivation)
//
// `radianceY` is an SH2 *radiance* projection and the consumer (DeferredCompositeCS.hlsl:44-50)
// integrates it against EvaluateCosineLobe(n), i.e. the receiver cosine is applied by the
// consumer. Chroma has no directional representation and is consumed as a bare value. So the two
// need different measures, and that - not a constant - is what makes them agree:
//
//   solidAngle*: |sin(theta_view)| * sin(PI/32) per bit, with the matching first moment
//                sum(dir * w). This is the plain solid-angle measure; folded into
//                Evaluate(dirWS) it is the SH radiance projection, and the consumer's cosine
//                lobe supplies the receiver cosine. Verified: for a fully covered hemisphere
//                sum(w) = 2 and the moment's normal component = 1, giving
//                L0 = 0.28209*2, L1 = 0.48860*1, and
//                FuncProductIntegral(sh, EvaluateCosineLobe(n)) = 0.5 + 0.5 = 1.
//   cosineWeight: dot(dir, N) * |sin(theta_view)| * sin(PI/32) per bit, i.e. the upstream
//                weight, which already contains the receiver cosine. Sums to 1 over a fully
//                covered hemisphere. This is the chroma measure.
//
// Note dot(dir, N) == projectedNormalLength * cos(theta_b) identically, because `dir` lies in the
// slice plane and the out-of-plane part of N is orthogonal to it - that is the upstream weight.
//
// The previous code applied the receiver cosine twice (once through the smoothstep-warped bit
// density, once through the SH cosine lobe) and the source cosine once too often, and used
// `CoCg *= 0.5` to bring the two channels back into rough agreement. Both cosines and the
// constant are now gone; the ratio is exact instead of averaged.
///////////////////////////////////////////////////////////////////////////////

static const uint SSGI_MAX_RAY = 32;

// sin/cos of the positive bit-centre offsets from the projected receiver normal. The negative
// half is mirrored, which halves the table and avoids a per-bit sincos.
// Verbatim from build-0816 diffuseGI.cs.hlsl:95-112.
static const float2 SSGI_BIT_SIN_COS[SSGI_MAX_RAY / 2] = {
	float2(0.9987954562, 0.0490676743),
	float2(0.9891765100, 0.1467304745),
	float2(0.9700312532, 0.2429801799),
	float2(0.9415440652, 0.3368898534),
	float2(0.9039892931, 0.4275550934),
	float2(0.8577286100, 0.5141027442),
	float2(0.8032075315, 0.5956993045),
	float2(0.7409511254, 0.6715589548),
	float2(0.6715589548, 0.7409511254),
	float2(0.5956993045, 0.8032075315),
	float2(0.5141027442, 0.8577286100),
	float2(0.4275550934, 0.9039892931),
	float2(0.3368898534, 0.9415440652),
	float2(0.2429801799, 0.9700312532),
	float2(0.1467304745, 0.9891765100),
	float2(0.0490676743, 0.9987954562)
};

// Bits this sample newly covers, folded into the running occlusion field.
// Verbatim from build-0816 diffuseGI.cs.hlsl:123-138. Unlike the open-coded
// `((1 << count) - 1) << start` it also handles a full 32-bit span, where that shift is
// undefined.
uint ComputeOccludedBitfield(float minHorizon, float maxHorizon, inout uint globalOccludedBitfield)
{
	uint startHorizonInt = min((uint)(saturate(minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY);
	uint angleHorizonInt = min((uint)ceil(saturate(maxHorizon - minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY - startHorizonInt);

	if (angleHorizonInt == 0)
		return 0;

	uint angleHorizonBitfield = 0xFFFFFFFFu;
	if (angleHorizonInt < SSGI_MAX_RAY)
		angleHorizonBitfield = (1u << angleHorizonInt) - 1u;
	uint currentOccludedBitfield = angleHorizonBitfield << startHorizonInt;
	currentOccludedBitfield &= ~globalOccludedBitfield;
	globalOccludedBitfield |= currentOccludedBitfield;
	return currentOccludedBitfield;
}

// Integrates the newly covered angular bins of one sample. All outputs are view space; the
// caller aggregates them and converts the direction moment to world space once per pixel.
void IntegrateBitfield(
	uint bitfield,
	float3 projectedNormal, float3 projectedNormalTangent,
	float projectedNormalLength, float projectedNormalSin, float projectedNormalCos,
	float3 sourceNormal, bool requireSourceFacing,
	out float o_solidAngleWeight, out float3 o_solidAngleMoment, out float o_cosineWeight)
{
	o_solidAngleWeight = 0;
	o_solidAngleMoment = 0;
	o_cosineWeight = 0;

	const float bitAngle = Math::PI / float(SSGI_MAX_RAY);
	// Midpoint integration of cos(theta) * |sin(theta)| over all bins yields
	// bitAngle / sin(bitAngle); sin(bitAngle) is therefore the analytic finite-bin
	// normalisation that makes a constant-radiance white furnace return 1.
	const float normalizedBitMeasure = sin(bitAngle);
	// Newly covered hit bits never overlap, so sparse iteration caps all hit integration work
	// across the whole ray march at 32 iterations per slice.
	[loop] while (bitfield != 0)
	{
		uint bit = (uint)firstbitlow(bitfield);
		bitfield &= bitfield - 1u;

		uint mirroredBit = bit < SSGI_MAX_RAY / 2 ? bit : SSGI_MAX_RAY - 1 - bit;
		float2 bitSinCos = SSGI_BIT_SIN_COS[mirroredBit];
		float bitSin = bit < SSGI_MAX_RAY / 2 ? bitSinCos.x : -bitSinCos.x;
		float bitCos = bitSinCos.y;

		float3 direction = projectedNormal * bitCos + projectedNormalTangent * bitSin;
		// Outgoing Lambertian radiance is angle-independent over the source's front hemisphere.
		// Gate the back hemisphere, but do not multiply by a second source cosine
		// (build-0816 diffuseGI.cs.hlsl:173-177).
		if (requireSourceFacing && dot(sourceNormal, -direction) <= 0.0)
			continue;

		float sineFromView = bitSin * projectedNormalCos + bitCos * projectedNormalSin;
		float solidAngle = abs(sineFromView) * normalizedBitMeasure;

		o_solidAngleWeight += solidAngle;
		o_solidAngleMoment += direction * solidAngle;
		o_cosineWeight += projectedNormalLength * bitCos * solidAngle;
	}
}

#if defined(DYNAMIC_CUBEMAPS)
///////////////////////////////////////////////////////////////////////////////
// (directional env v2) Per-direction environment radiance, replicated from the SSRT diffuse
// fallback (ssrt_raymarch.hlsl:1012-1070) -- the reference integrand this channel exists to
// reproduce inside the bitmask sweep. Line-by-line correspondence:
//
//   * mip: the diffuse fallback samples the cubemap at mip 2 (ssrt_raymarch.hlsl:1018), fine
//     enough that sky and wall stay separate texels (32x32 per face on the 128^2 capture,
//     ~2.8 degrees per texel). v1's aperture-driven coarse mip (up to the 2x2 level) is exactly
//     what averaged the whole scene into one tint; reference-identical mip 2 is the fix.
//   * normalisation: the same DALC-luminance / cubemap-mip-15-luminance ratio the fallback
//     applies with CubemapNormalization = 1 (:1020 numerator, :1060/:1064 application), with
//     two deliberate, documented deviations for THIS consumption chain:
//       - no ReflectionNormalisationScale. In SSRT that 0.65 cancels against the consumer's
//         1/PBRLightingScale (same 0.65 constant) when the radiance meets
//         `linAlbedo = IrradianceToLinear(albedo / PBRLightingScale)`. This channel's re-add is
//         multiplied by the raw gamma-space albedo in DeferredCompositeCS, so carrying the 0.65
//         here would darken the whole ambient by a third with nothing to cancel it.
//       - Color::Ambient() wraps the DALC sample. Identity without Linear Lighting; with LL it
//         is the ambient-gamma shaping the composite's own A_est applies to the very term this
//         replaces (DeferredCompositeCS directionalAmbientColor), so parity survives LL.
//   * skylighting: applied to the SKY part only, exactly like :1058-1061. The visibility comes
//     in per DIRECTION from the caller (see the segment loop) instead of once per ray.
//   * Color::IrradianceToLinear at the end (:1070): the integral accumulates in linear space,
//     which is also the space the IL chain filters in.
///////////////////////////////////////////////////////////////////////////////

// Verbatim duplicate of SSRT_CubemapNormalizationRatio (ssrt_raymarch.hlsl:696, guard G7),
// including the non-finiteness fallback to 1.0. Duplicated rather than included: features must
// not #include across feature directories that may be absent at runtime.
float DirEnvNormalizationRatio(float ambientLuminance, float envLuminance)
{
	float ratio = ambientLuminance / max(envLuminance, 1e-4);
	return isFiniteSafe(ratio) ? ratio : 1.0;
}

static const float DIR_ENV_SAMPLE_MIP = 2.0;

float3 DirEnvSampleEnvironment(float3 dirWS, float skyVisibility)
{
	float directionalAmbientLuminance = Color::RGBToLuminance(Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(dirWS, 1.0)))));
	float3 envColor;
	float envLuminance;
#	if defined(SKYLIGHTING)
	[branch] if (!SharedData::InInterior) {
		float3 envNoSkyColor = EnvTexture.SampleLevel(samplerLinearClamp, dirWS, DIR_ENV_SAMPLE_MIP);
		float3 envSkyColor = EnvReflectionsTexture.SampleLevel(samplerLinearClamp, dirWS, DIR_ENV_SAMPLE_MIP);
		float3 skyColor = max(envSkyColor - envNoSkyColor, 0);
		// Level 15 clamps to the top of the chain -- the same "mean luminance" fetch the
		// reference uses (:1059).
		envLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(samplerLinearClamp, dirWS, 15));
		envColor = envNoSkyColor * DirEnvNormalizationRatio(directionalAmbientLuminance, envLuminance);
		envColor += skyColor * skyVisibility;
	} else {
		envColor = EnvReflectionsTexture.SampleLevel(samplerLinearClamp, dirWS, DIR_ENV_SAMPLE_MIP);
		envLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(samplerLinearClamp, dirWS, 15));
		envColor *= DirEnvNormalizationRatio(directionalAmbientLuminance, envLuminance);
	}
#	else
	envColor = EnvReflectionsTexture.SampleLevel(samplerLinearClamp, dirWS, DIR_ENV_SAMPLE_MIP);
	envLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(samplerLinearClamp, dirWS, 15));
	envColor *= DirEnvNormalizationRatio(directionalAmbientLuminance, envLuminance);
#	endif
	return Color::IrradianceToLinear(envColor);
}

// Segment width of the run-length sweep over the unoccluded bits, in bins. Per-bin sampling
// (up to 128 environment lookups per pixel) exceeds any sane budget, so contiguous unoccluded
// runs are cut into segments of at most this many bins and each segment takes ONE environment
// lookup at its mean direction, weighted by the segment's analytic cosine measure. 4 bins =
// 22.5 degrees of arc, which is finer than the sky/wall structures the mip-2 sample resolves
// matters for -- the horizon line lands within one segment of its true elevation.
static const uint SSGI_ENV_SEG_BITS = 4;
#endif

void CalculateGI(
	uint2 dtid, float2 uv, float viewspaceZ, float3 viewspaceNormal,
	out float o_ao, out sh2 o_currY, out float2 o_currCoCg, out float4 o_currGIAOSpecular,
	out float3 o_bentNormalWS, out float o_bentAperture, out float4 o_envIrradiance,
	out float o_diffuseHitDistance)
{
	const float2 frameScale = FrameDim * RcpTexDim;

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	float2 normalizedScreenPos = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	const float rcpNumSlices = rcp((float)NumSlices);
	const float rcpNumSteps = rcp((float)NumSteps);

	// approx viewspace pixel size at pixCoord; approximation of NDCToViewspace( uv.xy + ViewportSize.xy, pixCenterPos.z ).xy - pixCenterPos.xy;
	const float2 pixelDirRBViewspaceSizeAtCenterZ = viewspaceZ.xx * (eyeIndex == 0 ? NDCToViewMul.xy : NDCToViewMul.zw) * RCP_OUT_FRAME_DIM;

	float screenspaceRadius = EffectRadius / pixelDirRBViewspaceSizeAtCenterZ.x;
	screenspaceRadius = max(MinScreenRadius, screenspaceRadius);
	// `s` is a fraction of EffectRadius, so a pixel-space floor has to be expressed in those
	// units; this converts the one into the other.
	const float rcpScreenspaceRadius = rcp(screenspaceRadius);

	//////////////////////////////////////////////////////////////////

	const float2 localNoise = SpatioTemporalNoise(dtid, FrameIndex);
	const float noiseSlice = localNoise.x;
	const float noiseStep = localNoise.y;

	//////////////////////////////////////////////////////////////////

	const float3 pixCenterPos = ScreenToViewPosition(normalizedScreenPos, viewspaceZ, eyeIndex);
	const float3 viewVec = normalize(-pixCenterPos);
#ifdef GI_SPECULAR
	const float NoV = clamp(dot(viewVec, viewspaceNormal), 1e-5, 1);
#endif

	// flip foliage normal
	if (dot(viewVec, pixCenterPos) > 0)
		viewspaceNormal = -viewspaceNormal;

#ifdef DYNAMIC_CUBEMAPS
	// (directional env v2) Whether the environment integration runs at all. Both gates are
	// runtime cbuffer reads, so toggling either feature needs no SSGI recompile:
	//  * EnableDirectionalEnv is pre-multiplied on the C++ side with "SSGI loaded and enabled";
	//  * with SSRT diffuse active this channel steps aside (the composite would ignore the
	//    surface anyway -- same A/B gate it uses), so the integration cost is not paid twice.
	const bool envActive = SharedData::ssgiSettings.EnableDirectionalEnv != 0 && !(SharedData::ssrtSettings.DiffuseMult > 0.0);
	float3 envIrradiance = 0;
#	ifdef SKYLIGHTING
	// Per-pixel skylighting state for the per-direction sky visibility, mirroring the SSRT
	// diffuse fallback's recipe (ssrt_raymarch.hlsl:1029-1052) with one deliberate change,
	// forced by budget and reported as a deviation: the reference re-samples the probe SH per
	// ray (8 taps of the 3D array each time) and collapses it to one scalar through the
	// surface-normal cosine lobe; at up to 32 environment segments per pixel that is untenable,
	// and a cosine-lobe scalar is not "per direction" anyway. Instead the probe SH is fetched
	// ONCE per pixel, biased along the receiver normal exactly like the forward diffuse path
	// (DeferredCompositeCS ambientSkylightingSH), and each segment reconstructs the visibility
	// in its own direction via SphericalHarmonics::Unproject -- the same SH, evaluated where the
	// segment actually points, which is the quantity the reference's per-ray scalar
	// approximates. The fade / upward-boost / MinDiffuseVisibility shaping is reproduced
	// verbatim; boost and fade are direction-independent and hoisted here.
	sh2 envSkySH = float4(sqrt(4.0 * Math::PI), 0, 0, 0);  // "fully open" prior, matches Skylighting's own unitSH
	float envSkyFade = 0.0;
	float envSkyBoost = 1.0;
	[branch] if (envActive && !SharedData::InInterior) {
		float3 envPositionMS = ViewToWorldPosition(pixCenterPos, FrameBuffer::CameraViewInverse[eyeIndex]);
#		ifdef VR
		envPositionMS += FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		endif
		float3 receiverNormalWS = ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse[eyeIndex]);
		envSkySH = Skylighting::sample(SharedData::skylightingSettings, SkylightingProbeArray, stbn_vec3_2Dx1D_128x128x64, dtid, envPositionMS, receiverNormalWS);
		envSkyFade = Skylighting::getFadeOutFactor(envPositionMS);
		envSkyBoost = 1.0 + saturate(receiverNormalWS.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);
	}
#	endif
#endif

	float visibility = 0;
	float visibilitySpecular = 0;
	// (directional env) Solid-angle mass and first moment of the UNOCCLUDED directions, i.e. the
	// complement of the per-slice linear-angle bitmask. Same measure the radiance integration
	// uses (|sin(theta_view)| * sin(PI/32) per bin), so a fully open hemisphere sums to exactly
	// 2 * NumSlices in mass and NumSlices in the moment's normal component - the baselines the
	// aperture mapping below is calibrated against (their ratio, 0.5, is the fully-open case).
	float bentWeightSum = 0;
	float3 bentMomentVS = 0;
	// Luminance is transported as an SH2 radiance projection. Because SphericalHarmonics::Evaluate
	// is affine in the direction, the whole march can be accumulated as a scalar mass plus a
	// view-space first moment and assembled into the SH once, which also means a single
	// view-to-world transform per pixel instead of one per sample.
	float radianceYScalar = 0;
	float3 radianceYMoment = 0;
	float2 radianceCoCg = 0;
	float3 radianceSpecular = 0;
#ifdef SSGI_REBLUR
	// REBLUR's hit distance is a signal statistic, not the AO/GI search radius. Weight
	// only samples that contributed newly visible diffuse angular bins.
	float diffuseHitDistanceSum = 0;
	float diffuseHitWeightSum = 0;
#endif

#ifdef GI_SPECULAR
	const float roughness = max(0.2, saturate(1 - FULLRES_LOAD(srcNormalRoughness, dtid, uv * frameScale, samplerLinearClamp).z));  // can't handle low roughness
#endif

	for (uint slice = 0; slice < NumSlices; slice++) {
		float phi = (Math::PI * rcpNumSlices) * (slice + noiseSlice);
		float3 directionVec = 0;
		sincos(phi, directionVec.y, directionVec.x);

		// convert to px units for later use
		float2 omega = float2(directionVec.x, -directionVec.y) * screenspaceRadius;

		const float3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec);
		const float3 axisVec = normalize(cross(orthoDirectionVec, viewVec));

		float3 projectedNormalVec = viewspaceNormal - axisVec * dot(viewspaceNormal, axisVec);
		float projectedNormalVecLength = length(projectedNormalVec);
		float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));
		float cosNorm = saturate(dot(projectedNormalVec, viewVec) / projectedNormalVecLength);

		float n = signNorm * FastMath::ACos(cosNorm);

		// Slice-plane frame for the analytic bit integration (build-0816 diffuseGI.cs.hlsl:290-297).
		// `projectedNormalTangent` completes an orthonormal pair with the projected normal inside
		// the slice plane. `sinNorm` is derived from the existing cosNorm and signNorm rather than
		// from a second dot product, which guarantees (cosNorm, sinNorm) is exactly the projected
		// normal expressed in the orthonormal (viewVec, orthoDirectionVec) basis and needs no extra
		// normalize. Upstream's `planeNormal`/`tangent` are this lineage's `axisVec` and
		// normalize(orthoDirectionVec): cross(directionVec, viewVec) == cross(orthoDirectionVec,
		// viewVec) and cross(viewVec, axisVec) == normalize(orthoDirectionVec), so the two frames
		// and therefore the angle-to-bit mapping are identical.
		//
		// (directional env) No longer GI-only: the bent-normal integration below uses the same
		// frame in every permutation.
		const float3 projectedNormalNorm = projectedNormalVec / max(projectedNormalVecLength, 1e-6);
		const float3 projectedNormalTangent = cross(projectedNormalNorm, axisVec);
		const float sinNorm = signNorm * sqrt(saturate(1.0 - cosNorm * cosNorm));

		uint bitmask = 0;
		// (directional env) Linear-angle occlusion field for the bent normal. Kept separate from
		// the AO `bitmask` above, whose smoothstep warp makes bit DENSITY the cosine weight -- the
		// per-bin quadrature in IntegrateBitfield needs bins uniform in angle, exactly like the GI
		// field. Gated on AORadius: the bent normal answers "which directions does the *occlusion*
		// see as open", so it uses the AO range, not the IL range.
		uint bitmaskBent = 0;
#ifdef GI
		uint bitmaskGI = 0;
#	ifdef GI_SPECULAR
		uint bitmaskGISpecular = 0;
		float3 domVec = getSpecularDominantDirection(viewspaceNormal, viewVec, roughness);
		float3 projectedDomVec = normalize(domVec - axisVec * dot(domVec, axisVec));
		float nDom = sign(dot(orthoDirectionVec, projectedDomVec)) * FastMath::ACos(saturate(dot(projectedDomVec, viewVec)));
#	endif
#endif

		// R1 sequence (http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/)
		float stepNoise = frac(noiseStep + slice * 0.6180339887498948482);

		[unroll] for (int sideSign = -1; sideSign <= 1; sideSign += 2)
		{
			[loop] for (uint step = 0; step < NumSteps; step++)
			{
				float s = (step + stepNoise) * rcpNumSteps;
				s *= s;  // default 2 is fine
				// Near-field step sequence (build-0816 diffuseGI.cs.hlsl:313). The quadratic ramp puts
				// almost nothing inside the first few pixels: with the previous `s += 1.3 /
				// screenspaceRadius` floor, steps 0 and 1 landed at roughly 1.3 and 8 px and step 2
				// already past 23 px, so contact-scale occlusion in between was structurally
				// unreachable. Flooring each step at (1 + step) pixels instead guarantees a dense
				// 1, 2, 3, ... px sequence per side while leaving the far field untouched, and it
				// subsumes the old floor's other job of never sampling the centre pixel.
				s = max(s, (1.0 + step) * rcpScreenspaceRadius);

				float2 sampleOffset = s * omega;

				float2 samplePxCoord = dtid + .5 + sampleOffset * sideSign;
				float2 sampleUV = samplePxCoord * RCP_OUT_FRAME_DIM;
				float2 sampleScreenPos = Stereo::ConvertFromStereoUV(sampleUV, eyeIndex);
				[branch] if (any(sampleScreenPos > 1.0) || any(sampleScreenPos < 0.0)) break;

				// Unified mip chain (build-0816 diffuseGI.cs.hlsl:321): one level per two steps, capped at
				// the coarsest level either pyramid has. Tying the level to the step index instead of to
				// log2(offset) is what lets step 0 read a 1:1 texel, which the previous
				// `clamp(log2(len) - 3.3, ...)` plus a hard resolution floor could not do.
				//
				// Adaptation for this lineage's resolution scheme, which upstream does not have: the
				// radiance pyramid is built at the GI *working* resolution while the depth pyramid is
				// full resolution, so the level that means "1:1" differs between them by exactly RES_MIP.
				// Adding RES_MIP to the depth level, rather than flooring both, is the correct way to keep
				// the two footprints aligned - and it is what removes the old radiance floor that made
				// half-res radiance effectively 1/8 x 1/8 of full resolution (H2), without pretending the
				// depth pyramid has levels finer than the working resolution.
				const float mipLevelRadiance = (float)min((step + 1u) / 2u, 4u);
				const float mipLevel = min(mipLevelRadiance + RES_MIP, 4.0);

				float SZ = srcWorkingDepth.SampleLevel(samplerPointClamp, sampleUV * frameScale, mipLevel);

				float3 samplePos = ScreenToViewPosition(sampleScreenPos, SZ, eyeIndex);
				float3 sampleDelta = samplePos - pixCenterPos;
				float3 sampleHorizonVec = normalize(sampleDelta);

				// Occluder thickness scales with view depth (build-0816 diffuseGI.cs.hlsl:331). An
				// absolute world thickness is the wrong unit for a screen-space trace: the same setting
				// covers a whole wall up close and less than a texel at range, so the back horizon - and
				// with it how many bits a sample claims - drifted with distance. Scaling by viewspaceZ
				// makes it a constant angular thickness instead. Thickness is now a ratio, not units.
				float3 sampleBackPos = samplePos - viewVec * Thickness * viewspaceZ;
				float3 sampleBackHorizonVec = normalize(sampleBackPos - pixCenterPos);

				float angleFront = FastMath::ACos(dot(sampleHorizonVec, viewVec));  // either clamp or use float version for whatever reason
				float angleBack = FastMath::ACos(dot(sampleBackHorizonVec, viewVec));
				float2 angleRange = -sideSign * (sideSign == -1 ? float2(angleFront, angleBack) : float2(angleBack, angleFront));
				// The math: https://www.desmos.com/calculator/je4y5ved2j
				float2 angleRangeNorm = (angleRange + n) * RCP_PI + .5;

				// Using smoothstep for cos: https://discord.com/channels/586242553746030596/586245736413528082/1102228968247144570
				float2 angleRangeAO = smoothstep(0, 1, angleRangeNorm);

				uint2 bitsRange = uint2(round(angleRangeAO.x * 32u), round((angleRangeAO.y - angleRangeAO.x) * 32u));
				uint maskedBits = s < AORadius ? ((1 << bitsRange.y) - 1) << bitsRange.x : 0;

				// Linear angle-to-bit mapping. The AO field above keeps the smoothstep warp - there the
				// bit *density* is the cosine weight, which is what makes a plain countbits() a
				// cosine-weighted visibility - but the analytic integration (IntegrateBitfield)
				// evaluates cos/sin at each bin centre and therefore needs bins that are uniform in
				// angle (build-0816 diffuseGI.cs.hlsl:336). Shared by the bent-normal field below and,
				// under GI, by the radiance field.
				float2 angleRangeLinNorm = saturate(angleRangeNorm);

				// (directional env) Bent-normal occlusion field. Gated on AORadius - the bent normal
				// answers "which directions does the *occlusion* see as open", so it uses the AO
				// range, not the IL range. ComputeOccludedBitfield rather than the open-coded shift
				// because a sample can legitimately claim the full 32-bit span, where the shift is
				// undefined; the returned newly-covered bits are not needed here, only the union.
				[branch] if (s < AORadius)
					ComputeOccludedBitfield(angleRangeLinNorm.x, angleRangeLinNorm.y, bitmaskBent);

#ifdef GI
				// IL shares the AO thickness now; the separate 300-unit GI thickness is gone.
				//
				// That constant existed to let one sample's back horizon reach far behind the surface, so
				// the hemisphere would fill up even from sparse hits. Once the covered bits became the
				// radiance's solid-angle weight (the analytic normalisation), that stopped being free: an
				// artificially wide bit span smears a single sample's colour across most of a slice,
				// which is exactly why indirect light used to appear only where AO appeared instead of
				// reading as bounced light. Upstream carries one thickness for both, and with a
				// viewZ-relative unit one value is meaningful at every distance, so they are unified.
				// The trade is real and deliberate: hemisphere coverage drops, so unfilled directions
				// simply contribute nothing (this lineage has no off-screen fallback), which GIStrength
				// accounts for.
				//
				// This also removes a normalize and an acos per sample, and lets the specular cone reuse
				// the same raw angle pair.
#	ifdef GI_SPECULAR
				float coneHalfAngles = max(5e-2, specularLobeHalfAngle(roughness));  // not too small
				float2 angleRangeSpecular = clamp((angleRange + nDom) * 0.5 / coneHalfAngles, -1, 1) * 0.5 + 0.5;

				uint2 bitsRangeGISpecular = uint2(round(angleRangeSpecular.x * 32u), round((angleRangeSpecular.y - angleRangeSpecular.x) * 32u));
				uint maskedBitsGISpecular = s < GIRadius ? ((1 << bitsRangeGISpecular.y) - 1) << bitsRangeGISpecular.x : 0;
#	endif

				uint validBits = 0;
				[branch] if (s < GIRadius)
					validBits = ComputeOccludedBitfield(angleRangeLinNorm.x, angleRangeLinNorm.y, bitmaskGI);

				bool checkGI = validBits != 0;

#	ifdef GI_SPECULAR
				uint overlappedBitsSpecular = maskedBitsGISpecular & 	bitmaskGISpecular;
				checkGI = checkGI || overlappedBitsSpecular;
#	endif

				if (checkGI) {
					// Artistic distance ramp, unchanged in meaning: 0 is neutral. It is now a plain
					// multiplier on an already-normalised weight instead of a scale on the 4*PI
					// heuristic that used to stand in for the normalisation.
					float distanceCompensation = 1 + GIDistanceCompensation * smoothstep(0, GICompensationMaxDist, s * EffectRadius);

					// IL
					float3 normalSample = GBuffer::DecodeNormal(srcNormalRoughness.SampleLevel(samplerPointClamp, sampleUV * frameScale, 0).xy);
					if (dot(samplePos, normalSample) > 0)
						normalSample = -normalSample;

					float3 sampleRadiance = max(0, srcRadiance.SampleLevel(samplerPointClamp, sampleUV * OUT_FRAME_SCALE, mipLevelRadiance).rgb);
					float3 sampleRadianceYCoCg = Color::RGBToYCoCg(sampleRadiance);

					[branch] if (validBits) {
						float solidAngleWeight, cosineWeight;
						float3 solidAngleMoment;
						IntegrateBitfield(
							validBits,
							projectedNormalNorm, projectedNormalTangent,
							projectedNormalVecLength, sinNorm, cosNorm,
							normalSample, true,
							solidAngleWeight, solidAngleMoment, cosineWeight);

						radianceYScalar += sampleRadianceYCoCg.r * solidAngleWeight * distanceCompensation;
						radianceYMoment += sampleRadianceYCoCg.r * solidAngleMoment * distanceCompensation;
						radianceCoCg += sampleRadianceYCoCg.gb * cosineWeight * distanceCompensation;
#						ifdef SSGI_REBLUR
						float hitWeight = max(solidAngleWeight, 0.0);
						diffuseHitDistanceSum += length(sampleDelta) * hitWeight;
						diffuseHitWeightSum += hitWeight;
#						endif
					}

#	ifdef GI_SPECULAR
					// HQ specular IL deliberately keeps its original, hand-tuned normalisation: the
					// measure derived above is a diffuse-irradiance measure (receiver cosine times
					// spherical Jacobian) and does not carry over to the GGX cone estimator below, and
					// re-deriving that estimator is outside the scope of this port. The one change is the
					// source-side cosine, which this commit removes as a spurious second cosine; it is
					// replaced by its analytic mean over the front hemisphere (1/2) so the specular level
					// is preserved instead of doubled. The back-hemisphere gate stays per sample here,
					// unlike the per-bit gate the diffuse path now uses.
					[branch] if (overlappedBitsSpecular && dot(normalSample, -sampleHorizonVec) > 0.0)
					{
						const float sourceCosineMean = 0.5;
						float giBoostSpecular = 4.0 * Math::PI * distanceCompensation * sourceCosineMean;

						// thank u Olivier!
						float NoH = clamp(dot(viewspaceNormal, normalize(viewVec + sampleHorizonVec)), 1e-2, 1);
						float NoL = clamp(dot(viewspaceNormal, sampleHorizonVec), 1e-2, 1);

						float3 specularRadiance = sampleRadiance * giBoostSpecular * countbits(validBits) * 0.03125 * countbits(overlappedBitsSpecular) * 0.03125;
						specularRadiance *= GetNormalDistributionFunctionGGX(roughness, NoH) * GetVisibilityFunctionSmithJointApprox(roughness, NoV, NoL);
						specularRadiance = max(0, specularRadiance);

						radianceSpecular += specularRadiance;
					}
#	endif
				}
#endif  // GI

				bitmask |= maskedBits;
			}
		}

		// (directional env) Fold this slice's UNOCCLUDED directions into the bent accumulators.
		// ~bitmaskBent is the complement of the linear-angle occlusion field: exactly the bins the
		// AO-range march never covered. Integrated with the same solid-angle measure as the GI
		// radiance (|sin(theta_view)| * sin(PI/32) per bin), so a fully open slice contributes 2
		// to the mass and 1 to the moment's normal component. requireSourceFacing is false: there
		// is no source surface here, the "sample" is the open environment beyond the horizon, so
		// every unoccluded bin counts. The unused cosine-measure output is dead and folded away.
		{
			float bentWeight, bentCosineUnused;
			float3 bentMoment;
			IntegrateBitfield(
				~bitmaskBent,
				projectedNormalNorm, projectedNormalTangent,
				projectedNormalVecLength, sinNorm, cosNorm,
				viewspaceNormal, false,
				bentWeight, bentMoment, bentCosineUnused);
			bentWeightSum += bentWeight;
			bentMomentVS += bentMoment;
		}

#ifdef DYNAMIC_CUBEMAPS
		// (directional env v2) Environment radiance over the same unoccluded bins, run-length
		// swept in segments of at most SSGI_ENV_SEG_BITS. Each segment takes one reference-grade
		// environment lookup (see DirEnvSampleEnvironment) at the segment's mean direction and
		// enters the sum with the segment's analytic COSINE measure -- the exact weight the IL
		// chroma uses (wc_b = |projN| * cos(theta_b) * |sin(theta_view)| * sin(PI/32), summed by
		// IntegrateBitfield over the segment's bins). Occluded bins carry the screen-space IL,
		// unoccluded bins carry this; the two halves therefore tile one hemisphere under one
		// normalisation, which is what makes the result an irradiance rather than a tint.
		//
		// Loop bounds: every segment consumes at least one bit of `openBits`, so the sweep is
		// hard-bounded at 32 iterations; a typical horizon-shaped mask yields 1-2 runs and a
		// fully open slice exactly 32 / SSGI_ENV_SEG_BITS = 8 segments.
		[branch] if (envActive) {
			uint openBits = ~bitmaskBent;
			[loop] while (openBits != 0) {
				uint segStart = (uint)firstbitlow(openBits);
				uint shifted = openBits >> segStart;
				uint inverted = ~shifted;
				// `shifted` has its run starting at bit 0; the first zero above it ends the run.
				// A fully open field is the one case with no zero at all (segStart 0, shifted
				// all-ones), where firstbitlow's input would be 0 and its result undefined.
				uint runLength = inverted != 0 ? (uint)firstbitlow(inverted) : SSGI_MAX_RAY;
				uint segLength = min(runLength, SSGI_ENV_SEG_BITS);
				uint segMask = ((1u << segLength) - 1u) << segStart;  // segLength <= 4, shift well-defined
				openBits &= ~segMask;

				float segWeight, segCosineWeight;
				float3 segMoment;
				IntegrateBitfield(
					segMask,
					projectedNormalNorm, projectedNormalTangent,
					projectedNormalVecLength, sinNorm, cosNorm,
					viewspaceNormal, false,
					segWeight, segMoment, segCosineWeight);

				// The solid-angle moment IS the segment's mean direction (up to length): the
				// same quadrature that weights the segment also points it, so direction and
				// weight cannot disagree. Never degenerate for a contiguous <= 22.5-degree
				// segment, but guarded anyway -- a zero-weight or zero-moment segment simply
				// contributes nothing, same as an occluded bin.
				float segMomentLen = length(segMoment);
				[branch] if (segCosineWeight > 1e-6 && segMomentLen > 1e-6) {
					float3 segDirWS = ViewToWorldVector(segMoment / segMomentLen, FrameBuffer::CameraViewInverse[eyeIndex]);
					float skyVisibility = 1.0;
#	ifdef SKYLIGHTING
					// Per-direction sky visibility: reconstruct the per-pixel probe SH in the
					// segment direction, then apply the reference's shaping chain in its
					// original order (fade towards 1 outside the probe volume, upward boost,
					// MinDiffuseVisibility floor -- ssrt_raymarch.hlsl:1046-1052).
					float visRaw = saturate(SphericalHarmonics::Unproject(envSkySH, segDirWS));
					skyVisibility = lerp(1.0, visRaw, envSkyFade);
					skyVisibility *= envSkyBoost;
					skyVisibility = Skylighting::mixDiffuse(SharedData::skylightingSettings, skyVisibility);
#	endif
					envIrradiance += DirEnvSampleEnvironment(segDirWS, skyVisibility) * segCosineWeight;
				}
			}
		}
#endif

		visibility += countbits(bitmask) * 0.03125;

#if defined(GI) && defined(GI_SPECULAR)
		visibilitySpecular += countbits(bitmaskGISpecular) * 0.03125;
#endif
	}

	float depthFade = GetDepthFade(viewspaceZ);

	visibility *= rcpNumSlices;
	visibility = lerp(saturate(visibility), 0, depthFade);
	visibility = 1 - pow(abs(1 - visibility), AOPower);

	// (directional env) Bent normal and aperture from the accumulated mass and first moment.
	//
	// Direction: the normalised moment, i.e. the mean unoccluded direction.
	// Aperture: the moment-length / mass ratio. Against the fully-open baselines (mass
	// 2 * NumSlices, moment NumSlices along the normal) the ratio is 0.5 for a completely open
	// hemisphere and rises towards 1 as the opening narrows to a single direction, so
	// 2 * (1 - ratio) maps it onto [0 = pinhole, 1 = open hemisphere]. Scale-invariant in
	// NumSlices by construction, so slice count changes quality, not meaning.
	float3 bentNormalVS = viewspaceNormal;
	float bentAperture = 0.0;
	{
		float bentMomentLen = length(bentMomentVS);
		[flatten] if (bentMomentLen > 1e-4 && bentWeightSum > 1e-4) {
			bentNormalVS = bentMomentVS / bentMomentLen;
			bentAperture = saturate(2.0 * (1.0 - bentMomentLen / bentWeightSum));
		}
	}
	// Far-field behaviour mirrors the other channels' depthFade, but towards the OPEN prior
	// rather than towards zero: at range the march sees too few pixels to measure occlusion,
	// and "surface normal, fully open" is what makes the composite's environment lookup degrade
	// to the isotropic ambient it replaces, instead of to a black or arbitrary direction.
	// normalize is safe: both inputs are unit and the bent normal cannot oppose the surface
	// normal (every bin lies in the surface's upper half-space).
	bentNormalVS = normalize(lerp(bentNormalVS, viewspaceNormal, depthFade));
	bentAperture = lerp(bentAperture, 1.0, depthFade);

	o_bentNormalWS = ViewToWorldVector(bentNormalVS, FrameBuffer::CameraViewInverse[eyeIndex]);
	o_bentAperture = bentAperture;

	// (directional env v2) Hemisphere environment irradiance, PREMULTIPLIED by the confidence
	// stored in A. rcpNumSlices is the whole normalisation, for the same reason as radianceY:
	// the azimuthal PI/NumSlices and Lambert's 1/PI cancel, and the per-slice cosine measure
	// already integrates a fully covered hemisphere to 1 -- so a constant-radiance environment
	// comes out at exactly that radiance.
	//
	// The confidence fades with depthFade like every other channel, but the composite CONSUMES
	// it differently: it lerps between this surface and the flat vanilla ambient by A, so at
	// range the channel hands back to the isotropic ambient instead of fading the light itself
	// to black. Premultiplied storage is what keeps that blend correct through every downstream
	// filter -- reprojection taps, blur and upsample all blend RGB and A with the same weights,
	// which is only meaningful when RGB already carries its own coverage.
	o_envIrradiance = 0;
#ifdef DYNAMIC_CUBEMAPS
	{
		float envConfidence = envActive ? (1.0 - depthFade) : 0.0;
		o_envIrradiance = float4(envIrradiance * rcpNumSlices * envConfidence, envConfidence);
	}
#endif

#ifdef GI
	// Assemble the SH2 radiance projection from the accumulated mass and first moment. This is
	// identical to summing Y * Evaluate(dirWS) per bit, because Evaluate is affine in the
	// direction: its L0 coefficient is constant and its L1 band is linear. Doing it here also
	// keeps the view-to-world transform (per eye, VR-correct) to one per pixel.
	//
	// rcpNumSlices is the *whole* normalisation: the azimuthal Monte Carlo factor PI/NumSlices
	// and Lambert's 1/PI cancel, so the consumer's
	// FuncProductIntegral(radianceY, EvaluateCosineLobe(n)) lands on E/PI - exactly the value
	// that wants one albedo multiplication in DeferredCompositeCS.
	radianceYScalar *= rcpNumSlices;
	radianceYMoment *= rcpNumSlices;
	float3 radianceYMomentWS = ViewToWorldVector(radianceYMoment, FrameBuffer::CameraViewInverse[eyeIndex]);
	sh2 radianceYBasis = SphericalHarmonics::Evaluate(radianceYMomentWS);
	sh2 radianceY = sh2(radianceYBasis.x * radianceYScalar, radianceYBasis.yzw);
	radianceY = lerp(radianceY, 0, depthFade);

	// Chroma carries the cosine-weighted measure (see IntegrateBitfield): the receiver cosine is
	// in the weight here, whereas for luminance the consumer's cosine lobe supplies it. Both
	// measures integrate a fully covered unit-radiance hemisphere to 1, so the two channels are
	// on the same scale by construction and the old hand-derived "* 0.5" is gone.
	// The depth fade is applied to chroma as well now; fading luminance alone left a pure
	// chroma vector at the far end of the fade, which YCoCgToRGB turns into a coloured residue
	// that max(0, ...) then clips per channel.
	radianceCoCg *= rcpNumSlices * GISaturation;
	radianceCoCg = lerp(radianceCoCg, 0, depthFade);

#	ifdef GI_SPECULAR
	radianceSpecular *= rcpNumSlices;
	radianceSpecular = lerp(radianceSpecular, 0, depthFade);

	visibilitySpecular *= rcpNumSlices;
	visibilitySpecular = lerp(saturate(visibilitySpecular), 0, depthFade);
#	endif
#else
	sh2 radianceY = 0;
#endif

	o_ao = visibility;
	o_currY = radianceY;
	o_currCoCg = radianceCoCg;
	o_currGIAOSpecular = float4(radianceSpecular, visibilitySpecular);
#ifdef SSGI_REBLUR
	// A pixel with no screen-space hit represents a ray miss, i.e. the maximum
	// normalized hit distance. Zero would claim a receiver-local occluder to NRD.
	o_diffuseHitDistance = diffuseHitWeightSum > 1e-6 ? diffuseHitDistanceSum / diffuseHitWeightSum : NRD_FP16_MAX;
#else
	o_diffuseHitDistance = 0.0;
#endif
}

[numthreads(8, 8, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
	const float2 frameScale = FrameDim * RcpTexDim;

	uint2 pxCoord = dtid;

	float2 uv = (pxCoord + .5) * RCP_OUT_FRAME_DIM;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	float viewspaceZ = READ_DEPTH(srcWorkingDepth, pxCoord);

	float2 normalSample = FULLRES_LOAD(srcNormalRoughness, pxCoord, uv * frameScale, samplerLinearClamp).xy;
	float3 viewspaceNormal = GBuffer::DecodeNormal(normalSample);

	float3 worldNormal = ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse[eyeIndex]);
	half2 encodedWorldNormal = GBuffer::EncodeNormal(worldNormal);
	outPrevGeo[pxCoord] = half3(viewspaceZ, encodedWorldNormal);

	// Move center pixel slightly towards camera to avoid imprecision artifacts due to depth buffer imprecision; offset depends on depth texture format used
	viewspaceZ *= 0.99920h;  // this is good for FP16 depth buffer

	float currAo = 0;
	float4 currY = 0;
	float2 currCoCg = 0;
	float4 currGIAOSpecular = float4(0, 0, 0, 0);
	float diffuseHitDistance = 0.0;
	// (directional env) Open-hemisphere prior for pixels the march never measures (sky,
	// first-person geometry, beyond the fade range): surface normal, aperture 1, so the
	// composite's environment lookup degrades to a wide-cone sample along the normal - the
	// closest thing to the isotropic ambient it replaces.
	float3 bentNormalWS = worldNormal;
	float bentAperture = 1.0;
	// (directional env v2) Default is "no data": zero irradiance, zero confidence. The
	// composite lerps by the confidence, so sky, first-person geometry and everything beyond
	// the fade range fall back to the flat vanilla ambient rather than to black -- the same
	// degrade target the v1 open-prior aimed for, reached through the weight instead of
	// through a fabricated direction.
	float4 currEnvIrradiance = 0;

	bool needGI = viewspaceZ > FP_Z && viewspaceZ < DepthFadeRange.y;
	if (needGI) {
		CalculateGI(
			pxCoord, uv, viewspaceZ, viewspaceNormal,
			currAo, currY, currCoCg, currGIAOSpecular,
			bentNormalWS, bentAperture, currEnvIrradiance, diffuseHitDistance);

#ifdef SSGI_REBLUR
		// Match the diffuse composite's SH2 resolve at this receiver normal, before the
		// legacy EMA changes the current-frame sample. REBLUR_DIFFUSE consumes RGB radiance;
		// SH2 cannot be reinterpreted as NRD's spherical-Gaussian SH representation.
		float diffuseY = SphericalHarmonics::SHHallucinateZH3Irradiance(currY, worldNormal);
		float3 diffuseRGB = Color::YCoCgToRGB(float3(diffuseY, currCoCg));
		diffuseRGB = clamp(filterInf(filterNaN(diffuseRGB)), 0.0, SSGI_MAX_OUTPUT);
		float normHitDistance = REBLUR_FrontEnd_GetNormHitDist(
			max(0.0, filterInf(filterNaN(diffuseHitDistance))), viewspaceZ,
			float3(3.0, 0.1, 20.0), 1.0);
		outNRDDiffuse[pxCoord] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
			diffuseRGB, normHitDistance, true);
#endif

#ifdef TEMPORAL_DENOISER
		const float accumFrames = srcAccumFrames[pxCoord] * 255;
		const float lerpFactor = rcp(accumFrames);

		// AO was the one channel left out of the temporal filter, even though the whole path for it
		// already existed: radianceDisocc.cs.hlsl writes the reprojected previous AO into the
		// texture this pass binds as srcPrevAo, and nothing read it. That is why AO was the channel
		// that visibly flickered while indirect light sat still - it was the only single-frame
		// estimate in the output. It shares accum_frames with the other channels, so the same
		// disocclusion test resets it.
		//
		// (F1) It does not share MaxAccumFrames, though. AO is the one output channel that is both
		// multiplicative (DeferredCompositeCS scales the whole ambient and indirect term by it) and
		// spatially unfiltered (blur.cs.hlsl only touches IL), so a stale AO sample is not diluted
		// by anything before it reaches the frame - it lands as a hard dark patch where the
		// occluder used to be. At MaxAccumFrames=16 the EMA has a ~36-frame tail, long enough to
		// leave a visible dark trail behind anything that moves. Capping the *effective* frame
		// count for this channel alone shortens the tail without touching IL, where the long
		// window is what suppresses the noise. Set MaxAccumFramesAO == MaxAccumFrames to get the
		// old shared behaviour back.
		const float lerpFactorAo = rcp(min(accumFrames, (float)MaxAccumFramesAO));
		currAo = lerp(srcPrevAo[pxCoord], currAo, lerpFactorAo);
		currY = lerp(srcPrevY[pxCoord], currY, lerpFactor);
		currCoCg = lerp(srcPrevCoCg[pxCoord], currCoCg, lerpFactor);
#	ifdef DYNAMIC_CUBEMAPS
		// (directional env v2) Radiance data on the IL chain, so it shares the IL channels'
		// FULL temporal window (lerpFactor), not the AO/bent-normal shortened one: like Y/CoCg
		// it is spatially filtered afterwards and additively consumed, so the long window
		// suppresses the segment-sampling noise the way it suppresses IL noise. Componentwise
		// lerp is correct for premultiplied data -- RGB and A age together.
		currEnvIrradiance = lerp(srcPrevEnvIrradiance[pxCoord], currEnvIrradiance, lerpFactor);
#	endif

		// (directional env) Bent-normal history EMA, in the decoded VECTOR domain - lerping the
		// octahedral encoding across its fold lines fabricates directions (see the codec note in
		// common.hlsli). Shares the AO channel's shortened window (MaxAccumFramesAO): like AO,
		// the bent normal is consumed multiplicatively (it steers the ambient chroma), so a
		// stale direction lags as a visible colour trail under the full IL window. On a
		// disocclusion accumFrames is 1, lerpFactorAo is 1, and the history term - whatever a
		// cleared or unwritten texel decodes to - vanishes entirely.
		float3 prevBentDir;
		float prevBentAperture;
		SSGI_DecodeBentNormal(srcPrevBentNormal[pxCoord], prevBentDir, prevBentAperture);
		float3 bentBlend = lerp(prevBentDir, bentNormalWS, lerpFactorAo);
		float bentBlendLen = length(bentBlend);
		[flatten] if (bentBlendLen > 1e-4)
			bentNormalWS = bentBlend / bentBlendLen;
		bentAperture = lerp(prevBentAperture, bentAperture, lerpFactorAo);
#	ifdef GI_SPECULAR
		currGIAOSpecular = lerp(srcPrevGISpecular[pxCoord], currGIAOSpecular, lerpFactor);
#	endif
#endif
	}
#ifdef SSGI_REBLUR
	else {
		// The NRD input is a separate, non-temporal surface and must be defined even on
		// sky or first-person pixels skipped by the GI march.
		outNRDDiffuse[pxCoord] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3(0.0, 0.0, 0.0), 1.0, true);
	}
#endif
	// (guard N3/N5) These three are the writers of the IL / specular history, so anything
	// non-finite that leaves here is permanent: the temporal EMA below is lerp(prev, curr, f),
	// and lerp(Inf, curr, f) is Inf for every finite f, so a single poisoned texel survives
	// forever and blur.cs.hlsl hands it to eight more neighbours every frame. Only NaN was being
	// rejected; Inf went straight through. Note the clamp is a magnitude clamp, not a floor at
	// zero -- currY is a set of SH2 coefficients and currCoCg is chroma, both legitimately
	// signed. currAo needs none of this: its target is R8_UNORM, which cannot store a non-finite
	// or out-of-range value.
	currY = clamp(filterInf(filterNaN(currY)), -SSGI_MAX_OUTPUT, SSGI_MAX_OUTPUT);
	currCoCg = clamp(filterInf(filterNaN(currCoCg)), -SSGI_MAX_OUTPUT, SSGI_MAX_OUTPUT);
	currGIAOSpecular = clamp(filterInf(filterNaN(currGIAOSpecular)), -SSGI_MAX_OUTPUT, SSGI_MAX_OUTPUT);
#ifdef DYNAMIC_CUBEMAPS
	// (guard, same discipline as N3/N5) This is a writer of the env-irradiance history, and its
	// target is a float format that CAN store a non-finite value, so the same containment
	// applies. The floor is 0 rather than -SSGI_MAX_OUTPUT: premultiplied radiance and its
	// confidence are both non-negative by construction, and clamping says so.
	currEnvIrradiance = clamp(filterInf(filterNaN(currEnvIrradiance)), 0, SSGI_MAX_OUTPUT);
#endif

	outAo[pxCoord] = currAo;
	outY[pxCoord] = currY;
	outCoCg[pxCoord] = currCoCg;
	// (directional env) No finiteness guard needed: the target is R8G8B8A8_UNORM, which cannot
	// store a non-finite or out-of-range value (same argument as currAo above), and a NaN lane
	// self-heals - the next frame's history tap decodes to a valid unit vector regardless of
	// what the poisoned write clamped to.
	outBentNormal[pxCoord] = SSGI_EncodeBentNormal(bentNormalWS, bentAperture);
#ifdef DYNAMIC_CUBEMAPS
	outEnvIrradiance[pxCoord] = currEnvIrradiance;
#endif
#ifdef GI_SPECULAR
	outGISpecular[pxCoord] = currGIAOSpecular;
#endif
}
