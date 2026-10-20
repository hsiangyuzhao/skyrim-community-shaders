// (46t) Ray-traced ambient occlusion: the constant buffer shared by its D3D11 passes (RTAOFilterCS, RTAOTemporalCS,
// RTAOCompositeCS). Mirrors RayTracing::AOCompositeCB in src/Features/RayTracing.h.

cbuffer RTAOCB : register(b0)
{
	uint4 Size;     // render width, height, flags, debug view
	float4 Params;  // x strength, y this frame's weight in the history, z history clamp slack, w RT fade-in (0..1)
	float4 Detail;  // (46t) x detail strength, y normal weight power (0 = only the 45 degree gate), zw unused
	float4 Camera;  // (46t) the game's CameraData (far, near, far - near, far * near): linear depth = w / (x - depth * z)
	// (47l) "RTAO: Temporal v2" (kFlagTemporalV2). Appended (the fields above keep their offsets); filled every frame.
	row_major float4x4 InvViewProj;   // this frame's (jittered, camera-relative), as TraceAOCS'
	row_major float4x4 PrevViewProj;  // last frame's (jittered), relative to its own camera: clip.w = its linear depth
	row_major float4x4 ViewInverse;   // this frame's view-to-world (rotation part: the G-buffer's view-space normal to world)
	float4 CameraDelta;               // xyz this camera minus last frame's (world units); w pixels per unit at linear depth 1 (0.5 x height x proj[1][1])
	float4 Temporal;                  // x min history weight (0.1), y large-scale filter sigma (world units), z history-fix frames, w max history length
	float4 Clamp;                     // x statistical clamp k (x the 5x5 standard deviation); its minimum half width: y detail, z large-scale; w unused
};

static const uint kFlagContact = 1u;         // the SSGI contact AO is kept (RTAO mode "keeps SSGI contact AO")
static const uint kFlagDarker = 2u;          // RTAO mode "darker of RTAO and SSGI AO"
static const uint kFlagHistory = 4u;         // the large-scale history holds last frame
static const uint kFlagSplit = 8u;           // debug split
static const uint kFlagDetail = 16u;         // (46t) the detail term is multiplied in (TraceAOCS .a, RTAOFilterCS RTAO_DETAIL)
static const uint kFlagValidity = 32u;       // (46t) TraceAOCS .b is the far sample's weight (two scales), not the hit distance
static const uint kFlagDetailHistory = 64u;  // (46t) the detail history holds last frame
static const uint kFlagNormalDebug = 128u;   // (47k) t5 of RTAOCompositeCS holds this frame's Smooth Normals debug view (AONormalPrepCS)
static const uint kFlagTemporalV2 = 256u;    // (47l) RTAO: Temporal v2 (RTAOTemporalCS RTAO_TEMPORAL_V2; RTAOFilterCS: the large-scale sigma in world units)
static const uint kFlagGeometryValid = 512u; // (47l) Temporal v2: last frame's geometry (linear depth, normal) and history lengths are valid

// Debug views (RayTracing::AODebugView)
static const uint kViewRaw = 1u;
static const uint kViewFiltered = 2u;
static const uint kViewSplit = 3u;
static const uint kViewRawDetail = 4u;       // (46t)
static const uint kViewFilteredDetail = 5u;  // (46t)
static const uint kViewFinal = 6u;           // (46t)
static const uint kViewNormalConfidence = 7u;  // (47k) Smooth Normals: confidence
static const uint kViewNormalNs = 8u;          // (47k) Smooth Normals: the hemisphere normal
static const uint kViewNormalNg = 9u;          // (47k) Smooth Normals: the depth normal
static const uint kViewHistory = 10u;          // (47n) Temporal v2: history rejected / clamp active / length / moving-occluder flag (RTAOTemporalCS writes it)

static const uint kFlagDynamicHistory = 1024u; // (47n) RTAO: Tight History Near Moving Things (RTAOTemporalCS RTAO_TEMPORAL_V2)

// (47n) TraceAOCS .b: the far sample's weight w (0, 0.5, 1) stored as 0.4 w, plus 0.6 where one of the pixel's rays was blocked by a
// character or a moving object (exact in 8 bits too).
float RawFarWeight(float a_b)
{
	return saturate((a_b > 0.5 ? a_b - 0.6 : a_b) * 2.5);
}
bool RawDynamic(float a_b)
{
	return a_b > 0.5;
}

// (47r) "RTAO: Filter Only Where Traced" (kFlagTileSkip): t10 holds, per 8x8 tile of the render size, the Chebyshev distance in tiles to the
// nearest tile with a traced pixel (TraceAOCS .g > 0; RTAOFilterCS RTAO_TILE_MARK / RTAO_TILE_DIST; 255 = farther than the largest reach).
// A pass skips a group whose tiles are all farther than the reach its consumers need from it (the traced pixels read nothing it skipped:
// every traced pixel's value is exactly 47q's). Unbound (the flag off): reads 0, nothing is skipped.
static const uint kFlagTileSkip = 2048u;
Texture2D<uint> TileDist : register(t10);

bool TilesFar(int2 a_lo, int2 a_hi, uint a_reach)
{
	if (!(Size.z & kFlagTileSkip))
		return false;
	const int2 maxT = int2((Size.xy + 7u) / 8u) - 1;
	const int2 hi = min(a_hi, maxT);
	uint d = 255u;
	for (int y = a_lo.y; y <= hi.y; ++y)
		for (int x = a_lo.x; x <= hi.x; ++x)
			d = min(d, TileDist[int2(x, y)]);
	return d > a_reach;
}
