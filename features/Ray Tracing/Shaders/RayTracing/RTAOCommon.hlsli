// (46t) Ray-traced ambient occlusion: the constant buffer shared by its D3D11 passes (RTAOFilterCS, RTAOTemporalCS,
// RTAOCompositeCS). Mirrors RayTracing::AOCompositeCB in src/Features/RayTracing.h.

cbuffer RTAOCB : register(b0)
{
	uint4 Size;     // render width, height, flags, debug view
	float4 Params;  // x strength, y this frame's weight in the history, z history clamp slack, w RT fade-in (0..1)
	float4 Detail;  // (46t) x detail strength, y normal weight power (0 = only the 45 degree gate), zw unused
	float4 Camera;  // (46t) the game's CameraData (far, near, far - near, far * near): linear depth = w / (x - depth * z)
};

static const uint kFlagContact = 1u;         // the SSGI contact AO is kept (RTAO mode "keeps SSGI contact AO")
static const uint kFlagDarker = 2u;          // RTAO mode "darker of RTAO and SSGI AO"
static const uint kFlagHistory = 4u;         // the large-scale history holds last frame
static const uint kFlagSplit = 8u;           // debug split
static const uint kFlagDetail = 16u;         // (46t) the detail term is multiplied in (TraceAOCS .a, RTAOFilterCS RTAO_DETAIL)
static const uint kFlagValidity = 32u;       // (46t) TraceAOCS .b is the far sample's weight (two scales), not the hit distance
static const uint kFlagDetailHistory = 64u;  // (46t) the detail history holds last frame
static const uint kFlagNormalDebug = 128u;   // (47k) t5 of RTAOCompositeCS holds this frame's Smooth Normals debug view (AONormalPrepCS)

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
