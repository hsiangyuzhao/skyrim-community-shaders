// (batch 39, item 2) Runs right after the opaque pass, only while grass or LOD terrain skips the
// depth prepass. Folds the finished main depth into Terrain Blending's blended depth copies
// (made right after the prepass), so every later reader of "the prepass depth" sees the
// geometry that skipped it. min() keeps the blended terrain where it was closer.

Texture2D<unorm float> MainDepthTexture : register(t0);

RWTexture2D<float> BlendedDepthTexture : register(u0);
RWTexture2D<unorm half> BlendedDepthTexture16 : register(u1);

[numthreads(8, 8, 1)] void main(uint3 DTid
								: SV_DispatchThreadID) {
	float depth = min(BlendedDepthTexture[DTid.xy], MainDepthTexture[DTid.xy]);
	BlendedDepthTexture[DTid.xy] = depth;
	BlendedDepthTexture16[DTid.xy] = depth;
}
