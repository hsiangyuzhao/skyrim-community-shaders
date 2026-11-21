#include "Common/SharedData.hlsli"

namespace InverseSquareLighting
{
	static const float SCALE = 0.8f;
	static const float METRES_TO_UNITS = 70.f;
	static const float METRES_TO_UNITS_SQ = METRES_TO_UNITS * METRES_TO_UNITS;
	static const float SCALED_UNITS_SQ = SCALE * METRES_TO_UNITS_SQ;

	float GetAttenuation(float distance, LightLimitFix::Light light)
	{
		float isEnabled = 1.0f - float((light.lightFlags & LightLimitFix::LightFlags::Disabled) != 0);
		float isInvSq = float((light.lightFlags & LightLimitFix::LightFlags::InverseSquare) != 0);

		float invSq = SCALED_UNITS_SQ * rcp(distance * distance + light.sizeBias);
		float t = saturate((light.radius - distance) * light.fadeZone);
		float fastSmoothstep = t * t * (3.0f - 2.0f * t);
		invSq *= fastSmoothstep;

		float intensityFactor = saturate(distance * light.invRadius);
		float reg = 1.0f - intensityFactor * intensityFactor;

		// (50c) Lamp Falloff: pad1 = half floats (t << 16 | t x k), set by InverseSquareLighting::ApplyLampFalloff for game-curve lights:
		// (1 - t) x reg + t x k x invSq. 0 (knob at Game, inverse-square lights, window light): the old value exactly (+ 0).
		float2 falloff = f16tof32(uint2(light.pad1 >> 16, light.pad1));
		return (lerp(reg, invSq, isInvSq) + (falloff.y * invSq - falloff.x * reg)) * isEnabled;
	}
}