namespace WaterEffects
{
	// https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
	// https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h

	// https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
	// http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
	// https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf

	// (batch 40b) SharedData::waterEffectsSettings.UpstreamFixParallax selects the upstream parallax:
	// c6c94acb7 (stochastic mip selection), d50af1036 (mip computed as if the texture were 512x512,
	// no fixed mip bias, height = sum of (1 - h) * amplitude) and b6de23b0c (march starts at the real
	// surface height). Flag 0 = the old maths unchanged.

	float GetMipLevel(float2 coords, Texture2D<float4> tex, float screenNoise, bool upstreamFix)
	{
		// Compute the current gradients:
		float2 actualTextureDims;
		tex.GetDimensions(actualTextureDims.x, actualTextureDims.y);

		float2 textureDims = actualTextureDims;
#if defined(VR)
		textureDims /= 16.0;
#else
		textureDims /= 8.0;
#endif
		if (upstreamFix)
			textureDims = float2(512.0, 512.0);

		float2 texCoordsPerSize = coords * textureDims;

		float2 dxSize = ddx(texCoordsPerSize);
		float2 dySize = ddy(texCoordsPerSize);

		// Find min of change in u and v across quad: compute du and dv magnitude across quad
		float2 dTexCoords = dxSize * dxSize + dySize * dySize;

		// Standard mipmapping uses max here
		float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);

		// Compute the current mip level  (* 0.5 is effectively computing a square root before )
		float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);

		if (upstreamFix) {
			// Offset mip level to sample as if texture were 512x512
			mipLevel = max(mipLevel + log2(actualTextureDims.x / 512.0), 0.0);
			// Stochastic mip selection: use screen noise to select between adjacent mip levels
			mipLevel = floor(mipLevel) + (screenNoise < frac(mipLevel) ? 1.0 : 0.0);
		}

		return mipLevel;
	}

	float GetHeight(PS_INPUT input, float2 currentOffset, float3 normalScalesRcp, float3 mipLevels, bool upstreamFix)
	{
		float3 heights;
		heights.x = Normals01Tex.SampleLevel(Normals01Sampler, input.TexCoord1.xy + currentOffset * normalScalesRcp.x, mipLevels.x).w;
		heights.y = Normals02Tex.SampleLevel(Normals02Sampler, input.TexCoord1.zw + currentOffset * normalScalesRcp.y, mipLevels.y).w;
		heights.z = Normals03Tex.SampleLevel(Normals03Sampler, input.TexCoord2.xy + currentOffset * normalScalesRcp.z, mipLevels.z).w;
		if (upstreamFix) {
			heights = 1.0 - heights;
			heights *= NormalsAmplitude.xyz;
			return heights.x + heights.y + heights.z;
		}
		heights *= NormalsAmplitude.xyz;
		return 1.0 - (heights.x + heights.y + heights.z);
	}

	float2 GetParallaxOffset(PS_INPUT input, float3 normalScalesRcp)
	{
		const bool upstreamFix = SharedData::waterEffectsSettings.UpstreamFixParallax != 0;

		float3 viewDirection = normalize(input.WPosition.xyz);
		float2 parallaxOffsetTS = viewDirection.xy / -viewDirection.z;

		// Parallax scale is also multiplied by normalScalesRcp
		parallaxOffsetTS *= 20.0;

		float screenNoise = upstreamFix ? Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount) : 0.0;

		float3 mipLevels;
		mipLevels.x = GetMipLevel(input.TexCoord1.xy, Normals01Tex, screenNoise, upstreamFix);
		mipLevels.y = GetMipLevel(input.TexCoord1.zw, Normals02Tex, screenNoise, upstreamFix);
		mipLevels.z = GetMipLevel(input.TexCoord2.xy, Normals03Tex, screenNoise, upstreamFix);

		if (!upstreamFix) {
#if defined(VR)
			mipLevels = mipLevels + 4;
#else
			mipLevels = mipLevels + 3;
#endif
		}

		float stepSize = rcp(16.0);
		float currBound = 0.0;
		float currHeight = upstreamFix ? GetHeight(input, 0.0.xx, normalScalesRcp, mipLevels, true) : 1.0;
		float prevHeight = currHeight;

		[loop] while (currHeight > currBound)
		{
			prevHeight = currHeight;
			currBound += stepSize;
			currHeight = GetHeight(input, currBound * parallaxOffsetTS.xy, normalScalesRcp, mipLevels, upstreamFix);
		}

		float prevBound = currBound - stepSize;

		float delta2 = prevBound - prevHeight;
		float delta1 = currBound - currHeight;
		float denominator = delta2 - delta1;
		float parallaxAmount = (currBound * delta2 - prevBound * delta1) / denominator;

		return parallaxOffsetTS.xy * parallaxAmount;
	}
}
