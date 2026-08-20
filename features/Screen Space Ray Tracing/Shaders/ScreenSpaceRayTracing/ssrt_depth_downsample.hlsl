Texture2D<float> depth : register(t0);
RWTexture2D<float> outDepth : register(u0);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
    // (audit P3) This shader only ever walks a Direct3D mip chain, where the ratio
    // between two consecutive levels is exactly 2 by definition
    // (dim(m+1) = max(1, dim(m) >> 1)). The former per-thread GetDimensions() pair and
    // the `ratio > 2` needExtraSampleX/Y branches could therefore never trigger; they
    // cost every thread two resource queries, a divide and five predicated Loads to
    // evaluate conditions that are compile-time false. A 2x2 min is the whole job.
    //
    // A single GetDimensions on the *source* is kept, for two reasons:
    //  - the dispatch is sized to the dynamic-resolution sub-rect of the destination
    //    mip and may overshoot it by up to 7 pixels, so reads can leave the texture.
    //    An out-of-bounds Load returns 0 == near plane, which would poison the min and
    //    propagate a phantom near surface up the pyramid (audit #8);
    //  - it gives the destination extent for the write guard for free.
    uint2 srcDim;
    depth.GetDimensions(srcDim.x, srcDim.y);

    const int2 maxReadCoord = int2(srcDim) - 1;
    const uint2 dstDim = max(uint2(1, 1), srcDim >> 1);

    if (any(DTid.xy >= dstDim))
        return;

    const int2 vReadCoord = int2(DTid.xy) << 1;

    const float4 depth_samples = float4(
        depth[min(vReadCoord, maxReadCoord)].x,
        depth[min(vReadCoord + int2(1, 0), maxReadCoord)].x,
        depth[min(vReadCoord + int2(0, 1), maxReadCoord)].x,
        depth[min(vReadCoord + int2(1, 1), maxReadCoord)].x);

    // min() with the far plane (1.0) is a no-op, so as long as mip 0 holds 1.0 outside
    // the dynamic-resolution sub-rect (see ssrt_preprocess_depth.hlsl and the clear in
    // ScreenSpaceRayTracing::Prepass) every coarser level inherits that property by
    // induction: a fully-outside 2x2 block yields exactly 1.0, and a partially covered
    // block yields the min over its valid pixels only. No explicit out-of-sub-rect
    // handling is needed here. (audit #8)
    outDepth[DTid.xy] = min(min(depth_samples.x, depth_samples.y), min(depth_samples.z, depth_samples.w));
}
