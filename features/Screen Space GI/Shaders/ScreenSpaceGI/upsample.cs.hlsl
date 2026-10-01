// depth-aware upsampling: https://gist.github.com/pixelmager/a4364ea18305ed5ca707d89ddc5f8743

#include "Common/FastMath.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<half> srcDepth : register(t0);
Texture2D<half> srcAo : register(t1);           // half-res
Texture2D<half4> srcIlY : register(t2);         // half-res
Texture2D<half2> srcIlCoCg : register(t3);      // half-res
Texture2D<half4> srcGiSpecular : register(t4);  // half-res
#ifdef CONTACT_AO
// (contact AO) Full-res contact visibility from contactAo.cs.hlsl, folded into the AO channel here
// because this pass is already reading and rewriting that channel at the render extent. Full
// resolution has no upsample pass, so there the contact pass composites the term itself; see
// CONTACT_COMPOSE in contactAo.cs.hlsl.
Texture2D<unorm float> srcContact : register(t5);
#endif
// (directional env) Bent normal + aperture at working resolution. Encoding: see
// SSGI_EncodeBentNormal in common.hlsli.
Texture2D<unorm float4> srcBentNormal : register(t6);  // half-res
#ifdef DYNAMIC_CUBEMAPS
// (directional env v2) Environment irradiance at working resolution. Radiance data
// (premultiplied RGB + confidence A): upsampled through the same two scalar paths as the IL
// channels, hardware bilinear included.
Texture2D<float4> srcEnvIrradiance : register(t7);  // half-res
#endif

RWTexture2D<half> outAo : register(u0);
RWTexture2D<half4> outIlY : register(u1);
RWTexture2D<half2> outIlCoCg : register(u2);
RWTexture2D<half4> outGiSpecular : register(u3);
RWTexture2D<unorm float4> outBentNormal : register(u4);
#ifdef DYNAMIC_CUBEMAPS
RWTexture2D<float4> outEnvIrradiance : register(u5);
#endif

// (batch 36) SSGI_AO_ONLY: see gi.cs.hlsl. Only the AO channel (with the contact term) is
// upsampled and written; the IL, specular, bent-normal and environment outputs are left alone,
// which is most of this pass's store traffic (32 of 33 bytes per render pixel with Dynamic
// Cubemaps loaded: R8 AO against RGBA16F Y, RG16F CoCg, RGBA16F specular, RGBA8 bent normal and
// RGBA16F environment). Without the define the file compiles exactly as before.
#ifdef SSGI_AO_ONLY
#	define SSGI_UPSAMPLE_IL 0
#else
#	define SSGI_UPSAMPLE_IL 1
#endif

#define min4(v) min(min(v.x, v.y), min(v.z, v.w))
#define max4(v) max(max(v.x, v.y), max(v.z, v.w))

#define BLEND_WEIGHT(a, b, c, d, w, sumw) ((a * w.x + b * w.y + c * w.z + d * w.w) / max(sumw, 1e-5))

[numthreads(8, 8, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside frame bounds
	if (any(dtid >= uint2(FrameDim)))
		return;
#ifdef HALF_RES
	int2 px00 = (dtid >> 1) + (dtid & 1) - 1;
#else  // QUARTER_RES
	int2 px00 = (dtid >> 2) + (dtid & 2) / 2 - 1;
#endif
	int2 px10 = px00 + int2(1, 0);
	int2 px01 = px00 + int2(0, 1);
	int2 px11 = px00 + int2(1, 1);

	float4 d = float4(
		srcDepth.Load(int3(px00, RES_MIP)),
		srcDepth.Load(int3(px01, RES_MIP)),
		srcDepth.Load(int3(px10, RES_MIP)),
		srcDepth.Load(int3(px11, RES_MIP)));

	// note: edge-detection
	float mind = min4(d);
	float maxd = max4(d);
	float diffd = maxd - mind;
	float avg = dot(d, 0.25.xxxx);
	bool d_edge = (diffd / avg) < 0.1;

	float ao;
#if SSGI_UPSAMPLE_IL
	float4 y;
	float2 coCg;
	float4 giSpecular;
#	ifdef DYNAMIC_CUBEMAPS
	float4 envIrradiance;
#	endif
#endif

	[branch] if (d_edge)
	{
		float bgdepth = srcDepth[dtid];

		//note: depth weighing from https://www.ppsloan.org/publications/ProxyPG.pdf#page=5
		float4 dd = abs(d - bgdepth);
		float4 w = 1.0 / (dd + 0.00001);
		float sumw = w.x + w.y + w.z + w.w;

		ao = BLEND_WEIGHT(srcAo[px00], srcAo[px01], srcAo[px10], srcAo[px11], w, sumw);
#if SSGI_UPSAMPLE_IL
		y = BLEND_WEIGHT(srcIlY[px00], srcIlY[px01], srcIlY[px10], srcIlY[px11], w, sumw);
		coCg = BLEND_WEIGHT(srcIlCoCg[px00], srcIlCoCg[px01], srcIlCoCg[px10], srcIlCoCg[px11], w, sumw);
		giSpecular = BLEND_WEIGHT(srcGiSpecular[px00], srcGiSpecular[px01], srcGiSpecular[px10], srcGiSpecular[px11], w, sumw);
#	ifdef DYNAMIC_CUBEMAPS
		envIrradiance = BLEND_WEIGHT(srcEnvIrradiance[px00], srcEnvIrradiance[px01], srcEnvIrradiance[px10], srcEnvIrradiance[px11], w, sumw);
#	endif
#endif
	}
	else
	{
		float2 uv = (dtid + .5) * RcpFrameDim * OUT_FRAME_DIM * RcpTexDim;
		ao = srcAo.SampleLevel(samplerLinearClamp, uv, 0);
#if SSGI_UPSAMPLE_IL
		y = srcIlY.SampleLevel(samplerLinearClamp, uv, 0);
		coCg = srcIlCoCg.SampleLevel(samplerLinearClamp, uv, 0);
		giSpecular = srcGiSpecular.SampleLevel(samplerLinearClamp, uv, 0);
#	ifdef DYNAMIC_CUBEMAPS
		envIrradiance = srcEnvIrradiance.SampleLevel(samplerLinearClamp, uv, 0);
#	endif
#endif
	}

	// (directional env) The bent normal cannot go through either scalar path above: hardware
	// bilinear on the octahedral encoding interpolates across fold lines, and BLEND_WEIGHT on
	// the raw channels is the same thing by hand (see the codec note in common.hlsli). So it is
	// upsampled once, in the decoded vector domain, with the same four taps and the same
	// 1/(|dz|+eps) depth weighting the edge branch uses - on flat depth those weights degrade
	// towards the plain average the linear branch approximates, so a single path serves both.
#if SSGI_UPSAMPLE_IL
	{
		float bgdepth = srcDepth[dtid];
		float4 dd = abs(d - bgdepth);
		float4 wB = 1.0 / (dd + 0.00001);
		float sumwB = max(wB.x + wB.y + wB.z + wB.w, 1e-5);

		float3 dir00, dir01, dir10, dir11;
		float ap00, ap01, ap10, ap11;
		SSGI_DecodeBentNormal(srcBentNormal[px00], dir00, ap00);
		SSGI_DecodeBentNormal(srcBentNormal[px01], dir01, ap01);
		SSGI_DecodeBentNormal(srcBentNormal[px10], dir10, ap10);
		SSGI_DecodeBentNormal(srcBentNormal[px11], dir11, ap11);

		float3 bentDir = dir00 * wB.x + dir01 * wB.y + dir10 * wB.z + dir11 * wB.w;
		float bentAperture = (ap00 * wB.x + ap01 * wB.y + ap10 * wB.z + ap11 * wB.w) / sumwB;

		float bentLen = length(bentDir);
		bentDir = bentLen > 1e-4 ? bentDir / bentLen : dir00;
		outBentNormal[dtid] = SSGI_EncodeBentNormal(bentDir, bentAperture);
	}
#endif

#ifdef CONTACT_AO
	// The channel stores occlusion, so the two visibilities multiply: 1 - (1 - occ) * contact.
	// Applied after the upsample, not before it: the contact term is a full-resolution signal and
	// running it through a depth-weighted blend of four half-res taps would throw away the only
	// thing it contributes.
	ao = saturate(1.0 - (1.0 - ao) * srcContact[dtid]);
#endif

	outAo[dtid] = ao;
#if SSGI_UPSAMPLE_IL
	outIlY[dtid] = y;
	outIlCoCg[dtid] = coCg;
	outGiSpecular[dtid] = giSpecular;
#	ifdef DYNAMIC_CUBEMAPS
	outEnvIrradiance[dtid] = envIrradiance;
#	endif
#endif
}
