#include "Common/Color.hlsli"
#include "Common/FastMath.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/PhysicalLighting.hlsli"
#include "Common/Random.hlsli"
#include "Common/VR.hlsli"

struct VS_INPUT
{
	float4 Position : POSITION0;

#if defined(TEX) || defined(HORIZFADE)
	float2 TexCoord : TEXCOORD0;
#endif

	float4 Color : COLOR0;
#if defined(VR)
	uint InstanceID : SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;

#if defined(DITHER) && defined(TEX)
	float4 TexCoord0 : TEXCOORD0;
#elif defined(DITHER)
	float2 TexCoord0 : TEXCOORD3;
#elif defined(TEX) || defined(HORIZFADE)
	float2 TexCoord0 : TEXCOORD0;
#endif

#if defined(TEXLERP)
	float2 TexCoord1 : TEXCOORD1;
#endif

#if defined(HORIZFADE)
	float TexCoord2 : TEXCOORD2;
#endif

#if defined(TEX) || defined(DITHER) || defined(HORIZFADE)
	float4 Color : COLOR0;
#endif

	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
#if defined(VR)
	float ClipDistance : SV_ClipDistance0;  // o11
	float CullDistance : SV_CullDistance0;  // p11
	uint EyeIndex : EYEIDX0;
#endif  // VR
};

#ifdef VSHADER
cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	row_major float4x4 WorldViewProj[1] : packoffset(c0);
	row_major float4x4 World[1] : packoffset(c4);
	row_major float4x4 PreviousWorld[1] : packoffset(c8);
	float3 EyePosition[1] : packoffset(c12);
	float VParams : packoffset(c12.w);
	float4 BlendColor[3] : packoffset(c13);
	float2 TexCoordOff : packoffset(c16);
#	else
	row_major float4x4 WorldViewProj[2] : packoffset(c0);
	row_major float4x4 World[2] : packoffset(c8);
	row_major float4x4 PreviousWorld[2] : packoffset(c16);
	float3 EyePosition[2] : packoffset(c24);
	float VParams : packoffset(c25.w);
	float4 BlendColor[3] : packoffset(c26);
	float2 TexCoordOff : packoffset(c29);
#	endif  // !VR
};

#	if defined(CLOUD_SHADOWS_CAPTURE)
// (48n, official CS PR #3166) Cloud Shadows' main-view capture: the cloud layer is redrawn into face CaptureFace of the
// occlusion cube. The inverse of DynamicCubemaps' GetCubemapSamplingVector (the cube is sampled with eye-relative world
// directions).
cbuffer CloudShadowsCapture : register(b7)
{
	uint CaptureFace;
};

float4 GetCaptureClipPosition(float3 direction)
{
	float3 clip;
	switch (CaptureFace) {
	case 0:
		clip = float3(-direction.z, direction.y, direction.x);
		break;
	case 1:
		clip = float3(direction.z, direction.y, -direction.x);
		break;
	case 2:
		clip = float3(direction.x, -direction.z, direction.y);
		break;
	case 3:
		clip = float3(direction.x, direction.z, -direction.y);
		break;
	case 4:
		clip = float3(direction.x, direction.y, direction.z);
		break;
	default:
		clip = float3(-direction.x, direction.y, -direction.z);
		break;
	}
	return clip.xyzz;
}
#	endif

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;
	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif
	);

	float4 inputPosition = float4(input.Position.xyz, 1.0);

#	if defined(OCCLUSION)

	// Intentionally left blank

#	elif defined(MOONMASK)

	vsout.TexCoord0 = input.TexCoord;
	vsout.Color = float4(VParams.xxx, 1.0);

#	elif defined(HORIZFADE)

	float worldHeight = mul(World[eyeIndex], inputPosition).z;
	float eyeHeightDelta = -EyePosition[eyeIndex].z + worldHeight;

	vsout.TexCoord0.xy = input.TexCoord;
	vsout.TexCoord2.x = saturate((1.0 / 17.0) * eyeHeightDelta);
	vsout.Color.xyz = BlendColor[0].xyz * VParams;
	vsout.Color.w = BlendColor[0].w;

#	else  // MOONMASK HORIZFADE

#		if defined(DITHER)

#			if defined(TEX)
	vsout.TexCoord0.xyzw = input.TexCoord.xyxy * float4(1.0, 1.0, 501.0, 501.0);
#			else
	float3 inputDirection = normalize(input.Position.xyz);
	inputDirection.y += inputDirection.z;

	vsout.TexCoord0.x = 501 * acos(inputDirection.x);
	vsout.TexCoord0.y = 501 * asin(inputDirection.y);
#			endif  // TEX

#		elif defined(CLOUDS)
	vsout.TexCoord0.xy = TexCoordOff + input.TexCoord;
#		else
	vsout.TexCoord0.xy = input.TexCoord;
#		endif  // DITHER CLOUDS

#		ifdef TEXLERP
	vsout.TexCoord1.xy = TexCoordOff + input.TexCoord;
#		endif  // TEXLERP

	float3 skyColor = BlendColor[0].xyz * input.Color.xxx + BlendColor[1].xyz * input.Color.yyy +
	                  BlendColor[2].xyz * input.Color.zzz;

	vsout.Color.xyz = VParams * skyColor;
	vsout.Color.w = BlendColor[0].w * input.Color.w;

#	endif  // OCCLUSION MOONMASK HORIZFADE

	vsout.Position = mul(WorldViewProj[eyeIndex], inputPosition).xyww;
	vsout.WorldPosition = mul(World[eyeIndex], inputPosition);
	vsout.PreviousWorldPosition = mul(PreviousWorld[eyeIndex], inputPosition);
#	if defined(CLOUD_SHADOWS_CAPTURE)
	vsout.Position = GetCaptureClipPosition(vsout.WorldPosition.xyz - EyePosition[eyeIndex].xyz);
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

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
	float4 MotionVectors : SV_Target1;
	float4 Normal : SV_Target2;
#if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	float4 CloudShadows : SV_Target3;
#endif
};

#ifdef PSHADER
SamplerState SampBaseSampler : register(s0);
SamplerState SampBlendSampler : register(s1);
SamplerState SampNoiseGradSampler : register(s2);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexBlendSampler : register(t1);
Texture2D<float4> TexNoiseGradSampler : register(t2);

cbuffer PerGeometry : register(b2)
{
	float2 PParams : packoffset(c0);
};

#	if !defined(VR)
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif

#	include "Common/MotionBlur.hlsli"
#	include "Common/Permutation.hlsli"
#	include "Common/SharedData.hlsli"

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	if defined(PHYSICAL_SKY)
#		define PS_SKY_SAMPLERS
#		include "PhysicalSky/Common.hlsli"
#		if defined(TEX) && defined(CLOUDS)
#			define PS_CLOUDS
#		endif
#	endif

Texture2D<float> TexDepthSampler : register(t17);

#	if defined(PHYSICAL_SKY)
// (batch 37b) Upstream 23156dc5f. In the cubemap reflection pass TexApShadow (t64) still holds
// the main view's half-res shadow -- or nothing bound for this pass at all -- so indexing it
// with the reflection target's pixel gave the reflected sky random dark patches. There, take
// the shadow from the cloud shadow cube along the view direction instead.
bool UseReflectionSkyShadow()
{
	// (47o) ReflectionSkyFix is always on (the Legacy 36f path is gone).
	return (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0;
}

float GetReflectionSkyShadow(float3 viewDir)
{
#		if defined(CLOUD_SHADOWS)
	const float cloudCubeSample = CloudShadows::CloudShadowsTexture.SampleLevel(SampBaseSampler, viewDir, 0).x;
	return saturate(cloudCubeSample * SharedData::cloudShadowsSettings.Opacity);
#		else
	return 0.0;
#		endif
}

// (49h) The procedural sun (its glow, disc and replace mode) is retired: Physical Sky draws the game's sun with the Sky Fixes halo.

// (47a) "Sky Fixes": PhysicalSky::NightFlags bit 3 and the NightCbData fields it fills, named here so
// that SharedData.hlsli (which every Lighting shader includes) stays byte-identical.
namespace SkyFixes
{
	static const uint Flag = (1 << 3);
	// Relative sky brightening that does not count yet: the night base is drawn into the LUT (fp16,
	// bilinear) but recomputed here exactly, so a moonless night must not fade anything.
	static const float StarDeadZone = 0.05;

	bool On() { return (SharedData::physSkyNightData.Flags & Flag) != 0; }
	float StarFadeStrength() { return SharedData::physSkyNightData.pad1; }
	float NightBaseUnfade() { return SharedData::physSkyNightData.pad2; }

	// Four uniform numbers in [0, 1) for this pixel and frame (a different set for each a_salt).
	float4 PixelFrameRandom(float2 a_pxCoord, uint a_salt)
	{
		const uint3 h0 = Random::pcg3d(uint3(uint2(a_pxCoord), SharedData::FrameCount ^ (a_salt * 0x9E3779B9u)));
		const uint3 h1 = Random::pcg3d(h0 ^ 0x68E31DA4u);
		return float4(float3(h0 >> 8), float(h1.x >> 8)) * (1.0 / 16777216.0);
	}

	// LutGen.cs.hlsl's NightSkyBase: the weather's night gradient (x strength x fade).
	float3 NightSkyBase(float3 a_dir)
	{
		const SharedData::PhysSkyNightData night = SharedData::physSkyNightData;
		[flatten] if (a_dir.z >= 0.0)
			return lerp(night.BaseHorizon, night.BaseUpper, smoothstep(0.0, 0.5, a_dir.z));
		return lerp(night.BaseHorizon, night.BaseLower, smoothstep(0.0, 0.3, -a_dir.z));
	}

	// Stars and the Milky Way against the sky behind them. k = the physical sky's brightness in this
	// direction / the night base light at full strength there: 1 on a moonless night, more under a moon or
	// at dawn. The extra brightness (k - 1) drowns a texel in proportion to how faint it is itself (c0 =
	// its own brightness x alpha, before the weather tint and the clock fade), so the Milky Way's glow and
	// the faint stars go first and the brightest stars last. Moonless night: exactly 1.
	// (48b) a_moonDrown: "Moonlight Fades Stars" (MoonSky::StarDrown, 0 when off or no moon is up), added to the same
	// drowning amount; without Sky Fixes' own fade it is the only term. 0 gives exactly the 47a / 48a result.
	float StarVisibility(float3 a_dir, float a_texelBrightness, float a_moonDrown)
	{
		const bool skyFade = On() && StarFadeStrength() > 0.0;
		const float unfade = skyFade ? NightBaseUnfade() : 1.0;
		float visibility = 0.0;  // the night base light has faded out: daylight
		[branch] if (!skyFade)
			visibility = saturate(1.0 - a_moonDrown / max(a_texelBrightness, 1e-6));
		else [branch] if (unfade > 0.0)
		{
			float drown = a_moonDrown;
			const float base = Color::RGBToLuminance(NightSkyBase(a_dir)) * unfade;
			[branch] if (base > 0.0)
			{
				const float sky = Color::RGBToLuminance(PhysSky::TexSvLut.SampleLevel(PhysSky::SampSv, PhysSky::SkyViewLutUv(a_dir), 0).rgb);
				const float excess = max(sky / base - 1.0 - StarDeadZone, 0.0);
				drown += StarFadeStrength() * excess;
			}
			visibility = saturate(1.0 - drown / max(a_texelBrightness, 1e-6));
		}
		return visibility;
	}

#		if defined(TEX) && !defined(MOONMASK) && !defined(CLOUDS) && !defined(OCCLUSION)
	// The game's sun / sun glare picture. NAT's sun.dds keeps its halo in the alpha channel as 8-bit steps
	// tens to a hundred texels wide, each a visible ring over a dark sky; a dither on the output cannot hide
	// a step that is already in the texture, so the halo is blurred along the radius first (the picture is
	// radially symmetric; 8 taps with a per-pixel per-frame offset, so the taps fill the gaps over a few
	// frames), then dithered by +-0.5/255. The opaque disc body (alpha > 0.5) of the sun picture is kept.
	// Not multiplied by any transmittance: its brightness stays the game's.
	float SunHaloAlpha(float2 a_uv, float a_texAlpha, float a_vertexAlpha, bool a_hasBody, float2 a_pxCoord)
	{
		const float bodyWeight = a_hasBody ? smoothstep(0.3, 0.6, a_texAlpha) : 0.0;
		const float4 rnd = PixelFrameRandom(a_pxCoord, 1);
		float alpha = a_texAlpha;
		const float2 fromCentre = a_uv - 0.5;
		const float r = length(fromCentre);
		[branch] if (bodyWeight < 1.0 && r > 1e-4)
		{
			const float2 radial = fromCentre / r;
			// The steps get wider away from the centre: about 35 texels of a 2048 picture at the inner halo,
			// 90 at the outer edge.
			const float halfWidth = 0.1 * r + 0.004;
			float sum = 0.0;
			[unroll] for (int k = 0; k < 8; ++k)
			{
				const float t = ((k + rnd.x) * 0.125) * 2.0 - 1.0;
				// Capped so taps that reach into the disc do not light a ring around it.
				sum += min(TexBaseSampler.SampleLevel(SampBaseSampler, a_uv + radial * (t * halfWidth), 0).w, 0.3);
			}
			alpha = lerp(sum * 0.125, a_texAlpha, bodyWeight);
		}
		// Triangular dither, +-0.5/255, shrinking to nothing where the halo ends (no speckles around it).
		const float dither = (rnd.y + rnd.z - 1.0) * (0.5 / 255.0);
		alpha = max(alpha + dither * saturate(alpha * 510.0), 0.0);
		return a_vertexAlpha * alpha;
	}

	// Upstream 6491de101: the share of the sun's neighbourhood that shows open sky (16 depth taps in a 0.02
	// screen-UV disc around the glare's centre), so ridges between the sun and the camera hide the glare.
	// The centre comes from the quad itself: world position and texture coordinate are affine on the quad,
	// so their screen derivatives give the world point at uv (0.5, 0.5).
	Texture2D<float> TexGlareDepth : register(t65);  // bound only for the glare draw (PhysicalSky::BindGlareDepth)

	float GlareRidgeVisibility(float2 a_uv, float3 a_worldPos, float2 a_uvDx, float2 a_uvDy, float3 a_posDx, float3 a_posDy, uint a_eyeIndex)
	{
#			if defined(VR)
		return 1.0;
#			else
		if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection)
			return 1.0;
		const float det = a_uvDx.x * a_uvDy.y - a_uvDx.y * a_uvDy.x;
		[branch] if (!(abs(det) > 1e-14))
			return 1.0;
		const float2 toCentre = 0.5 - a_uv;
		const float a = (toCentre.x * a_uvDy.y - toCentre.y * a_uvDy.x) / det;
		const float b = (a_uvDx.x * toCentre.y - a_uvDx.y * toCentre.x) / det;
		const float3 centre = a_worldPos + a * a_posDx + b * a_posDy;
		const float4 centreCS = mul(FrameBuffer::CameraViewProj[a_eyeIndex], float4(centre, 1.0));
		[branch] if (centreCS.w <= 0.0)
			return 1.0;
		const float2 sunUV = centreCS.xy / centreCS.w * float2(0.5, -0.5) + 0.5;
		uint visible = 0, loaded = 0, empty = 0;
		[unroll] for (uint i = 0; i < 16; i++)
		{
			const float2 sampleUV = sunUV + Random::PoissonSampleOffsets16[i] * 0.02;
			[branch] if (FrameBuffer::IsOutsideFrame(sampleUV))
			{
				visible++;
				continue;
			}
			const float depth = TexGlareDepth.Load(SharedData::ConvertUVToSampleCoord(sampleUV, a_eyeIndex)).x;
			loaded++;
			visible += depth >= 1.0 ? 1 : 0;
			empty += depth <= 0.0 ? 1 : 0;
		}
		[branch] if (loaded > 0 && empty == loaded)
			return 1.0;  // nothing bound (or no usable depth): leave the glare as the game draws it
		return smoothstep(0.0, 1.0, visible / 16.0);
#			endif
	}
#		endif
}

// (49h stage 2) Physical Sky "Clouds, Stars and Water Follow": PhysicalSky::PhysLightFlags bit 14 in horizonFixData.Flags (named here, not
// in PhysicalLighting.hlsli, which Lighting includes); the star scale rides in horizonFixData.AnchorOffset (padding since 47m).
namespace PhysFollow
{
	static const uint FollowFlag = (1u << 14);
	/// The star layer's scale: the physical night base over the night base the stars were set against; 1 while off.
	float StarScale()
	{
		return (SharedData::horizonFixData.Flags & FollowFlag) != 0u ? max(SharedData::horizonFixData.AnchorOffset, 0.0) : 1.0;
	}
}

// (48b) "Moonlight Fades Stars" and "Moon Planetshine": PhysicalSky::NightFlags bits 6 / 7 and the two spare floats of
// PhysSkyMoonData (each holds two halves: x = Masser, y = Secunda), named here so SharedData.hlsli stays byte-identical.
namespace MoonSky
{
	static const uint StarFadeFlag = (1 << 6);
	static const uint PlanetshineFlag = (1 << 7);
	// Brightness of the planetshine grey, relative to the moon's lit colour: the mean luminance of the game's full
	// moon picture (masser_full.dds after the sky gamma, 0.26).
	static const float PlanetshineGrey = 0.25;

	bool StarFadeOn() { return (SharedData::physSkyNightData.Flags & StarFadeFlag) != 0; }
	bool PlanetshineOn() { return (SharedData::physSkyNightData.Flags & PlanetshineFlag) != 0; }

	float2 UnpackHalves(float a_packed)
	{
		const uint bits = asuint(a_packed);
		return float2(f16tof32(bits & 0xFFFF), f16tof32(bits >> 16));
	}
	// Drowning amount of each moon (phase x height x weather x daylight x its light, C++ UpdateMoonData).
	float2 StarFadeAmounts() { return UnpackHalves(SharedData::physSkyMoonData.pad0.x); }
	// Opacity of each moon's planetshine fill ((1 - phase) x strength x night, C++ UpdateMoonData).
	float2 PlanetshineAmounts() { return UnpackHalves(SharedData::physSkyMoonData.pad0.y); }

	// How much of the sky around a moon is clouded over (0 clear .. 1 covered): Cloud Shadows' cloud cube, five taps
	// within about 2 degrees of the moon so a cloud edge passing the disc does not switch the stars on and off.
	// Without Cloud Shadows there is no cloud term.
	float CloudCoverAt(float3 a_dir, SamplerState a_sampler)
	{
#		if defined(CLOUD_SHADOWS)
		const float3 up = abs(a_dir.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
		const float3 t = normalize(cross(up, a_dir)) * 0.035;
		const float3 b = cross(a_dir, t);
		float sum = CloudShadows::CloudShadowsTexture.SampleLevel(a_sampler, a_dir, 0).x;
		sum += CloudShadows::CloudShadowsTexture.SampleLevel(a_sampler, a_dir + t, 0).x;
		sum += CloudShadows::CloudShadowsTexture.SampleLevel(a_sampler, a_dir - t, 0).x;
		sum += CloudShadows::CloudShadowsTexture.SampleLevel(a_sampler, a_dir + b, 0).x;
		sum += CloudShadows::CloudShadowsTexture.SampleLevel(a_sampler, a_dir - b, 0).x;
		return saturate(sum * 0.2);
#		else
		return 0.0;
#		endif
	}

	// The moonlit sky's drowning amount for the star layer (same units as SkyFixes' T x excess): both moons, each
	// x (1 - cloud cover around it). 0 with no moon up.
	float StarDrown(SamplerState a_sampler)
	{
		const float2 amounts = StarFadeAmounts();
		float drown = 0.0;
		[branch] if (amounts.x > 0.0)
			drown += amounts.x * (1.0 - CloudCoverAt(normalize(SharedData::physSkyData.masserDir), a_sampler));
		[branch] if (amounts.y > 0.0)
			drown += amounts.y * (1.0 - CloudCoverAt(normalize(SharedData::physSkyData.secundaDir), a_sampler));
		return drown;
	}
}
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	float3 yyy = Color::Sky(PParams.yyy);

#	if !defined(OCCLUSION)
	// (batch 37c) A new moon: the game draws the moon with its new-moon texture, which is solid
	// opaque black (masser_new.dds / secunda_new.dds), plus the star mask under it, so the moon
	// shows as a black disc over the stars. Flagged per draw by PhysicalSky::SetSunDrawFlags while
	// "Hide New Moon Disc" is on; both draws are dropped and the stars stay visible.
	if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsNewMoon)
		discard;
#	endif
#	if !defined(VR)
	uint eyeIndex = 0;
#	else
	uint eyeIndex = input.EyeIndex;
#	endif  // !VR

#	if defined(PS_CLOUDS)
	float psCloudDist = 1e3f / 1.428e-2;
	float3 viewDir = normalize(input.WorldPosition.xyz);
#		if defined(CLOUD_SHADOWS) 
	if (SharedData::physSkyData.enabled)
		psCloudDist = CloudShadows::IntersectCloudDist(float3(0, 0, 0), viewDir);
#		endif
#	endif

#	ifndef OCCLUSION
#		ifndef TEXLERP
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	baseColor.xyz = Color::Sky(baseColor.xyz);
#			ifdef TEXFADE
	baseColor.w *= PParams.x;
#			endif
#		else
	float4 blendColor = TexBlendSampler.Sample(SampBlendSampler, input.TexCoord1.xy);
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	blendColor.xyz = Color::Sky(blendColor.xyz);
	baseColor.xyz = Color::Sky(baseColor.xyz);
	baseColor = PParams.xxxx * (-baseColor + blendColor) + baseColor;
#		endif

#		if defined(PS_CLOUDS) && defined(CLOUD_SHADOWS)
	// (44h) "Moonlight on Clouds": the moons' light, added after the weather's cloud colour below.
	float3 cloudMoonLight = 0.0;
	const float3 cloudAlbedo = baseColor.rgb;  // (49h stage 1, S2) the texture's colour, before the relight
	if (SharedData::physSkyData.enabled) {
		// (42b) "Cloud Lighting": Sun + Moons + Sky, or the single directional light as before.
		[branch] if (SharedData::physSkyCloudData.Flags & SharedData::PhysSkyCloudFlags::MultiLight) {
			cloudMoonLight = PhysSky::CloudMoonLight(baseColor, viewDir, float3(0, 0, 0) + viewDir * psCloudDist, PhysSky::SampTr, SampBaseSampler);
			baseColor.rgb = PhysSky::RelightCloudMulti(baseColor, viewDir, float3(0, 0, 0) + viewDir * psCloudDist, PhysSky::SampTr, PhysSky::SampSv, SampBaseSampler);
		} else
			baseColor.rgb = PhysSky::RelightCloud(baseColor, viewDir, float3(0, 0, 0) + viewDir * psCloudDist, PhysSky::SampTr, SampBaseSampler);
	}
#		endif

#		if defined(DITHER)
	// (48n, official CS #3104) Whole-texel per-frame offset into the 8x8 noise tile, so DLSS / TAA resolve the dither
	// instead of keeping (and upscaling) it as a fixed grid.
	float2 noiseGradUv = float2(0.125, 0.125) * (input.Position.xy + floor(Random::R2Sequence(SharedData::FrameCount) * 8.0));
	float noiseGrad =
		TexNoiseGradSampler.Sample(SampNoiseGradSampler, noiseGradUv).x * 0.03125 + -0.0078125;

#			ifdef TEX
	psout.Color.xyz = (Color::Sky(input.Color.xyz) * baseColor.xyz + yyy) + noiseGrad;
	psout.Color.w = baseColor.w * input.Color.w;
#			else
	psout.Color.xyz = (yyy + Color::Sky(input.Color.xyz)) + noiseGrad;
	psout.Color.w = input.Color.w;
#			endif  // TEX

#		elif defined(MOONMASK)
	psout.Color.xyzw = baseColor;

	if (baseColor.w - AlphaTestRefRS.x < 0) {
		discard;
	}

#		elif defined(HORIZFADE)
	psout.Color.xyz = float3(1.5, 1.5, 1.5) * (Color::Sky(input.Color.xyz) * baseColor.xyz + yyy);
	psout.Color.xyz *= PhysFollow::StarScale();  // (49h stage 2) stars keep their brightness against the physical night sky
	psout.Color.w = input.TexCoord2.x * (baseColor.w * input.Color.w);
#		else
	psout.Color.w = input.Color.w * baseColor.w;
	psout.Color.xyz = Color::Sky(input.Color.xyz) * baseColor.xyz + yyy;
#			if defined(PS_CLOUDS) && defined(CLOUD_SHADOWS)
	psout.Color.xyz += cloudMoonLight;  // (44h) not dimmed by the (near-black night) weather cloud colour
	// (49h stage 1, S2) "Clouds Get the Sky's Ambient Light": the ambient a white ground surface gets (the weather ambient's mean over
	// the sphere, converted as Lighting converts it) x the cloud's colour, not x the weather's (near-black at night) cloud colour, which
	// only gives the hue. The blend applies the cloud's opacity. Replaces the sky-view LUT term of RelightCloudMulti.
	[branch] if (SharedData::physSkyData.enabled && PhysLight::On(PhysLight::kCloudAmbient))
	{
		const float3 weatherCloud = max(Color::Sky(input.Color.xyz), 0.0);
		const float weatherLuma = Color::RGBToLuminance(weatherCloud);
		const float3 hue = weatherLuma > 1e-4 ? weatherCloud / weatherLuma : float3(1.0, 1.0, 1.0);
		const float3 ambient = Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(0, 0, 0, 1))));
		psout.Color.xyz += cloudAlbedo * Color::RGBToLuminance(ambient) * hue * SharedData::physSkyCloudData.SkyAmbient;
	}
#			endif
#		endif

#	else
	psout.Color = float4(0, 0, 0, 1.0);
#	endif  // OCCLUSION

#if defined(PHYSICAL_SKY)
#	if defined(TEX) && !defined(MOONMASK) && !defined(CLOUDS) && !defined(OCCLUSION)
	// (47a) Screen derivatives for the glare's ridge test, taken here in uniform control flow.
	const float2 glareUvDx = ddx(input.TexCoord0.xy);
	const float2 glareUvDy = ddy(input.TexCoord0.xy);
	const float3 glarePosDx = ddx(input.WorldPosition.xyz);
	const float3 glarePosDy = ddy(input.WorldPosition.xyz);
#	endif
	if (SharedData::physSkyData.enabled)
	{
# 		if defined(DITHER) && !defined(TEX)
		// SKY
		float3 skyColor;
		[branch] if (UseReflectionSkyShadow())
			skyColor = PhysSky::SampleSkyShadow(normalize(input.WorldPosition.xyz), GetReflectionSkyShadow(normalize(input.WorldPosition.xyz)), PhysSky::SampSv);
		else
			skyColor = PhysSky::SampleSky(normalize(input.WorldPosition.xyz), input.Position.xy, PhysSky::SampSv);
		psout.Color = lerp(float4(skyColor, 1.0), psout.Color, SharedData::physSkyData.vanillaMix);  // upstream 5846ad833 ((47o) SkyAlphaOpaque always on)

		// (47a) Sky Fixes: the main target (R11G11B10_FLOAT: 6 significant bits in red and green, 5 in blue)
		// shows smooth dark gradients as steps. Per pixel and frame, triangular, about half a step (2^-7
		// relative in red/green, 2^-6 in blue); the upscaler averages it out. Not in reflections.
		[branch] if (SkyFixes::On() && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))
		{
			const float4 rndA = SkyFixes::PixelFrameRandom(input.Position.xy, 2);
			const float4 rndB = SkyFixes::PixelFrameRandom(input.Position.xy, 3);
			const float3 tpdf = rndA.xyz + rndB.xyz - 1.0;
			psout.Color.xyz = max(psout.Color.xyz * (1.0 + tpdf * float3(1.0 / 128.0, 1.0 / 128.0, 1.0 / 64.0)), 0.0);
		}

#		elif defined(PS_CLOUDS)
		float4 apColor;
		[branch] if (UseReflectionSkyShadow())
			apColor = PhysSky::SampleApShadow(viewDir, psCloudDist, GetReflectionSkyShadow(viewDir), PhysSky::SampSv);
		else
			apColor = PhysSky::SampleAp(viewDir, input.Position.xy, psCloudDist, PhysSky::SampSv);
		psout.Color.xyz = psout.Color.xyz * apColor.a + apColor.rgb;
#		elif defined(HORIZFADE)
		// (47a) Sky Fixes: the star layer (stars and the Milky Way) fades as the physical sky behind it gets
		// brighter than the moonless night base (moonlight, dawn); the game's clock fade stays multiplied in.
		// (48b) "Moonlight Fades Stars": the moonlit sky (both moons in full, not their dimmed glow in the LUT) drowns the
		// same faint-first way, also without Sky Fixes. Off or no moon up: the 48a result exactly.
		const bool moonStarFade = MoonSky::StarFadeOn();
		[branch] if ((SkyFixes::On() && SkyFixes::StarFadeStrength() > 0.0) || moonStarFade)
			psout.Color.w *= SkyFixes::StarVisibility(normalize(input.WorldPosition.xyz), Color::RGBToLuminance(baseColor.xyz) * baseColor.w,
				moonStarFade ? MoonSky::StarDrown(SampBaseSampler) : 0.0);
#		elif defined(DEFERRED) && defined(TEX) && !defined(MOONMASK)
		// (batch 37c) Not the moon/star mask: it is TEX too. (49h) The procedural sun disc that was drawn here is retired.
		// (40d) Moon Physical Brightness: the vanilla disc (phase picture and fade alpha kept) is
		// brightened to moonlight / disc solid angle, so it stands out of its own sky glow.
		// IsMoon is only set on the two discs, never on the star mask.
		[branch] if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsMoon)  // (47o) MoonPhysicalRadiance always on
			psout.Color.xyz *= (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSecunda) ? SharedData::physSkyMoonData.SecundaDiskScale : SharedData::physSkyMoonData.MasserDiskScale;
		// (48b) "Moon Planetshine". The game's phase pictures are see-through on the unlit side (alpha 0), so in front of
		// the Milky Way that side showed as a starless hole. A faint grey fill now lies under the lit part, only where the
		// picture is unlit (alpha below 0.02-0.2; the lit side and its translucent parts are left exactly as they were),
		// inside the round disc (the pictures' disc fills the quad, edge at r ~0.97), with the game's horizon fade
		// (vertex alpha). The fill's colour is the sky drawn behind it plus the grey, so over the sky it only adds the
		// grey (x its opacity) and never darkens. Opacity from C++: strength x (1 - phase) x night; 0 at full and new moon.
		[branch] if (MoonSky::PlanetshineOn() && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsMoon))
		{
			const bool secunda = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSecunda) != 0;
			const float2 planetshine = MoonSky::PlanetshineAmounts();
			const float amount = secunda ? planetshine.y : planetshine.x;
			const float r = length(input.TexCoord0.xy * 2.0 - 1.0);
			const float fillAlpha = amount * (1.0 - smoothstep(0.90, 0.99, r)) * (1.0 - smoothstep(0.02, 0.2, baseColor.w)) * saturate(input.Color.w);
			[branch] if (fillAlpha > 0.0)
			{
				const float3 moonViewDir = normalize(input.WorldPosition.xyz);
				float3 behind;
				[branch] if (UseReflectionSkyShadow())
					behind = PhysSky::SampleSkyShadow(moonViewDir, GetReflectionSkyShadow(moonViewDir), PhysSky::SampSv);
				else
					behind = PhysSky::SampleSky(moonViewDir, input.Position.xy, PhysSky::SampSv);
				const float discLuminance = Color::RGBToLuminance(Color::Sky(input.Color.xyz)) *
				                            (secunda ? SharedData::physSkyMoonData.SecundaDiskScale : SharedData::physSkyMoonData.MasserDiskScale);
				// Slightly darker towards the limb, as the lit side.
				const float grey = discLuminance * MoonSky::PlanetshineGrey * (0.6 + 0.4 * sqrt(saturate(1.0 - r * r)));
				const float3 fillColor = max(behind, 0.0) + grey;
				const float litAlpha = psout.Color.w;
				const float outAlpha = litAlpha + fillAlpha * (1.0 - litAlpha);
				psout.Color.xyz = (psout.Color.xyz * litAlpha + fillColor * (fillAlpha * (1.0 - litAlpha))) / max(outAlpha, 1e-6);
				psout.Color.w = outAlpha;
			}
		}
		// (42b) Moons Fade in Daylight. The disc is drawn in the weather's moon colour whatever the time
		// of day; the physical sky by day is far darker than that colour (it is brought up later by the
		// exposure), so a daytime moon stood out as at night. A real daytime moon is pale: the bright
		// air in front of it adds to it. Here the disc becomes more see-through as the sky around it
		// gets brighter compared with the disc: alpha x 1 / (1 + strength x day x sky / disc).
		// The sky is read 30 degrees to either side of the moon, outside the moon's own glow, and the
		// whole effect is gated by daylight (sun -6 -> +6 degrees), so nights are never changed.
		[branch] if ((SharedData::physSkyCloudData.Flags & SharedData::PhysSkyCloudFlags::MoonDaylightFade) && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsMoon))
		{
			const float3 moonViewDir = normalize(input.WorldPosition.xyz);
			const float rotCos = 0.8660254, rotSin = 0.5;
			const float3 sideA = float3(moonViewDir.x * rotCos - moonViewDir.y * rotSin, moonViewDir.x * rotSin + moonViewDir.y * rotCos, moonViewDir.z);
			const float3 sideB = float3(moonViewDir.x * rotCos + moonViewDir.y * rotSin, -moonViewDir.x * rotSin + moonViewDir.y * rotCos, moonViewDir.z);
			const float day = smoothstep(-0.1045285, 0.1045285, normalize(SharedData::physSkyNightData.LutSunDir).z);
			const float skyLuminance = day * 0.5 *
			                           (Color::RGBToLuminance(PhysSky::TexSvLut.SampleLevel(PhysSky::SampSv, PhysSky::SkyViewLutUv(sideA), 0).rgb) +
										   Color::RGBToLuminance(PhysSky::TexSvLut.SampleLevel(PhysSky::SampSv, PhysSky::SkyViewLutUv(sideB), 0).rgb));
			float discLuminance = Color::RGBToLuminance(Color::Sky(input.Color.xyz));
			discLuminance *= (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSecunda) ? SharedData::physSkyMoonData.SecundaDiskScale : SharedData::physSkyMoonData.MasserDiskScale;
			psout.Color.w *= rcp(1.0 + SharedData::physSkyCloudData.MoonDaylightFade * max(skyLuminance, 0.0) / max(discLuminance, 1e-4));
		}
#		endif

#		if defined(TEX) && !defined(MOONMASK) && !defined(CLOUDS) && !defined(OCCLUSION)
		// (47a) Sky Fixes: the game's sun picture (while the procedural sun is off; with it on the disc code
		// above owns that quad) and its glare: halo smoothed and dithered; the glare also hides behind ridges.
		[branch] if (SkyFixes::On())
		{
			const bool vanillaSunQuad = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun) != 0;  // (49h) no procedural sun any more
			const bool sunGlareQuad = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSunGlare) != 0;
			[branch] if (vanillaSunQuad || sunGlareQuad)
			{
				psout.Color.w = SkyFixes::SunHaloAlpha(input.TexCoord0.xy, baseColor.w, input.Color.w, vanillaSunQuad, input.Position.xy);
				[branch] if (sunGlareQuad)
					psout.Color.w *= SkyFixes::GlareRidgeVisibility(input.TexCoord0.xy, input.WorldPosition.xyz, glareUvDx, glareUvDy, glarePosDx, glarePosDy, eyeIndex);
			}
		}
#		endif

	}
#endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);

	psout.MotionVectors = float4(screenMotionVector, 0, psout.Color.w);
	psout.Normal = float4(0.5, 0.5, 0, psout.Color.w);

// #	if defined(DEFERRED) && defined(CLOUDS) && defined(CLOUD_SHADOWS) && defined(PHYSICAL_SKY)
// 	float3 viewPosition = normalize(input.WorldPosition.xyz);
// 	float brightness = saturate(dot(SharedData::DirLightDirection.xyz, viewPosition) * 0.5 + 0.5);

// 	if (brightness > 0.0 && baseColor.w > 0.0)
// 	{
// 		float noise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

// 		float rayStep = 1.0 / 32.0;
// 		float rayPos = rayStep * noise; 
// 		float4 rayShadow = 0.0;

// 		float3 startPosition = viewPosition;
// 		float3 endPosition = SharedData::DirLightDirection.xyz;

// 		float3 PoissonDisc[] = {
// 			float3(0.460921f, 0.615192f, 0.887539f),
// 			float3(0.757347f, 0.911008f, 0.189581f),
// 			float3(0.548753f, 0.145482f, 0.0548723f),
// 			float3(0.90051f, 0.157048f, 0.623493f)
// 		};

// 		for(int i = 0; i < 4; i++)
// 		{		
// 			float3 raySample = normalize(lerp(startPosition, endPosition, rayPos));

// 			raySample += (PoissonDisc[i] * 2.0 - 1.0) * 0.01;

// 			if (raySample.z < 0.0)
// 				rayShadow[i % 4] += -raySample.z; // World shadow
// 			else
// 				rayShadow[i % 4] = max(rayShadow[i % 4], CloudShadows::CloudShadowsTexture.SampleLevel(SampBaseSampler, raySample, 0).x);

// 			rayPos += rayStep;
// 		}

// 		psout.Color.xyz += baseColor.a * baseColor.xyz * brightness * (1.0 - saturate(dot(rayShadow, 0.25))) * SharedData::DirLightColor.xyz;
// 	}
// #	endif

#	if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	psout.CloudShadows = float4(1, 1, 1, psout.Color.w);

	float depth = TexDepthSampler.Load(int3(input.Position.xy, 0));
	if (depth < input.Position.z)
		psout.Color.w = 0;

#	endif

	return psout;
}
#endif
