#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

#define Pow2(x) ((x) * (x))
#define SSRT_FLOAT_MAX	3.402823466e+38

// The engine depth buffer is *not* inverted: 0 = near plane, 1 = far plane.
// (Confirmed by SharedData::GetScreenDepth, SharedData.hlsli:403 --
// `CameraData.w / (-depth * CameraData.z + CameraData.x)` grows with depth.)
// Anything at (very nearly) 1.0 is sky / background: no deferred geometry was
// rasterised there, so the normal-roughness and albedo G-buffers hold cleared
// garbage and nothing we compute for those pixels can reach the frame.
// Threshold matches the existing far-plane guard inside the Hi-Z traversal.
#define SSRT_FAR_PLANE_EPSILON 1.0e-6f
#define SSRT_IS_FAR_PLANE(z) ((z) >= 1.0f - SSRT_FAR_PLANE_EPSILON)

Texture2D<unorm float3> NormalRoughnessTexture : register(t2);

SamplerState LinearSampler : register(s0);

// Brian Karis, Epic Games "Real Shading in Unreal Engine 4"
float4 ImportanceSampleGGX(float2 E, float a2)
{
	float Phi = 2 * Math::PI * E.x;
	float CosTheta = sqrt( (1 - E.y) / ( 1 + (a2 - 1) * E.y ) );
	float SinTheta = sqrt( 1 - CosTheta * CosTheta );

	float3 H;
	H.x = SinTheta * cos( Phi );
	H.y = SinTheta * sin( Phi );
	H.z = CosTheta;
	
	float d = ( CosTheta * a2 - CosTheta ) * CosTheta + 1;
	float D = a2 / ( Math::PI*d*d );
	float PDF = D * CosTheta;

	return float4( H, PDF );
}

float VisibleGGXPDF_aniso(float3 V, float3 H, float2 Alpha, bool bLimitVDNFToReflection = true)
{
	float NoV = V.z;
	float NoH = H.z;
	float VoH = dot(V, H);
	float a2 = Alpha.x * Alpha.y;
	float3 Hs = float3(Alpha.y * H.x, Alpha.x * H.y, a2 * NoH);
	float S = dot(Hs, Hs);
	float D = (1.0f / Math::PI) * a2 * Pow2(a2 / S);
	float LenV = length(float3(V.x * Alpha.x, V.y * Alpha.y, NoV));
	float k = 1.0;
	if (bLimitVDNFToReflection)
	{
		float a = saturate(min(Alpha.x, Alpha.y));
		float s = 1.0f + length(V.xy);
		float ka2 = a * a, s2 = s * s;
		k = (s2 - ka2 * s2) / (s2 + ka2 * V.z * V.z); // Eq. 5
	}
	float Pdf = (2 * D * VoH) / (k * NoV + LenV);
	return Pdf;
}

// PDF = G_SmithV * VoH * D / NoV / (4 * VoH)
// PDF = G_SmithV * D / (4 * NoV)
float4 ImportanceSampleVisibleGGX(float2 E, float2 Alpha, float3 V, bool bLimitVDNFToReflection = true)
{
	// stretch
	float3 Vh = normalize(float3(Alpha * V.xy, V.z));

	// "Sampling Visible GGX Normals with Spherical Caps"
	// Jonathan Dupuy & Anis Benyoub - High Performance Graphics 2023
	float Phi = (2 * Math::PI) * E.x;
	float k = 1.0;
	if (bLimitVDNFToReflection)
	{
		// If we know we will be reflecting the view vector around the sampled micronormal, we can
		// tweak the range a bit more to eliminate some of the vectors that will point below the horizon
		float a = saturate(min(Alpha.x, Alpha.y));
		float s = 1.0 + length(V.xy);
		float a2 = a * a, s2 = s * s;
		k = (s2 - a2 * s2) / (s2 + a2 * V.z * V.z);
	}
	float Z = lerp(1.0, -k * Vh.z, E.y);
	float SinTheta = sqrt(saturate(1 - Z * Z));
	float X = SinTheta * cos(Phi);
	float Y = SinTheta * sin(Phi);
	float3 H = float3(X, Y, Z) + Vh;

	// unstretch
	H = normalize(float3(Alpha * H.xy, max(0.0, H.z)));

	return float4(H, VisibleGGXPDF_aniso(V, H, Alpha));
}

float3 ConcentricDiskSamplingHelper(float2 E)
{
	// Rescale input from [0,1) to (-1,1). This ensures the output radius is in [0,1)
	float2 p = 2 * E - 0.99999994;
	float2 a = abs(p);
	float Lo = min(a.x, a.y);
	float Hi = max(a.x, a.y);
	float Epsilon = 5.42101086243e-20; // 2^-64 (this avoids 0/0 without changing the rest of the mapping)
	float Phi = (Math::PI / 4) * (Lo / (Hi + Epsilon) + 2 * float(a.y >= a.x));
	float Radius = Hi;
	// copy sign bits from p
	const uint SignMask = 0x80000000;
	float2 Disk = asfloat((asuint(float2(cos(Phi), sin(Phi))) & ~SignMask) | (asuint(p) & SignMask));
	// return point on the circle as well as the radius
	return float3(Disk, Radius);
}

float4 CosineSampleHemisphere( float2 E )
{
	float Phi = 2 * Math::PI * E.x;
	float CosTheta = sqrt(E.y);
	float SinTheta = sqrt(1 - CosTheta * CosTheta);

	float3 H;
	H.x = SinTheta * cos(Phi);
	H.y = SinTheta * sin(Phi);
	H.z = CosTheta;

	float PDF = CosTheta * (1.0 / Math::PI);

	return float4(H, PDF);
}

float4 CosineSampleHemisphereConcentric(float2 E)
{
	float3 Result = ConcentricDiskSamplingHelper(E);
	float SinTheta = Result.z;
	float CosTheta = sqrt(1 - SinTheta * SinTheta);
	return float4(Result.xy * SinTheta, CosTheta, CosTheta * (1.0 / Math::PI));
}

void GetNormalRoughness(uint2 dtid, out float3 normal, out float roughness)
{
    float3 normalGlossiness = NormalRoughnessTexture[dtid];
    // Normal is in view space
    normal = GBuffer::DecodeNormal(normalGlossiness.xy);
    roughness = 1.0f - normalGlossiness.z;
}

void GetNormalRoughness(Texture2D<float4> NormalRoughness, uint2 dtid, out float3 normal, out float roughness)
{
    float3 normalGlossiness = NormalRoughness[dtid].xyz;
    // Normal is in view space
    normal = GBuffer::DecodeNormal(normalGlossiness.xy);
    roughness = 1.0f - normalGlossiness.z;
}

void GetNormalRoughnessUV(float2 uv, out float3 normal, out float roughness)
{
    float3 normalGlossiness = NormalRoughnessTexture.SampleLevel(LinearSampler, uv, 0);
    // Normal is in view space
    normal = GBuffer::DecodeNormal(normalGlossiness.xy);
    roughness = 1.0f - normalGlossiness.z;
}

// [ Duff et al. 2017, "Building an Orthonormal Basis, Revisited" ]
float3x3 GetTangentBasis( float3 TangentZ )
{
	const float Sign = TangentZ.z >= 0 ? 1 : -1;
	const float a = -rcp( Sign + TangentZ.z );
	const float b = TangentZ.x * TangentZ.y * a;
	
	float3 TangentX = { 1 + Sign * a * Pow2( TangentZ.x ), Sign * b, -Sign * TangentZ.x };
	float3 TangentY = { b,  Sign + a * Pow2( TangentZ.y ), -TangentZ.y };

	return float3x3( TangentX, TangentY, TangentZ );
}

float2 Hammersley16( uint Index, uint NumSamples, uint2 Random )
{
	float E1 = frac( (float)Index / NumSamples + float( Random.x ) * (1.0 / 65536.0) );
	float E2 = float( ( reversebits(Index) >> 16 ) ^ Random.y ) * (1.0 / 65536.0);
	return float2( E1, E2 );
}

static const int2 kStackowiakSampleSet4[15] = { int2(0, 1), int2(-2, 1), int2(2, -3), int2(-3, 0), int2(1, 2), int2(-1, -2), int2(3, 0), int2(-3, 3), int2(0, -3), int2(-1, -1), int2(2, 1), int2(-2, -2), int2(1, 0), int2(0, 2), int2(3, -1) };

float2 GetMotionVector(float sceneDepth, float2 screenUV, float4x4 matrix_LastViewProj, float4x4 matrix_ViewProj)
{
    float4 positionWS = float4(2 * float2(screenUV.x, -screenUV.y + 1) - 1, sceneDepth, 1);

    float4 curClipPos = mul(matrix_ViewProj, positionWS);
    float4 lastClipPos = mul(matrix_LastViewProj, positionWS);

    float2 CurNDC = curClipPos.xy / curClipPos.w;
    float2 LastNDC = lastClipPos.xy / lastClipPos.w;

    float2 CurUV = CurNDC.xy * float2(0.5f, -0.5f) + 0.5;
    float2 LastUV = LastNDC.xy * float2(0.5f, -0.5f) + 0.5;
    
    return CurUV - LastUV;
}

uint3 Rand3DPCG16(int3 p)
{
	// taking a signed int then reinterpreting as unsigned gives good behavior for negatives
	uint3 v = uint3(p);

	// Linear congruential step. These LCG constants are from Numerical Recipies
	// For additional #'s, PCG would do multiple LCG steps and scramble each on output
	// So v here is the RNG state
	v = v * 1664525u + 1013904223u;

	// PCG uses xorshift for the final shuffle, but it is expensive (and cheap
	// versions of xorshift have visible artifacts). Instead, use simple MAD Feistel steps
	//
	// Feistel ciphers divide the state into separate parts (usually by bits)
	// then apply a series of permutation steps one part at a time. The permutations
	// use a reversible operation (usually ^) to part being updated with the result of
	// a permutation function on the other parts and the key.
	//
	// In this case, I'm using v.x, v.y and v.z as the parts, using + instead of ^ for
	// the combination function, and just multiplying the other two parts (no key) for 
	// the permutation function.
	//
	// That gives a simple mad per round.
	v.x += v.y*v.z;
	v.y += v.z*v.x;
	v.z += v.x*v.y;
	v.x += v.y*v.z;
	v.y += v.z*v.x;
	v.z += v.x*v.y;

	// only top 16 bits are well shuffled
	return v >> 16u;
}

float3 SampleGGXVNDF(float3 Ve, float alpha_x, float alpha_y, float U1, float U2) {
    // Input Ve: view direction
    // Input alpha_x, alpha_y: roughness parameters
    // Input U1, U2: uniform random numbers
    // Output Ne: normal sampled with PDF D_Ve(Ne) = G1(Ve) * max(0, dot(Ve, Ne)) * D(Ne) / Ve.z
    //
    //
    // Section 3.2: transforming the view direction to the hemisphere configuration
    float3 Vh = normalize(float3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis (with special case if cross product is zero)
    float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0 ? float3(-Vh.y, Vh.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 T2 = cross(Vh, T1);
    // Section 4.2: parameterization of the projected area
    float       r = sqrt(U1);
    const float M_PI = 3.14159265358979f;
    float       phi = 2.0 * M_PI * U2;
    float       t1 = r * cos(phi);
    float       t2 = r * sin(phi);
    float       s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
    // Section 4.3: reprojection onto hemisphere
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    // Section 3.4: transforming the normal back to the ellipsoid configuration
    float3 Ne = normalize(float3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0, Nh.z)));
    return Ne;
}

void ReprojectHit(Texture2D MotionTexture, float3 hitUVz, uint eyeIndex, out float2 outPrevUV)
{
	// Camera motion for pixel (in ScreenPos space).
	float2 thisScreen = (hitUVz.xy - 0.5f) * float2(2.0f, -2.0f);
	float4 thisClip = float4(thisScreen, hitUVz.z, 1);
    float4 thisView = mul(FrameBuffer::CameraProjUnjitteredInverse[eyeIndex], thisClip);
    thisView.xyz = thisView.xyz / thisView.w;
    float4 thisWorld = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(thisView.xyz, 1.0f));
    thisWorld.xyz = thisWorld.xyz / thisWorld.w;
	float4 prevClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(thisWorld.xyz, 1.0f));
	float2 prevScreen = prevClip.xy / prevClip.w;

	// (defect D3) Point load, not a filtered sample. A bilinear tap on the motion-vector
	// target averages the motion of two different surfaces wherever its 2x2 footprint
	// straddles a silhouette, and the average points at neither of them: the history lookup
	// lands *between* the two surfaces, which is the one place no acceptance test can match,
	// so the pixel is rejected however permissive the criterion is. Skyrim exteriors are
	// almost entirely silhouette -- alpha-tested grass and leaf cards put an edge within a
	// texel or two of nearly every pixel -- so this was never an edge case, and the same
	// defect is on record in ScreenSpaceGI as F5.
	//
	// The integer coordinate is exact rather than an approximation of the sample position: the
	// caller's uv is (DTid + 0.5) * BufferDim.zw * DynamicResolutionParams2.xy, and
	// DynamicResolutionParams2.xy is the reciprocal of DynamicResolutionParams1.xy, so the
	// product below is DTid + 0.5 and truncates to DTid. The clamp is the dynamic-resolution
	// sub-rect, matching the clamp SampleLevel's addressing used to provide.
	const int2 motionMax = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy) - 1;
	const int2 motionPixel = clamp(int2(hitUVz.xy * FrameBuffer::DynamicResolutionParams1.xy * SharedData::BufferDim.xy),
								   int2(0, 0), motionMax);
	float2 velocity = MotionTexture[motionPixel].xy;

	prevScreen = thisClip.xy + velocity * float2(2.f, -2.f);

	float2 prevUV = prevScreen.xy * float2(0.5f, -0.5f) + 0.5f;
	
	outPrevUV = prevUV;
}

float GetSpecularOcclusionFromAmbientOcclusion(float NdotV, float ao, float roughness) {
    return saturate(pow(abs(NdotV + ao), exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

// by Profjack
#define ISNAN(x) (!(x < 0.f || x > 0.f || x == 0.f))
float filterNaN(float v)
{
	return ISNAN(v) ? 0 : v;
}
float2 filterNaN(float2 v) { return float2(filterNaN(v.x), filterNaN(v.y)); }
float3 filterNaN(float3 v) { return float3(filterNaN(v.x), filterNaN(v.y), filterNaN(v.z)); }
float4 filterNaN(float4 v) { return float4(filterNaN(v.x), filterNaN(v.y), filterNaN(v.z), filterNaN(v.w)); }

// (guard G1/G2/G9) Spelled as an explicit exponent test rather than `isinf(v)`, for the same
// reason ISNAN above is spelled out rather than calling isnan(): fxc is entitled to assume
// its inputs are finite unless /Gis is passed, and Util::CompileShader passes
// D3DCOMPILE_ENABLE_STRICTNESS | OPTIMIZATION_LEVEL3 -- not IEEE strictness. Using isinf()
// here made fxc emit "warning X3577: value cannot be infinity, isinf() may not be
// necessary. /Gis may force isinf() to be performed" the moment these helpers acquired
// their first caller, i.e. the compiler was telling us it reserved the right to delete the
// guard. The bit test is unfoldable and lowers to the same three instructions fxc generated
// for isinf() anyway (and 0x7fffffff / ieq 0x7f800000 / movc), so this costs nothing and
// removes the assumption.
float filterInf(float v) { return ((asuint(v) & 0x7FFFFFFFu) == 0x7F800000u) ? 0 : v; }
float2 filterInf(float2 v) { return float2(filterInf(v.x), filterInf(v.y)); }
float3 filterInf(float3 v) { return float3(filterInf(v.x), filterInf(v.y), filterInf(v.z)); }
float4 filterInf(float4 v) { return float4(filterInf(v.x), filterInf(v.y), filterInf(v.z), filterInf(v.w)); }

// (guard G4) "Is this value usable arithmetic?", i.e. neither NaN nor +-Inf.
//
// Deliberately *not* `isfinite()`. `isfinite` is specified as `!isnan(v) && !isinf(v)` and
// therefore inherits both of the assumptions fxc is allowed to make about its inputs
// without /Gis (see the note on filterInf above), so the compiler is entitled to fold the
// whole guard away.
//
// A single exponent test covers both cases at once and cannot be folded at all: an IEEE-754
// binary32 value is non-finite exactly when its 8 exponent bits are all set -- mantissa 0
// gives +-Inf and any other mantissa gives a NaN -- so the sign and mantissa need not be
// looked at. Two integer ops per component, against the seven the
// `!ISNAN(v) && exponentTest` spelling costs, which matters because this runs up to 13
// times per lane in the temporal pass's history search.
bool isFiniteSafe(float v) { return (asuint(v) & 0x7F800000u) != 0x7F800000u; }
bool isFiniteSafe(float2 v) { return isFiniteSafe(v.x) && isFiniteSafe(v.y); }
bool isFiniteSafe(float3 v) { return isFiniteSafe(v.x) && isFiniteSafe(v.y) && isFiniteSafe(v.z); }
bool isFiniteSafe(float4 v) { return isFiniteSafe(v.x) && isFiniteSafe(v.y) && isFiniteSafe(v.z) && isFiniteSafe(v.w); }

// (guards G2 / G9) Ceiling on any single radiance channel leaving the ray march and
// entering the frame.
//
// Purpose is overflow containment, not tone mapping: filterNaN / filterInf only catch
// values that have *already* become non-finite, and the two places that can manufacture a
// fresh Inf out of finite inputs are (a) squaring for the luminance second moment (see
// SSRT_MOMENT_LUMINANCE_MAX in ssrt_temporal.hlsl) and (b) the half-float moments target,
// whose largest representable value is 65504. A radiance of 128 squares to 16384, an
// order of magnitude of headroom under that, and the temporal EMA can only ever move
// *towards* the sample it is handed, so bounding the input bounds the whole chain.
//
// Why this cannot touch healthy imagery: the values here are linear scene radiance in the
// same units kMAIN carries, where 1.0 is a diffuse white surface under full sunlight. The
// brightest legitimate GI *source* in the game -- a torch flame, a sun-facing snow
// specular -- lands in the low tens after DiffuseMult, and anything above that is either a
// firefly (which the S1 clamp handles on statistical grounds, two orders of magnitude
// lower) or a poisoned texel. 128 is therefore ~10x above the top of the real signal
// range: it is a tripwire, and a pixel that hits it was already broken.
#define SSRT_MAX_RADIANCE 128.0f

// (batch 1, item 2) The encoding of the per-pixel diffuse hit distance, shared by the writer
// (ssrt_raymarch.hlsl) and the reader (ssrt_spatial.hlsl) so the pair cannot drift.
//
// WHAT THE SURFACE CARRIES, AND WHY IT IS NOT A DISTANCE
//
// The quantity the a-trous kernel needs is not "how far did the ray go" in game units, because
// game units say nothing on their own: twenty units is a contact bounce at arm's length from the
// camera and a whole room at four thousand. What the kernel needs is the ray length measured
// against the *screen*: how many render texels of separation does it take before two pixels stop
// being lit by the same thing.
//
// Deriving that takes one step. Diffuse irradiance arriving from a source at distance L varies
// over a world-space length scale of order L -- move the receiver much less than L and the solid
// angle the source subtends barely changes; move it much more and the source has gone. Two
// pixels d texels apart are separated in world space by d * texelWorldSize, so
//     correlationLengthInTexels = L / texelWorldSize,
//     texelWorldSize = viewZ * 2 / (P00 * renderWidth).
// texelWorldSize is taken from the projection matrix and the render extent rather than from an
// assumed field of view -- the same construction ssrt_temporal.hlsl's plane tolerance uses and
// ScreenSpaceGI builds as NDCToViewMul / OUT_FRAME_DIM -- so the result is dynamic-resolution
// proof, FOV independent and per-eye correct under VR.
//
// WHY THE STORED VALUE IS A RECIPROCAL AND NOT THAT NUMBER
//
// The correlation length in texels spans four orders of magnitude across a Skyrim exterior: a
// 5-unit contact bounce with the camera 4000 units out is a single texel, and a 500-unit bounce
// with the camera at 100 is four thousand. An 8-bit *linear* encoding cannot hold that. The
// first version of this constant tried, with a linear scale of 16 texels, and the CPU harness in
// the branch's scratchpad showed the consequence immediately: at 1920 px and a 100 degree
// horizontal FOV one texel is 1.24 units at viewZ 1000, so a 20-unit contact hit is 16 texels
// and saturated the encoding at 1.0 -- "as distant as the encoding can say" -- i.e. the
// mechanism was inert over most of the screen and only fired at extreme range. That is the kind
// of error a plausible derivation hides and a table of numbers does not.
//
// The stored value is therefore the bounded, well-conditioned reciprocal
//     u = t / (t + SSRT_HITT_REF_TEXELS),      t = correlation length in texels,
// which is exactly the shape REBLUR uses (`hitDist / (hitDist + frustumSize)`), and it has the
// two endpoint properties the mechanism needs:
//   * t = 0 gives u = 0, the narrowest kernel;
//   * "no screen-space hit at all" is written as the literal 1.0, which the consumer maps to
//     beta = 0 and therefore to the unmodified kernel, bit for bit.
// SSRT_HITT_REF_TEXELS is the correlation length at which u reads 0.5. 16 texels puts the
// encoding's most sensitive band across the range where the interesting answers live: at viewZ
// 1000 in the example above, u is 0.20 for a 5-unit bounce, 0.50 for 20 units, 0.83 for 100 and
// 0.96 for 500.
//
// Being R8_UNORM means one byte per render texel (~8 MB of a 4K allocation, against ~33 MB for
// each of the eight RGBA16F surfaces this feature already holds) and that a read is a [0, 1]
// value by construction, so no consumer needs a finiteness guard -- the same argument the
// confidence surfaces are built on. The 1/255 quantum is a smooth perturbation of a weight
// rather than a threshold crossing, so it cannot produce a visible boundary; near u = 1 it
// corresponds to tens of texels of correlation length, which is exactly where the consumer stops
// caring.
#define SSRT_HITT_REF_TEXELS 16.0f

// (batch 1, item 2) How many times its own world-space footprint the light's correlation length
// must exceed before the a-trous kernel is trusted at full width.
//
// This is the one free constant in the mechanism, and it is what turns the stored correlation
// length into a per-iteration confidence. The kernel at iteration i reaches
// SSRT_SPATIAL_KERNEL_RADIUS * stride texels, i.e. that many texelWorldSize in world space, and
// the question is how large t has to be relative to that reach before averaging over it is
// harmless:
//     f = t / (t + SSRT_HITT_KERNEL_REACH * hardRadius),
// so f = 0.5 when the correlation length is REACH kernel radii.
//
// 8 is an error budget rather than a taste: the irradiance varies by roughly (reach / t) of
// itself across the filter footprint, so demanding t >= 8 * reach for full confidence is
// demanding that the filter's outermost tap disagrees with the centre by under about an eighth
// of the value. At the default AtrousIterations 2 and the diffuse kernel radius 2 that puts
// f = 0.5 at 16 texels for the stride-1 iteration and at 32 for the stride-2 one -- the wider
// iteration is judged more strictly, which is the correct direction, since it is the wide
// iterations that reach across a contact feature.
//
// It is deliberately not a user setting. HitRadiusStrength already exposes the useful knob (how
// hard to act on the answer); a second one controlling where the answer's midpoint sits would
// make the pair under-determined.
#define SSRT_HITT_KERNEL_REACH 8.0f

// (audit #12) Tolerated *relative* linear-depth change per texel of tap distance.
//
// Derivation: for a surface at view depth z the per-pixel depth gradient is
//   dz/dpixel = z * pixelAngularSize * |slope|,
// so the relative change per texel, (dz/dpixel)/z, is pixelAngularSize * |slope| and is
// independent of distance. At 1920 px across a ~90 deg horizontal FOV
// pixelAngularSize ~= 1e-3 rad, so a face-on surface gives ~1e-3 per texel and an
// extremely grazing one (~84 deg, slope ~10) about 1e-2 per texel. A real depth
// discontinuity is orders of magnitude larger (0.3 .. 1e2 relative). 0.05 therefore
// leaves same-surface taps at exp(-0.02/0.05 * |k|) ~= 0.67 in the worst grazing case
// and >= 0.96 for typical geometry, while a 1.5x depth step is cut to ~1e-4.
//
// Callers pass phiD = tap distance in texels, which cancels the linear growth of the
// expected same-surface delta with tap distance -- so weightDepth ends up
// approximately (relative gradient per texel) / SSRT_DEPTH_WEIGHT_SCALE, i.e. a pure
// measure of surface slope that no longer changes with the a-trous stride.
#define SSRT_DEPTH_WEIGHT_SCALE 0.05f

float CalculateWeight(float depthCenter, float depthP, float phiD, float3 normalCenter, float3 normalP, float phiN,
					  float luminanceCenter, float luminanceP, float phiL)
{
	float epsilon = 0.0000001;

	// Depth weight.
	// (audit #12) This used to take the difference of *raw NDC* depths and divide it by
	// phiD >= 1. Post-projection depth differences between neighbouring pixels are on
	// the order of 1e-5, and even a terrain-versus-sky step is only ~1e-2, so
	// exp(-weightDepth) was indistinguishable from 1 everywhere: depth edge-stopping
	// did not exist and the filter blurred straight across depth discontinuities.
	// Compare linearised view depths as a relative difference instead, which is
	// scale-free, and rescale phiD by SSRT_DEPTH_WEIGHT_SCALE accordingly.
	float linearCenter = SharedData::GetScreenDepth(depthCenter);
	float linearP = SharedData::GetScreenDepth(depthP);
	float difference = abs(linearCenter - linearP) / max(linearCenter, 1e-5f);
	float weightDepth = (phiD == 0) ? 0.f : difference / max(phiD * SSRT_DEPTH_WEIGHT_SCALE, epsilon);

	// Normal weight
	float weightNormal = pow(max(0.f, dot(normalCenter, normalP)), phiN);

	// Luminance weight
	float weightLuminance = abs(luminanceCenter - luminanceP) / phiL;

	float weight = exp(-weightDepth - weightLuminance) * weightNormal;
	return weight;
}
// ============================================================================================
// (batch 12) SPARSE SAMPLING -- the geometry shared by the sparse ray march and its resolve.
//
// Two sparse modes, each a compile-time permutation of ssrt_raymarch.hlsl and of
// ssrt_sparse_resolve.hlsl, and both of them halve the number of diffuse rays the frame traces:
//
//   SSRT_SPARSE_HALFRES       one ray per 2x2 block of render pixels. The compact grid is
//                             floor(renderExtent / 2), which is exactly the extent
//                             ssrt_depth_downsample.hlsl writes into mip 1 of the Hi-Z pyramid,
//                             so the traversal can start at mip 1 and its cell grid still
//                             coincides with a texel grid.
//   SSRT_SPARSE_CHECKERBOARD  one ray per horizontal pair of render pixels, alternating which
//                             half of the pair every frame. The diffuse grid has floor(width/2)
//                             columns; specular has ceil(width/2) so the odd last column gets
//                             its own sample. Every ray starts at a real full-resolution pixel
//                             and traverses mip 0 without approximating its G-buffer origin.
//
// Resolution is a permutation and not a runtime branch because it changes the traversal's
// finest Hi-Z level, the extent the dispatch clamps against, and how the G-buffer is addressed.
// All variants are always compiled, so switching modes never triggers a recompile.
//
// The denoisers remain full resolution. Diffuse resolves with ssrt_sparse_resolve.hlsl;
// specular checkerboard resolves both its radiance and R32 hit-distance guide with
// ssrt_specular_checker_resolve.hlsl before REBLUR, SVGF, Off or DLSS-RR reads them.
#if defined(SSRT_SPARSE_HALFRES) && defined(SSRT_SPARSE_CHECKERBOARD)
#	error "SSRT_SPARSE_HALFRES and SSRT_SPARSE_CHECKERBOARD are alternatives, not a pair."
#endif
#if defined(SSRT_SPARSE_HALFRES) && defined(SSRT_SPECULAR)
#	error "Half-resolution specular would discard reflection detail; only checkerboard is supported."
#endif

#if defined(SSRT_SPARSE_HALFRES) || defined(SSRT_SPARSE_CHECKERBOARD)
#	define SSRT_SPARSE 1
#endif

// Extent of the render sub-rect, in full-resolution texels. Same truncation
// Util::ConvertToDynamic performs on the CPU side and the same expression the ray march and
// every denoiser pass already compute inline, so the three land on the same texel.
uint2 SSRT_GetRenderExtent()
{
	return uint2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
}

// Extent of the compact grid the current permutation's ray march runs on.
//
// The half-resolution grid is floor(renderExtent / 2) rather than ceil, because floor is exactly
// what ssrt_depth_downsample.hlsl writes into mip 1 of the pyramid (its dispatch is sized
// `max(1, size >> 1)`). With ceil, an odd render width would leave the last compact column
// backed by a mip 1 texel nobody ever wrote, which by the audit #8 convention reads as the far
// plane -- a column of rays that would silently classify as sky. The resolve pass covers the
// full render extent regardless of the compact extent, so an odd render extent's last
// full-resolution column is reconstructed from its clamped neighbour rather than left stale.
uint2 SSRT_GetSparseExtent(uint2 renderExtent)
{
#if defined(SSRT_SPARSE_HALFRES)
	return max(uint2(1, 1), renderExtent >> 1);
#elif defined(SSRT_SPARSE_CHECKERBOARD)
#if defined(SSRT_SPECULAR)
	// Specular has no mip-1 traversal dependency, so include the final unpaired column.
	// It traces its own full-resolution pixel on both phases instead of borrowing a
	// neighbouring reflection. This also keeps a one-pixel-wide DRS sub-rect valid.
	return uint2(max(1u, (renderExtent.x + 1u) >> 1), renderExtent.y);
#else
	return uint2(max(1u, renderExtent.x >> 1), renderExtent.y);
#endif
#else
	return renderExtent;
#endif
}

// (batch 12) Which half of each horizontal pair the checkerboard traces this frame.
//
// Taken from SharedData::FrameCount rather than from a constant-buffer field, deliberately: the
// ray march and the resolve are two dispatches of the same frame reading the same shared
// constant buffer, which is uploaded once per frame, so they cannot disagree -- and adding a
// field to SSRTCB would have changed the reflection chunk of *every* permutation of
// ssrt_raymarch.hlsl, including the ones that must stay bit-identical.
//
// It has to alternate. With a fixed phase, half the pixels are never traced and the frame is
// permanently half-resolution in x; alternating means every pixel is traced every other frame
// and the denoiser's temporal accumulation carries the other one. NRD's own checkerboard
// support has the same requirement, for the same reason.
uint SSRT_SparseCheckerPhase()
{
	return SharedData::FrameCount & 1u;
}

// The full-resolution column that compact texel `compact` traced. The phase flips with y, so
// the traced set is a checkerboard rather than a set of columns.
uint SSRT_SparseCheckerColumn(uint2 compact)
{
#if defined(SSRT_SPECULAR)
	return min((compact.x << 1) | ((compact.y + SSRT_SparseCheckerPhase()) & 1u),
	           SSRT_GetRenderExtent().x - 1u);
#else
	return (compact.x << 1) | ((compact.y + SSRT_SparseCheckerPhase()) & 1u);
#endif
}

// Whether full-resolution pixel `pixel` is the one its own 2x1 pair traced this frame. The
// exact inverse of SSRT_SparseCheckerColumn: pixel is traced iff the column that pair traced
// equals pixel.x.
bool SSRT_SparseCheckerIsTraced(uint2 pixel)
{
#if defined(SSRT_SPECULAR)
	const uint renderWidth = SSRT_GetRenderExtent().x;
	if ((renderWidth & 1u) != 0u && pixel.x == renderWidth - 1u)
		return true;
#endif
	return (pixel.x & 1u) == ((pixel.y + SSRT_SparseCheckerPhase()) & 1u);
}
