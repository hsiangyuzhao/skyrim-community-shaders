#ifndef SSS_RAYMARCH_CB
#define SSS_RAYMARCH_CB

// Mirrors ScreenSpaceShadows::RaymarchCB (src/Features/ScreenSpaceShadows.h). Shared by the ray
// march and the edge smoothing pass.
cbuffer PerFrame : register(b1)
{
	// Runtime data returned from BuildDispatchList():
	float4 LightCoordinate;  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
	int2 WaveOffset;         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

	// Renderer Specific Values:
	float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
	float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

	// Sampling data:
	float2 InvDepthTextureSize;  // Inverse of the render (dynamic) size, in render pixels
	float2 DynamicRes;

	// BendSettings
	float SurfaceThickness;
	float BilinearThreshold;
	float ShadowContrast;
	uint SettingsEnable;
	uint SettingsSampleCount;
	uint WorldSpaceThickness;
	float MinThickness;
	float ContactLength;
	uint EdgeSmoothing;
	// (45k) Ray Tracing "non-RT casters only" (RT_CASTER_MASK variant): occluders that are ray-traced
	// casters shadow only within RtContactScale x the ray tracer's start offset (bias) along the light ray.
	float RtContactScale;
	float RtBiasConstant;
	float RtBiasPerUnit;

	// (45 SSS) World-space path data
	float4 CameraData;  // far, near, far - near, far * near
	float4 SunDirVS;    // xyz: towards the sun, view space
	float4 ProjInfo;    // 1 / P00, 1 / P11, render width, render height
};

#endif  // SSS_RAYMARCH_CB
