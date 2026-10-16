#include "Common/Color.hlsli"
#include "Common/FastMath.hlsli"
#include "Common/FrameBuffer.hlsli"
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
	return (SharedData::physSkyExtData.Flags & SharedData::PhysSkyExtFlags::ReflectionSkyFix) &&
	       (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection);
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

// (batch 37c) Glow around the procedural sun, drawn into the sky dome. The disk alone is a few
// dozen pixels of "far above white": the tonemapper turns it into a flat white dot, and nothing
// spreads it (vanilla bloom is skipped by "Disable Vanilla Tonemapping", COD Bloom is opt-in).
// Real eyes and cameras see the sun through a bright scattered glow; this adds it, coloured by
// the same transmittance as the disk so noon is white and sunset orange. Units: 1 = a sunlit
// white diffuse wall (sunlight / pi). Not in reflections (the water shader adds its own sun).
float3 GetProceduralSunGlow(float3 viewDir)
{
	const float intensity = SharedData::physSkyExtData.SunGlowIntensity;
	const float diskCos = SharedData::physSkyData.sunDiskCos;
	[branch] if (intensity <= 0.0 || diskCos <= 0.0)
		return 0.0;

	const float3 sunDir = normalize(SharedData::physSkyData.sunDir);
	const float cosTheta = dot(viewDir, sunDir);
	[branch] if (cosTheta <= 0.0)
		return 0.0;

	// atan2 form: acos() of a cosine this close to 1 loses most of its precision in fp32.
	const float theta = atan2(length(cross(viewDir, sunDir)), cosTheta);
	const float diskRadius = atan2(sqrt(saturate(1.0 - diskCos * diskCos)), diskCos);
	const float x = max(theta - diskRadius, 0.0);
	const float width = max(SharedData::physSkyExtData.SunGlowWidth, 1e-4);
	const float falloff = 0.85 * exp(-x / width) + 0.15 * exp(-x / (5.0 * width));

	const float3 sunTransmittance = PhysSky::SampleTr(sunDir, PhysSky::SampTr);
	return SharedData::physSkyData.sunlightColor * sunTransmittance * (intensity * falloff / Math::PI);
}

// (46z) PhysicalSky::ExtFlags bits added in 46z. Defined here (and in LutGen.cs.hlsl) instead of
// SharedData.hlsli, which every Lighting shader includes.
namespace PhysSkyExt46z
{
	static const uint SunHaloSmooth = (1 << 10);     // A1 "Vanilla Sun Halo": Smooth
	static const uint SunHaloOff = (1 << 11);        // A1: Off (disc body only)
	static const uint SkyDitherFine = (1 << 12);     // A2 "Sky Dither": R11G11B10-matched
	static const uint SkyDitherVanilla = (1 << 13);  // A2: vanilla noise at full strength
	static const uint StarsFade = (1 << 15);         // D1 "Stars Fade With Sky Brightness"
}

// (46z) Four uniform numbers in [0, 1) for this pixel and frame (a different set for each a_salt).
float4 PixelFrameRandom(float2 pxCoord, uint a_salt)
{
	const uint3 h0 = Random::pcg3d(uint3(uint2(pxCoord), SharedData::FrameCount ^ (a_salt * 0x9E3779B9u)));
	const uint3 h1 = Random::pcg3d(h0 ^ 0x68E31DA4u);
	return float4(float3(h0 >> 8), float(h1.x >> 8)) * (1.0 / 16777216.0);
}

#		if defined(TEX)
// (46z, A1) Alpha of the game's sun / sun glare picture under "Vanilla Sun Halo". NAT's sun.dds keeps
// its whole halo in the alpha channel, stored as 8-bit steps (12/255 ... 1/255 then 0) tens to a
// hundred texels wide: over the dark physical sky at dawn each step is a visible ring. Smooth blurs the
// halo along the radius (the picture is radially symmetric; 8 taps, a per-pixel per-frame offset so the
// taps fill the gaps over a few frames) and fades it in with the LUT sun (-1 -> +2 degrees) x the light
// left after the air towards the sun, then dithers it by +-0.5/255. The opaque disc body (alpha > 0.5)
// is left alone; it shows or hides as the game (or Sky Sync's "Vanilla Sun Visibility") decides.
// Off keeps the body only.
float SmoothedSunHaloAlpha(float2 uv, float texAlpha, float bodyWeight, float2 pxCoord)
{
	const float4 rnd = PixelFrameRandom(pxCoord, 1);
	float alpha = texAlpha;
	const float2 fromCentre = uv - 0.5;
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
			sum += min(TexBaseSampler.SampleLevel(SampBaseSampler, uv + radial * (t * halfWidth), 0).w, 0.3);
		}
		alpha = lerp(sum * 0.125, texAlpha, bodyWeight);
	}

	const float3 lutSunDir = normalize(SharedData::physSkyNightData.LutSunDir);
	float fade = smoothstep(-0.0174524, 0.0348995, lutSunDir.z);  // sin(-1 deg), sin(2 deg)
	[branch] if (fade > 0.0)
	{
		const float3 sunTransmittance = PhysSky::SampleTr(lutSunDir, PhysSky::SampTr);
		fade *= saturate(max(sunTransmittance.r, max(sunTransmittance.g, sunTransmittance.b)));
	}
	alpha *= lerp(fade, 1.0, bodyWeight);

	// Triangular dither, +-0.5/255, shrinking to nothing where the halo ends (no speckles around it).
	const float dither = (rnd.y + rnd.z - 1.0) * (0.5 / 255.0);
	return max(alpha + dither * saturate(alpha * 510.0), 0.0);
}

float VanillaSunHaloAlpha(float2 uv, float texAlpha, float vertexAlpha, bool hasBody, bool smooth, float2 pxCoord)
{
	const float bodyWeight = hasBody ? smoothstep(0.3, 0.6, texAlpha) : 0.0;
	float alpha = texAlpha * bodyWeight;  // Off
	[branch] if (smooth)
		alpha = SmoothedSunHaloAlpha(uv, texAlpha, bodyWeight, pxCoord);
	return vertexAlpha * alpha;
}
#		endif
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
	float2 noiseGradUv = float2(0.125, 0.125) * input.Position.xy;
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
	psout.Color.w = input.TexCoord2.x * (baseColor.w * input.Color.w);
#		else
	psout.Color.w = input.Color.w * baseColor.w;
	psout.Color.xyz = Color::Sky(input.Color.xyz) * baseColor.xyz + yyy;
#			if defined(PS_CLOUDS) && defined(CLOUD_SHADOWS)
	psout.Color.xyz += cloudMoonLight;  // (44h) not dimmed by the (near-black night) weather cloud colour
#			endif
#		endif

#	else
	psout.Color = float4(0, 0, 0, 1.0);
#	endif  // OCCLUSION

#if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
	{
# 		if defined(DITHER) && !defined(TEX)
		// SKY
		float3 skyColor;
		[branch] if (UseReflectionSkyShadow())
			skyColor = PhysSky::SampleSkyShadow(normalize(input.WorldPosition.xyz), GetReflectionSkyShadow(normalize(input.WorldPosition.xyz)), PhysSky::SampSv);
		else
			skyColor = PhysSky::SampleSky(normalize(input.WorldPosition.xyz), input.Position.xy, PhysSky::SampSv);
		if (!(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))
			skyColor += GetProceduralSunGlow(normalize(input.WorldPosition.xyz));
		[branch] if (SharedData::physSkyExtData.Flags & SharedData::PhysSkyExtFlags::SkyAlphaOpaque)
			psout.Color = lerp(float4(skyColor, 1.0), psout.Color, SharedData::physSkyData.vanillaMix);  // upstream 5846ad833
		else
			psout.Color.xyz = lerp(skyColor, psout.Color.xyz, SharedData::physSkyData.vanillaMix);

		// (46z, A2) "Sky Dither". Only the Vanilla Mix share of the vanilla sky's noise was left, and the
		// main target (R11G11B10_FLOAT: 6 significant bits in red and green, 5 in blue) shows smooth dark
		// gradients as steps. Fine: per pixel and frame, triangular, about half a step (2^-7 relative in
		// red/green, 2^-6 in blue); DLSS averages it out. Vanilla: the dome's own noise texture in full,
		// made zero-mean so it does not lift the sky. Not in reflections (cubemaps keep their noise).
		const uint skyDitherFlags = SharedData::physSkyExtData.Flags & (PhysSkyExt46z::SkyDitherFine | PhysSkyExt46z::SkyDitherVanilla);
		[branch] if (skyDitherFlags && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))
		{
			[branch] if (skyDitherFlags & PhysSkyExt46z::SkyDitherFine)
			{
				const float4 rndA = PixelFrameRandom(input.Position.xy, 2);
				const float4 rndB = PixelFrameRandom(input.Position.xy, 3);
				const float3 tpdf = rndA.xyz + rndB.xyz - 1.0;
				psout.Color.xyz = max(psout.Color.xyz * (1.0 + tpdf * float3(1.0 / 128.0, 1.0 / 128.0, 1.0 / 64.0)), 0.0);
			}
			else
				psout.Color.xyz = max(psout.Color.xyz + (noiseGrad - 0.0078125) * (1.0 - SharedData::physSkyData.vanillaMix), 0.0);
		}

#		elif defined(PS_CLOUDS)
		float4 apColor;
		[branch] if (UseReflectionSkyShadow())
			apColor = PhysSky::SampleApShadow(viewDir, psCloudDist, GetReflectionSkyShadow(viewDir), PhysSky::SampSv);
		else
			apColor = PhysSky::SampleAp(viewDir, input.Position.xy, psCloudDist, PhysSky::SampSv);
		psout.Color.xyz = psout.Color.xyz * apColor.a + apColor.rgb;
#		elif defined(HORIZFADE)
		// (46z, D1) "Stars Fade With Sky Brightness". The star layer (stars and any Milky Way painted into
		// the same picture) comes in the weather's colour by the clock; the vanilla dawn sky was bright
		// enough to drown it, the physical sky is not. Like the moons by day (42b): alpha x 1 / (1 +
		// strength x sky / star), with the sky luminance of the sky-view LUT behind each star pixel, so
		// stars near the bright horizon go first and bright stars last. Enabled from astronomical twilight
		// (LUT sun -18 -> -9 degrees): full nights are unchanged (no fading next to a moonlit glow).
		[branch] if (SharedData::physSkyExtData.Flags & PhysSkyExt46z::StarsFade)
		{
			const float twilight = smoothstep(-0.309017, -0.156434, normalize(SharedData::physSkyNightData.LutSunDir).z);  // sin(-18), sin(-9)
			[branch] if (twilight > 0.0)
			{
				const float3 starViewDir = normalize(input.WorldPosition.xyz);
				const float skyLuminance = Color::RGBToLuminance(PhysSky::TexSvLut.SampleLevel(PhysSky::SampSv, PhysSky::SkyViewLutUv(starViewDir), 0).rgb);
				const float starLuminance = Color::RGBToLuminance(psout.Color.xyz);
				const float strength = twilight * SharedData::physSkyNightData.pad0;
				psout.Color.w *= rcp(1.0 + strength * max(skyLuminance, 0.0) / max(starLuminance, 1e-4));
			}
		}
#		elif defined(DEFERRED) && defined(TEX) && !defined(MOONMASK)
		// (batch 37c) Not the moon/star mask: it is TEX too, but a sun disk has no business on it.
		const uint extFlags = SharedData::physSkyExtData.Flags;
		// (40d) Moon Physical Brightness: the vanilla disc (phase picture and fade alpha kept) is
		// brightened to moonlight / disc solid angle, so it stands out of its own sky glow.
		// IsMoon is only set on the two discs, never on the star mask.
		[branch] if ((extFlags & SharedData::PhysSkyExtFlags::MoonPhysicalRadiance) && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsMoon))
			psout.Color.xyz *= (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSecunda) ? SharedData::physSkyMoonData.SecundaDiskScale : SharedData::physSkyMoonData.MasserDiskScale;
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
			[branch] if (extFlags & SharedData::PhysSkyExtFlags::MoonPhysicalRadiance)
				discLuminance *= (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSecunda) ? SharedData::physSkyMoonData.SecundaDiskScale : SharedData::physSkyMoonData.MasserDiskScale;
			psout.Color.w *= rcp(1.0 + SharedData::physSkyCloudData.MoonDaylightFade * max(skyLuminance, 0.0) / max(discLuminance, 1e-4));
		}
		// (batch 37b) Replace mode: the disk lives on the vanilla sun quad only (main view, not
		// reflections), and the quad's own texture is dropped. Without it the disk is added on
		// top of every TEX quad as in 37a -- including the glare quad.
		const bool replaceSun = (extFlags & SharedData::PhysSkyExtFlags::SunReplace) && SharedData::physSkyData.sunDiskCos > 0.0 &&
		                        (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) &&
		                        !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection);
		const bool isSunQuad = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun) != 0;

		float3 sunDir = normalize(SharedData::physSkyData.sunDir);
		float cosTheta = saturate(dot(normalize(input.WorldPosition.xyz), sunDir));
		if (cosTheta > SharedData::physSkyData.sunDiskCos && SharedData::physSkyData.sunDiskCos > 0.0 && (!replaceSun || isSunQuad))
		{
			float sunDiskSin = sqrt(1.0 - SharedData::physSkyData.sunDiskCos * SharedData::physSkyData.sunDiskCos);
			float tanTheta = sqrt(1.0 - cosTheta * cosTheta) / cosTheta;
			float normDist = tanTheta * SharedData::physSkyData.sunDiskCos * rcp(sunDiskSin);
			float3 limbFactor = PhysSky::LimbDarkenHestroffer(normDist);

			// (batch 37b) TrLut edge fix: the LUT through Physical Sky's own clamp sampler (upstream
			// fec65ed15 swapped samplers for the same reason) instead of the sun texture's sampler.
			float3 sunTransmittance;
			[branch] if (extFlags & SharedData::PhysSkyExtFlags::TrLutEdgeFix)
				sunTransmittance = PhysSky::SampleTr(normalize(input.WorldPosition.xyz), PhysSky::SampTr);
			else
				sunTransmittance = PhysSky::SampleTr(normalize(input.WorldPosition.xyz), SampBaseSampler);

			float3 dirLightColor;
			[branch] if (extFlags & SharedData::PhysSkyExtFlags::SunPhysicalRadiance)
			{
				// Upstream 728eedd61 + d08484aef: the light colour is an irradiance, the disk shows
				// radiance = irradiance / solid angle. Capped, keeping the hue (upstream clamps each
				// channel at a fixed 62250).
				const float sunSolidAngle = Math::TAU * (1.0 - SharedData::physSkyData.sunDiskCos);
				float3 radiance = SharedData::physSkyData.sunlightColor / max(sunSolidAngle, 1e-6);
				radiance *= sunTransmittance;
				const float peak = max(max(radiance.r, radiance.g), radiance.b);
				radiance *= min(1.0, SharedData::physSkyExtData.SunRadianceCap / max(peak, 1e-6));
				dirLightColor = radiance * limbFactor;
			}
			else
			{
				dirLightColor = SharedData::physSkyData.sunlightColor * limbFactor;
				dirLightColor *= sunTransmittance;
			}

			[branch] if (extFlags & SharedData::PhysSkyExtFlags::SunSoftEdge)
			{
				// Upstream fec65ed15: fade the outer 1/8 of the disk instead of a hard cut.
				dirLightColor *= saturate(8.0 * (cosTheta - SharedData::physSkyData.sunDiskCos) / (1.0 - SharedData::physSkyData.sunDiskCos));
			}

			if (replaceSun)
				psout.Color.xyz = dirLightColor;
			else
				psout.Color.xyz += dirLightColor;
			psout.Color.w = 1.0;
		} else if (replaceSun && isSunQuad) {
			// Upstream 735ec68e4: the rest of the vanilla sun quad is cleared.
			psout.Color = 0.0;
		}
#		endif

#		if defined(TEX) && !defined(MOONMASK) && !defined(CLOUDS) && !defined(OCCLUSION)
		// (46z, A1) "Vanilla Sun Halo (with Physical Sky)": the game's sun picture while the procedural
		// sun is off (with it on, the disc code above owns that quad), and the sun glare picture.
		{
			const uint sunHaloFlags = SharedData::physSkyExtData.Flags & (PhysSkyExt46z::SunHaloSmooth | PhysSkyExt46z::SunHaloOff);
			const bool vanillaSunQuad = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun) && SharedData::physSkyData.sunDiskCos <= 0.0;
			const bool sunGlareQuad = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSunGlare) != 0;
			[branch] if (sunHaloFlags && (vanillaSunQuad || sunGlareQuad))
				psout.Color.w = VanillaSunHaloAlpha(input.TexCoord0.xy, baseColor.w, input.Color.w, vanillaSunQuad,
					(sunHaloFlags & PhysSkyExt46z::SunHaloSmooth) != 0, input.Position.xy);
		}
#		endif

#		if !defined(OCCLUSION)
		// (batch 37b) Optionally hide the vanilla sun glare while the procedural sun is on.
		if ((SharedData::physSkyExtData.Flags & SharedData::PhysSkyExtFlags::HideSunGlare) && SharedData::physSkyData.sunDiskCos > 0.0 &&
			(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSunGlare))
			psout.Color = 0.0;
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
