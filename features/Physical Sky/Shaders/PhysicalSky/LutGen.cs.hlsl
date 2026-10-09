#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#ifndef LUTGEN
#	define LUTGEN 0
#endif

#define PS_PREPASS_SAMPLERS
#define PS_PREPASS_RSRCS
#define OMIT_PS_NAMESPACE
#include "PhysicalSky/Common.hlsli"

#if LUTGEN == 3
RWTexture3D<float4> RWTexOutput : register(u0);
#else
RWTexture2D<float4> RWTexOutput : register(u0);
#endif

void rayMarch(
	float3 pos, float3 rayDir, 
#if LUTGEN == 0
	float3 sunDir,
	inout float3 tr
#elif LUTGEN == 1
	float3 sunDir,
	inout float3 tr,
	inout float3 lum, inout float3 lumFactor
#elif LUTGEN == 2
	inout float3 tr,
	inout float3 lum
#elif LUTGEN == 3
	uint2 tid, uint depth,
	inout float3 tr,
	inout float3 lum
#endif
)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

#if LUTGEN == 0
	const uint nsteps = 40;
#elif LUTGEN == 1
	const uint nsteps = 20;
#elif LUTGEN == 2
	const uint nsteps = 30;
#else
	const uint nsteps = depth - 1;
#endif

#if LUTGEN > 1
	// (41b, F4a/F4b) The LUTs' own sun: the true height and/or a stretched twilight. Equal to
	// data.sunDir when both options are off.
	const float3 sunDir = SharedData::physSkyNightData.LutSunDir;
#endif
#if LUTGEN == 3
	// (41b, F1) Night sky base light in the aerial perspective: the air itself glows evenly with
	// the weather's night horizon colour (in-scattered like the moonlight, without a phase lobe),
	// so a long path tends to that colour and distant mountains fade into the night horizon.
	// Accumulated apart from lum and added only when on, so 0 writes exactly the 41a values.
	const bool nightBaseLight = (SharedData::physSkyNightData.Flags & SharedData::PhysSkyNightFlags::BaseLight) != 0;
	const float3 nightGlow = SharedData::physSkyNightData.BaseHorizon;
	float3 lumNight = 0;
#endif

	float tGround = RayIntersectSphere(pos, rayDir, 0, data.rPlanet);
#if LUTGEN == 0
	if (tGround > 0.0) {
		tr = 0;
		return;
	}
#endif

	float tAtmos = RayIntersectSphere(pos, rayDir, 0, data.rAtmosphere);
#if LUTGEN == 3
	float tMax = AP_MAX_DIST;
#else
	float tMax = tGround > 0 ? tGround : tAtmos;
#endif
	float dt = tMax / float(nsteps);
	float3 stride = dt * rayDir;

#if LUTGEN != 0
	float uSun = dot(rayDir, sunDir);
	float phaseAerosolSun = Phase::CornetteShanks(uSun, data.aerosolPhaseG);
	float phaseRayleighSun = Phase::Rayleigh(uSun);
#	if LUTGEN == 1
	// (batch 37b) Upstream c14664115: the higher-order scattering this LUT approximates is
	// isotropic. With the peaked aerosol phase, 16 directions decide whether a sample happens to
	// land near the sun.
	[branch] if (SharedData::physSkyExtData.Flags & SharedData::PhysSkyExtFlags::MultiScatterFix)
	{
		phaseAerosolSun = 0.25 * RCP_PI;
		phaseRayleighSun = 0.25 * RCP_PI;
	}
#	endif

#	if LUTGEN != 1
	float uMasser = dot(rayDir, data.masserDir);
	float phaseAerosolMasser = Phase::CornetteShanks(uMasser, data.aerosolPhaseG);
	float phaseRayleighMasser = Phase::Rayleigh(uMasser);

	float uSecunda = dot(rayDir, data.secundaDir);
	float phaseAerosolSecunda = Phase::CornetteShanks(uSecunda, data.aerosolPhaseG);
	float phaseRayleighSecunda = Phase::Rayleigh(uSecunda);
#	endif
#endif

	float3 curr_pos = pos;
	[loop] for (uint i = 0; i < nsteps; ++i)
	{
		curr_pos += stride;

		float rouRayleigh, rouAerosol, rouOzone;
		SampleAtmosphere(
			max(0.f, (length(curr_pos) - data.rPlanet)),
			rouRayleigh, rouAerosol, rouOzone);
		float3 muSRayleigh = rouRayleigh * data.rayleighScatter;
		float3 muSAerosol = rouAerosol * data.aerosolScatter;
		float3 extinction = muSRayleigh + muSAerosol +
		                    rouAerosol * data.aerosolAbsorption +
		                    rouOzone * data.ozoneAbsorption;

		float3 trSample = exp(-dt * extinction);

#if LUTGEN != 0
		float3 scatterFactor = (1 - trSample) / extinction;

		float3 scatterNoPhase = muSRayleigh + muSAerosol;
#	if LUTGEN == 1  // multiscatter
		float3 fScatter = scatterNoPhase * scatterFactor;
		lumFactor += tr * fScatter;
#	endif

		float2 lutUvSun = TrLutUvPlanet(curr_pos, sunDir);
		float3 trSun = TexTrLut.SampleLevel(SampTr, lutUvSun, 0).rgb;
#	if LUTGEN != 1
		float2 lutUvMasser = TrLutUvPlanet(curr_pos, data.masserDir);
		float3 trMasser = TexTrLut.SampleLevel(SampTr, lutUvMasser, 0).rgb;

		float2 lutUvSecunda = TrLutUvPlanet(curr_pos, data.secundaDir);
		float3 trSecunda = TexTrLut.SampleLevel(SampTr, lutUvSecunda, 0).rgb;
		
		float3 psiMs = TexMsLut.SampleLevel(SampTr, lutUvSun, 0).rgb * data.sunlightColor;
		psiMs += TexMsLut.SampleLevel(SampTr, lutUvMasser, 0).rgb * data.masserColor;
		psiMs += TexMsLut.SampleLevel(SampTr, lutUvSecunda, 0).rgb * data.secundaColor;
#	endif

		float3 inscatter = (muSRayleigh * phaseRayleighSun + muSAerosol * phaseAerosolSun) * trSun;
#	if LUTGEN != 1
		inscatter *= data.sunlightColor;
		inscatter += (muSRayleigh * phaseRayleighMasser + muSAerosol * phaseAerosolMasser) * trMasser * data.masserColor;
		inscatter += (muSRayleigh * phaseRayleighSecunda + muSAerosol * phaseAerosolSecunda) * trSecunda * data.secundaColor;
		inscatter += scatterNoPhase * psiMs;
#	endif

		float3 scatterIntegeral = inscatter * scatterFactor;

		lum += scatterIntegeral * tr;
#endif
#if LUTGEN == 3
		[branch] if (nightBaseLight)
			lumNight += scatterNoPhase * nightGlow * scatterFactor * tr;
#endif
		tr *= trSample;

#if LUTGEN == 3
		float3 lumOut = lum;
		[branch] if (nightBaseLight)
			lumOut = lum + lumNight;
		RWTexOutput[uint3(tid.xy, i + 1)] = float4(lumOut, dot(tr, float3(0.2126, 0.7152, 0.0722)));
#endif
	}

#if LUTGEN == 1  // multiscatter
	if (tGround > 0) {
		float3 hit_pos = pos + tGround * rayDir;
		if (dot(pos, sunDir) > 0) {
			hit_pos = normalize(hit_pos) * data.rPlanet;
			float2 lutUv = TrLutUvPlanet(hit_pos, sunDir);
			lum += tr * data.groundAlbedo * TexTrLut.SampleLevel(SampTr, lutUv, 0).rgb;
		}
	}
#endif
}

#if LUTGEN == 2
// (41b, F1) Night sky base light on the sky dome: the weather's own night gradient (horizon colour
// at the horizon, its sky-upper colour from about 30 degrees up, sky-lower below), added on top of
// the scattering. Not injected as scattering: an even glow of the air comes out ~10x brighter at
// the horizon than at the zenith, while weathers often want the zenith as bright or brighter.
float3 NightSkyBase(float3 rayDir)
{
	const SharedData::PhysSkyNightData night = SharedData::physSkyNightData;
	[flatten] if (rayDir.z >= 0.0)
		return lerp(night.BaseHorizon, night.BaseUpper, smoothstep(0.0, 0.5, rayDir.z));
	return lerp(night.BaseHorizon, night.BaseLower, smoothstep(0.0, 0.3, -rayDir.z));
}
#endif

[numthreads(8, 8, 1)] void main(uint3 tid
								: SV_DispatchThreadID) {
	const SharedData::PhysSkyData data = SharedData::physSkyData;

#if LUTGEN == 3
	RWTexOutput[uint3(tid.xy, 0)] = float4(0, 0, 0, 1);
#endif

	uint3 outDims;
#if LUTGEN == 3
	RWTexOutput.GetDimensions(outDims.x, outDims.y, outDims.z);
#else
	RWTexOutput.GetDimensions(outDims.x, outDims.y);
#endif
	float2 uv = (tid.xy + 0.5) / outDims.xy;

#if LUTGEN < 2
	float altitude = lerp(data.rPlanet, data.rAtmosphere, uv.y);
	float3 pos = float3(0, 0, altitude);

	// float horZenithCos = HorizonZenithCos(altitude);
	float horZenithCos = -0.414;
	float zenithCos = lerp(horZenithCos, 1, uv.x);
	float3 sunDir = float3(0, sqrt(1 - zenithCos * zenithCos), zenithCos);
#else
	float3 rayDir = InvSkyViewLutUv(uv);
	float3 pos = float3(0, 0, data.zCameraPlanet);
#endif

	float3 tr = 1.0;
#if LUTGEN == 0
	rayMarch(pos, sunDir, sunDir, tr);
	RWTexOutput[tid.xy] = float4(tr, 1.0);

#elif LUTGEN == 1
	const uint sqrtSamples = 4;
	const float rcpSqrtSamples = rcp(sqrtSamples);
	const float rcpSamples = rcpSqrtSamples * rcpSqrtSamples;

	float3 lumTotal = 0;
	float3 fMs = 0;
	// (batch 37b) Upstream c14664115: the azimuth covered only [0, pi), half the sphere.
	const float azimuthRange = (SharedData::physSkyExtData.Flags & SharedData::PhysSkyExtFlags::MultiScatterFix) ? 2.0 * Math::PI : Math::PI;
	for (uint i = 0; i < sqrtSamples; ++i)
		for (uint j = 0; j < sqrtSamples; ++j) {
			const float theta = (i + 0.5) * azimuthRange * rcpSqrtSamples;
			const float phi = acos(1.0 - 2.0 * (j + 0.5) * rcpSqrtSamples);
			const float3 rayDir = SphericalDir(theta, phi);

			tr = 1;
			float3 lum = 0;
			float3 lumFactor = 0;
			rayMarch(pos, rayDir, sunDir, tr, lum, lumFactor);

			fMs += lumFactor;
			lumTotal += lum;
		}
	RWTexOutput[tid.xy] = float4(lumTotal * rcpSamples / (1 - fMs * rcpSamples), 1.0);

#elif LUTGEN == 2
	float3 lum = 0;
	rayMarch(pos, rayDir, tr, lum);
	[branch] if (SharedData::physSkyNightData.Flags & SharedData::PhysSkyNightFlags::BaseLight)
		lum += NightSkyBase(rayDir);
	RWTexOutput[tid.xy] = float4(lum, 1.0);

#elif LUTGEN == 3
	float3 lum = 0;
	rayMarch(pos, rayDir, tid.xy, outDims.z, tr, lum);
#endif
}