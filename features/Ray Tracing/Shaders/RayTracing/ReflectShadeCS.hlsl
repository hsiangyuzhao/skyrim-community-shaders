// (48g) RT reflections, the shading half of the material step: the colour of every traced reflection hit from its surface record, so the
// SSRT specular ray march's gap fill can use a real material colour for hits off screen (instead of the dynamic cubemap guess).
//
// Dispatched right before the SSRT specular ray march (RayTracing::AcquireReflectionGapFill, after the reflection list's wait), at render size,
// only with RT Reflections = Gap Fill, Capture Materials and "Off-screen Hit Color: Material". The game's constant buffers of that pass are
// bound (SharedData b5 / b6, FrameBuffer b12); this pass binds only b0, t0-t15, u0-u2, s0 and unbinds them.
//
// Per traced pixel with a hit (ReflectOut type static / object / cut-out / character / terrain):
//   hit point  = the pixel's surface (this frame's depth, FrameBuffer::CameraViewProjInverse as the trace) + R x the hit distance;
//   albedo     = the material's 128x128 copy at the interpolated texcoord (terrain: the layers' average colours x the quadrant's vertex colour),
//                as ReflectDebugCS's RT Albedo; Color::Diffuse (linear lighting aware);
//   sun        = DirLightColor (as RunGrass / Effect: linear lighting multipliers) x N.L x the trace's sun visibility (ReflectOut bit 9 tested,
//                bit 8 lit; untested = 1); no Physical Sky transmittance, no cloud shadows (approximation);
//   ambient    = Color::Ambient(DirectionalAmbient . N) x Skylighting's diffuse visibility at the hit (as DeferredCompositeCS's
//                ambientSkyVisibility; without its MultiBounceAO tint, without IBL) - approximation;
//   lamps      = Light Limit Fix's light list (every light, no cluster: the hit is usually off screen), the forward path's attenuation
//                (Inverse Square Lighting when the light has it, else 1 - (d / r)^2), x N.L x fade, no shadows (approximation);
//   emissive   = the shape's own emissive colour x multiplier (no glow maps);
//   colour     = albedo x (sun + ambient + lamps) + emissive in the game's lighting space, then Color::IrradianceToLinear - the same space the
//                ray march's screen colour has after IrradianceToLinear (it applies SpecularMult and "Off-screen Hit Brightness").
// (48h) "Material Hits: Glow" (Size.w & kFlagGlow) replaces the emissive line by Lighting.hlsl's own emission (non-PBR path):
//   emit       = Color::EmitColor(EmitColor) (EmitColor = emissive colour x multiplier of OwnEmit, External Emittance - the reference's source
//                colour, refreshed by the CPU as the time of day changes it - or glow-map shapes), x Color::Glowmap(glow texture) for the
//                glow-map technique (its copy in the same pages, Materials[].Glow.x);
//   colour     = Color::Diffuse(albedo) x (sun + ambient + lamps + emit), as Lighting.hlsl's diffuseColor += emitColor; color += diffuseColor
//                x baseColor (the emission is tinted by the texture: stained glass, lamp shades). A glow-map hit whose glow copy is still
//                pending has no material data yet (cubemap path) and fades in from the later of its two copies.
//   Off: exactly 48g's line above (Emissive.rgb is still 48g's own-emit product).
// No fog, no specular of the hit surface, no subsurface / translucency, no wetness (all approximations; "RT Shading vs Screen" measures it).
// Output u0 RGBA16F: rgb the colour, a = how much of it to use (0 = no material data: no attributes, no material, copy pending; fades in over
// Params.x frames once its copy is ready, x Params.y the global fade of the Off-screen Hit Color switch). Untraced / miss pixels: 0.
// Stats (u1, 8 uints, cleared by the CPU each frame): [0] hits with material data, [1] hits without, [2] / [3] / [4] the brightness
// comparison where a hit is also on screen (per 8x8 group: mean shaded / mean screen luminance x 256, summed; groups); (48h) [5] the sum of
// the per-pixel log2(shaded / screen) (each clamped to +-4) x 32 as a signed 32-bit integer (rounded per group, two's complement adds; at
// most 64 x 4 x 32 = 8192 a group, so even a 4K render size stays far inside int32), [6] its pixel count: the geometric mean the panel shows.
// Debug (Size.w & kFlagDebug, u2): "RT Shading vs Screen".

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Skylighting/Skylighting.hlsli"

cbuffer ReflectShadeCB : register(b0)
{
	uint4 Size;     // render width, height, frame counter (as MaterialCopy readyFrame), flags (kFlag*)
	float4 Params;  // x fade-in frames, y global fade (0..1), z light count, w unused
};

static const uint kFlagSkylighting = 1u, kFlagLights = 2u, kFlagDebug = 4u, kFlagStats = 8u, kFlagSunVisibility = 16u;
static const uint kFlagGlow = 32u;                                           // (48h) "Material Hits: Glow"
static const uint kEmitOn = 1u, kEmitGlowMap = 2u, kEmitGlowPending = 4u;  // (48h) Materials[].Glow.z (Backend kEmit*)

Texture2D<uint> ReflectOut : register(t0);
Texture2D<uint> ReflectDir : register(t1);
Texture2D<uint> ReflectSurf0 : register(t2);
Texture2D<uint> ReflectSurf1 : register(t3);
struct MaterialGpu
{
	uint Layer;  // global page layer, 0xFFFF = none / pending
	uint Flags;  // 1 terrain, 2 textured, 4 pending
	uint Terrain0;
	uint Terrain1;
	uint Terrain2;
	uint Tint;
	uint ReadyFrame;
	uint Pad;
	float4 UvTransform;
	float4 Emissive;  // (48g) rgb own emissive colour x multiplier (OwnEmit only): the Glow switch's off path
	float4 Emit;      // (48h) rgb the emissive colour x multiplier (EmitColor; External Emittance refreshed), w the multiplier
	uint4 Glow;       // (48h) x glow copy layer (0xFFFF none / pending), y its ready frame, z kEmit* flags, w unused (MaterialGpu 96 bytes)
};
StructuredBuffer<MaterialGpu> Materials : register(t4);
Texture2DArray<float4> Page0 : register(t5);
Texture2DArray<float4> Page1 : register(t6);
Texture2DArray<float4> Page2 : register(t7);
Texture2DArray<float4> Page3 : register(t8);
Texture2DArray<float4> Page4 : register(t9);
Texture2DArray<float4> Page5 : register(t10);
Texture3D<sh2> SkylightingProbeArray : register(t11);
Texture2DArray<float3> stbn_vec3_2Dx1D_128x128x64 : register(t12);
struct ShadeLight  // Light Limit Fix's Light (LightLimitFix/Common.hlsli, 96 bytes)
{
	float3 color;
	float fade;
	float radius;
	float invRadius;
	float fadeZone;
	float sizeBias;
	float4 positionWS[2];
	uint4 roomFlags;
	uint lightFlags;
	uint shadowLightIndex;
	uint rtShadow;
	uint pad1;
};
StructuredBuffer<ShadeLight> Lights : register(t13);
Texture2D<float> SceneDepth : register(t14);
Texture2D<float4> ScreenColor : register(t15);
SamplerState LinearWrap : register(s0);

RWTexture2D<float4> ShadeOut : register(u0);
RWByteAddressBuffer Stats : register(u1);
RWTexture2D<unorm float4> DebugColor : register(u2);

static const uint kPageLayers = 256u;  // Backend kMaterialPageLayers
static const uint kMatMarkers = 0xFFF0u;
static const uint kLightDisabled = 1u << 9, kLightInverseSquare = 1u << 10, kLightLinear = 1u << 11;  // LightLimitFix::LightFlags
static const float kIslScaledUnitsSq = 0.8 * 70.0 * 70.0;                                             // InverseSquareLighting::SCALED_UNITS_SQ

groupshared float gsShaded[64];
groupshared float gsScreen[64];
groupshared uint gsCounts[64];
groupshared float gsLog[64];  // (48h) per-pixel log2(shaded / screen), clamped; only where gsShaded >= 0

float3 OctDecode(float2 e)
{
	float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
	const float t = saturate(-n.z);
	n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

float3 OctDecode16(uint w)
{
	return OctDecode(float2(w & 0xFFFFu, w >> 16) / 65535.0 * 2.0 - 1.0);
}

float3 OctDecode8(uint w)
{
	return OctDecode(float2(w & 0xFFu, (w >> 8) & 0xFFu) / 255.0 * 2.0 - 1.0);
}

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

float3 PositionAt(uint2 p, float depth)
{
	const float2 uv = (float2(p) + 0.5) / float2(Size.xy);
	const float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	const float4 pos = mul(FrameBuffer::CameraViewProjInverse[0], clip);
	return pos.xyz / pos.w;
}

// The hit's albedo (game texture values as stored) and whether it is known (0 = no data / pending). (48h) o_uv / o_lod: where the copy was
// read (the glow copy is read at the same place: Lighting.hlsl samples the glow map with the diffuse texcoord).
float Albedo(uint s0, MaterialGpu m, float distance, out float3 o_albedo, out float2 o_uv, out float o_lod)
{
	float3 albedo = 0.0;
	float known = 0.0;
	o_uv = 0.0;
	o_lod = 0.0;
	[branch] if (m.Flags & 1u)
	{
		float3 sum = 0.0;
		float total = 0.0;
		float missing = 0.0;
		[unroll] for (uint i = 0u; i < 6u; ++i)
		{
			const uint word = i < 2u ? m.Terrain0 : (i < 4u ? m.Terrain1 : m.Terrain2);
			const uint layer = (i & 1u) ? (word >> 16) : (word & 0xFFFFu);
			const float w = float((s0 >> (5u * i)) & 31u) / 31.0;
			[branch] if (w > 0.0 && layer != 0xFFFFu)
			{
				sum += w * SamplePage(layer, float2(0.5, 0.5), 7.0);
				total += w;
			}
			else
			{
				missing += w;
			}
		}
		const float3 tint = float3(m.Tint & 0xFFu, (m.Tint >> 8) & 0xFFu, (m.Tint >> 16) & 0xFFu) / 255.0;
		albedo = total > 0.0 ? sum / max(total, 1e-6) * tint : 0.0;
		known = total > 0.0 ? total / (total + missing) : 0.0;
	}
	else if (m.Layer != 0xFFFFu)
	{
		float2 uv = f16tof32(uint2(s0 & 0xFFFFu, s0 >> 16));
		uv = uv * m.UvTransform.zw + m.UvTransform.xy;
		if (!all(isfinite(uv)))
			uv = 0.0;
		o_lod = clamp(log2(1.0 + distance / 512.0), 0.0, 6.0);
		o_uv = uv;
		albedo = SamplePage(m.Layer, uv, o_lod);
		known = 1.0;
	}
	o_albedo = albedo;
	return known;
}

// (48h) Lighting.hlsl's emission (non-PBR): Color::EmitColor(EmitColor) [x Color::Glowmap(glow)] - Color::EmitColor's LIGHTING-only
// form with this material's multiplier instead of the LLPerGeometry constant.
float3 VanillaEmission(MaterialGpu m, float2 uv, float lod)
{
	float3 emit = 0.0;
	[branch] if (m.Glow.z & kEmitOn)
	{
		const float mult = max(m.Emit.w, 0.0);
		emit = max(m.Emit.rgb, 0.0);
		if (SharedData::linearLightingSettings.enableLinearLighting)
			emit = pow(abs(emit / max(mult, 1e-5)), SharedData::linearLightingSettings.emitColorGamma) * mult * SharedData::linearLightingSettings.emitColorMult;
		[branch] if (m.Glow.z & kEmitGlowMap)
		{
			// No usable glow copy (refused texture): no emission rather than an unmasked glow over the whole shape.
			float3 glow = 0.0;
			[branch] if ((m.Glow.x & 0xFFFFu) != 0xFFFFu)
				glow = Color::Glowmap(SamplePage(m.Glow.x & 0xFFFFu, uv, lod));
			emit *= glow;
		}
	}
	return emit;
}

float3 ShadeHit(uint2 p, uint w0, float3 hitPos, float3 N, float3 albedo, MaterialGpu m, float2 uv, float lod)
{
	// The game's lighting space (as Lighting.hlsl before its output; linear lighting aware through Color::*).
	const float3 diffuse = Color::Diffuse(albedo);
	const float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ?
	                                 SharedData::linearLightingSettings.dirLightMult :
	                                 1.0;
	const float3 dirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) *
	                             llDirLightMult;
	float sunVisibility = 1.0;
	if ((Size.w & kFlagSunVisibility) && (w0 & 0x200u))
		sunVisibility = (w0 & 0x100u) ? 1.0 : 0.0;
	float3 light = dirLightColor * saturate(dot(N, normalize(SharedData::DirLightDirection.xyz))) * sunVisibility;

	float3 ambient = Color::Ambient(max(0.0, mul(SharedData::DirectionalAmbient, float4(N, 1.0))));
	[branch] if ((Size.w & kFlagSkylighting) && !SharedData::InInterior)
	{
		// As DeferredCompositeCS's ambientSkyVisibility (folded normal, upward gain, fade at the probe grid's edge, MinDiffuseVisibility).
		const sh2 sh = Skylighting::sample(SharedData::skylightingSettings, SkylightingProbeArray, stbn_vec3_2Dx1D_128x128x64, p, hitPos, N);
		const float3 folded = normalize(float3(N.xy, max(0.0, N.z)));
		float visibility = saturate(SphericalHarmonics::FuncProductIntegral(sh, SphericalHarmonics::EvaluateCosineLobe(folded)) / Math::PI);
		visibility = lerp(1.0, visibility, Skylighting::getFadeOutFactor(hitPos));
		visibility *= 1.0 + saturate(N.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);
		ambient *= Skylighting::mixDiffuse(SharedData::skylightingSettings, visibility);
	}
	light += ambient;

	[branch] if (Size.w & kFlagLights)
	{
		const uint count = (uint)Params.z;
		[loop] for (uint i = 0u; i < count; ++i)
		{
			const ShadeLight l = Lights[i];
			if (l.lightFlags & kLightDisabled)
				continue;
			const float3 toLight = l.positionWS[0].xyz - hitPos;
			const float d = length(toLight);
			if (d >= l.radius || d <= 1e-3)
				continue;
			float attenuation;
			[branch] if (l.lightFlags & kLightInverseSquare)
			{
				const float t = saturate((l.radius - d) * l.fadeZone);
				attenuation = kIslScaledUnitsSq / (d * d + l.sizeBias) * (t * t * (3.0 - 2.0 * t));
			}
			else
			{
				const float f = saturate(d * l.invRadius);
				attenuation = 1.0 - f * f;
			}
			const float nl = saturate(dot(N, toLight / d));
			if (nl <= 0.0 || attenuation <= 0.0)
				continue;
			light += Color::PointLight(l.color, (l.lightFlags & kLightLinear) != 0) * attenuation * l.fade * nl;
		}
	}
	float3 color;
	[branch] if (Size.w & kFlagGlow)
		color = diffuse * (light + VanillaEmission(m, uv, lod));  // (48h) Lighting.hlsl: diffuseColor += emitColor; color = diffuseColor x baseColor
	else
		color = diffuse * light + max(m.Emissive.rgb, 0.0);  // 48g
	return Color::IrradianceToLinear(max(0.0, color));
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
	const uint2 p = dtid.xy;
	const bool inside = all(p < Size.xy);
	float4 shade = 0.0;
	float4 debug = 0.0;
	float shadedLum = -1.0, screenLum = -1.0, logRatio = 0.0;
	uint withMaterial = 0u, without = 0u;
	[branch] if (inside)
	{
		const uint w0 = ReflectOut[p];
		const uint type = w0 & 7u;
		[branch] if ((w0 & 0x80u) && type != 0u && type != 6u)
		{
			const float distance = f16tof32(w0 >> 16);
			const float3 R = OctDecode16(ReflectDir[p]);
			const float3 P = PositionAt(p, SceneDepth[p]);
			const float3 hitPos = P + R * distance;
			const uint s1 = ReflectSurf1[p];
			const uint material = s1 & 0xFFFFu;
			float use = 0.0;
			float3 color = 0.0;
			[branch] if (material < kMatMarkers)
			{
				const MaterialGpu m = Materials[material];
				float3 albedo;
				float2 uv;
				float lod;
				float known = Albedo(ReflectSurf0[p], m, distance, albedo, uv, lod);
				uint readyFrame = m.ReadyFrame;
				[branch] if ((Size.w & kFlagGlow) && (m.Glow.z & (kEmitOn | kEmitGlowMap)) == (kEmitOn | kEmitGlowMap))
				{
					// (48h) A glow-map hit waits for its glow copy too (else it shows as dark glass, then pops): the cubemap path until then,
					// then it fades in from the later of its two copies.
					if (m.Glow.z & kEmitGlowPending)
						known = 0.0;
					else if ((m.Glow.x & 0xFFFFu) != 0xFFFFu && Size.z - m.Glow.y < Size.z - readyFrame)
						readyFrame = m.Glow.y;
				}
				[branch] if (known > 0.0)
				{
					const float fade = saturate(float(Size.z - readyFrame) / max(Params.x, 1.0));
					use = known * fade * Params.y;
					color = ShadeHit(p, w0, hitPos, OctDecode8(s1 >> 16), albedo, m, uv, lod);
				}
			}
			shade = float4(color, use);
			withMaterial = use > 0.5 ? 1u : 0u;
			without = use > 0.5 ? 0u : 1u;
			// On screen as well (projected, the depth there is the hit's): the brightness comparison and the debug view.
			[branch] if ((Size.w & (kFlagDebug | kFlagStats)) && use > 0.0)
			{
				const float4 clip = mul(FrameBuffer::CameraViewProj[0], float4(hitPos, 1.0));
				bool onScreen = false;
				float3 screen = 0.0;
				[branch] if (clip.w > 1e-4)
				{
					const float3 ndc = clip.xyz / clip.w;
					const float2 uvHit = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
					[branch] if (all(uvHit > 0.02) && all(uvHit < 0.98) && ndc.z > 0.0 && ndc.z < 1.0)
					{
						const uint2 q = min(uint2(uvHit * float2(Size.xy)), Size.xy - 1u);
						const float3 Q = PositionAt(q, SceneDepth[q]);
						const float tolerance = 16.0 + 0.03 * length(hitPos);
						onScreen = length(Q - hitPos) < tolerance;
						if (onScreen)
							screen = Color::IrradianceToLinear(max(ScreenColor[q].rgb, 0.0));
					}
				}
				const float a = Color::RGBToLuminance(color);
				[branch] if (onScreen)
				{
					const float b = Color::RGBToLuminance(screen);
					shadedLum = min(a, 64.0);
					screenLum = min(b, 64.0);
					logRatio = clamp(log2(max(a, 1e-4) / max(b, 1e-4)), -4.0, 4.0);  // (48h) the robust (geometric) mean's sample
					const float ratio = clamp(log2(max(a, 1e-4) / max(b, 1e-4)), -2.0, 2.0);
					// Green = within about 20 %, red = RT brighter, blue = RT darker (full colour at 4x).
					debug = abs(ratio) < 0.25 ? float4(0.1, 0.9, 0.2, 0.75) : (ratio > 0.0 ? float4(1.0, 0.25 * (1.0 - ratio * 0.5), 0.1, 0.4 + 0.2 * ratio) : float4(0.1, 0.35 * (1.0 + ratio * 0.5), 1.0, 0.4 - 0.2 * ratio));
				}
				else
				{
					debug = float4(color / (1.0 + color), 0.55);  // off screen: the shaded colour itself (tone-mapped)
				}
			}
		}
		ShadeOut[p] = shade;
		if (Size.w & kFlagDebug)
			DebugColor[p] = debug;
	}
	[branch] if (Size.w & kFlagStats)
	{
		gsShaded[gi] = shadedLum;
		gsScreen[gi] = screenLum;
		gsCounts[gi] = withMaterial | (without << 16);
		gsLog[gi] = logRatio;
		GroupMemoryBarrierWithGroupSync();
		[branch] if (gi == 0u)
		{
			float sumA = 0.0, sumB = 0.0, sumLog = 0.0;
			uint n = 0u, counts = 0u;
			for (uint k = 0u; k < 64u; ++k) {
				counts += gsCounts[k];
				if (gsShaded[k] >= 0.0) {
					sumA += gsShaded[k];
					sumB += gsScreen[k];
					sumLog += gsLog[k];
					++n;
				}
			}
			uint previous;
			if (counts & 0xFFFFu)
				Stats.InterlockedAdd(0, counts & 0xFFFFu, previous);
			if (counts >> 16)
				Stats.InterlockedAdd(4, counts >> 16, previous);
			if (n > 0u) {
				Stats.InterlockedAdd(8, uint(round(sumA / float(n) * 256.0)), previous);
				Stats.InterlockedAdd(12, uint(round(sumB / float(n) * 256.0)), previous);
				Stats.InterlockedAdd(16, 1u, previous);
				Stats.InterlockedAdd(20, asuint(int(round(sumLog * 32.0))), previous);  // (48h) signed, two's complement
				Stats.InterlockedAdd(24, n, previous);
			}
		}
	}
}
