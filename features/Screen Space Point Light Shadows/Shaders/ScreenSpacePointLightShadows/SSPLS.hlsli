#ifndef SSPLS_COMMON
#define SSPLS_COMMON

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

#define MAX_SAMPLES 4

namespace ScreenSpacePointLightShadows
{
	/// (46y) Screen-Space Shadows for Non-RT Casters: is the visible surface at this screen UV a ray-traced caster? The point-light
	/// trace marks it in bit 7 of the 4th byte of its per-pixel source ids (RayTracing/PointLightShadows.hlsli Sources, included
	/// before this file). False where nothing was traced (outside its area, no light near) and without Ray Tracing.
	bool IsRtCaster(float2 a_uv, uint a_eyeIndex)
	{
#if defined(RT_POINT_LIGHT_SHADOWS_HLSLI)
		const int3 texel = SharedData::ConvertUVToSampleCoord(saturate(a_uv), a_eyeIndex);
		return (uint(RTPointShadows::Sources.Load(texel).w * 255.0 + 0.5) & 0x80u) != 0u;
#else
		return false;
#endif
	}

	/// (46y) a_rtContact < 0: every occluder counts (as before). Else an occluder sample that is a ray-traced caster counts only
	/// within a_rtContact view units of the start (the ray tracer handles it farther along the ray).
	bool IgnoreOccluder(float a_rtContact, float a_travelled, float2 a_uv, uint a_eyeIndex)
	{
		bool ignore = false;
		[branch] if (a_rtContact >= 0.0 && a_travelled > a_rtContact)
			ignore = IsRtCaster(a_uv, a_eyeIndex);
		return ignore;
	}

	float Raymarch(SamplerState s, float3 viewPosition, float3 lightDirectionVS, uint steps, float step, float2 stepScale, float compareToleranceScale, float radius, float2x2 rotationMatrix, uint a_eyeIndex = 0, float a_rtContact = -1.0)
	{
		float opacity = 1.0;
		radius *= SharedData::ssplsSettings.SoftShadowScale * 0.0001;
		const float3 startPosition = viewPosition;
		const float3 normalizedLightDirection = normalize(lightDirectionVS);

		float totalCompareTolerance = abs(lightDirectionVS.z - viewPosition.z) * step * compareToleranceScale;

		[loop] for (uint i = 0; i < steps; i++)
		{
			// Step the ray
			const float stepScaleMult = steps == 1 ? 1.0 : (stepScale.x + i * (stepScale.y - stepScale.x) / (steps - 1));
			viewPosition += lightDirectionVS * step * stepScaleMult;
			float compareTolerance = totalCompareTolerance * stepScaleMult;
			float2 rayUV = FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex);

			// Ensure the UV coordinates are inside the screen
			if (!(rayUV.x >= 0.0 && rayUV.x <= 1.0 && rayUV.y >= 0.0 && rayUV.y <= 1.0))
				break;

			// Compute the difference between the ray's and the camera's depth
			float rayDepth = SharedData::GetScreenDepth(rayUV, a_eyeIndex);
			if (rayDepth < 16.5 || rayDepth > SharedData::ssplsSettings.MaxDistance)
				break;

			float depthDelta = viewPosition.z - rayDepth;
			const float distanceScale = 1 - rayDepth / SharedData::ssplsSettings.MaxDistance;
			if (rayDepth != startPosition.z) {
				// (46y) Non-RT casters: a traced occluder past the contact length is the ray tracer's.
				const float travelled = length(viewPosition - startPosition);
				[branch] if (i == 0 || !SharedData::ssplsSettings.EnableSoftShadows)
				{
					bool hit = abs(depthDelta - compareTolerance) < compareTolerance;
					if (hit && !IgnoreOccluder(a_rtContact, travelled, rayUV, a_eyeIndex)) {
						return 0.0;
					}
				}
				else
				{
					float distanceMult = abs(rayDepth - startPosition.z) / (abs(lightDirectionVS.z - rayDepth) + 0.00001) * distanceScale;
					const uint sampleTimes = min(MAX_SAMPLES, (uint)(i >> 2) + 1);
					float sampleOpacity = 1.0;
					bool hit = (depthDelta > 0.5 * compareTolerance) && (depthDelta < compareTolerance * 1.5);
					bool possibleHit = (depthDelta + compareTolerance > 0.0) && (depthDelta < compareTolerance * 3.0);
					if (hit && !IgnoreOccluder(a_rtContact, travelled, rayUV, a_eyeIndex)) {
						return 0.0;
					}
					if (possibleHit)
						[loop] for (uint j = 0; j < sampleTimes; j++)
						{
							float2 sampleOffset = mul(Random::PoissonSampleOffsets16[(j * i) % 16], rotationMatrix);
							float2 sampleUV = sampleOffset * distanceMult * radius + rayUV;
							float sampleDepth = SharedData::GetScreenDepth(sampleUV, a_eyeIndex);

							float sampleDepthDelta = viewPosition.z - sampleDepth;
							bool hit = abs(sampleDepthDelta - compareTolerance) < compareTolerance;
							if (hit && !IgnoreOccluder(a_rtContact, travelled, sampleUV, a_eyeIndex)) {
								sampleOpacity -= 1.0 / sampleTimes;
							}
						}
					opacity = min(opacity, sampleOpacity);
				}
				if (opacity <= 0.05) {
					return opacity;
				}
			}
		}

		return opacity;
	}

	// (45 SSPLS) Switches packed into SSPLSSettings.Enable (the struct size is pinned by the
	// FeatureData layout); C++ sends Enable = 0 when the feature is off, so `Enable != 0` still
	// means "on" in Lighting.hlsl / Water.hlsl.
	namespace SettingFlags
	{
		static const uint Enable = (1 << 0);
		static const uint FixedMath = (1 << 1);
		static const uint StableNoise = (1 << 2);
		static const uint CoverageView = (1 << 3);  // (46y) Ray Tracing debug view SSPLS Coverage, drawn by Lighting.hlsl
		static const uint SkipTranslucent = (1 << 4);  // (47h) not in forward (non-DEFERRED) Lighting draws: they read the depth behind them
		static const uint SkipWeakLights = (1 << 5);   // (47k) no march for a light far weaker than the pixel's strongest
		static const uint SmarterSteps = (1 << 6);     // (47k) GetShadowFixed: start past the receiver, >= 2 px steps, contact rays 2-3 steps
		static const uint SkipOnWater = (1 << 7);      // (47k) Water.hlsl: no screen-space light shadows
		static const uint MaxLightsShift = 8;          // (47k) bits 8-11: Max Shadowed Lights per Pixel (0 = unlimited)
	}

	/// (47k) Max Shadowed Lights per Pixel (1..7; 0 = unlimited, 47j).
	uint MaxShadowedLights()
	{
		return (SharedData::ssplsSettings.Enable >> SettingFlags::MaxLightsShift) & 0xFu;
	}

	/// (47k) How much one light matters at this pixel for the light budget: its colour's luminance (with falloff and fade) x N.L.
	/// No texture reads (the same value in the scoring pre-pass and the lighting loop).
	float LightScore(float3 a_lightColor, float a_lightAngle)
	{
		return dot(max(a_lightColor, 0.0), float3(0.2126, 0.7152, 0.0722)) * saturate(a_lightAngle);
	}

	/// (47k) Keeps the eight largest scores, descending, in a (first four) and b (next four).
	void InsertScore(inout float4 a, inout float4 b, float x)
	{
		float t;
		[unroll] for (uint i = 0; i < 4; ++i)
		{
			t = max(a[i], x);
			x = min(a[i], x);
			a[i] = t;
		}
		[unroll] for (uint j = 0; j < 4; ++j)
		{
			t = max(b[j], x);
			x = min(b[j], x);
			b[j] = t;
		}
	}

	/// (47k) The i-th largest score (0-based, i < 8).
	float ScoreAt(float4 a, float4 b, uint i)
	{
		const uint k = i & 3u;
		const float4 sel = float4(k == 0u, k == 1u, k == 2u, k == 3u);
		return dot(i < 4u ? a : b, sel);
	}

	/// (47k) The share of SSPLS strength a light gets from the light budget: 0 = not marched (lit unshadowed by SSPLS), 1 = full.
	/// a_k: the budget (0 = unlimited), a_top / a_next: the budget's last score and the first one past it, a_strongest: the pixel's
	/// strongest; a_skipWeak: Skip Weak Lights.
	float BudgetWeight(float a_score, uint a_k, float a_last, float a_next, float a_strongest, bool a_skipWeak)
	{
		float w = 1.0;
		[branch] if (a_k > 0u)
		{
			// In the top K; the ones close to the first light out fade with it (no seam when the set changes).
			w = a_score >= a_last * 0.999 && a_score > 0.0 ? (a_next > 0.0 ? smoothstep(1.0, 1.5, a_score / a_next) : 1.0) : 0.0;
		}
		[branch] if (a_skipWeak)
			w *= smoothstep(0.035, 0.07, a_score / max(a_strongest, 1e-8)) * smoothstep(0.001, 0.002, a_score);
		return w;
	}

	/// Legacy (pre-45) ray march setup. noiseOffset / noiseRotation are both the caller's noise when
	/// Stable Noise is off, which keeps this path identical to the old one.
	float GetShadowLegacy(SamplerState s, float3 viewPosition, float noiseOffset, float noiseRotation, float offsetAmplitude, float3 lightDirectionVS, uint steps, float radius, uint a_eyeIndex, bool isShadowCaster, float a_rtContact)
	{
		const float rayLength = SharedData::ssplsSettings.RayLength;
		if (rayLength <= 0.0)
			return 1.0;
		lightDirectionVS *= rayLength;

		// Offset with interleaved gradient noise
		const float offset = (noiseOffset - 0.5) * offsetAmplitude;
		float2 rotation;
		sincos(Math::TAU * noiseRotation, rotation.y, rotation.x);
		float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);

		const float step = 1.0 / steps;

		viewPosition += lightDirectionVS * offset * step;

#if defined(SKIN) || defined(HAIR) || defined(EYE)
		const float2 stepScale = float2(0.5, 1.5);
		const float shadowCasterToleranceScale = SharedData::ssplsSettings.CompareToleranceScale;
#else
		const float2 stepScale = float2(0.75, 1.25);
		const float shadowCasterToleranceScale = 0.4;
#endif

		const float compareToleranceScale = isShadowCaster ? shadowCasterToleranceScale : SharedData::ssplsSettings.CompareToleranceScale;

		return Raymarch(s, viewPosition, lightDirectionVS, steps, step, stepScale, compareToleranceScale, radius, rotationMatrix, a_eyeIndex, a_rtContact);
	}

	/// (45 SSPLS) How much one depth comparison says "blocked": 0 in front of the scene surface,
	/// ramping to 1 just behind it, back to 0 past the assumed thickness. Fractional so DLSS can
	/// average the edges instead of seeing 0/1 speckle.
	float HitAmount(float depthDelta, float thickness)
	{
		const float invRamp = 4.0 / thickness;
		return saturate(depthDelta * invRamp) * saturate((thickness - depthDelta) * invRamp);
	}

	/// (45 SSPLS) Fixed Math ray march.
	/// - thickness in view units, independent of the step count: max(0.5 * z * CompareToleranceScale, |dz per step|)
	///   (before: |light.z - pos.z| mixed a direction with a position and was divided by the step count)
	/// - start lifted ~1 pixel footprint towards the light and the camera, hits within 2 px of the receiver skipped
	///   (there is no geometric normal here without changing Lighting.hlsl, so the lift uses those two directions)
	/// - soft shadows: 4 taps, radius from the penumbra (receiver - blocker) / (blocker - light), clamped to 8 px
	/// - occlusion is accumulated as a fraction, casters near the end of the ray weigh less
	/// - smooth fade towards the distance where the caller's step count drops to 0 (no step bands)
	/// - lights with a shadow map only get contact-range shadows (no double darkening)
	/// - (46y) a_rtContact >= 0 (Screen-Space Shadows for Non-RT Casters): an occluder sample that is a ray-traced caster counts
	///   only within a_rtContact units along the ray; the ray tracer has the rest
	float GetShadowFixed(float3 viewPosition, float noiseOffset, float noiseRotation, float offsetAmplitude, float3 toLight, uint steps, float lightRadius, uint a_eyeIndex, bool isShadowCaster, float a_rtContact)
	{
		static const float kShadowCasterContactRange = 24.0;  // game units (~35 cm)
		static const float2 kSoftTaps[4] = { float2(0.85, 0.15), float2(-0.15, 0.85), float2(-0.85, -0.15), float2(0.15, -0.85) };

		const float rayLengthFraction = SharedData::ssplsSettings.RayLength;
		const float distanceToLight = length(toLight);
		if (rayLengthFraction <= 0.0 || distanceToLight < 1.0)
			return 1.0;
		const float3 lightDir = toLight / distanceToLight;

		float rayLength = min(distanceToLight * rayLengthFraction, distanceToLight * 0.95);
		if (isShadowCaster)
			rayLength = min(rayLength, kShadowCasterContactRange);

		const float2 renderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
		const float tanPerPixel = 2.0 / (abs(FrameBuffer::CameraProj[a_eyeIndex][1][1]) * renderSize.y);
		const float receiverZ = viewPosition.z;
		const float footprint = receiverZ * tanPerPixel;

		const float3 toCamera = -normalize(viewPosition);
		const float3 start = viewPosition + (lightDir * 1.5 + toCamera) * footprint;
		const float2 receiverUV = FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex);

		// (47k) Smarter Steps: the part of the ray still on the receiver's own 2 pixels is skipped (its samples were skipped anyway),
		// the steps are at least 2 px apart on screen, the contact rays of shadow-map lights take 2-3 steps. Off: 47j.
		float tStart = 0.0;
		uint stepCount = steps;
		[branch] if (SharedData::ssplsSettings.Enable & SettingFlags::SmarterSteps)
		{
			const float3 rayEnd = start + lightDir * rayLength;
			[branch] if (rayEnd.z > 1.0)
			{
				const float screenPixels = length((FrameBuffer::ViewToUV(rayEnd, true, a_eyeIndex) - receiverUV) * renderSize);
				if (screenPixels <= 2.0)
					return 1.0;  // the whole ray is on the receiver's own pixels (47j: every sample skipped)
				tStart = rayLength * (2.0 / screenPixels);
				const uint bySize = max(uint(ceil((screenPixels - 2.0) * 0.5)), 1u);
				stepCount = min(steps, isShadowCaster ? min(bySize, screenPixels > 8.0 ? 3u : 2u) : bySize);
			}
		}
		const float stepLength = (rayLength - tStart) / stepCount;
		const float jitter = 0.5 + (noiseOffset - 0.5) * offsetAmplitude;
		const float thicknessScale = 0.5 * SharedData::ssplsSettings.CompareToleranceScale;
		const float stepDepth = abs(lightDir.z) * stepLength;
		const float maxDistance = SharedData::ssplsSettings.MaxDistance;

		const bool soft = SharedData::ssplsSettings.EnableSoftShadows && SharedData::ssplsSettings.SoftShadowScale > 0.0;
		const float sourceSize = lightRadius * SharedData::ssplsSettings.SoftShadowScale * 0.01;
		float2 rotation;
		sincos(Math::TAU * noiseRotation, rotation.y, rotation.x);
		const float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);

		float occlusion = 0.0;

		[loop] for (uint i = 0; i < stepCount; i++)
		{
			const float t = tStart + (i + jitter) * stepLength;
			const float3 rayPosition = start + lightDir * t;
			if (rayPosition.z <= 1.0)
				break;

			const float2 rayUV = FrameBuffer::ViewToUV(rayPosition, true, a_eyeIndex);
			if (!(rayUV.x >= 0.0 && rayUV.x <= 1.0 && rayUV.y >= 0.0 && rayUV.y <= 1.0))
				break;

			// Still on the receiver's own pixels: anything found here is the surface itself.
			const float2 pixelDelta = (rayUV - receiverUV) * renderSize;
			if (dot(pixelDelta, pixelDelta) < 4.0)
				continue;

			const float sceneZ = SharedData::GetScreenDepth(rayUV, a_eyeIndex);
			if (sceneZ < 16.5 || sceneZ > maxDistance)
				break;

			const float thickness = max(thicknessScale * rayPosition.z, stepDepth);
			const float depthDelta = rayPosition.z - sceneZ;
			float hit = HitAmount(depthDelta, thickness);
			// (46y) Non-RT casters: past the contact length a traced occluder is the ray tracer's (its soft taps too).
			const bool rtFilter = a_rtContact >= 0.0 && t > a_rtContact;
			[branch] if (rtFilter && hit > 0.0)
			{
				if (IsRtCaster(rayUV, a_eyeIndex))
					hit = 0.0;
			}

			[branch] if (soft && depthDelta > -thickness && depthDelta < 3.0 * thickness)
			{
				// Penumbra width at the receiver = source size * (receiver - blocker) / (blocker - light).
				const float blockerToLight = max(distanceToLight - t, 1.0);
				const float penumbraPixels = min(sourceSize * t / blockerToLight / (rayPosition.z * tanPerPixel), 8.0);
				if (penumbraPixels > 0.5) {
					float taps = hit;
					[unroll] for (uint j = 0; j < 4; j++)
					{
						const float2 tapUV = rayUV + mul(kSoftTaps[j], rotationMatrix) * penumbraPixels / renderSize;
						float tap = HitAmount(rayPosition.z - SharedData::GetScreenDepth(saturate(tapUV), a_eyeIndex), thickness);
						[branch] if (rtFilter && tap > 0.0)
						{
							if (IsRtCaster(tapUV, a_eyeIndex))
								tap = 0.0;
						}
						taps += tap;
					}
					hit = taps * 0.2;
				}
			}

			const float endFade = 1.0 - smoothstep(0.5, 1.0, t / rayLength);  // (47k) the same as (i + jitter) / steps without Smarter Steps
			occlusion = max(occlusion, hit * endFade);
			if (occlusion > 0.95)
				break;
		}

		// The caller's step count reaches 0 at MaxDistance * (1 - 0.5 / StepLimit); fade out before it.
		const float cutoff = maxDistance * (1.0 - 0.5 / max(SharedData::ssplsSettings.StepLimit, 1u));
		const float distanceFade = 1.0 - smoothstep(0.7 * cutoff, 0.98 * cutoff, receiverZ);

		return 1.0 - occlusion * distanceFade;
	}

	/// (45 SSPLS) Stable Noise: the spatial IGN pattern plus a per-frame R1 (golden ratio) offset on the
	/// start jitter only; the soft-tap rotation keeps the spatial pattern. x = start jitter, y = rotation.
	float2 GetStableNoise(float3 viewPosition, uint a_eyeIndex)
	{
		const float2 renderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
		const float2 pixel = floor(FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex) * renderSize);
		const float spatial = Random::InterleavedGradientNoise(pixel);
		// frac(FrameCount * golden ratio) in 32-bit fixed point (exact for any frame count).
		const float r1 = (float)((SharedData::FrameCount * 2654435769u) >> 8) * (1.0 / 16777216.0);
		return float2(frac(spatial + r1), spatial);
	}

	/// (46y) a_rtContact: see GetShadowFixed (< 0 = every occluder counts, as before).
	float GetShadow(SamplerState s, float3 viewPosition, float noise2D, float3 lightDirectionVS, uint steps, float radius, uint a_eyeIndex = 0, bool isShadowCaster = false, float a_rtContact = -1.0)
	{
		if (steps == 0)
			return 1.0;

		const uint flags = SharedData::ssplsSettings.Enable;

		float noiseOffset = noise2D;
		float noiseRotation = noise2D;
		float offsetAmplitude = 1.0;
		[branch] if (flags & SettingFlags::StableNoise)
		{
			const float2 stable = GetStableNoise(viewPosition, a_eyeIndex);
			noiseOffset = stable.x;
			noiseRotation = stable.y;
			offsetAmplitude = 0.5;
		}

		[branch] if (flags & SettingFlags::FixedMath)
			return GetShadowFixed(viewPosition, noiseOffset, noiseRotation, offsetAmplitude, lightDirectionVS, steps, radius, a_eyeIndex, isShadowCaster, a_rtContact);

		return GetShadowLegacy(s, viewPosition, noiseOffset, noiseRotation, offsetAmplitude, lightDirectionVS, steps, radius, a_eyeIndex, isShadowCaster, a_rtContact);
	}
}
#endif  // SSPLS_COMMON
