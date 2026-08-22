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

// with additional edits by FiveLimbedCat/ProfJack

#ifndef SSGI_COMMON
#define SSGI_COMMON

///////////////////////////////////////////////////////////////////////////////

#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

cbuffer SSGICB : register(b1)
{
	float4x4 PrevInvViewMat[2];
	float4 NDCToViewMul;
	float4 NDCToViewAdd;

	float2 TexDim;
	float2 RcpTexDim;
	float2 FrameDim;
	float2 RcpFrameDim;

	uint FrameIndex;

	uint NumSlices;
	uint NumSteps;

	float MinScreenRadius;
	float AORadius;
	float GIRadius;
	float EffectRadius;
	float Thickness;
	float2 DepthFadeRange;
	float DepthFadeScaleConst;

	float GISaturation;
	float GIDistanceCompensation;
	float GICompensationMaxDist;
	// (contact AO) World-space search radius of the contact kernel, in centimetres. Read by
	// contactAo.cs.hlsl only; it took one of this buffer's two spare slots so the layout is
	// unchanged.
	float ContactRadius;

	float AOPower;
	float GIStrength;

	float DepthDisocclusion;
	float NormalDisocclusion;
	uint MaxAccumFrames;

	uint MaxAccumFramesAO;
	float BlurRadius;
	float DistanceNormalisation;
	// (contact AO) Scales the contact kernel's occlusion. Read by contactAo.cs.hlsl only; took
	// this buffer's other spare slot.
	float ContactStrength;
};

SamplerState samplerPointClamp : register(s0);
SamplerState samplerLinearClamp : register(s1);

///////////////////////////////////////////////////////////////////////////////

// first person z
#define FP_Z (18.0)

#define ISNAN(x) (!(x < 0.f || x > 0.f || x == 0.f))
float filterNaN(float v)
{
	return ISNAN(v) ? 0 : v;
}
float2 filterNaN(float2 v) { return float2(filterNaN(v.x), filterNaN(v.y)); }
float3 filterNaN(float3 v) { return float3(filterNaN(v.x), filterNaN(v.y), filterNaN(v.z)); }
float4 filterNaN(float4 v) { return float4(filterNaN(v.x), filterNaN(v.y), filterNaN(v.z), filterNaN(v.w)); }

// (guard N1) Spelled as an explicit exponent test rather than `isinf(v)`, for the same reason
// ISNAN above is spelled out rather than calling isnan(): fxc is entitled to assume its inputs
// are finite unless /Gis is passed, and Util::CompileShader passes
// D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 (== /Ges /O3) -- not IEEE
// strictness. On the Screen Space Ray Tracing side, using isinf() made fxc emit "warning X3577:
// value cannot be infinity, isinf() may not be necessary. /Gis may force isinf() to be
// performed" the moment the equivalent helpers acquired their first caller, i.e. the compiler
// was telling us it reserved the right to delete the guard. Every filterInf() below therefore
// had to be assumed dead. The bit test is unfoldable and lowers to the same three instructions
// fxc generated for isinf() anyway (and 0x7fffffff / ieq 0x7f800000 / movc), so this costs
// nothing and removes the assumption.
//
// Independent copy of the helper in
// features/Screen Space Ray Tracing/Shaders/ScreenSpaceRayTracing/ssrt_common.hlsli (guards
// G1/G2/G9). Duplicated on purpose: features must not #include across feature directories,
// since each ships as its own package and either may be absent at runtime.
float filterInf(float v) { return ((asuint(v) & 0x7FFFFFFFu) == 0x7F800000u) ? 0 : v; }
float2 filterInf(float2 v) { return float2(filterInf(v.x), filterInf(v.y)); }
float3 filterInf(float3 v) { return float3(filterInf(v.x), filterInf(v.y), filterInf(v.z)); }
float4 filterInf(float4 v) { return float4(filterInf(v.x), filterInf(v.y), filterInf(v.z), filterInf(v.w)); }

// (guard N4) "Is this value usable arithmetic?", i.e. neither NaN nor +-Inf.
//
// Deliberately *not* `isfinite()`. `isfinite` is specified as `!isnan(v) && !isinf(v)` and
// therefore inherits both of the assumptions fxc is allowed to make about its inputs without
// /Gis (see the note on filterInf above), so the compiler is entitled to fold the whole guard
// away.
//
// A single exponent test covers both cases at once and cannot be folded at all: an IEEE-754
// binary32 value is non-finite exactly when its 8 exponent bits are all set -- mantissa 0 gives
// +-Inf and any other mantissa gives a NaN -- so the sign and mantissa need not be looked at.
// Two integer ops per component, against the seven the `!ISNAN(v) && exponentTest` spelling
// costs, which matters because this runs once per history tap in radianceDisocc.cs.hlsl, i.e.
// four times per lane.
//
// Same provenance as filterInf above (ssrt_common.hlsli guard G4), copied rather than shared.
bool isFiniteSafe(float v) { return (asuint(v) & 0x7F800000u) != 0x7F800000u; }
bool isFiniteSafe(float2 v) { return isFiniteSafe(v.x) && isFiniteSafe(v.y); }
bool isFiniteSafe(float3 v) { return isFiniteSafe(v.x) && isFiniteSafe(v.y) && isFiniteSafe(v.z); }
bool isFiniteSafe(float4 v) { return isFiniteSafe(v.x) && isFiniteSafe(v.y) && isFiniteSafe(v.z) && isFiniteSafe(v.w); }

// (guard N2) Ceiling on any single radiance channel entering the temporal chain.
//
// Purpose is overflow containment, not tone mapping. filterNaN / filterInf only catch values
// that have *already* become non-finite, and radianceDisocc.cs.hlsl can manufacture a fresh Inf
// out of perfectly finite inputs: with Linear Lighting disabled, Color::RadianceToLinear is
// GammaToLinear, i.e. pow(x, 2.2). The source is the forward colour target (R11G11B10, up to
// ~65024) scaled by IL Source Brightness (UI range 0..6), and the destination texRadiance is
// also R11G11B10, whose largest representable value is ~65024. So any input above
// 65024^(1/2.2) ~= 320 exponentiates past the target's range and is stored as +Inf -- reachable
// on a sun-facing snow specular or an emissive, not just on corrupt data. From there the Inf
// spreads through the radiance mip prefilter and into the IL history, where it is permanent.
// Clamping the post-conversion value bounds the whole chain, because the temporal EMA can only
// ever move *towards* the sample it is handed.
//
// Why this cannot touch healthy imagery: after RadianceToLinear these are linear scene radiances
// in the same units kMAIN carries, where 1.0 is a diffuse white surface under full sunlight. The
// brightest legitimate IL *source* -- a torch flame, a sunlit snow highlight -- lands in the low
// tens. 128 is therefore ~10x above the top of the real signal range: a tripwire, and a pixel
// that hits it was already broken. Same value and same reasoning as SSRT_MAX_RADIANCE in
// features/Screen Space Ray Tracing/Shaders/ScreenSpaceRayTracing/ssrt_common.hlsli, kept
// numerically identical so the two features cannot disagree about what "too bright" means.
#define SSGI_MAX_RADIANCE 128.0f

// (guard N3/N5) Magnitude ceiling on the IL / specular channels leaving gi.cs.hlsl.
//
// Every one of those targets is half precision (texIlY and texGiSpecular are
// R16G16B16A16_FLOAT, texIlCoCg is R16G16_FLOAT; see ScreenSpaceGI::SetupResources), so the
// largest value they can represent is 65504 and anything above it is stored as +-Inf. Once one
// lands in the IL history it is permanent -- lerp(Inf, curr, f) is Inf for every finite f -- and
// blur.cs.hlsl then spreads it to eight more texels per frame.
//
// 16384 == 2^14 leaves two binades of headroom under 65504, which is what the *consumers* need:
// the value is read back as half, then scaled by AO Power / IL Source Brightness and multiplied
// into the ambient term in DeferredCompositeCS, and any of those products must still be
// representable. The bound is self-maintaining above this point, because everything downstream
// of the clamp is a convex combination: the temporal EMA factor rcp(accumFrames) is in (0, 1],
// and the blur writes ySum/wSum with all weights positive, so neither can exceed the largest
// input it was given.
//
// Why this cannot touch healthy imagery: with radiance capped at SSGI_MAX_RADIANCE the SH2
// luminance coefficients integrate to the same order of magnitude as their input times an O(1)
// solid-angle factor, i.e. the low hundreds in the worst case. 16384 is two orders of magnitude
// above that. AO is exempt: it lives in an R8_UNORM target, which cannot carry a non-finite or
// out-of-range value in the first place.
#define SSGI_MAX_OUTPUT 16384.0f

// (directional env) Bent-normal + aperture codec for the R8G8B8A8_UNORM bent-normal surface.
//
// Layout: RG = octahedral-encoded world-space bent normal mapped to [0, 1], B = aperture
// (fraction of the hemisphere the occlusion sees as open, 0 = sealed, 1 = fully open),
// A = spare (written 1, never read).
//
// Octahedral rather than the GBuffer codec so this file stays self-contained: the decoder is
// duplicated in DeferredCompositeCS.hlsl (package side), which must not #include across a
// feature directory that may be absent at runtime -- same reasoning as filterInf above. Any
// change here must be mirrored there.
//
// The encoding is DIRECTION data: no consumer may interpolate the RG channels directly. Every
// filter (temporal EMA in gi.cs.hlsl, reprojection taps in radianceDisocc.cs.hlsl, the spatial
// blur, the upsample) decodes to a vector, blends in the vector domain, renormalises, and
// re-encodes. Interpolating across the octahedron's fold lines yields directions unrelated to
// either endpoint.
float2 SSGI_OctWrap(float2 v)
{
	return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? float2(1, 1) : float2(-1, -1));
}

float4 SSGI_EncodeBentNormal(float3 dir, float aperture)
{
	float3 n = dir / max(abs(dir.x) + abs(dir.y) + abs(dir.z), 1e-6);
	float2 oct = n.z >= 0.0 ? n.xy : SSGI_OctWrap(n.xy);
	return float4(oct * 0.5 + 0.5, saturate(aperture), 1.0);
}

void SSGI_DecodeBentNormal(float4 enc, out float3 o_dir, out float o_aperture)
{
	float2 f = enc.xy * 2.0 - 1.0;
	float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
	float t = saturate(-n.z);
	n.xy += n.xy >= 0.0 ? float2(-t, -t) : float2(t, t);
	// normalize is safe: |n| >= 1/sqrt(2) for every representable input, including the all-zero
	// texel a cleared surface produces (which decodes to (0, 0, -1)).
	o_dir = normalize(n);
	o_aperture = enc.z;
}

// screenPos - normalised position in FrameDim, one eye only
// uv - normalised position in FrameDim, both eye
// texCoord - texture coordinate

#ifdef HALF_RES
#	define RES_MIP 1
#	define READ_DEPTH(tex, px) tex.Load(int3(px, RES_MIP))
#	define FULLRES_LOAD(tex, px, texCoord, samp) tex.SampleLevel(samp, texCoord, 0)
#	define OUT_FRAME_DIM (FrameDim * 0.5)
#	define RCP_OUT_FRAME_DIM (RcpFrameDim * 2)
#	define OUT_FRAME_SCALE (frameScale * 0.5)
#elif defined(QUARTER_RES)
#	define RES_MIP 2
#	define READ_DEPTH(tex, px) tex.Load(int3(px, RES_MIP))
#	define FULLRES_LOAD(tex, px, texCoord, samp) tex.SampleLevel(samp, texCoord, 0)
#	define OUT_FRAME_DIM (FrameDim * 0.25)
#	define RCP_OUT_FRAME_DIM (RcpFrameDim * 4)
#	define OUT_FRAME_SCALE (frameScale * 0.25)
#else
#	define RES_MIP 0
#	define READ_DEPTH(tex, px) tex[px]
#	define FULLRES_LOAD(tex, px, texCoord, samp) tex[px]
#	define OUT_FRAME_DIM FrameDim
#	define RCP_OUT_FRAME_DIM RcpFrameDim
#	define OUT_FRAME_SCALE frameScale
#endif

///////////////////////////////////////////////////////////////////////////////

// Inputs are screen XY and viewspace depth, output is viewspace position
float3 ScreenToViewPosition(const float2 screenPos, const float viewspaceDepth, const uint eyeIndex)
{
	const float2 _mul = eyeIndex == 0 ? NDCToViewMul.xy : NDCToViewMul.zw;
	const float2 _add = eyeIndex == 0 ? NDCToViewAdd.xy : NDCToViewAdd.zw;

	float3 ret;
	ret.xy = (_mul * screenPos.xy + _add) * viewspaceDepth;
	ret.z = viewspaceDepth;
	return ret;
}

float ScreenToViewDepth(const float screenDepth)
{
	return (SharedData::CameraData.w / (-screenDepth * SharedData::CameraData.z + SharedData::CameraData.x));
}

float3 ViewToWorldPosition(const float3 pos, const float4x4 invView)
{
	float4 worldpos = mul(invView, float4(pos, 1));
	return worldpos.xyz / worldpos.w;
}

float3 ViewToWorldVector(const float3 vec, const float4x4 invView)
{
	return mul((float3x3)invView, vec);
}

///////////////////////////////////////////////////////////////////////////////

// "Efficiently building a matrix to rotate one vector to another"
// http://cs.brown.edu/research/pubs/pdfs/1999/Moller-1999-EBA.pdf / https://dl.acm.org/doi/10.1080/10867651.1999.10487509
// (using https://github.com/assimp/assimp/blob/master/include/assimp/matrix3x3.inl#L275 as a code reference as it seems to be best)
float3x3 RotFromToMatrix(float3 from, float3 to)
{
	const float e = dot(from, to);
	const float f = abs(e);  //(e < 0)? -e:e;

	// WARNING: This has not been tested/worked through, especially not for 16bit floats; seems to work in our special use case (from is always {0, 0, -1}) but wouldn't use it in general
	if (f > float(1.0 - 0.0003))
		return float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);

	const float3 v = cross(from, to);
	/* ... use this hand optimized version (9 mults less) */
	const float h = (1.0) / (1.0 + e); /* optimization by Gottfried Chen */
	const float hvx = h * v.x;
	const float hvz = h * v.z;
	const float hvxy = hvx * v.y;
	const float hvxz = hvx * v.z;
	const float hvyz = hvz * v.y;

	float3x3 mtx;
	mtx[0][0] = e + hvx * v.x;
	mtx[0][1] = hvxy - v.z;
	mtx[0][2] = hvxz + v.y;

	mtx[1][0] = hvxy + v.z;
	mtx[1][1] = e + h * v.y * v.y;
	mtx[1][2] = hvyz - v.x;

	mtx[2][0] = hvxz - v.y;
	mtx[2][1] = hvyz + v.x;
	mtx[2][2] = e + hvz * v.z;

	return mtx;
}

///////////////////////////////////////////////////////////////////////////////

// credit: Olivier Therrien
float specularLobeHalfAngle(float roughness)
{
	float roughness2 = roughness * roughness;
	return clamp(4.1679 * roughness2 * roughness2 - 9.0127 * roughness2 * roughness + 4.6161 * roughness2 + 1.7048 * roughness + 0.1, 0, Math::HALF_PI);
}

// https://www.gdcvault.com/play/1026701/Fast-Denoising-With-Self-Stabilizing
float3 getSpecularDominantDirection(float3 N, float3 V, float roughness)
{
	float f = (1 - roughness) * (sqrt(1 - roughness) + roughness);
	float3 R = reflect(-V, N);
	float3 D = lerp(N, R, f);

	return normalize(D);
}

#endif