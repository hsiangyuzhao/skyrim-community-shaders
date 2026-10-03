#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/VR.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<half4> srcDiffuse : register(t0);
Texture2D<half> srcCurrDepth : register(t1);
Texture2D<half4> srcCurrNormal : register(t2);
Texture2D<half3> srcPrevGeo : register(t3);  // maybe half-res
Texture2D<float4> srcMotionVec : register(t4);
Texture2D<unorm float> srcAccumFrames : register(t5);  // maybe half-res
Texture2D<half> srcPrevAo : register(t6);              // maybe half-res
Texture2D<half4> srcPrevIlY : register(t7);            // maybe half-res
Texture2D<half2> srcPrevIlCoCg : register(t8);         // maybe half-res
Texture2D<half4> srcPrevGISpecular : register(t9);    // maybe half-res

RWTexture2D<float3> outRadianceDisocc : register(u0);
RWTexture2D<unorm float> outAccumFrames : register(u1);
RWTexture2D<float> outRemappedAo : register(u2);
RWTexture2D<float4> outRemappedIlY : register(u3);
RWTexture2D<float2> outRemappedIlCoCg : register(u4);
RWTexture2D<float4> outRemappedPrevGISpecular : register(u5);

#if defined(TEMPORAL_DENOISER) || defined(HALF_RATE)
#	define REPROJECTION
#endif

void readHistory(
	uint eyeIndex, float curr_depth, float3 curr_pos, int2 pixCoord, float bilinear_weight,
	inout half prev_ao, inout half4 prev_y, inout half2 prev_co_cg, inout half3 prev_ambient, inout float accum_frames, inout half4 prev_gi_specular, inout float wsum)
{
	const float2 uv = (pixCoord + .5) * RCP_OUT_FRAME_DIM;
	const float2 screen_pos = Stereo::ConvertFromStereoUV(uv, eyeIndex);
	if (any(screen_pos < 0) || any(screen_pos > 1))
		return;

	const half3 prev_geo = srcPrevGeo[pixCoord];
	const float prev_depth = prev_geo.x;
	// const float3 prev_normal = GBuffer::DecodeNormal(prev_geo.yz);  // prev normal is already world
	float3 prev_pos = ScreenToViewPosition(screen_pos, prev_depth, eyeIndex);
	prev_pos = ViewToWorldPosition(prev_pos, PrevInvViewMat[eyeIndex]) + FrameBuffer::CameraPreviousPosAdjust[eyeIndex].xyz;

	float3 delta_pos = curr_pos - prev_pos;
	// float normal_prod = dot(curr_normal, prev_normal);

	const float movement_thres = curr_depth * DepthDisocclusion;

	bool depth_pass = dot(delta_pos, delta_pos) < movement_thres * movement_thres;
	// bool normal_pass = normal_prod * normal_prod > NormalDisocclusion;
	if (!depth_pass)
		return;

#ifdef TEMPORAL_DENOISER
	const float4 hist_y = srcPrevIlY[pixCoord];
	const float2 hist_co_cg = srcPrevIlCoCg[pixCoord];
	bool hist_finite = isFiniteSafe(hist_y) && isFiniteSafe(hist_co_cg);
#	ifdef GI_SPECULAR
	const float4 hist_gi_specular = srcPrevGISpecular[pixCoord];
	hist_finite = hist_finite && isFiniteSafe(hist_gi_specular);
#	endif

	// (guard N4) The only self-healing guard in the chain. Everything else here bounds what may
	// *enter* the history; this is what lets the history get out of a bad state once it is in
	// one. A non-finite tap is treated exactly like a failed depth test -- dropped from both the
	// weighted sum and wsum -- so if all four taps are poisoned wsum stays 0, the caller's
	// `wsum > 1e-2` test fails, accum_frames resets to 1 and the pixel takes the existing
	// disocclusion path. No new mechanism, no new failure mode: the worst case is one frame of
	// single-sample GI on that texel, which is what a genuine disocclusion already produces.
	// Matches SSRT_LoadHistory's semantics in ssrt_temporal.hlsl.
	//
	// The whole tap is rejected, not just the offending channel: all channels share one
	// accum_frames, so accepting AO from a tap whose IL was thrown away would leave the surviving
	// channel weighted as if it had been accumulating for N frames when it had not.
	//
	// prev_ao and accum_frames are not tested. Their sources are R8_UNORM, which has no encoding
	// for a non-finite value, so a test on them could never fire.
	if (!hist_finite)
		return;

	prev_ao += srcPrevAo[pixCoord] * bilinear_weight;
	prev_y += hist_y * bilinear_weight;
	prev_co_cg += hist_co_cg * bilinear_weight;
	accum_frames += srcAccumFrames[pixCoord] * bilinear_weight;
#	ifdef GI_SPECULAR
	prev_gi_specular += hist_gi_specular * bilinear_weight;
#	endif
#endif
	wsum += bilinear_weight;
};

[numthreads(8, 8, 1)] void main(const uint2 pixCoord
								: SV_DispatchThreadID) {
	const float2 frameScale = FrameDim * RcpTexDim;

	const float2 uv = (pixCoord + .5) * RCP_OUT_FRAME_DIM;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float2 screen_pos = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float2 prev_screen_pos = screen_pos;
#ifdef REPROJECTION
	prev_screen_pos += FULLRES_LOAD(srcMotionVec, pixCoord, uv * frameScale, samplerLinearClamp).xy;
#endif
	float2 prev_uv = Stereo::ConvertToStereoUV(prev_screen_pos, eyeIndex);

	half3 prev_ambient = 0;
	half prev_ao = 0;
	half4 prev_y = 0;
	half2 prev_co_cg = 0;
	half4 prev_gi_specular = 0;
	float accum_frames = 0;
	float wsum = 0;

	const float curr_depth = READ_DEPTH(srcCurrDepth, pixCoord);

	if (curr_depth < FP_Z) {
		outRadianceDisocc[pixCoord] = half3(0, 0, 0);
		outAccumFrames[pixCoord] = 1.0 / 255.0;
		outRemappedIlY[pixCoord] = half4(0, 0, 0, 0);
		outRemappedIlCoCg[pixCoord] = half2(0, 0);
		return;
	}

#ifdef REPROJECTION
	if ((curr_depth <= DepthFadeRange.y) && !(any(prev_screen_pos < 0) || any(prev_screen_pos > 1))) {
		// float3 curr_normal = GBuffer::DecodeNormal(srcCurrNormal[pixCoord]);
		// curr_normal = ViewToWorldVector(curr_normal, FrameBuffer::CameraViewInverse[eyeIndex]);
		float3 curr_pos = ScreenToViewPosition(screen_pos, curr_depth, eyeIndex);
		curr_pos = ViewToWorldPosition(curr_pos, FrameBuffer::CameraViewInverse[eyeIndex]) + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

		float2 prev_px_coord = prev_uv * OUT_FRAME_DIM;
		int2 prev_px_lu = floor(prev_px_coord - 0.5);
		float2 bilinear_weights = prev_px_coord - 0.5 - prev_px_lu;

		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu, (1 - bilinear_weights.x) * (1 - bilinear_weights.y),
			prev_ao, prev_y, prev_co_cg, prev_ambient, accum_frames, prev_gi_specular, wsum);
		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu + int2(1, 0), bilinear_weights.x * (1 - bilinear_weights.y),
			prev_ao, prev_y, prev_co_cg, prev_ambient, accum_frames, prev_gi_specular, wsum);
		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu + int2(0, 1), (1 - bilinear_weights.x) * bilinear_weights.y,
			prev_ao, prev_y, prev_co_cg, prev_ambient, accum_frames, prev_gi_specular, wsum);
		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu + int2(1, 1), bilinear_weights.x * bilinear_weights.y,
			prev_ao, prev_y, prev_co_cg, prev_ambient, accum_frames, prev_gi_specular, wsum);

		if (wsum > 1e-2) {
			float rcpWsum = rcp(wsum + 1e-10);
#	ifdef TEMPORAL_DENOISER
			prev_ao *= rcpWsum;
			prev_y *= rcpWsum;
			prev_co_cg *= rcpWsum;
			accum_frames *= rcpWsum;
#		ifdef GI_SPECULAR
			prev_gi_specular *= rcpWsum;
#		endif
#	endif
		}
	}
#endif

	half3 radiance = 0;
#ifdef GI
	radiance = Color::RadianceToLinear(FULLRES_LOAD(srcDiffuse, pixCoord, uv * frameScale, samplerLinearClamp).rgb * GIStrength);
	radiance = filterNaN(radiance);
	radiance = filterInf(radiance);
	// (guard N2) filterNaN/filterInf above only reject values that are *already* non-finite;
	// RadianceToLinear's pow(x, 2.2) (Linear Lighting off) turns a finite but bright source into
	// a value the R11G11B10 target below cannot represent, which becomes a fresh Inf. See
	// SSGI_MAX_RADIANCE in common.hlsli for the range argument.
	radiance = min(radiance, SSGI_MAX_RADIANCE);
	outRadianceDisocc[pixCoord] = radiance;
#endif

#ifdef TEMPORAL_DENOISER
	accum_frames = max(1, min(accum_frames * 255 + 1, MaxAccumFrames));
	outAccumFrames[pixCoord] = accum_frames / 255.0;
	outRemappedAo[pixCoord] = prev_ao;
	outRemappedIlY[pixCoord] = prev_y;
	outRemappedIlCoCg[pixCoord] = prev_co_cg;
	outRemappedPrevGISpecular[pixCoord] = prev_gi_specular;
#endif
}