#pragma once

/// (50c) Window Light (Linear Lighting page, "Window Light" section): lights and glass whose emittance is tied to a region
/// (XEMI -> REGN, e.g. Window Shadows Ultimate's beams and window glass). The game colours them with the region weather's Effect
/// Lighting keys blended by the climate clock; Linear Lighting then raises the colour to the light gamma per channel. Here the colour
/// is rebuilt from the same keys with four switches (No Extra Saturation, Follow the Real Sun, Transition Brightness, Follow the
/// Outdoor Weather). Interiors only. Every switch off (Transition Brightness on Mod's Keys): nothing is touched (the engine's colour path exactly).
/// Detection is data only (the reference's ExtraEmittanceSource is a TESRegion), cached per light / shape and dropped on a cell change.
namespace WindowLight
{
	/// Once per frame before the light upload (Light Limit Fix UpdateLights; Linear Lighting Prepass as a fallback; idempotent).
	void BeginFrame();
	/// Replaces a region-tied light's colour (gamma-space units, as NiLight diffuse) with this frame's window colour. Others untouched.
	void ApplyToLight(RE::NiLight* a_niLight, float3& io_color);
	/// True for a light this frame treats as window light (excluded from the Lamp Falloff knob).
	bool IsWindowLight(RE::NiLight* a_niLight);
	/// Glass: before BSLightingShader::SetupGeometry, points a region-tied shape's emissive colour at this frame's window colour
	/// (the engine writes EmitColor = *emissiveColor x emissiveMult there); RestoreGlass puts the engine's pointer back after it.
	void SetupGlass(RE::BSRenderPass* a_pass, RE::BSLightingShaderProperty* a_property);
	void RestoreGlass();
	/// Menu status lines (Linear Lighting > Window Light).
	void DrawStatus();
}
