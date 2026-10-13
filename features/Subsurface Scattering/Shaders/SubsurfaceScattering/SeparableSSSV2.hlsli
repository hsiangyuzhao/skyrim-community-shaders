/**
 * Copyright (C) 2012 Jorge Jimenez (jorge@iryoku.com)
 * Copyright (C) 2012 Diego Gutierrez (diegog@unizar.es)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the following disclaimer
 *       in the documentation and/or other materials provided with the
 *       distribution:
 *
 *       "Uses Separable SSS. Copyright (C) 2012 by Jorge Jimenez and Diego
 *        Gutierrez."
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the copyright holders.
 */

// (batch 38, item 3) Separable SSS blur, upgraded. Port of upstream 98ffc7f66:
//   - input is the pre-pass output (linear, albedo taken out per Scatter Mode), point-sampled
//     in DR-space UVs;
//   - step scaled into DR UVs (blur width in rendered pixels no longer depends on the DLSS mode);
//   - per-pixel rotation of the kernel to break up axis-aligned banding;
//   - taps clamped to the rendered rectangle.
// Re-added for VR (upstream dropped it in 98ffc7f66/c9d7c0524): the horizontal step is halved
// because each eye covers half the buffer, and taps are clamped to the current eye's half.
// Based on Jimenez et al., Separable Subsurface Scattering (licence above).

#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

float4 SSSSBlurCSV2(
	float2 texcoord,
	float2 dir,
	float sssAmount,
	bool humanProfile)
{
	// texcoord is in DR-space UVs (DTid / full buffer dim), so it already addresses the rendered
	// sub-rectangle of the texture. Non-DR UVs are only used to pick the eye in VR.
	float2 texcoordNonDR = texcoord * FrameBuffer::DynamicResolutionParams2.xy;

	// Input is already linear and albedo-free from the pre-pass.
	float4 colorM = ColorTexture.SampleLevel(PointSampler, texcoord, 0);

	if (sssAmount == 0)
		return colorM;

	float depthM = DepthTexture.SampleLevel(PointSampler, texcoord, 0).r;
	depthM = SharedData::GetScreenDepth(depthM);

	float2 profile = humanProfile ? HumanProfile.xy : BaseProfile.xy;
	uint kernelOffset = humanProfile ? SSSS_N_SAMPLES : 0;

	// Accumulate center sample, multiplying it with its gaussian weight:
	float4 colorBlurred = colorM;
	colorBlurred.rgb *= Kernels[kernelOffset].rgb;

	// World-space width
	float distanceToProjectionWindow = 1.0 / tan(0.5 * radians(SSSS_FOVY));
	float scale = distanceToProjectionWindow / depthM;

	// Step in (one eye's) screen UVs.
	float2 finalStep = scale * dir;
	finalStep *= sssAmount;
	finalStep *= profile.x;  // Modulate it using the profile

	// Per-pixel rotation to break separable axis-aligned banding
	float jitter = Random::InterleavedGradientNoise(texcoord * SharedData::BufferDim.xy, SharedData::FrameCount) * Math::TAU;
	float2x2 rotationMatrix = float2x2(cos(jitter), sin(jitter), -sin(jitter), cos(jitter));

	// Screen UV -> texture UV: dynamic resolution, and in VR each eye is half the buffer wide.
	float2 uvToTexture = FrameBuffer::DynamicResolutionParams1.xy;
#if defined(VR)
	uvToTexture.x *= 0.5;
#endif

	// Accumulate the other samples:
	for (uint i = kernelOffset + 1; i < kernelOffset + SSSS_N_SAMPLES; i++) {
		float2 offset = Kernels[i].a * finalStep;

		// Apply randomized rotation (in screen space, before the anisotropic VR/DR scale)
		offset = mul(offset, rotationMatrix) * uvToTexture;

		float2 sampleCoord = texcoord + offset;

		// Clamp to the DR-rendered region (per eye in VR) to avoid sampling outside it.
		sampleCoord = SSSClampToRenderedRegion(sampleCoord, texcoordNonDR);

		float3 color = ColorTexture.SampleLevel(PointSampler, sampleCoord, 0).rgb;

		float depth = DepthTexture.SampleLevel(PointSampler, sampleCoord, 0).r;
		depth = SharedData::GetScreenDepth(depth);

		// If the difference in depth is huge, we lerp color back to "colorM":
		float s = saturate(profile.y * distanceToProjectionWindow * abs(depthM - depth));
		color = lerp(color, colorM.rgb, s * s);

		// Accumulate:
		colorBlurred.rgb += Kernels[i].rgb * color.rgb;
	}

	return colorBlurred;
}
