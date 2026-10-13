#ifndef __DYNAMIC_SNOW_TRAIL_TIME_HLSLI__
#define __DYNAMIC_SNOW_TRAIL_TIME_HLSLI__

// (44e) Time-stamped trail texels: no decay pass. Each R32_UINT texel holds
//   bits 31..20  print code (12 bits): 0 = untouched; 1..2047 = rim, height = code / 2047;
//                2048..4095 = dent, depth = (code - 2048) / 2047
//   bits 19..10  stamp time: trail clock tick modulo 1024
//   bits  9..0   the foot's height modulo 1024 units, whole units
// One tick = TrailRefillSeconds / 256 of game-running time, so a full print refills in 256 ticks:
// the refill is computed when the texel is read, from (now - stamp time). A sweep clears texels
// older than 256 ticks before their 10-bit time could wrap (DynamicSnow::UpdateTrailMap).
// Mirrors DynamicSnow::kTrailTicksPerRefill in DynamicSnow.h.
namespace TrailTime
{
	static const uint kTicksPerRefill = 256;
	static const float kInvTicksPerRefill = 1.0 / 256.0;

	uint Pack(float height, uint nowTick, uint z10)
	{
		// height < 0: dent of depth -height; > 0: rim of height height.
		uint code = height < 0.0 ? 2048 + uint(saturate(-height) * 2047.0 + 0.5) : uint(saturate(height) * 2047.0 + 0.5);
		return (code << 20) | ((nowTick & 1023) << 10) | (z10 & 1023);
	}

	uint Code(uint v) { return v >> 20; }
	uint Age(uint v, uint nowTick) { return (nowTick - (v >> 10)) & 1023; }
	bool Expired(uint v, uint nowTick) { return Age(v, nowTick) >= kTicksPerRefill; }
	float FootZ(uint v) { return float(v & 1023) + 0.5; }

	/// What is left of the print now, 0..1 (depth for a dent, height for a rim).
	float Remaining(uint v, uint nowTick, out bool dent)
	{
		const uint code = Code(v);
		dent = code >= 2048;
		const float full = dent ? float(code - 2048) * (1.0 / 2047.0) : float(code) * (1.0 / 2047.0);
		const uint age = Age(v, nowTick);
		return age >= kTicksPerRefill ? 0.0 : max(full - float(age) * kInvTicksPerRefill, 0.0);
	}

	/// Ordering for the stamp's compare-exchange: any live dent beats any rim, deeper / higher wins,
	/// all measured as they are now.
	float Rank(uint v, uint nowTick)
	{
		if (Code(v) == 0)
			return 0.0;
		bool dent;
		const float rem = Remaining(v, nowTick, dent);
		if (rem <= 0.0)
			return 0.0;
		return dent ? 1.0 + rem : rem;
	}
}

#endif  // __DYNAMIC_SNOW_TRAIL_TIME_HLSLI__
