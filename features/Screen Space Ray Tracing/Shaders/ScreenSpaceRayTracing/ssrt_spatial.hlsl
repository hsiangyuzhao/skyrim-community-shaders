#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> HistoryTexture : register(t0);
Texture2D<float4> SSRColorTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);

RWTexture2D<float4> FilteredOutput : register(u0);

cbuffer DenoiserCB : register(b2)
{
    float invMaxAccumulatedFrames;
    uint atrousIterations;
    float colorPhi;
    float normalPhi;
};

float GaussianBlur(uint2 id)
{
    float sum = 0.f;
    float kernelSum = 0.f;
    const float kernel[2][2] =
    {
        { 1.0 / 4.0, 1.0 / 8.0 },
        { 1.0 / 8.0, 1.0 / 16.0 }
    };
    
    const int radius = 1;
    
    for (int y = -radius; y <= radius; y++)
    {
        for (int x = -radius; x <= radius; x++)
        {
            const int2 p = id + int2(x, y);
            const bool inside = (p.x >= 0 && p.y >= 0) && (p.x < SharedData::BufferDim.x * FrameBuffer::DynamicResolutionParams1.x && p.y < SharedData::BufferDim.y * FrameBuffer::DynamicResolutionParams1.y);

            if (inside)
            {
                const float k = kernel[abs(x)][abs(y)];
                kernelSum += k;
                sum += SSRColorTexture[p].w * k;
            }
        }
    }

    return sum / kernelSum;
}

static const float kernelWeights[3] = { 1.0, 2.0 / 3.0, 1.0 / 6.0 };

#define VAR_EPSILON 0.00001f

// Spatiotemporal Variance-Guided Filter
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 screen_size = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
    if (DTid.x >= screen_size.x || DTid.y >= screen_size.y)
        return;

    float2 uv = float2(DTid.xy + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;

    // (audit P1) Sky / far-plane early-out. The existing `depthCenter > 0` gate below
    // does *not* cover the far plane (sky depth is 1.0, which is > 0), so today every
    // sky pixel pays the full 25-tap a-trous kernel. Write 0 rather than just
    // returning: this shader ping-pongs between two textures, so skipping the write
    // would leave the previous iteration's (or previous frame's) content behind.
    float depthCenter = DepthTexture[DTid.xy];
    if (SSRT_IS_FAR_PLANE(depthCenter)) {
        FilteredOutput[DTid.xy] = 0.0;
        return;
    }

    float3 blendedColor = 0;
    float4 historyColor = HistoryTexture[DTid.xy];
    float4 ssrColor = SSRColorTexture[DTid.xy];

    float3 normalVS;
    float roughness;
    GetNormalRoughness(DTid.xy, normalVS, roughness);
    roughness = clamp(roughness, 0.001f, 1.0f);

    float luminanceCenter = Color::RGBToLuminance(ssrColor.rgb);
    float variance = GaussianBlur(DTid.xy);

    // (audit #11) Variance travels in .w through the ping-pong, and it is what drives
    // phiLuminance above. The output used to hard-code .w = 1.0, so from the second
    // a-trous iteration on GaussianBlur() read back a constant 1 and the filter stopped
    // being variance-guided. Filter the variance alongside the colour with the squared
    // weights, as SVGF prescribes, and carry the result.
    float filteredVariance = ssrColor.w;

    if (depthCenter > 0)
    {
        float phiLuminance = max(colorPhi * sqrt(abs(variance) + VAR_EPSILON), VAR_EPSILON);
        float phiNormal = normalPhi;
#if defined(SSRT_SPECULAR)
        // Trying to reduce blurriness on glossy surfaces
        phiLuminance *= roughness;
        phiNormal /= roughness;
#endif
        float phiDepth = (atrousIterations + 1);
        float weightSum = 0.f;
        float varianceSum = 0.f;

        for (int ky = -2; ky <= 2; ky++)
        {
            for (int kx = -2; kx <= 2; kx++)
            {
                // A-Trous sampling
                int2 samplePos = int2(DTid.xy) + int2(kx, ky) * (atrousIterations + 1);
                bool inside = (samplePos.x >= 0 && samplePos.y >= 0) && (samplePos.x < screen_size.x && samplePos.y < screen_size.y);
                if (inside)
                {
                    float4 sampleSSRColor = SSRColorTexture[samplePos];
                    float sampleDepth = DepthTexture[samplePos];
                    if (sampleDepth > 0)
                    {
                        float3 sampleNormalVS;
                        float sampleRoughness;
                        GetNormalRoughness(samplePos, sampleNormalVS, sampleRoughness);

                        float luminanceP = Color::RGBToLuminance(sampleSSRColor.rgb);
                        float weight = CalculateWeight(depthCenter, sampleDepth, phiDepth, normalVS, sampleNormalVS, phiNormal, luminanceCenter, luminanceP, phiLuminance) * kernelWeights[abs(kx)] * kernelWeights[abs(ky)];

                        blendedColor += sampleSSRColor.rgb * weight;
                        // Variance of a weighted mean scales with the squared weights.
                        varianceSum += sampleSSRColor.w * weight * weight;
                        weightSum += weight;
                    }
                }
            }
        }
        if (weightSum > 0.f)
        {
            blendedColor /= weightSum;
            filteredVariance = varianceSum / (weightSum * weightSum);
        }
        else
        {
            blendedColor = ssrColor.rgb;
            filteredVariance = ssrColor.w;
        }
    }

    FilteredOutput[DTid.xy] = float4(blendedColor, filteredVariance);
}