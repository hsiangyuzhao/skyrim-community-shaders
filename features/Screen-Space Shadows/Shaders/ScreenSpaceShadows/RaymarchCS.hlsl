#include "Common/SharedData.hlsli"

#if defined(RT_CASTER_MASK)
// (45k) Ray Tracing visibility texture: alpha bit 4 = the visible surface is a ray-traced caster.
Texture2D<unorm float4> RTCasterMask : register(t1);
// (47r) Per 8x8 tile of RTCasterMask: bit 0 some traced caster, bit 1 some other pixel (RtTileMask; RayTracing DenoiseTileCS SSS_CASTER_TILES).
Texture2D<uint> RTCasterTiles : register(t2);
#endif

#include "ScreenSpaceShadows/bend_sss_gpu.hlsli"

// (45 SSS, upstream) Match bend_sss_gpu.hlsli's DepthTexture type.
// TERRAIN_BLENDING ON -> R32_FLOAT (no unorm). OFF -> R24_UNORM_X8_TYPELESS (unorm).
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t0);  // Depth Buffer Texture (R32_FLOAT blended depth)
#else
Texture2D<unorm half> DepthTexture : register(t0);  // Depth Buffer Texture (rasterized non-linear depth)
#endif
RWTexture2D<unorm half> OutputTexture : register(u0);  // Output screen-space shadow buffer (typically single-channel, 8bit)
SamplerState PointBorderSampler : register(s0);        // A point sampler, with Wrap Mode set to Clamp-To-Border-Color (D3D12_TEXTURE_ADDRESS_MODE_BORDER), and Border Color set to "FarDepthValue" (typically zero), or some other far-depth value out of DepthBounds.
													   // If you have issues where invalid shadows are appearing from off-screen, it is likely that this sampler is not correctly setup

#include "ScreenSpaceShadows/RaymarchCB.hlsli"

[numthreads(WAVE_SIZE, 1, 1)] void main(
	int3 groupID
	: SV_GroupID,
	int groupThreadID
	: SV_GroupThreadID) {
	DispatchParameters parameters;
	parameters.SetDefaults();

	parameters.LightCoordinate = LightCoordinate;
	parameters.WaveOffset = WaveOffset;
	parameters.FarDepthValue = 1;
	parameters.NearDepthValue = 0;
	parameters.InvDepthTextureSize = InvDepthTextureSize;
	parameters.DepthTexture = DepthTexture;
	parameters.OutputTexture = OutputTexture;
	parameters.PointBorderSampler = PointBorderSampler;

	parameters.SurfaceThickness = SurfaceThickness;
	parameters.BilinearThreshold = BilinearThreshold;
	parameters.ShadowContrast = ShadowContrast;

	parameters.DynamicRes = DynamicRes;

	parameters.UsePrecisionOffset = true;

	parameters.CameraData = CameraData;
	parameters.SunDirVS = SunDirVS.xyz;
	parameters.ProjInfo = ProjInfo.xy;
	parameters.RtContactScale = RtContactScale;
	parameters.RtBias = float2(RtBiasConstant, RtBiasPerUnit);
	parameters.RtTileMask = RtTileMask;

	WriteScreenSpaceShadow(parameters, groupID, groupThreadID);
}
