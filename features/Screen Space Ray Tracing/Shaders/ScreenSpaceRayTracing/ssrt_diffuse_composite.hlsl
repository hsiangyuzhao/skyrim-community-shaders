// ssrt_common.hlsli supersedes the Common/Color.hlsli + Common/SharedData.hlsli pair this
// file used to include (it includes both) and brings filterNaN / filterInf and
// SSRT_MAX_RADIANCE for the G9 guard below. Its own resource declarations
// (NormalRoughnessTexture at t2, LinearSampler at s0) are not referenced here and fxc
// strips them, so the compiled binding table is unchanged.
#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> SSRTDiffuseTexture : register(t0);
Texture2D<float4> AlbedoTexture : register(t1);

RWTexture2D<float4> ColorTextureRW : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
    // (audit P9) SharedData::BufferDim.xy is the same full-resolution extent
    // ColorTextureRW.GetDimensions() returned (this UAV is the kMAIN render target), so
    // this is output-identical while dropping a per-thread resource query.
    if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
        return;

    float4 ssrtDiffuse = SSRTDiffuseTexture[dispatchID.xy];
    // (guard G9) The last gate in the chain, and the one that decides whether an SSRT
    // failure is a local artefact or a global one. ColorTextureRW is kMAIN: whatever is
    // written here is what the upscaler, the bloom chain and the tonemapper consume, and
    // every one of those spreads a NaN far beyond the pixel that produced it -- a single
    // non-finite texel entering a downsample pyramid takes the whole mip with it, which is
    // how a handful of dead pixels becomes a black screen.
    //
    // The guard is deliberately duplicated with G2 at the ray march's own output rather
    // than being trusted from there. Between the two sit the temporal, variance and
    // a-trous passes plus a CopyResource, i.e. the entire denoiser, and this pass is also
    // reached with EnableSVGF off, where the ceiling costs one min() on a value that is
    // already bounded.
    //
    // No-op on healthy data for the same reason as G2: filterNaN is the identity on ordered
    // values, filterInf on finite ones, and min(x, SSRT_MAX_RADIANCE) on the whole
    // legitimate radiance range. .w is unused by the composite, so only the colour channels
    // are handled.
    ssrtDiffuse.rgb = min(filterInf(filterNaN(ssrtDiffuse.rgb)), SSRT_MAX_RADIANCE);
    float4 albedo = AlbedoTexture[dispatchID.xy];
    float4 originalColor = ColorTextureRW[dispatchID.xy];

    float3 color = Color::IrradianceToGamma(ssrtDiffuse.xyz * Color::IrradianceToLinear(albedo.xyz) + Color::IrradianceToLinear(originalColor.xyz));
    ColorTextureRW[dispatchID.xy] = float4(color, originalColor.w);
}