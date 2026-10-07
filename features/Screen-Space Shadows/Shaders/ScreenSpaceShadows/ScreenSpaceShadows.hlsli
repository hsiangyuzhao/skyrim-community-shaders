
namespace ScreenSpaceShadows
{
	Texture2D<unorm half> ScreenSpaceShadowsTexture : register(t45);

	float GetScreenSpaceShadow(float3 screenPosition, float2 uv, float noise, uint eyeIndex)
	{
		// (batch 38, upstream 8b91a37de) SV_Position is already the pixel centre (n + 0.5); adding
		// another 0.5 before truncating read the pixel one step down and to the right.
		return ScreenSpaceShadowsTexture.Load(int3(int2(screenPosition.xy), 0)).x;
	}
}