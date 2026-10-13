// FidelityFX Super Resolution - Robust Contrast Adaptive Sharpening (RCAS)
// Based on https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/ffx-fsr/ffx_fsr1.h
// Ported from jiayev/skyrim-community-shaders (fd3f6f994, 499687986).
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// (45a) HDR-safe: the input is the linear scene (unbounded; the sun reaches ~62k), but RCAS's
// peak limiter assumes a 0..1 signal. The five taps are wrapped scale-invariantly around their
// mean luma m, y = x / (x + m), RCAS runs on y in (0, 1), and the result is unwrapped with
// x = m * y / (1 - y). No exposure input is needed and SDR / HDR frames behave the same.

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

cbuffer RCASConfig : register(b0)
{
	float sharpness;
	float3 pad;
};

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Dest : register(u0);

// Largest wrapped value handed back to the inverse: x <= 1023 * m.
static const float kMaxWrapped = 1.0 - 1.0 / 1024.0;
// The unwrapped result may not exceed this multiple of the brightest tap (halo / firefly guard).
static const float kMaxOvershoot = 2.0;

float3 Wrap(float3 x, float m)
{
	return x / (x + m);
}

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	uint2 texDim;
	Dest.GetDimensions(texDim.x, texDim.y);

	if (DTid.x >= texDim.x || DTid.y >= texDim.y)
		return;

	// Algorithm uses minimal 3x3 pixel neighborhood.
	//    b
	//  d e f
	//    h
	int2 sp = int2(DTid.xy);
	int2 maxCoord = int2(texDim) - 1;
	float4 eFull = Source.Load(int3(sp, 0));
	float3 xb = max(Source.Load(int3(clamp(sp + int2(0, -1), 0, maxCoord), 0)).rgb, 0.0);
	float3 xd = max(Source.Load(int3(clamp(sp + int2(-1, 0), 0, maxCoord), 0)).rgb, 0.0);
	float3 xe = max(eFull.rgb, 0.0);
	float3 xf = max(Source.Load(int3(clamp(sp + int2(1, 0), 0, maxCoord), 0)).rgb, 0.0);
	float3 xh = max(Source.Load(int3(clamp(sp + int2(0, 1), 0, maxCoord), 0)).rgb, 0.0);

	// Scale-invariant wrap around the local mean luma (Rec. 709 weights, linear light).
	const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
	float m = 0.2 * (dot(xb, lumaWeights) + dot(xd, lumaWeights) + dot(xe, lumaWeights) + dot(xf, lumaWeights) + dot(xh, lumaWeights));
	if (!(m > 1e-8)) {
		Dest[DTid.xy] = float4(xe, eFull.a);
		return;
	}

	float3 b = Wrap(xb, m);
	float3 d = Wrap(xd, m);
	float3 e = Wrap(xe, m);
	float3 f = Wrap(xf, m);
	float3 h = Wrap(xh, m);

	// Luma times 2.
	float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
	float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
	float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
	float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
	float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

	// Noise detection.
	float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
	float lumaRange = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
	nz = saturate(abs(nz) * rcp(max(lumaRange, 1e-6)));
	nz = -0.5 * nz + 1.0;

	// Min and max of ring.
	float3 mn4 = min(min(min(b, d), f), h);
	float3 mx4 = max(max(max(b, d), f), h);

	// Immediate constants for peak range.
	float2 peakC = float2(1.0, -1.0 * 4.0);

	// Limiters, these need to be high precision RCPs.
	float3 hitMin = min(mn4, e) * rcp(max(4.0 * mx4, 1e-6));
	float3 hitMax = (peakC.x - max(mx4, e)) * rcp(4.0 * mn4 + peakC.y);
	float3 lobeRGB = max(-hitMin, hitMax);
	float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * sharpness;

	// Apply noise removal.
	lobe *= nz;

	// Resolve, which needs the medium precision rcp approximation to avoid visible tonality changes.
	float rcpL = rcp(4.0 * lobe + 1.0);
	float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;

	// Back to linear light.
	pix = clamp(pix, 0.0, kMaxWrapped);
	float3 x = m * pix / (1.0 - pix);
	float3 xMax = max(max(max(xb, xd), max(xe, xf)), xh);
	x = min(x, kMaxOvershoot * xMax);

	Dest[DTid.xy] = float4(x, eFull.a);
}
