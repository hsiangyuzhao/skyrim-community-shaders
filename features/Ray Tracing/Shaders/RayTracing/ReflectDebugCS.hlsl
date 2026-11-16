// (48a) RT reflections step 1: the debug views of the reflection rays (TraceReflectCS output, render size) into the debug colour texture
// (drawn over the frame by OverlayCS like the other Ray Tracing views). Not traced pixels stay transparent.
//   1 Hit Type: green static, orange object, cyan foliage / cut-out, purple character, brown terrain, blue miss (left the scene / too far),
//     (48d) grey: nothing within the shortened range of a rough ray (Rough Rays: Leaves Solid; the gap fill keeps the old cubemap there);
//   2 Hit Distance: white near -> dark far (log scale up to Params.x units); a miss is blue;
//   3 Normal Confidence: how sure the reprojected last-frame normal is (green sure .. red fell back to the depth normal).
//   (48f) 5 RT Albedo (Capture Materials): the hit's own texture colour from its material's 128x128 copy at the interpolated texcoord, terrain
//     as its six layers' average colours blended by the weights at the hit times the quadrant's vertex colour. Missing data has its own
//     colour: magenta = the hit has no attributes (captured before / without materials), red-orange = no material (no readable texture),
//     yellow / black checks = the texture copy is still pending, purple = a character (48g), dim blue / grey = miss / range end; dark
//     magenta everywhere = this frame has no surface record (materials off or not ready).

cbuffer ReflectDebugCB : register(b0)
{
	uint4 Size;     // render width, height, view (1..3, 5), (48f) w: 1 = the surface record of this frame is valid
	float4 Params;  // x max ray length (units)
};

Texture2D<uint> ReflectOut : register(t0);
RWTexture2D<unorm float4> DebugColor : register(u0);

// (48f) RT Albedo only (unbound otherwise).
Texture2D<uint> ReflectSurf0 : register(t1);
Texture2D<uint> ReflectSurf1 : register(t2);
struct MaterialGpu
{
	uint Layer;  // global page layer, 0xFFFF = none / pending
	uint Flags;  // 1 terrain, 2 textured, 4 pending
	uint Terrain0;
	uint Terrain1;
	uint Terrain2;
	uint Tint;
	uint Pad0;
	uint Pad1;
	float4 UvTransform;
};
StructuredBuffer<MaterialGpu> Materials : register(t3);
Texture2DArray<float4> Page0 : register(t4);
Texture2DArray<float4> Page1 : register(t5);
Texture2DArray<float4> Page2 : register(t6);
Texture2DArray<float4> Page3 : register(t7);
Texture2DArray<float4> Page4 : register(t8);
Texture2DArray<float4> Page5 : register(t9);
SamplerState LinearWrap : register(s0);

static const uint kPageLayers = 256u;  // Backend kMaterialPageLayers
static const uint kMatNoAttr = 0xFFFEu, kMatCharacter = 0xFFFDu, kMatMarkers = 0xFFF0u;

float3 SamplePage(uint layer, float2 uv, float lod)
{
	const uint page = layer / kPageLayers;
	const float3 at = float3(uv, float(layer % kPageLayers));
	[branch] switch (page)
	{
	case 0u:
		return Page0.SampleLevel(LinearWrap, at, lod).rgb;
	case 1u:
		return Page1.SampleLevel(LinearWrap, at, lod).rgb;
	case 2u:
		return Page2.SampleLevel(LinearWrap, at, lod).rgb;
	case 3u:
		return Page3.SampleLevel(LinearWrap, at, lod).rgb;
	case 4u:
		return Page4.SampleLevel(LinearWrap, at, lod).rgb;
	default:
		return Page5.SampleLevel(LinearWrap, at, lod).rgb;
	}
}

float4 Checks(uint2 p, float3 a, float3 b)
{
	return float4(((p.x >> 3) ^ (p.y >> 3)) & 1u ? a : b, 0.9);
}

float4 AlbedoColor(uint2 p, uint type, float distance)
{
	if (type == 0u)
		return float4(0.2, 0.4, 1.0, 0.35);  // miss
	if (type == 6u)
		return float4(0.5, 0.5, 0.5, 0.35);  // range end
	if (Size.w == 0u)
		return float4(0.45, 0.0, 0.45, 0.6);  // no surface record this frame
	const uint s0 = ReflectSurf0[p];
	const uint s1 = ReflectSurf1[p];
	const uint material = s1 & 0xFFFFu;
	if (material == kMatCharacter)
		return float4(0.75, 0.2, 1.0, 0.9);
	if (material == kMatNoAttr)
		return float4(1.0, 0.0, 1.0, 0.95);
	if (material >= kMatMarkers)
		return float4(1.0, 0.3, 0.0, 0.95);
	const MaterialGpu m = Materials[material];
	const float lod = clamp(log2(1.0 + distance / 512.0), 0.0, 6.0);
	[branch] if (m.Flags & 1u)
	{
		// Terrain: the layers' average colours (their copies' 1x1 mip) by the weights at the hit, times the vertex colour.
		const uint layers[6] = { m.Terrain0 & 0xFFFFu, m.Terrain0 >> 16, m.Terrain1 & 0xFFFFu, m.Terrain1 >> 16, m.Terrain2 & 0xFFFFu, m.Terrain2 >> 16 };
		float3 sum = 0.0;
		float total = 0.0;
		bool pending = false;
		[unroll] for (uint i = 0u; i < 6u; ++i)
		{
			const float w = float((s0 >> (5u * i)) & 31u) / 31.0;
			if (w <= 0.0)
				continue;
			if (layers[i] == 0xFFFFu) {
				pending = pending || (m.Flags & 4u) != 0u;
				continue;
			}
			sum += w * SamplePage(layers[i], float2(0.5, 0.5), 7.0);
			total += w;
		}
		if (total <= 0.0)
			return pending ? Checks(p, float3(1.0, 0.9, 0.0), float3(0.0, 0.0, 0.0)) : float4(1.0, 0.3, 0.0, 0.95);
		const float3 tint = float3(m.Tint & 0xFFu, (m.Tint >> 8) & 0xFFu, (m.Tint >> 16) & 0xFFu) / 255.0;
		return float4(sum / total * tint, 1.0);
	}
	if (m.Layer == 0xFFFFu)
		return (m.Flags & 4u) ? Checks(p, float3(1.0, 0.9, 0.0), float3(0.0, 0.0, 0.0)) : float4(1.0, 0.3, 0.0, 0.95);
	float2 uv = f16tof32(uint2(s0 & 0xFFFFu, s0 >> 16));
	uv = uv * m.UvTransform.zw + m.UvTransform.xy;
	if (!all(isfinite(uv)))
		uv = 0.0;
	return float4(SamplePage(m.Layer, uv, lod), 1.0);
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const uint v = ReflectOut[dtid.xy];
	float4 color = 0.0;
	if (v & 0x80u) {
		const uint type = v & 7u;
		const float confidence = float((v >> 3) & 7u) / 7.0;
		const float distance = f16tof32(v >> 16);
		if (Size.z == 5u) {
			color = AlbedoColor(dtid.xy, type, distance);
		} else if (Size.z == 1u) {
			if (type == 0u)
				color = float4(0.2, 0.4, 1.0, 0.6);
			else if (type == 1u)
				color = float4(0.1, 0.9, 0.2, 0.6);
			else if (type == 2u)
				color = float4(1.0, 0.55, 0.1, 0.6);
			else if (type == 3u)
				color = float4(0.1, 0.9, 0.9, 0.6);
			else if (type == 4u)
				color = float4(0.75, 0.2, 1.0, 0.6);
			else if (type == 6u)
				color = float4(0.5, 0.5, 0.5, 0.6);  // (48d) range end
			else
				color = float4(0.55, 0.35, 0.15, 0.7);
		} else if (Size.z == 2u) {
			if (type == 0u) {
				color = float4(0.2, 0.4, 1.0, 0.6);
			} else if (type == 6u) {
				color = float4(0.5, 0.5, 0.5, 0.6);  // (48d) range end
			} else {
				const float t = saturate(log2(1.0 + distance / 70.0) / log2(1.0 + max(Params.x, 70.0) / 70.0));
				color = float4((1.0 - t).xxx, 0.8);
			}
		} else {
			color = float4(lerp(float3(1.0, 0.15, 0.1), float3(0.1, 1.0, 0.2), confidence), 0.6);
		}
	}
	DebugColor[dtid.xy] = color;
}
