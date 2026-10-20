// (45) Ray Tracing (prototype): constants shared by the D3D11 helper passes. Mirrors
// RayTracing::CombineCB in src/Features/RayTracing.h.

cbuffer RayTracingCB : register(b0)
{
	row_major float4x4 InvViewProj;   // FrameBuffer::CameraViewProjInverse of the main view
	float4 Rect;                      // camera-relative xy min / max of the ray-traced area
	float4 Band;                      // x hand-off band width, y filter radius (px), z flags, w temporal blend (new frame weight)
	uint4 Size;                       // render width, height, debug view, frame
	row_major float4x4 PrevViewProj;  // (45j) last combined frame's CameraViewProj (camera-relative to its own camera)
	float4 CameraDelta;               // (45j) xyz camera position now minus then, w unused
	float4 Denoise;                   // (45k) x tan(sun cone half angle), y max filter radius (px), z max history frames, w history clamp (sigmas)
	float4 Extra;                     // (45k) x fade-in of the traced result (0..1), y blocker range, z unused, w (45l) max filter taps per side
	float4 Falloff;                   // (45l) x / y distance fall-off start / end, z filter radius scale beyond the end, w unused
	row_major float4x4 ViewProjUnjittered;      // (45l) SIGMA: camera motion vectors (camera-relative)
	row_major float4x4 PrevViewProjUnjittered;  // (45l) last frame's, relative to its own camera (CameraDelta)
	float4 CameraData;                // (45l) SIGMA viewZ: the game's far, near, far - near, far * near
	float4 SunDirection;              // (45l) SIGMA: xyz to the sun (world axes), w tan(sun cone half angle)
};

static const uint kFlagHandOffSSS = 1u;          // (45i-45k) Screen Space Shadows hand-off; removed in 45l (value kept free)
static const uint kFlagDebug = 2u;               // write the debug colour texture
static const uint kFlagCombineSSS = 4u;          // (45j) Screen Space Shadows kept, merged with the mask as min() instead of a product
static const uint kFlagHistoryValid = 32u;       // (45j) the history textures hold the last frame
static const uint kFlagTraceSkipped = 64u;       // (45j) pixels with a negative DepthMask were not traced (their 0 is a placeholder)
static const uint kFlagPenumbra = 128u;          // (45k) Denoiser Penumbra: rt from the penumbra filter (Denoised), clamped history (History4)
static const uint kFlagBlockerDist = 256u;       // (45k) Visibility.z holds the blocker distance (not the coverage hit delta)
static const uint kFlagSpatial = 512u;           // (45l) Denoised holds the penumbra filter (Penumbra); without it the temporal stage takes the small filter (Temporal)
static const uint kFlagDenoiseHalf = 1024u;      // (45l) the penumbra filter ran at half resolution (Denoised is half size)
static const uint kFlagSigma = 2048u;            // (45l) Denoiser NRD SIGMA: rt from its output
static const uint kFlagFalloffDenoise = 4096u;   // (45l) the filter radius shrinks with distance (Falloff)
static const uint kFlagUniformTiles = 8192u;     // (47r) CombineCS: the fast path for uniform tiles (t6 = the denoiser's tiles)

static const uint kViewOff = 0u;
static const uint kViewShadowSource = 1u;
static const uint kViewDisagreement = 2u;
static const uint kViewCoverage = 3u;
static const uint kViewRaw = 4u;
static const uint kViewHitDepth = 5u;  // (45e)
static const uint kViewPenumbra = 6u;    // (45k) penumbra filter radius
static const uint kViewHistory = 7u;     // (45k) history length
static const uint kViewAdaptive = 8u;    // (45k) pixels with adaptive extra rays
static const uint kViewRtSurface = 9u;   // (45k) Screen Space Shadows mask: traced casters
static const uint kViewDistance = 10u;   // (45l) quality by distance: fall-off, pattern-filled pixels
static const uint kViewCharRays = 11u;   // (45l) where the character ray ran

// Visibility.w bit flags (TraceShadowsCS, 45j)
static const uint kOutAlphaCandidate = 1u;
static const uint kOutCharacter = 2u;
static const uint kOutRtSurface = 4u;   // (45k)
static const uint kOutExtraRays = 8u;   // (45k)
static const uint kOutCharRay = 16u;    // (45l)
static const uint kOutFilled = 32u;     // (45l)
static const float kBlockerRange = 4096.0;  // (45k) Visibility.z = sqrt(t / range)

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
