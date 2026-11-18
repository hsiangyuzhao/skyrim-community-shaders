Texture2D<float4> WaterCaustics : register(t65);

namespace WaterEffects
{
	// (48n, official CS #3114) The caustics UV used to be wrapped with frac() per pixel: where it wrapped, a 2x2 quad saw a
	// jump of ~1 in UV, the implicit derivatives picked the smallest mip and a thin seam line appeared. The integer part is
	// now taken from the quad's top-left pixel, so the UV stays continuous inside every quad.

	/** @brief The pixel's slot in its 2x2 quad, 0 or 1 per axis. @param pixelPos SV_Position.xy. */
	float2 CausticsQuadPixel(float2 pixelPos)
	{
		return float2(uint2(pixelPos) & 1u);
	}

	/** @brief Value of the quad's top-left pixel through fine derivatives (SM5 has no QuadReadLaneAt). Exact for integer
	 * input. Needs uniform control flow. */
	float4 CausticsQuadReadTopLeft(float4 v, float2 quadPixel)
	{
		v -= quadPixel.x * ddx_fine(v);
		return v - quadPixel.y * ddy_fine(v);
	}

	/** @brief Panned caustics UV, unwrapped; wrap it with WrapCausticsUV before sampling. */
	float2 PanCausticsUV(float2 uv, float speed, float tiling)
	{
		return (float2(1, 0) * SharedData::Timer * speed) + (uv * tiling);
	}

	/** @brief frac() with the integer part taken from the quad's top-left pixel. */
	float4 WrapCausticsUV(float4 uv, float2 quadPixel)
	{
		return uv - CausticsQuadReadTopLeft(floor(uv), quadPixel);
	}

	float SampleCaustics(float2 uv)
	{
		return WaterCaustics.SampleBias(SampColorSampler, uv, SharedData::MipBias).x;
	}

	/** @brief Caustics multiplier for the directional light. @param pixelPos SV_Position.xy. */
	float ComputeCaustics(float4 waterData, float3 worldPosition, uint eyeIndex, float2 pixelPos)
	{
		float causticsDistToWater = waterData.w - worldPosition.z;
		float shoreFactorCaustics = saturate(causticsDistToWater / 64.0);

		// Wrapped before the branch, since the quad read needs the whole quad active.
		float2 quadPixel = CausticsQuadPixel(pixelPos);
		float2 causticsUV = (worldPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy) * 0.005;
		float4 causticsHighUV = WrapCausticsUV(float4(PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0), PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5)), quadPixel);
		causticsUV *= 0.5;
		float4 causticsLowUV = WrapCausticsUV(float4(PanCausticsUV(causticsUV, 0.5 * 0.1, 1.0), PanCausticsUV(causticsUV, 1.0 * 0.1, -0.5)), quadPixel);

		// Quads that diverge here sit at the shoreline, where shoreFactorCaustics is near 0 and hides the samples.
		if (shoreFactorCaustics > 0.0) {
			float causticsFade = 1.0 - saturate(causticsDistToWater / 1024.0);
			causticsFade *= causticsFade;

			float causticsHigh = 1.0;

			if (causticsFade > 0.0)
				causticsHigh = min(SampleCaustics(causticsHighUV.xy), SampleCaustics(causticsHighUV.zw)) * 4.0;

			float causticsLow = 1.0;

			if (causticsFade < 1.0)
				causticsLow = min(SampleCaustics(causticsLowUV.xy), SampleCaustics(causticsLowUV.zw)) * 4.0;

			float caustics = lerp(causticsLow, causticsHigh, causticsFade);

			return lerp(1.0, caustics, shoreFactorCaustics);
		}

		return 1.0;
	}
}
