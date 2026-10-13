#ifndef SSS_RAYMARCH_CB
#define SSS_RAYMARCH_CB

// Mirrors ScreenSpaceShadows::RaymarchCB (src/Features/ScreenSpaceShadows.h).
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
};

#endif  // SSS_RAYMARCH_CB
