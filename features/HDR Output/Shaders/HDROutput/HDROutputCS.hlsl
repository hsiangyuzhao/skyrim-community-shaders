// (batch 43) Native HDR10 output: the passes between the finished scene and the swap chain.
//
// Units. Everything the game hands us is "display gamma" -- the value an SDR monitor would show,
// what ISHDR wrote after Linear Lighting's gamma encode. Its display-linear value is v^2.2 with
// 1.0 = SDR white. In HDR, SDR white is shown at the paper-white level, so display-linear 1.0 =
// PaperWhiteNits. The scene can go above 1.0 (Color Grading's HDR highlight extension), up to
// PeakRel = peak / paper white; nothing is ever sent above the peak.
//
// HDR10 = BT.2020 primaries + SMPTE ST 2084 (PQ), written to an R10G10B10A2 target.
//
// Entry points (one file, compiled per entry):
//   CSEncodeScene  scene (fp16, display gamma) -> PQ; the DLSS-G HUD-less image / 1a gameplay output
//   CSResolveSDR   scene (fp16) -> the 10-bit fake back buffer as plain SDR (menus, HDR off)
//   CSComposite    final HDR10 image: scene + game UI layer + Present-hook overlays -> PQ
//   CSGammaToLinear / CSLinearToGamma   NIS "Linear" mode around the sharpener (fp16 <-> R11G11B10)

cbuffer HDROutputCB : register(b0)
{
	float PaperWhiteNits;  // display-linear 1.0
	float PeakRel;         // peak / paper white (> 1)
	float HudScale;        // HUD brightness / paper white
	float SceneIsSDR;      // 1: clamp the scene to SDR white first (menus, main menu, loading)
	uint CompositeMode;    // CSComposite: 0 = whole frame from the back buffer (SDR), 1 = scene + layers
	uint UseOverlay;       // CSComposite mode 1: blend the back buffer (Present-hook overlays) on top
	uint SceneBT2020;      // (44a) the scene's primaries are BT.2020 already (Color Grading's HDR output)
	uint Pad1;
};

static const float3x3 kBT709ToBT2020 = {
	0.6274039, 0.3292830, 0.0433131,
	0.0690973, 0.9195404, 0.0113623,
	0.0163914, 0.0880133, 0.8955953
};

static const float3x3 kBT2020ToBT709 = {
	1.6604910, -0.5876411, -0.0728499,
	-0.1245505, 1.1328999, -0.0083494,
	-0.0181508, -0.1005789, 1.1187297
};

float3 GammaToLinear(float3 v)
{
	return pow(max(v, 0.0), 2.2);
}

// display-linear BT.2020 (1.0 = paper white) -> PQ code values. (44a) Everything is converted to
// BT.2020 before this (the scene only when it is not BT.2020 already), so nothing is converted twice.
float3 EncodeHDR10(float3 linear2020)
{
	linear2020 = max(linear2020, 0.0);
	// Never above the peak: scale the whole colour so its brightest channel sits on it (keeps hue).
	const float m = max(max(linear2020.r, linear2020.g), linear2020.b);
	if (m > PeakRel)
		linear2020 *= PeakRel / m;
	const float3 y = saturate(linear2020 * (PaperWhiteNits / 10000.0));
	const float3 p = pow(y, 0.1593017578125);
	return saturate(pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375));
}

// The scene as ISHDR wrote it (display gamma, fp16 target, so above 1.0 survives) -> display-linear
// BT.2020. An SDR scene (menus) is clamped to SDR white in BT.709 first, as before.
float3 SceneToLinear2020(float3 v)
{
	float3 c = GammaToLinear(v);
	if (SceneIsSDR > 0.5) {
		if (SceneBT2020)
			c = mul(kBT2020ToBT709, c);
		return mul(kBT709ToBT2020, saturate(c));
	}
	return SceneBT2020 ? c : mul(kBT709ToBT2020, c);
}

#if defined(ENCODE_SCENE)
Texture2D<float4> Scene : register(t0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void CSEncodeScene(uint3 id : SV_DispatchThreadID) {
	uint w, h;
	Output.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;
	Output[id.xy] = float4(EncodeHDR10(SceneToLinear2020(Scene[id.xy].rgb)), 1.0);
}
#endif

#if defined(RESOLVE_SDR)
Texture2D<float4> Scene : register(t0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void CSResolveSDR(uint3 id : SV_DispatchThreadID) {
	uint w, h;
	Output.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;
	float3 c = Scene[id.xy].rgb;
	// (44a) A BT.2020 scene shown as SDR: back to BT.709 in linear light, then display gamma again.
	[branch] if (SceneBT2020)
		c = pow(saturate(mul(kBT2020ToBT709, GammaToLinear(c))), 1.0 / 2.2);
	Output[id.xy] = float4(saturate(c), 1.0);
}
#endif

#if defined(COMPOSITE)
Texture2D<float4> Scene : register(t0);    // mode 0: the 10-bit back buffer (whole SDR frame); mode 1: the fp16 scene
Texture2D<float4> UILayer : register(t1);  // mode 1: the game's UI and ImGui, premultiplied, display gamma
Texture2D<float4> Overlay : register(t2);  // mode 1: the back buffer, cleared at UI start: whatever Present hooks drew
RWTexture2D<float4> Output : register(u0);

// Premultiplied "over" in display-linear light. The layer's colour is un-premultiplied before the
// gamma decode, so a half-transparent panel keeps its colour; a texel with colour but no alpha
// (a mod that does not write alpha) is added on top instead of being lost. The layer is BT.709 (UI,
// overlays) and is converted to BT.2020 here; below is BT.2020 already.
float3 Over(float3 below, float4 layer, float brightness)
{
	const float a = saturate(layer.a);
	const float3 straight = a > 1e-3 ? saturate(layer.rgb / a) : 0.0;
	const float3 premultiplied = a > 1e-3 ? GammaToLinear(straight) * a : GammaToLinear(layer.rgb);
	return mul(kBT709ToBT2020, premultiplied) * brightness + below * (1.0 - a);
}

[numthreads(8, 8, 1)] void CSComposite(uint3 id : SV_DispatchThreadID) {
	uint w, h;
	Output.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;

	float3 color;
	[branch] if (CompositeMode == 0)
	{
		// Everything is in the back buffer already, as SDR: show it at paper white.
		color = mul(kBT709ToBT2020, GammaToLinear(saturate(Scene[id.xy].rgb)));
	}
	else
	{
		color = SceneToLinear2020(Scene[id.xy].rgb);
		color = Over(color, UILayer[id.xy], HudScale);
		[branch] if (UseOverlay)
			color = Over(color, Overlay[id.xy], HudScale);
	}
	Output[id.xy] = float4(EncodeHDR10(color), 1.0);
}
#endif

#if defined(NIS_DECODE)
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void CSGammaToLinear(uint3 id : SV_DispatchThreadID) {
	uint w, h;
	Output.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;
	Output[id.xy] = float4(GammaToLinear(Source[id.xy].rgb), 1.0);
}
#endif

#if defined(NIS_ENCODE)
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void CSLinearToGamma(uint3 id : SV_DispatchThreadID) {
	uint w, h;
	Output.GetDimensions(w, h);
	if (id.x >= w || id.y >= h)
		return;
	Output[id.xy] = float4(pow(max(Source[id.xy].rgb, 0.0), 1.0 / 2.2), 1.0);
}
#endif
