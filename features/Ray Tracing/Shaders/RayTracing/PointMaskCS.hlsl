// (46c) Ray-traced point-light shadows indoors: the engine's shadow mask channels of its shadow lights.
//
// Indoors the engine's spot / omni shadow lights (fireplaces, Window Shadows Ultimate's window beams) take their
// shadow from the shadow mask (Lighting: shadowColor[light.shadowLightIndex]). For each such light with a ray-traced
// source this frame its channel is rewritten here, screen-space like the engine's own mask:
//   - the pixel traced the light (its source is in one of the pixel's slots): the denoised visibility, times the spot's
//     cone (the engine's spot mask: 1 - r^falloff, r = the projected distance from the axis, 1 at the half width);
//   - else, when the engine drew the light's shadow map this frame: the engine's value (unchanged);
//   - else (its shadow map was left out, Stop Engine Shadow Maps): the cone (spot) or 1 (omni) -- unshadowed.
// The other channels are copied. Output: the work texture copied over the mask's render area afterwards.

#include "RayTracing/Common.hlsli"

struct Channel
{
	float4 Pos;    // camera-relative light position, w source slot + 1 (0 = no traced result)
	float4 Dir;    // spot direction (light to surface), w falloff exponent
	float4 Right;  // w tan half width
	float4 Up;     // w tan half height
	uint4 Flags;   // x: kWrite | kSpot | kSkipped
};

cbuffer PointMaskCB : register(b1)
{
	Channel Channels[4];
};

Texture2D<float4> Mask : register(t0);
Texture2D<float> Depth : register(t1);          // the depth the engine's mask was computed from
Texture2D<unorm float4> PLVis : register(t2);   // denoised point-light visibility per slot (PointDenoiseCS)
Texture2D<unorm float4> PLIdx : register(t3);   // per slot: source + 1 (low 7 bits)
RWTexture2D<unorm float4> OutMask : register(u0);

static const uint kWrite = 1u;
static const uint kSpot = 2u;
static const uint kSkipped = 4u;

float Cone(Channel a_channel, float3 a_position)
{
	const float3 v = a_position - a_channel.Pos.xyz;
	const float along = dot(v, a_channel.Dir.xyz);
	if (along <= 1e-3)
		return 0.0;
	const float x = dot(v, a_channel.Right.xyz) / (along * max(a_channel.Right.w, 1e-4));
	const float y = dot(v, a_channel.Up.xyz) / (along * max(a_channel.Up.w, 1e-4));
	const float r = sqrt(x * x + y * y);
	if (r >= 1.0)
		return 0.0;
	return saturate(1.0 - pow(max(r, 1e-6), max(a_channel.Dir.w, 0.01)));
}

// The channel's new value (a_engine: the engine's).
float ChannelValue(Channel a_channel, float a_engine, bool a_surface, float3 a_position, uint4 a_ids, float4 a_vis)
{
	const uint flags = a_channel.Flags.x;
	float value = a_engine;
	[branch] if (flags & kWrite)
	{
		if (!a_surface) {
			if (flags & kSkipped)
				value = 1.0;
		} else {
			const float cone = (flags & kSpot) ? Cone(a_channel, a_position) : 1.0;
			const uint source = uint(a_channel.Pos.w + 0.5);
			const bool4 match = (source != 0u).xxxx && (a_ids == source.xxxx);
			if (any(match))
				value = cone * (match.x ? a_vis.x : (match.y ? a_vis.y : (match.z ? a_vis.z : a_vis.w)));
			else if (flags & kSkipped)
				value = cone;
		}
	}
	return value;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 p = int2(dtid.xy);
	const float4 mask = Mask[p];
	const float depth = Depth[p];
	const bool surface = !RTIsSky(depth);
	const float3 P = surface ? RTPosition(p, depth) : float3(0.0, 0.0, 0.0);
	uint4 ids = 0u;
	float4 vis = 1.0;
	[branch] if (surface && (Channels[0].Pos.w + Channels[1].Pos.w + Channels[2].Pos.w + Channels[3].Pos.w) > 0.0)
	{
		ids = uint4(PLIdx[p] * 255.0 + 0.5) & 0x7Fu;
		vis = PLVis[p];
	}
	float4 result;
	result.x = ChannelValue(Channels[0], mask.x, surface, P, ids, vis);
	result.y = ChannelValue(Channels[1], mask.y, surface, P, ids, vis);
	result.z = ChannelValue(Channels[2], mask.z, surface, P, ids, vis);
	result.w = ChannelValue(Channels[3], mask.w, surface, P, ids, vis);
	OutMask[p] = result;
}
