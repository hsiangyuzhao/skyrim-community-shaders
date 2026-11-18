#include "Common/SharedData.hlsli"

Texture2D<float> InVLTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
RWTexture2D<float> OutVLTexture : register(u0);

#ifndef TG_DIM
#	define TG_DIM 256
#endif

#ifndef WINDOW
#	define WINDOW 12
#endif

cbuffer CBSize : register(b0)
{
	float4 invSize;
	float4 maxUV;
}

cbuffer VLData : register(b1)
{
	int2 screenSize;
	int2 screenSizeMin1;
	// (48n) 1 = reject blur taps by relative linear depth (official CS #2880), 0 = the old raw-depth sum test.
	uint relativeDepthReject;
	uint3 pad0;
}

static const int TapOffsets[5] = { -12, -6, 0, 6, 12 };
static const float TapWeights[5] = { 0.178400, 0.210431, 0.222338, 0.210431, 0.178400 };
// Relative linear-depth difference range over which a tap fades out, so rejection is the same near and far.
static const float DepthRejectStart = 0.1;
static const float DepthRejectEnd = 0.3;

groupshared float vl[TG_DIM];
groupshared float depth[TG_DIM];

[numthreads(TG_DIM, 1, 1)] void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID) {
	int idx = groupThreadId.x;
	int base = idx - WINDOW;
	int x = groupId.x * (TG_DIM - WINDOW * 2) + base;
	int y = groupId.y;

	int2 pix = relativeDepthReject ? clamp(int2(x, y), 0, screenSizeMin1.xy) : min(int2(x, y), screenSizeMin1.xy);
	float vlValue = InVLTexture[pix];
	vl[idx] = vlValue;
	float depthValue = DepthTexture[pix];
	if (relativeDepthReject)
		depthValue = SharedData::GetScreenDepth(depthValue);
	depth[idx] = depthValue;

	GroupMemoryBarrierWithGroupSync();

	if (base >= 0 && base < TG_DIM - WINDOW * 2) {
		[branch] if (relativeDepthReject)
		{
			// (48n, official CS #2880) The raw-depth sum test let a distant tree line or wall top pass against the sky
			// and copied it 6 and 12 render pixels further (ghosts above it, worse at lower upscaler presets).
			float rcpCenterDepth = rcp(max(depthValue, 1e-4));
			// The center tap always has full weight, so weightSum is never zero.
			float weightedSum = 0.0;
			float weightSum = 0.0;
			[unroll] for (uint i = 0; i < 5; i++)
			{
				int tap = idx + TapOffsets[i];
				float relativeDelta = abs(depth[tap] - depthValue) * rcpCenterDepth;
				float weight = TapWeights[i] * (1.0 - smoothstep(DepthRejectStart, DepthRejectEnd, relativeDelta));
				weightedSum += weight * vl[tap];
				weightSum += weight;
			}
			vlValue = weightedSum / weightSum;
		}
		else
		{
			int min12 = idx - 12;
			int min6 = idx - 6;
			int plus6 = idx + 6;
			int plus12 = idx + 12;

			float diff = depthValue * 4 - depth[min12] - depth[min6] - depth[plus6] - depth[plus12];
			vlValue = abs(diff) <= 0.002f ? vl[min12] * 0.178400f + vl[min6] * 0.210431f + vlValue * 0.222338f + vl[plus6] * 0.210431f + vl[plus12] * 0.178400f : vlValue;
		}

		OutVLTexture[int2(x, y)] = vlValue;
	}
}
