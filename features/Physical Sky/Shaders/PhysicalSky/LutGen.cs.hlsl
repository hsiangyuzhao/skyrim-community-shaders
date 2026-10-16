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

// (47a) PhysicalSky::NightFlags bits and the NightCbData fields used by "Sky Fixes". Named here, not in
// SharedData.hlsli (which every Lighting shader includes), so that file stays byte-identical.
static const uint kNightMidpoint = (1 << 1);   // ray-march samples in the middle of each step
static const uint kNightGroundFix = (1 << 2);  // multiscatter ground bounce x cos / pi, sun tested at the ground point
static const uint kNightSkyFixes = (1 << 3);   // Sky Fixes on: the sky brightness factor below applies
static const uint kNightDistantSea = (1 << 4); // (47b) Sky Fixes: sky-view rays below the planet horizon also see the sky reflected at grazing incidence
#define SKY_FIXES_SCALE SharedData::physSkyNightData.pad0  // sky brightness factor on the SUN's in-scatter (47a2: not the moons')

// (47b, Sky Precision) Upstream 35c759765: transmittance quadrature concentrated around the ray's lowest point,
// where the density changes fastest; a grazing ray gets samples on both sides of it.
float TransmittanceRayDistance(float u, float distance, float closest)
{
	if (closest <= 0.0)
		return distance * u * u;
	if (u < 0.5) {
		const float v = 1.0 - 2.0 * u;
		return closest * (1.0 - v * v);
	}
	const float v = 2.0 * u - 1.0;
	return closest + (distance - closest) * v * v;
}

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
	inout float3 lum,
	inout float3 lumSun  // (47a2) the sun's share of lum (Sky Fixes only)
#elif LUTGEN == 3
	uint2 tid, uint depth,
	inout float3 tr,
	inout float3 lum
#endif
)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const uint fixFlags = SharedData::physSkyNightData.Flags;  // (47a)
	const bool precision = SkyPrecision();                      // (47b)

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
#if LUTGEN == 3
	float3 lumSun = 0;  // (47a2) the sun's share of lum (Sky Fixes only)
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
	// (47a) Sky Fixes: sample each step in its middle (the loop advances first, so the original samples
	// the far end of each step: twilight horizons came out too bright, a low sun's sky too dark).
	// (47b) Sky Precision samples the sky-view rays in the middle of each step too (upstream dcd3593d1).
#if LUTGEN == 2
	[branch] if ((fixFlags & kNightMidpoint) || precision)
#else
	[branch] if (fixFlags & kNightMidpoint)
#endif
		curr_pos -= 0.5 * stride;
	[loop] for (uint i = 0; i < nsteps; ++i)
	{
		curr_pos += stride;
		float stepDt = dt;
#if LUTGEN == 3
		// (47b, Sky Precision) Upstream 183ec20b3: slices concentrated near the camera (quadratic in distance), each
		// interval integrated at its midpoint and stored at its far end.
		[branch] if (precision)
		{
			const float tNear = ApSliceDistance(i, depth);
			const float tFar = ApSliceDistance(i + 1, depth);
			stepDt = tFar - tNear;
			curr_pos = pos + (0.5 * (tNear + tFar)) * rayDir;
		}
#endif

		float rouRayleigh, rouAerosol, rouOzone;
		SampleAtmosphere(
			max(0.f, (length(curr_pos) - data.rPlanet)),
			rouRayleigh, rouAerosol, rouOzone);
		float3 muSRayleigh = rouRayleigh * data.rayleighScatter;
		float3 muSAerosol = rouAerosol * data.aerosolScatter;
		float3 extinction = muSRayleigh + muSAerosol +
		                    rouAerosol * data.aerosolAbsorption +
		                    rouOzone * data.ozoneAbsorption;

		float3 trSample = exp(-stepDt * extinction);

#if LUTGEN != 0
		float3 scatterFactor = (1 - trSample) / extinction;

		float3 scatterNoPhase = muSRayleigh + muSAerosol;
#	if LUTGEN == 1  // multiscatter
		float3 fScatter = scatterNoPhase * scatterFactor;
		lumFactor += tr * fScatter;
#	endif

		// (47b) SampleLightTr / the multiscatter coordinates are the 47a2 lookups without Sky Precision.
		float3 trSun = SampleLightTr(TexTrLut, SampTr, curr_pos, sunDir);
#	if LUTGEN != 1
		float3 trMasser = SampleLightTr(TexTrLut, SampTr, curr_pos, data.masserDir);
		float3 trSecunda = SampleLightTr(TexTrLut, SampTr, curr_pos, data.secundaDir);

		const float2 lutUvSun = precision ? MsLutUvPlanet(curr_pos, sunDir) : TrLutUvPlanet(curr_pos, sunDir);
		const float2 lutUvMasser = precision ? MsLutUvPlanet(curr_pos, data.masserDir) : TrLutUvPlanet(curr_pos, data.masserDir);
		const float2 lutUvSecunda = precision ? MsLutUvPlanet(curr_pos, data.secundaDir) : TrLutUvPlanet(curr_pos, data.secundaDir);

		const float3 psiMsSun = TexMsLut.SampleLevel(SampTr, lutUvSun, 0).rgb * data.sunlightColor;
		float3 psiMs = psiMsSun;
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
#	if LUTGEN != 1
		// (47a2) Sky Fixes scales only the sun's light (its single scatter and its share of the multiple
		// scattering); the moons' glow keeps its 46y brightness.
		[branch] if (fixFlags & kNightSkyFixes)
			lumSun += ((muSRayleigh * phaseRayleighSun + muSAerosol * phaseAerosolSun) * trSun * data.sunlightColor + scatterNoPhase * psiMsSun) * scatterFactor * tr;
#	endif
#endif
#if LUTGEN == 3
		[branch] if (nightBaseLight)
			lumNight += scatterNoPhase * nightGlow * scatterFactor * tr;
#endif
		tr *= trSample;

#if LUTGEN == 3
		float3 lumOut = lum;
		[branch] if (fixFlags & kNightSkyFixes)  // (47a) Sky Fixes: the sky brightness factor, sun only (47a2)
			lumOut += lumSun * (SKY_FIXES_SCALE - 1.0);
		[branch] if (nightBaseLight)
			lumOut += lumNight;
		RWTexOutput[uint3(tid.xy, i + 1)] = float4(lumOut, dot(tr, float3(0.2126, 0.7152, 0.0722)));
#endif
	}

#if LUTGEN == 1  // multiscatter
	if (tGround > 0) {
		float3 hit_pos = pos + tGround * rayDir;
		[branch] if (fixFlags & kNightGroundFix) {
			// (47a) Sky Fixes: a Lambertian ground sends back albedo x cos(sun zenith at the ground point) / pi
			// per unit of sunlight; the original left out cos / pi (about 3x too bright) and tested the sun
			// against the ray's start instead of the ground point.
			const float3 groundUp = normalize(hit_pos);
			const float groundCosSun = dot(groundUp, sunDir);
			if (groundCosSun > 0) {
				lum += tr * data.groundAlbedo * SampleLightTr(TexTrLut, SampTr, groundUp * data.rPlanet, sunDir) * (groundCosSun * RCP_PI);
			}
		} else if (dot(pos, sunDir) > 0) {
			hit_pos = normalize(hit_pos) * data.rPlanet;
			lum += tr * data.groundAlbedo * SampleLightTr(TexTrLut, SampTr, hit_pos, sunDir);
		}
	}
#endif
}

#if LUTGEN == 0
// (47b, Sky Precision) Upstream 35c759765: a transmittance texel (Bruneton parameters) is the unoccluded path to the
// top of the atmosphere (the planet is tested when the LUT is read), 40 nonuniform steps, each at its midpoint.
float3 TransmittanceBruneton(float2 uv)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	float r, mu;
	TrLutParametersBruneton(uv, r, mu);
	const float3 pos = float3(0, 0, r);
	const float3 dir = float3(0, sqrt(max(0.0, 1.0 - mu * mu)), mu);
	const float projected = r * mu;
	const float tMax = max(0.0, -projected + sqrt(max(0.0, projected * projected + (data.rAtmosphere - r) * (data.rAtmosphere + r))));
	const float tClosest = clamp(-projected, 0.0, tMax);
	const uint nsteps = 40;
	float3 opticalDepth = 0;
	[loop] for (uint i = 0; i < nsteps; ++i)
	{
		const float tNear = TransmittanceRayDistance(float(i) / nsteps, tMax, tClosest);
		const float tFar = TransmittanceRayDistance(float(i + 1) / nsteps, tMax, tClosest);
		const float3 p = pos + (0.5 * (tNear + tFar)) * dir;
		float rouRayleigh, rouAerosol, rouOzone;
		SampleAtmosphere(max(0.f, length(p) - data.rPlanet), rouRayleigh, rouAerosol, rouOzone);
		const float3 extinction = rouRayleigh * data.rayleighScatter + rouAerosol * (data.aerosolScatter + data.aerosolAbsorption) + rouOzone * data.ozoneAbsorption;
		opticalDepth += (tFar - tNear) * extinction;
	}
	return exp(-opticalDepth);
}
#endif

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
	// (47b) Sky Precision: rows around the true horizon (InvSkyViewLutUvH), a camera under the ground taken as on it.
	float3 rayDir = InvSkyViewLutUvH(uv, outDims.y);
	float3 pos = float3(0, 0, SkyPrecision() ? max(data.zCameraPlanet, data.rPlanet) : data.zCameraPlanet);
#endif

	float3 tr = 1.0;
#if LUTGEN == 0
	[branch] if (SkyPrecision())
		tr = TransmittanceBruneton(uv);  // (47b)
	else
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
	float3 lumSun = 0;
	rayMarch(pos, rayDir, tr, lum, lumSun);
	// (47b) Sky Fixes "distant sea": below the planet's horizon the sky dome showed only the short stretch of air down to
	// the physical ground, a dark band under the horizon wherever the game world ends (seen from high places). Here that
	// ground is a calm sea: the sky reflected at the ground point (Fresnel, Schlick with water's F0 = 0.02, which goes to
	// 1 at grazing so the band joins the horizon haze) x the air in between, on top of that air's own glow.
	[branch] if (SharedData::physSkyNightData.Flags & kNightDistantSea)
	{
		const float tGround = RayIntersectSphere(pos, rayDir, 0, data.rPlanet);
		[branch] if (tGround > 0.0)
		{
			const float3 groundUp = normalize(pos + tGround * rayDir);
			const float3 reflected = reflect(rayDir, groundUp);
			// Started about 14 m above the ground so the reflected ray cannot find the ground again (fp32 at the
			// planet's radius in game units).
			const float3 start = groundUp * (data.rPlanet + 1000.0);
			const float cosIncidence = saturate(-dot(rayDir, groundUp));
			const float fresnel = 0.02 + 0.98 * pow(1.0 - cosIncidence, 5.0);
			float3 trReflected = 1.0;
			float3 lumReflected = 0;
			float3 lumSunReflected = 0;
			rayMarch(start, reflected, trReflected, lumReflected, lumSunReflected);
			lum += tr * fresnel * lumReflected;
			lumSun += tr * fresnel * lumSunReflected;
		}
	}
	[branch] if (SharedData::physSkyNightData.Flags & kNightSkyFixes)  // (47a) Sky Fixes: the sky brightness factor, sun only (47a2), not the night base
		lum += lumSun * (SKY_FIXES_SCALE - 1.0);
	[branch] if (SharedData::physSkyNightData.Flags & SharedData::PhysSkyNightFlags::BaseLight)
		lum += NightSkyBase(rayDir);
	RWTexOutput[tid.xy] = float4(lum, 1.0);

#elif LUTGEN == 3
	float3 lum = 0;
	rayMarch(pos, rayDir, tid.xy, outDims.z, tr, lum);
#endif
}