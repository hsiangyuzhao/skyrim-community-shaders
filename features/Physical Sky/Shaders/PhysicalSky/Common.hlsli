#ifndef COMMON_PHYS_SKY_HLSLI
#define COMMON_PHYS_SKY_HLSLI

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

#ifndef OMIT_PS_NAMESPACE
namespace PhysSky
{
#endif

#ifdef PS_PREPASS_SAMPLERS
SamplerState SampTr : register(s0);  // in lighting, use shadow
SamplerState SampSv : register(s1);  // in lighting, use color
SamplerState SampNoise : register(s2);
#endif

#ifdef PS_SKY_SAMPLERS
SamplerState SampTr : register(s3);
SamplerState SampSv : register(s4); 
#endif

#ifdef PS_DEFERRED_SAMPLERS
SamplerState SampSv : register(s2); 
#endif

#ifdef PS_PREPASS_RSRCS
Texture2D<float4> TexTrLut : register(t0);
Texture2D<float4> TexMsLut : register(t1);
Texture2D<float4> TexSvLut : register(t2);
Texture3D<float4> TexApLut : register(t3);
#elif defined(PS_DEFERRED_RSRCS)
Texture3D<float4> TexApLut : register(t17);
Texture2D<unorm float> TexApShadow : register(t18);
#else
Texture2D<float4> TexTrLut : register(t61);
Texture2D<float4> TexSvLut : register(t62);
Texture3D<float4> TexApLut : register(t63);
Texture2D<unorm float> TexApShadow : register(t64);
#endif

static const float RCP_PI = 1 / Math::PI;         // PI
static const float AP_MAX_DIST = 40 / 1.428e-5f;  // 40 km
static const uint3 CLOUD_DIM = uint3(512, 512, 64);
static const float3 CLOUD_RANGE = float3(4.f, 4.f, .5f) / 1.428e-5f;
static const float3 CLOUD_RANGE_M = float3(4.f, 4.f, .5f) * 1e3;

#ifndef ISNAN
#	define ISNAN(x) (!(x < 0.f || x > 0.f || x == 0.f))
#endif

// (47b) "Sky Precision": PhysicalSky::ExtFlags bit 10, named here so that SharedData.hlsli stays untouched.
// Off = the 47a2 LUT mappings exactly. On = upstream 35c759765 (Bruneton transmittance LUT, analytic planet
// occlusion, nonuniform midpoint quadrature, separate multiscatter coordinates), dcd3593d1 (sky-view rows
// around the true horizon) and 183ec20b3 (quadratic aerial perspective slices).
static const uint kPhysSkyPrecisionFlag = (1 << 10);
static const float SV_LUT_HEIGHT = 150.0;  // PhysicalSky::kSvLutH

bool SkyPrecision()
{
	return (SharedData::physSkyExtData.Flags & kPhysSkyPrecisionFlag) != 0;
}

// (47b, Sky Precision) Aerial perspective slices concentrated near the camera: slice k at AP_MAX_DIST (k / (n - 1))^2.
float ApSliceDistance(float slice, float sliceCount)
{
	const float t = slice / (sliceCount - 1.0);
	return AP_MAX_DIST * t * t;
}

float ApDepthUv(float dist, uint sliceCount)
{
	const float clampedDist = clamp(dist, 0.0, AP_MAX_DIST);
	const float slice = min(floor(sqrt(clampedDist / AP_MAX_DIST) * (sliceCount - 1.0)), sliceCount - 2.0);
	const float nearDist = ApSliceDistance(slice, sliceCount);
	const float farDist = ApSliceDistance(slice + 1.0, sliceCount);
	// Interpolate in physical distance, not sqrt(distance).
	const float weight = saturate((clampedDist - nearDist) / max(farDist - nearDist, 1e-3));
	return (slice + weight + 0.5) / sliceCount;
}

float Remap(float x, float in_a, float in_b, float out_a, float out_b)
{
	if (in_a == in_b)
		return out_a;
	return lerp(out_a, out_b, saturate((x - in_a) / (in_b - in_a)));
}

float3 PosWs2Planet(float3 posWorld)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	return posWorld - float3(FrameBuffer::CameraPosAdjust[0].xy, data.zBottom - data.rPlanet);
}

// return distance to sphere surface
// url: https://viclw17.github.io/2018/07/16/raytracing-ray-sphere-intersection
float RayIntersectSphere(float3 orig, float3 dir, float3 center, float r)
{
	float3 oc = orig - center;
	float b = dot(oc, dir);
	float c = dot(oc, oc) - r * r;
	float discr = b * b - c;
	if (discr < 0.0)
		return -1.0;
	// Special case: inside sphere, use far discriminant
	return (discr > b * b) ? (-b + sqrt(discr)) : (-b - sqrt(discr));
}

float3 SphericalDir(float azimuth, float zenith)
{
	float cosZenith, sinZenith, cosAzimuth, sinAzimuth;
	sincos(zenith, sinZenith, cosZenith);
	sincos(azimuth, sinAzimuth, cosAzimuth);
	return float3(sinZenith * cosAzimuth, sinZenith * sinAzimuth, cosZenith);
}

float HorizonZenithCos(float r)
{
	float sinZenith = SharedData::physSkyData.rPlanet / r;
	return -sqrt(1 - sinZenith * sinZenith);
}

float2 TrLutUv(float r, float cosSunZenith)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	// float cosHorZenith = HorizonZenithCos(r);
	const float cosHorZenith = -0.414;
	float2 uv = float2(
		saturate((cosSunZenith - cosHorZenith) / (1 - cosHorZenith)),
		saturate((r - data.rPlanet) / (data.rAtmosphere - data.rPlanet)));
	// (batch 37b) Upstream 9fbd052ad: keep the lookup on texel centres of the 256x64 LUT, so a
	// wrapping sampler cannot blend in the opposite edge. (47o) Always (the Legacy 36f path is gone).
	uv = clamp(uv, float2(0.5 / 256.0, 0.5 / 64.0), float2(1.0 - 0.5 / 256.0, 1.0 - 0.5 / 64.0));
	return uv;
}

float2 TrLutUvPlanet(float3 pos, float3 sunDir)
{
	float r = length(pos);
	float3 up = pos / r;
	return TrLutUv(r, dot(sunDir, up));
}

// (47b, Sky Precision) Multiple scattering keeps the affine (sun zenith, height) map and the 47a2 edge clamp
// (upstream MsLutUv; the CPU copy in PhysicalSkyModel reads it the same way).
float2 MsLutUv(float r, float cosSunZenith)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const float cosHorZenith = -0.414;
	float2 uv = float2(
		saturate((cosSunZenith - cosHorZenith) / (1 - cosHorZenith)),
		saturate((r - data.rPlanet) / (data.rAtmosphere - data.rPlanet)));
	return clamp(uv, float2(0.5 / 256.0, 0.5 / 64.0), float2(1.0 - 0.5 / 256.0, 1.0 - 0.5 / 64.0));
}

float2 MsLutUvPlanet(float3 pos, float3 lightDir)
{
	const float r = length(pos);
	return MsLutUv(r, dot(lightDir, pos / max(r, 1.0)));
}

// (47b, Sky Precision) Bruneton's transmittance parameterisation (upstream 35c759765): low altitudes and long grazing
// paths get most of the 256x64 texels. The LUT holds unoccluded paths to the top of the atmosphere; the planet is
// tested analytically (PlanetVisibility).
// https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/functions.glsl.html
float2 TrLutUvBruneton(float r, float cosSunZenith)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	r = clamp(r, data.rPlanet, data.rAtmosphere);
	const float atmosphereHorizon = sqrt((data.rAtmosphere - data.rPlanet) * (data.rAtmosphere + data.rPlanet));
	const float localHorizon = sqrt(max(0.0, (r - data.rPlanet) * (r + data.rPlanet)));
	const float mu = max(cosSunZenith, -localHorizon / r);
	const float rMu = r * mu;
	const float distance = max(0.0, sqrt(max(0.0, rMu * rMu + (data.rAtmosphere - r) * (data.rAtmosphere + r))) - rMu);
	const float nearest = data.rAtmosphere - r;
	const float farthest = localHorizon + atmosphereHorizon;
	const float2 unitUv = saturate(float2((distance - nearest) / max(farthest - nearest, 1.0), localHorizon / atmosphereHorizon));
	const float2 texel = 1.0 / float2(256, 64);
	return 0.5 * texel + unitUv * (1.0 - texel);
}

void TrLutParametersBruneton(float2 uv, out float r, out float mu)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const float2 texel = 1.0 / float2(256, 64);
	const float2 unitUv = saturate((uv - 0.5 * texel) / (1.0 - texel));
	const float horizonSquared = (data.rAtmosphere - data.rPlanet) * (data.rAtmosphere + data.rPlanet);
	const float localHorizon = sqrt(horizonSquared) * unitUv.y;
	r = sqrt(data.rPlanet * data.rPlanet + localHorizon * localHorizon);
	r = clamp(r, data.rPlanet, data.rAtmosphere);
	const float distance = lerp(data.rAtmosphere - r, localHorizon + sqrt(horizonSquared), unitUv.x);
	mu = distance > 0.0 ? (horizonSquared - localHorizon * localHorizon - distance * distance) / (2.0 * r * distance) : 1.0;
	mu = clamp(mu, -localHorizon / r, 1.0);
}

// (47b, Sky Precision) Visible fraction of a light the size of the sun (0.266 degree radius) at radius r, cos zenith mu.
float PlanetVisibility(float r, float mu)
{
	const float sinHorizon = saturate(SharedData::physSkyData.rPlanet / max(r, 1.0));
	const float cosHorizon = -sqrt(max(0.0, 1.0 - sinHorizon * sinHorizon));
	const float discWidth = max(0.00465 * sinHorizon, 1e-6);
	return smoothstep(-discWidth, discWidth, mu - cosHorizon);
}

// Transmittance from a point (planet-centred) towards a light, through the LUT. With Sky Precision: the Bruneton
// lookup x the analytic planet occlusion (a point below the ground counts as on it, so a camera under the
// physical ground keeps its light); without: the 47a2 lookup (occlusion baked into the LUT).
float3 SampleLightTr(Texture2D<float4> a_lut, SamplerState a_samp, float3 a_pos, float3 a_lightDir)
{
	float3 tr = 0.0;
	[branch] if (SkyPrecision())
	{
		const float r = length(a_pos);
		const float mu = dot(a_pos, a_lightDir) / max(r, 1.0);
		const float rClamped = max(r, SharedData::physSkyData.rPlanet);
		const float visibility = PlanetVisibility(rClamped, mu);
		[branch] if (visibility > 0.0)
			tr = a_lut.SampleLevel(a_samp, TrLutUvBruneton(rClamped, mu), 0).rgb * visibility;
	}
	else
		tr = a_lut.SampleLevel(a_samp, TrLutUvPlanet(a_pos, a_lightDir), 0).rgb;
	return tr;
}

// The same without the planet's shadow (Sky Precision); for callers that apply their own (CloudBodyLight).
float3 SampleLightTrUnoccluded(Texture2D<float4> a_lut, SamplerState a_samp, float3 a_pos, float3 a_lightDir)
{
	const float r = length(a_pos);
	const float2 uv = SkyPrecision() ? TrLutUvBruneton(max(r, SharedData::physSkyData.rPlanet), dot(a_pos, a_lightDir) / max(r, 1.0)) : TrLutUvPlanet(a_pos, a_lightDir);
	return a_lut.SampleLevel(a_samp, uv, 0).rgb;
}

// (47b, Sky Precision) Zenith angle of the planet's horizon seen from the camera (upstream dcd3593d1).
float SkyViewHorizonZenith()
{
	const float radius = SharedData::physSkyData.rPlanet;
	const float viewRadius = max(SharedData::physSkyData.zCameraPlanet, radius);
	const float horizonDistance = sqrt(max(0.0, (viewRadius - radius) * (viewRadius + radius)));
	return acos(clamp(-horizonDistance / viewRadius, -1.0, 0.0));
}

// cylinder map. a_lutHeight: rows of the LUT (Sky Precision maps onto texel centres, with the true horizon on the
// middle row; without it the 47a2 mapping, which ignores the height).
float2 SkyViewLutUvH(float3 rayDir, float a_lutHeight)
{
	float2 uv;
	[branch] if (SkyPrecision())
	{
		const float u = atan2(rayDir.y, rayDir.x) * 0.5 * RCP_PI;
		const float zenith = acos(clamp(rayDir.z, -1.0, 1.0));
		const float horizon = SkyViewHorizonZenith();
		const float v = zenith <= horizon ?
		                    0.5 * (1.0 - sqrt(saturate(1.0 - zenith / horizon))) :
		                    0.5 + 0.5 * sqrt(saturate((zenith - horizon) / (Math::PI - horizon)));
		uv = float2(frac(u), (0.5 + v * (a_lutHeight - 1.0)) / a_lutHeight);
	}
	else
	{
		float azimuth = atan2(rayDir.y, rayDir.x);
		float u = azimuth * .5 * RCP_PI;  // sampler wraps around so ok
		float zenith = asin(rayDir.z);
		float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * RCP_PI);
		v = max(v, 0.01);
		uv = frac(float2(u, v));
	}
	return uv;
}

float2 SkyViewLutUv(float3 rayDir)
{
	return SkyViewLutUvH(rayDir, SV_LUT_HEIGHT);
}

// url: http://www.physics.hmc.edu/faculty/esin/a101/limbdarkening.pdf
float3 LimbDarkenHestroffer(float norm_dist)
{
	float mu = sqrt(1.0 - norm_dist * norm_dist);

	// coefficient for RGB wavelength (680, 550, 440)
	float3 a0 = float3(0.34685, 0.26073, 0.15248);
	float3 a1 = float3(1.37539, 1.27428, 1.38517);
	float3 a2 = float3(-2.04425, -1.30352, -1.49615);
	float3 a3 = float3(2.70493, 1.47085, 1.99886);
	float3 a4 = float3(-1.94290, -0.96618, -1.48155);
	float3 a5 = float3(0.55999, 0.26384, 0.44119);

	float mu2 = mu * mu;
	float mu3 = mu2 * mu;
	float mu4 = mu2 * mu2;
	float mu5 = mu4 * mu;

	float3 factor = a0 + a1 * mu + a2 * mu2 + a3 * mu3 + a4 * mu4 + a5 * mu5;
	return factor;
}

float3 InvSkyViewLutUv(float2 uv)
{
	float azimuth = uv.x * 2 * Math::PI;
	float vm = 1 - 2 * uv.y;
	float zenith = Math::PI * .5 * (1 - sign(vm) * vm * vm);
	return SphericalDir(azimuth, zenith);
}

// (47b) The inverse of SkyViewLutUvH.
float3 InvSkyViewLutUvH(float2 uv, float a_lutHeight)
{
	float3 dir;
	[branch] if (SkyPrecision())
	{
		const float azimuth = uv.x * 2.0 * Math::PI;
		const float v = saturate((uv.y * a_lutHeight - 0.5) / (a_lutHeight - 1.0));
		const float horizon = SkyViewHorizonZenith();
		const float vm = 1.0 - 2.0 * v;
		const float zenith = v <= 0.5 ? horizon * (1.0 - vm * vm) : horizon + (Math::PI - horizon) * vm * vm;
		dir = SphericalDir(azimuth, zenith);
	}
	else
		dir = InvSkyViewLutUv(uv);
	return dir;
}

void SampleAtmosphere(
	float altitude,
	out float rayleighDensity,
	out float aerosolDensity,
	out float ozoneDensity)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	rayleighDensity = exp(-altitude * data.rayleighFalloff);
	aerosolDensity = exp(-altitude * data.aerosolFalloff);
	ozoneDensity = max(0.f, 1 - abs(altitude - data.ozoneAltitude) / (data.ozoneThickness * 0.5f));
}

namespace Phase
{
	float HG(float cos_theta, float g)
	{
		static const float scale = .25 * RCP_PI;
		const float g2 = g * g;

		float num = (1.0 - g2);
		float denom = pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5);

		return scale * num / denom;
	}

	float HGDualLobe(float cos_theta, float g_0, float g_1, float w)
	{
		return lerp(HG(cos_theta, g_0), HG(cos_theta, g_1), w);
	}

	float CornetteShanks(float cos_theta, float g)
	{
		static const float scale = .375 * RCP_PI;
		const float g2 = g * g;

		float num = (1.0 - g2) * (1.0 + cos_theta * cos_theta);
		float denom = (2.0 + g2) * pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5);

		return scale * num / denom;
	}

	float Draine(float cos_theta, float g, float alpha)
	{
		static const float scale = .25 * RCP_PI;
		const float g2 = g * g;

		float num = (1.0 - g2) * (1.0 + alpha * cos_theta * cos_theta);
		float denom = pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5) * (1.0 + alpha * (1.0 + 2.0 * g2) * .333333333333333);

		return scale * num / denom;
	}

	// https://www.shadertoy.com/view/tl33Rn
	float smoothstep_unchecked(float x) { return (x * x) * (3.0 - x * 2.0); }
	float smoothbump(float a, float r, float x) { return 1.0 - smoothstep_unchecked(min(abs(x - a), r) / r); }
	float powerful_scurve(float x, float p1, float p2) { return pow(1.0 - pow(1.0 - saturate(x), p2), p1); }
	float3 CloudFit(float cos_theta)
	{
		float x = acos(cos_theta);
		float x2 = max(0., x - 2.45) / (Math::PI - 2.15);
		float x3 = max(0., x - 2.95) / (Math::PI - 2.95);
		float y = (exp(-max(x * 1.5 + 0.0, 0.0) * 30.0)         // front peak
				   + smoothstep(1.7, 0., x) * 0.45 * 0.8        // front ramp
				   + smoothbump(0.4, 0.5, cos_theta) * 0.02     // front bump middle
				   - smoothstep(1., 0.2, x) * 0.06              // front ramp damp wave
				   + smoothbump(2.18, 0.20, x) * 0.06           // first trail wave
				   + smoothstep(2.28, 2.45, x) * 0.18           // trailing piece
				   - powerful_scurve(x2 * 4.0, 3.5, 8.) * 0.04  // trail
				   + x2 * -0.085 + x3 * x3 * 0.1);              // trail peak

		float3 ret = y;
		// spectralize a bit
		ret = lerp(ret, ret + 0.008 * 2., smoothstep(0.94, 1., cos_theta) * sin(x * 10. * float3(8, 4, 2)));
		ret = lerp(ret, ret - 0.008 * 2., smoothbump(-0.7, 0.14, cos_theta) * sin(x * 20. * float3(8, 4, 2)));   // fogbow
		ret = lerp(ret, ret - 0.008 * 5., smoothstep(-0.994, -1., cos_theta) * sin(x * 30. * float3(3, 4, 2)));  // glory

		// scale and offset should be tweaked so integral on sphere is 1
		ret += 0.13 * 1.4;
		return ret * 3.9 * 0.25 * RCP_PI;  // Edit: additonal 1/4pi to be consistent with the rest
	}

	// numerical fit https://www.shadertoy.com/view/4sjBDG
	float ThomasSchander(float costh)
	{
		// This function was optimized to minimize (delta*delta)/reference in order to capture
		// the low intensity behavior.
		float bestParams[10];
		bestParams[0] = 9.805233e-06;
		bestParams[1] = -6.500000e+01;
		bestParams[2] = -5.500000e+01;
		bestParams[3] = 8.194068e-01;
		bestParams[4] = 1.388198e-01;
		bestParams[5] = -8.370334e+01;
		bestParams[6] = 7.810083e+00;
		bestParams[7] = 2.054747e-03;
		bestParams[8] = 2.600563e-02;
		bestParams[9] = -4.552125e-12;

		float p1 = costh + bestParams[3];
		float4 expValues = exp(float4(bestParams[1] * costh + bestParams[2], bestParams[5] * p1 * p1, bestParams[6] * costh, bestParams[9] * costh));
		float4 expValWeight = float4(bestParams[0], bestParams[4], bestParams[7], bestParams[8]);
		return dot(expValues, expValWeight) * 0.25;
	}

	float Rayleigh(float cos_theta)
	{
		const float k = .1875 * RCP_PI;
		return k * (1.0 + cos_theta * cos_theta);
	}
}

#ifndef PS_PREPASS_RSRCS

#	ifndef PS_DEFERRED_RSRCS
float3 SampleSky(float3 viewDir, uint2 pxCoord, SamplerState sampSv)
{
	SharedData::PhysSkyData data = SharedData::physSkyData;

	const float2 skyLutUv = SkyViewLutUv(viewDir);
	float3 skyColor = TexSvLut.SampleLevel(sampSv, skyLutUv, 0).rgb;

	float shadow = TexApShadow[pxCoord / 2]; // this actually works???
	skyColor *= 1 - shadow;

	if (data.tonemapper == 1)
        skyColor = Color::LinearToGamma(skyColor);
    else if (data.tonemapper == 2)
        skyColor = skyColor / (1 + skyColor);

	return skyColor;
}

// (batch 37b) SampleSky with the AP shadow passed in (upstream 23156dc5f), for the reflection
// pass, where TexApShadow holds main-view pixels. The original above is left untouched so the
// other shaders that call it compile exactly as before.
float3 SampleSkyShadow(float3 viewDir, float shadow, SamplerState sampSv)
{
	SharedData::PhysSkyData data = SharedData::physSkyData;

	const float2 skyLutUv = SkyViewLutUv(viewDir);
	float3 skyColor = TexSvLut.SampleLevel(sampSv, skyLutUv, 0).rgb;
	skyColor *= 1 - shadow;

	if (data.tonemapper == 1)
		skyColor = Color::LinearToGamma(skyColor);
	else if (data.tonemapper == 2)
		skyColor = skyColor / (1 + skyColor);

	return skyColor;
}

float3 SampleTr(float3 sunDir, SamplerState sampSv)
{
	SharedData::PhysSkyData data = SharedData::physSkyData;

	if (data.trMix < 1e-8)
		return 1;

	float3 tr;
	[branch] if (SkyPrecision())  // (47b)
		tr = SampleLightTr(TexTrLut, sampSv, float3(0, 0, data.zCameraPlanet), sunDir);
	else
		tr = TexTrLut.SampleLevel(sampSv, TrLutUv(data.zCameraPlanet, sunDir.z), 0).rgb;
	tr = lerp(1, tr, data.trMix);

	return tr;
}

#		if defined(CLOUD_SHADOWS)
float3 RelightCloud(float4 baseColor, float3 viewDir, float3 cloudPosWS, SamplerState sampTr, SamplerState sampCube)
{
	if(baseColor.w <= 0)
		return baseColor.rgb;

	SharedData::PhysSkyData data = SharedData::physSkyData;

	// TODO: use proper light Dir
	float3 dirLightDir = SharedData::DirLightDirection.xyz;
	float3 dirLightColor = SharedData::DirLightColor.xyz;  // (47b) .xyz: no truncation warning (Water includes this file now)

	// TODO: planet shadowing

	float3 trAtmos = SampleLightTr(TexTrLut, sampTr, cloudPosWS + float3(0, 0, data.zCameraPlanet), dirLightDir);  // (47b) the 47a2 lookup without Sky Precision
	trAtmos = lerp(1, trAtmos, data.trMix);
	dirLightColor *= trAtmos;
	
	float u = dot(viewDir, dirLightDir);
	float phaseCloud = Remap(
		baseColor.w,
		data.silverLiningSpread > 0 ? data.silverLiningSpread: 0,
		data.silverLiningSpread < 0 ? 1 + data.silverLiningSpread : 1,
		lerp(0.25 * RCP_PI, Phase::ThomasSchander(u), data.silverLiningMix),
		0.25 * RCP_PI) * 2 * Math::PI * data.cloudRelightMix;

	float3 cloudColor = baseColor.rgb * data.cloudOriginalMix;

	if (baseColor.w > 0.0)
	{
		float rayStep = 1.0 / 32.0;
		float rayPos = rayStep * 0.5; 
		float4 rayShadow = 0.0;

		float3 PoissonDisc[] = {
			float3(0.460921f, 0.615192f, 0.887539f),
			float3(0.757347f, 0.911008f, 0.189581f),
			float3(0.548753f, 0.145482f, 0.0548723f),
			float3(0.90051f, 0.157048f, 0.623493f)
		};

		[unroll]
		for(int i = 0; i < 4; i++)
		{		
			float3 raySample = normalize(lerp(viewDir, SharedData::DirLightDirection.xyz, rayPos));

			raySample += (PoissonDisc[i] * 2.0 - 1.0) * 0.01;

			if (raySample.z < 0.0)
				rayShadow[i % 4] += -raySample.z; // World shadow
			else
				rayShadow[i % 4] = max(rayShadow[i % 4], CloudShadows::CloudShadowsTexture.SampleLevel(sampCube, raySample, 0).x);

			rayPos += rayStep;
		}

		cloudColor += baseColor.a * baseColor.xyz * phaseCloud * (1.0 - saturate(dot(rayShadow, 0.25))) * dirLightColor;
	}

	return cloudColor;
}

// (42b) "Cloud Lighting: Sun + Moons + Sky". RelightCloud above is left exactly as it was (the
// "Single light (old)" choice).

// Light from a body reaching a point (planet-centred, game units): 0 once the body is below that
// point's geometric horizon, i.e. inside the planet's shadow, softened over about half a degree
// (the sun's own size) so the edge does not crawl.
float PlanetShadow(float3 posPlanet, float3 lightDir)
{
	const float r = length(posPlanet);
	const float mu = dot(posPlanet / r, lightDir);
	const float sinHorizon = saturate(SharedData::physSkyData.rPlanet / r);
	const float muHorizon = -sqrt(saturate(1.0 - sinHorizon * sinHorizon));
	return smoothstep(muHorizon - 0.005, muHorizon + 0.005, mu);
}

// The self-shadow of RelightCloud, towards any light.
float CloudSelfShadow(float3 viewDir, float3 lightDir, SamplerState sampCube)
{
	float rayStep = 1.0 / 32.0;
	float rayPos = rayStep * 0.5;
	float4 rayShadow = 0.0;

	float3 PoissonDisc[] = {
		float3(0.460921f, 0.615192f, 0.887539f),
		float3(0.757347f, 0.911008f, 0.189581f),
		float3(0.548753f, 0.145482f, 0.0548723f),
		float3(0.90051f, 0.157048f, 0.623493f)
	};

	[unroll] for (int i = 0; i < 4; i++)
	{
		float3 raySample = normalize(lerp(viewDir, lightDir, rayPos));
		raySample += (PoissonDisc[i] * 2.0 - 1.0) * 0.01;

		if (raySample.z < 0.0)
			rayShadow[i] += -raySample.z;  // World shadow
		else
			rayShadow[i] = max(rayShadow[i], CloudShadows::CloudShadowsTexture.SampleLevel(sampCube, raySample, 0).x);

		rayPos += rayStep;
	}
	return saturate(dot(rayShadow, 0.25));
}

// RelightCloud's phase term, towards any light.
float CloudPhase(float alpha, float3 viewDir, float3 lightDir)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const float u = dot(viewDir, lightDir);
	return Remap(
		alpha,
		data.silverLiningSpread > 0 ? data.silverLiningSpread : 0,
		data.silverLiningSpread < 0 ? 1 + data.silverLiningSpread : 1,
		lerp(0.25 * RCP_PI, Phase::ThomasSchander(u), data.silverLiningMix),
		0.25 * RCP_PI) * 2 * Math::PI * data.cloudRelightMix;
}

// One body's light on the cloud: colour x the air between it and the cloud x the planet's shadow x
// phase x self-shadow.
float3 CloudBodyLight(float4 baseColor, float3 viewDir, float3 posPlanet, float3 lightDir, float3 lightColor, SamplerState sampTr, SamplerState sampCube)
{
	float3 result = 0.0;
	const float planetShadow = PlanetShadow(posPlanet, lightDir);
	[branch] if (any(lightColor > 0.0) && planetShadow > 0.0)
	{
		float3 trAtmos = SampleLightTrUnoccluded(TexTrLut, sampTr, posPlanet, lightDir);  // (47b) planetShadow is the occlusion
		trAtmos = lerp(1, trAtmos, SharedData::physSkyData.trMix);
		result = lightColor * trAtmos * planetShadow * CloudPhase(baseColor.w, viewDir, lightDir) * (1.0 - CloudSelfShadow(viewDir, lightDir, sampCube));
	}
	return result;
}

float3 RelightCloudMulti(float4 baseColor, float3 viewDir, float3 cloudPosWS, SamplerState sampTr, SamplerState sampSv, SamplerState sampCube)
{
	if (baseColor.w <= 0)
		return baseColor.rgb;

	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const SharedData::PhysSkyCloudData cloud = SharedData::physSkyCloudData;

	// The cloud as the light sees it: raised by "Cloud Height for Sunlight" (the drawn layer is not).
	const float3 posPlanet = cloudPosWS + float3(0, 0, data.zCameraPlanet + cloud.HeightOffset);

	// The sun along the same direction the sky's LUTs use (true height, Twilight Length), so the
	// clouds turn with the sky around them; the moons along their glow's directions.
	float3 light = CloudBodyLight(baseColor, viewDir, posPlanet, normalize(SharedData::physSkyNightData.LutSunDir), cloud.SunColor, sampTr, sampCube);
	// (44h) With "Moonlight on Clouds" the moons go through CloudMoonLight instead (added by Sky.hlsl
	// after the weather's cloud colour), so they are not counted twice.
	[branch] if (!(cloud.Flags & SharedData::PhysSkyCloudFlags::MoonDirect))
	{
		light += CloudBodyLight(baseColor, viewDir, posPlanet, normalize(data.masserDir), cloud.MasserColor, sampTr, sampCube);
		light += CloudBodyLight(baseColor, viewDir, posPlanet, normalize(data.secundaDir), cloud.SecundaColor, sampTr, sampCube);
	}

	// The sky's own light: the sky-view LUT (sun and moon scattering, the night base light) averaged
	// over the upper sky, the zenith and four directions 15 degrees up. A white matte layer under an
	// even sky of radiance L sends back L; a light of irradiance E sends back E / pi, which the phase
	// normalisation above turns into E / 2. The same factor (pi / 2) is applied to the sky.
	[branch] if (cloud.SkyAmbient > 0.0)
	{
		const float c15 = 0.9659258, s15 = 0.2588190;
		// the zenith: SkyViewLutUv's v floor, or (47b, Sky Precision) the first row's centre
		float3 sky = 0.3 * TexSvLut.SampleLevel(sampSv, SkyPrecision() ? float2(0.0, 0.5 / SV_LUT_HEIGHT) : float2(0.0, 0.01), 0).rgb;
		sky += 0.175 * TexSvLut.SampleLevel(sampSv, SkyViewLutUv(float3(c15, 0, s15)), 0).rgb;
		sky += 0.175 * TexSvLut.SampleLevel(sampSv, SkyViewLutUv(float3(-c15, 0, s15)), 0).rgb;
		sky += 0.175 * TexSvLut.SampleLevel(sampSv, SkyViewLutUv(float3(0, c15, s15)), 0).rgb;
		sky += 0.175 * TexSvLut.SampleLevel(sampSv, SkyViewLutUv(float3(0, -c15, s15)), 0).rgb;
		light += sky * (Math::PI * 0.5 * cloud.SkyAmbient * data.cloudRelightMix);
	}

	float3 cloudColor = baseColor.rgb * data.cloudOriginalMix;
	cloudColor += baseColor.a * baseColor.rgb * light;
	return cloudColor;
}

// (44h) "Moonlight on Clouds". Sky.hlsl multiplies the relit cloud by the weather's cloud colour,
// which is near black at night, so the moons' light on clouds was cut to a few percent. This is
// the moons' share on its own, for Sky.hlsl to ADD after that multiply: each moon's body light
// (as in RelightCloudMulti) x a capped forward-scatter halo, which thick cloud mostly hides, made
// partly grey, scaled by "Moonlight Strength" and capped. 0 unless MoonDirect is set; 0 by day
// (the moon colours carry MoonDayFade).
float3 CloudMoonLight(float4 baseColor, float3 viewDir, float3 cloudPosWS, SamplerState sampTr, SamplerState sampCube)
{
	const SharedData::PhysSkyCloudData cloud = SharedData::physSkyCloudData;
	float3 result = 0.0;
	[branch] if ((cloud.Flags & SharedData::PhysSkyCloudFlags::MoonDirect) && baseColor.w > 0 && cloud.MoonCloudStrength > 0)
	{
		const SharedData::PhysSkyData data = SharedData::physSkyData;
		const float3 posPlanet = cloudPosWS + float3(0, 0, data.zCameraPlanet + cloud.HeightOffset);
		const float haloFade = 1.0 - 0.6 * saturate(baseColor.w);

		const float3 masserDir = normalize(data.masserDir);
		const float3 secundaDir = normalize(data.secundaDir);
		const float masserHalo = 1.0 + min(cloud.MoonCloudHalo * Phase::HG(dot(viewDir, masserDir), 0.75), 8.0) * haloFade;
		const float secundaHalo = 1.0 + min(cloud.MoonCloudHalo * Phase::HG(dot(viewDir, secundaDir), 0.75), 8.0) * haloFade;
		float3 light = CloudBodyLight(baseColor, viewDir, posPlanet, masserDir, cloud.MasserColor, sampTr, sampCube) * masserHalo;
		light += CloudBodyLight(baseColor, viewDir, posPlanet, secundaDir, cloud.SecundaColor, sampTr, sampCube) * secundaHalo;

		light = lerp(Color::RGBToLuminance(light), light, saturate(cloud.MoonCloudSaturation));
		result = min(baseColor.a * baseColor.rgb * light * cloud.MoonCloudStrength, 16.0);
	}
	return result;
}
#		endif
#	endif

float4 SampleAp(float3 viewDir, uint2 pxCoord, float dist, SamplerState sampSv)
{
	SharedData::PhysSkyData data = SharedData::physSkyData;

	uint3 apDims;
	TexApLut.GetDimensions(apDims.x, apDims.y, apDims.z);
	const float2 skyLutUv = SkyViewLutUvH(viewDir, apDims.y);  // (47b) the AP LUT's own rows with Sky Precision
	const float depth_slice = SkyPrecision() ? ApDepthUv(dist, apDims.z) : lerp(.5 / apDims.z, 1 - .5 / apDims.z, saturate(dist / AP_MAX_DIST));
	float4 apColor = TexApLut.SampleLevel(sampSv, float3(skyLutUv, depth_slice), 0);

	float shadow = TexApShadow[pxCoord / 2];
	apColor.rgb *= 1 - shadow;

	if (data.tonemapper == 1)
        apColor.rgb = Color::LinearToGamma(apColor.rgb);
    else if (data.tonemapper == 2)
        apColor.rgb = apColor.rgb / (1 + apColor.rgb);

	apColor.rgb = lerp(0, apColor.rgb, data.apLumMix);
	apColor.a = lerp(1, apColor.a, data.apTrMix);

	return apColor;
}

// (batch 37b) SampleAp with the AP shadow passed in (upstream 23156dc5f); see SampleSkyShadow.
float4 SampleApShadow(float3 viewDir, float dist, float shadow, SamplerState sampSv)
{
	SharedData::PhysSkyData data = SharedData::physSkyData;

	uint3 apDims;
	TexApLut.GetDimensions(apDims.x, apDims.y, apDims.z);
	const float2 skyLutUv = SkyViewLutUvH(viewDir, apDims.y);  // (47b)
	const float depth_slice = SkyPrecision() ? ApDepthUv(dist, apDims.z) : lerp(.5 / apDims.z, 1 - .5 / apDims.z, saturate(dist / AP_MAX_DIST));
	float4 apColor = TexApLut.SampleLevel(sampSv, float3(skyLutUv, depth_slice), 0);
	apColor.rgb *= 1 - shadow;

	if (data.tonemapper == 1)
		apColor.rgb = Color::LinearToGamma(apColor.rgb);
	else if (data.tonemapper == 2)
		apColor.rgb = apColor.rgb / (1 + apColor.rgb);

	apColor.rgb = lerp(0, apColor.rgb, data.apLumMix);
	apColor.a = lerp(1, apColor.a, data.apTrMix);

	return apColor;
}
#endif

#ifndef OMIT_PS_NAMESPACE
}
#endif
#endif