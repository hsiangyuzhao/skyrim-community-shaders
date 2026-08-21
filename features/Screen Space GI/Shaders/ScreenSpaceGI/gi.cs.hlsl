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

RWTexture2D<unorm float> outAo : register(u0);
RWTexture2D<float4> outY : register(u1);
RWTexture2D<float2> outCoCg : register(u2);
RWTexture2D<float4> outGISpecular : register(u3);
RWTexture2D<half3> outPrevGeo : register(u4);

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

#ifdef GI

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

#endif  // GI

void CalculateGI(
	uint2 dtid, float2 uv, float viewspaceZ, float3 viewspaceNormal,
	out float o_ao, out sh2 o_currY, out float2 o_currCoCg, out float4 o_currGIAOSpecular)
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

	float visibility = 0;
	float visibilitySpecular = 0;
	// Luminance is transported as an SH2 radiance projection. Because SphericalHarmonics::Evaluate
	// is affine in the direction, the whole march can be accumulated as a scalar mass plus a
	// view-space first moment and assembled into the SH once, which also means a single
	// view-to-world transform per pixel instead of one per sample.
	float radianceYScalar = 0;
	float3 radianceYMoment = 0;
	float2 radianceCoCg = 0;
	float3 radianceSpecular = 0;

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

#ifdef GI
		// Slice-plane frame for the analytic bit integration (build-0816 diffuseGI.cs.hlsl:290-297).
		// `projectedNormalTangent` completes an orthonormal pair with the projected normal inside
		// the slice plane. `sinNorm` is derived from the existing cosNorm and signNorm rather than
		// from a second dot product, which guarantees (cosNorm, sinNorm) is exactly the projected
		// normal expressed in the orthonormal (viewVec, orthoDirectionVec) basis and needs no extra
		// normalize. Upstream's `planeNormal`/`tangent` are this lineage's `axisVec` and
		// normalize(orthoDirectionVec): cross(directionVec, viewVec) == cross(orthoDirectionVec,
		// viewVec) and cross(viewVec, axisVec) == normalize(orthoDirectionVec), so the two frames
		// and therefore the angle-to-bit mapping are identical.
		const float3 projectedNormalNorm = projectedNormalVec / max(projectedNormalVecLength, 1e-6);
		const float3 projectedNormalTangent = cross(projectedNormalNorm, axisVec);
		const float sinNorm = signNorm * sqrt(saturate(1.0 - cosNorm * cosNorm));
#endif

		uint bitmask = 0;
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

				// Linear angle-to-bit mapping. The AO field above keeps the smoothstep warp - there the
				// bit *density* is the cosine weight, which is what makes a plain countbits() a
				// cosine-weighted visibility - but the analytic integration below evaluates cos/sin at
				// each bin centre and therefore needs bins that are uniform in angle (build-0816
				// diffuseGI.cs.hlsl:336).
				float2 angleRangeGINorm = saturate(angleRangeNorm);

				uint validBits = 0;
				[branch] if (s < GIRadius)
					validBits = ComputeOccludedBitfield(angleRangeGINorm.x, angleRangeGINorm.y, bitmaskGI);

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

		visibility += countbits(bitmask) * 0.03125;

#if defined(GI) && defined(GI_SPECULAR)
		visibilitySpecular += countbits(bitmaskGISpecular) * 0.03125;
#endif
	}

	float depthFade = GetDepthFade(viewspaceZ);

	visibility *= rcpNumSlices;
	visibility = lerp(saturate(visibility), 0, depthFade);
	visibility = 1 - pow(abs(1 - visibility), AOPower);

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

	half2 encodedWorldNormal = GBuffer::EncodeNormal(ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse[eyeIndex]));
	outPrevGeo[pxCoord] = half3(viewspaceZ, encodedWorldNormal);

	// Move center pixel slightly towards camera to avoid imprecision artifacts due to depth buffer imprecision; offset depends on depth texture format used
	viewspaceZ *= 0.99920h;  // this is good for FP16 depth buffer

	float currAo = 0;
	float4 currY = 0;
	float2 currCoCg = 0;
	float4 currGIAOSpecular = float4(0, 0, 0, 0);

	bool needGI = viewspaceZ > FP_Z && viewspaceZ < DepthFadeRange.y;
	if (needGI) {
		CalculateGI(
			pxCoord, uv, viewspaceZ, viewspaceNormal,
			currAo, currY, currCoCg, currGIAOSpecular);

#ifdef TEMPORAL_DENOISER
		float lerpFactor = rcp(srcAccumFrames[pxCoord] * 255);

		// AO was the one channel left out of the temporal filter, even though the whole path for it
		// already existed: radianceDisocc.cs.hlsl:147 writes the reprojected previous AO into the
		// texture this pass binds as srcPrevAo, and nothing read it. That is why AO was the channel
		// that visibly flickered while indirect light sat still - it was the only single-frame
		// estimate in the output. It shares accum_frames with the other channels, so the same
		// disocclusion test resets it and the same MaxAccumFrames bounds it.
		currAo = lerp(srcPrevAo[pxCoord], currAo, lerpFactor);
		currY = lerp(srcPrevY[pxCoord], currY, lerpFactor);
		currCoCg = lerp(srcPrevCoCg[pxCoord], currCoCg, lerpFactor);
#	ifdef GI_SPECULAR
		currGIAOSpecular = lerp(srcPrevGISpecular[pxCoord], currGIAOSpecular, lerpFactor);
#	endif
#endif
	}
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

	outAo[pxCoord] = currAo;
	outY[pxCoord] = currY;
	outCoCg[pxCoord] = currCoCg;
#ifdef GI_SPECULAR
	outGISpecular[pxCoord] = currGIAOSpecular;
#endif
}