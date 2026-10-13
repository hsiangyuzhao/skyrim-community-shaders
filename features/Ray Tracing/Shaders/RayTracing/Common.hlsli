// (45) Ray Tracing (prototype): constants shared by the D3D11 helper passes. Mirrors
// RayTracing::CombineCB in src/Features/RayTracing.h.

cbuffer RayTracingCB : register(b0)
{
	row_major float4x4 InvViewProj;  // FrameBuffer::CameraViewProjInverse of the main view
	float4 Rect;                     // camera-relative xy min / max of the ray-traced area
	float4 Band;                     // x hand-off band width, y filter radius (px), z flags, w unused
	uint4 Size;                      // render width, height, debug view, frame
};

static const uint kFlagHandOffSSS = 1u;  // Screen Space Shadows off inside the traced area
static const uint kFlagDebug = 2u;       // write the debug colour texture

static const uint kViewOff = 0u;
static const uint kViewShadowSource = 1u;
static const uint kViewDisagreement = 2u;
static const uint kViewCoverage = 3u;
static const uint kViewRaw = 4u;
static const uint kViewHitDepth = 5u;  // (45e)

float3 RTPosition(int2 p, float depth)
{
	float2 uv = (float2(p) + 0.5) / float2(Size.xy);
	float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	float4 pos = mul(InvViewProj, clip);
	return pos.xyz / pos.w;
}

bool RTIsSky(float depth)
{
	return depth >= 0.999999 || depth <= 0.0;
}
