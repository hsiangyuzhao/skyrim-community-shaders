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
