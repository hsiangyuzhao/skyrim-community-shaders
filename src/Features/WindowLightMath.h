#pragma once

#include <algorithm>
#include <cmath>

/// (50c) Window Light: the pure colour maths (no game types), so the numbers in the spec / GUIDE come from this very code.
/// Colours are 0..1 key colours as the game stores them (bytes / 255); "linear" = what the Lighting shader multiplies after Linear
/// Lighting's gamma (the engine path is pow(colour, gamma) per channel).
namespace WindowLightMath
{
	struct Float3
	{
		float r = 0.0f, g = 0.0f, b = 0.0f;
	};

	/// TESWeather colour-time order.
	enum Key : int
	{
		kSunrise = 0,
		kDay = 1,
		kSunset = 2,
		kNight = 3
	};

	struct Weights
	{
		float w[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	};

	/// The lower edge of the twilight band: the sun's elevation where the weather's colours start to leave their night key (Sky Sync's
	/// warp puts the sun there at "sunrise begin - fDaytimeColorExtension"; the sunrise colours peak at the horizon).
	inline constexpr float kNightElevation = -12.0f;
	/// The day-key edge is kept at least this high (very low sun paths).
	inline constexpr float kMinDayElevation = 2.0f;

	inline float Luminance(const Float3& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
	inline float Clamp01(float x) { return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0.0f; }
	inline float SmoothStep01(float x)
	{
		x = Clamp01(x);
		return x * x * (3.0f - 2.0f * x);
	}
	inline Float3 Scale(const Float3& c, float s) { return { c.r * s, c.g * s, c.b * s }; }
	inline Float3 PowPerChannel(const Float3& c, float p)
	{
		auto f = [p](float v) { return std::pow(std::isfinite(v) ? std::max(v, 0.0f) : 0.0f, p); };
		return { f(c.r), f(c.g), f(c.b) };
	}

	/// The climate's colour-key hours (TNAM in hours, fDaytimeColorExtension), laid out as the game blends its weather colours: night
	/// until csb = sunrise begin - extension, sunrise peak at midR (half way to sunrise end), day from sunrise end to sunset begin,
	/// sunset peak at midS (half way to sunset end + extension), night from cse.
	struct Timing
	{
		float csb = 5.0f, midR = 7.5f, sre = 10.0f, ssb = 17.0f, midS = 19.25f, cse = 21.5f;
		bool valid = false;
	};

	inline Timing MakeTiming(float a_srb, float a_sre, float a_ssb, float a_sse, float a_ext)
	{
		Timing t;
		t.csb = std::max(0.0f, a_srb - a_ext);
		t.sre = a_sre;
		t.ssb = a_ssb;
		t.cse = std::min(23.99f, a_sse + a_ext);
		t.midR = t.csb + (t.sre - t.csb) * 0.5f;
		t.midS = t.ssb + (t.cse - t.ssb) * 0.5f;
		t.valid = t.csb < t.midR && t.midR < t.sre && t.sre <= t.ssb && t.ssb < t.midS && t.midS < t.cse;
		return t;
	}

	inline Weights Pair(int a_from, int a_to, float a_t)
	{
		Weights w;
		w.w[kNight] = 0.0f;
		const float t = Clamp01(a_t);
		w.w[a_from] += 1.0f - t;
		w.w[a_to] += t;
		return w;
	}

	/// The game's own clock weights (linear in the hour between the keys). Invalid timing: night 22-4, day otherwise (never used for
	/// the off path, which is the engine's colour itself).
	inline Weights ClockWeights(const Timing& a_t, float a_hour)
	{
		if (!a_t.valid)
			return (a_hour < 4.0f || a_hour >= 22.0f) ? Pair(kNight, kNight, 0.0f) : Pair(kDay, kDay, 0.0f);
		const float h = a_hour;
		if (h < a_t.csb || h >= a_t.cse)
			return Pair(kNight, kNight, 0.0f);
		if (h < a_t.midR)
			return Pair(kNight, kSunrise, (h - a_t.csb) / (a_t.midR - a_t.csb));
		if (h < a_t.sre)
			return Pair(kSunrise, kDay, (h - a_t.midR) / (a_t.sre - a_t.midR));
		if (h < a_t.ssb)
			return Pair(kDay, kDay, 0.0f);
		if (h < a_t.midS)
			return Pair(kDay, kSunset, (h - a_t.ssb) / (a_t.midS - a_t.ssb));
		return Pair(kSunset, kNight, (h - a_t.midS) / (a_t.cse - a_t.midS));
	}

	/// "Follow the Real Sun": the same keys placed on the sun's true elevation. The climate's night -> sunrise span becomes
	/// kNightElevation -> 0 degrees (night hue gone at the horizon), its sunrise -> day span 0 -> the elevation the sun has at sunrise end
	/// (a_dayRise; a_daySet = at sunset begin for the evening); smoothstep in elevation. a_rising: the sun is climbing.
	inline Weights AltitudeWeights(float a_elev, bool a_rising, float a_dayRise, float a_daySet)
	{
		const int warm = a_rising ? kSunrise : kSunset;
		const float dayEdge = std::max(a_rising ? a_dayRise : a_daySet, kMinDayElevation);
		if (!std::isfinite(a_elev) || a_elev <= kNightElevation)
			return Pair(kNight, kNight, 0.0f);
		if (a_elev < 0.0f)
			return Pair(kNight, warm, SmoothStep01((a_elev - kNightElevation) / -kNightElevation));
		if (a_elev < dayEdge)
			return Pair(warm, kDay, SmoothStep01(a_elev / dayEdge));
		return Pair(kDay, kDay, 0.0f);
	}

	/// "Transition Brightness: Smooth": 0 (night key luminance) .. 1 (day key luminance) in log space, from the sun's elevation only,
	/// the same rising and setting: 0 below kNightElevation, 0.5 at the horizon (the geometric mean of night and day), x^3 below
	/// (stays dim until the last few degrees), 1 - (1 - x)^2 above up to the day-key elevation.
	inline float SmoothBrightnessFraction(float a_elev, bool a_rising, float a_dayRise, float a_daySet)
	{
		const float dayEdge = std::max(a_rising ? a_dayRise : a_daySet, kMinDayElevation);
		if (!std::isfinite(a_elev) || a_elev <= kNightElevation)
			return 0.0f;
		if (a_elev < 0.0f) {
			const float x = (a_elev - kNightElevation) / -kNightElevation;
			return 0.5f * x * x * x;
		}
		if (a_elev < dayEdge) {
			const float x = a_elev / dayEdge;
			return 0.5f + 0.5f * (1.0f - (1.0f - x) * (1.0f - x));
		}
		return 1.0f;
	}

	/// One key in linear light. Engine: pow per channel (saturates). "No Extra Saturation": the key's own chromaticity, with the
	/// luminance the engine gives it (lum(pow(c, gamma))), so each key's brightness is unchanged.
	inline Float3 KeyLinear(const Float3& a_key, float a_gamma, bool a_noExtraSaturation)
	{
		const Float3 engine = PowPerChannel(a_key, a_gamma);
		if (!a_noExtraSaturation)
			return engine;
		const float y = Luminance(a_key);
		if (!(y > 1e-6f))
			return engine;
		return Scale(a_key, Luminance(engine) / y);
	}

	/// The window colour in linear light (before the light's own LIGH colour and the weather factor): keys blended in linear light with
	/// a_weights; a_smooth: chromaticity of that blend at the luminance exp(lerp(ln Y_night, ln Y_day, a_fraction)).
	inline Float3 Compose(const Float3 a_keys[4], float a_gamma, const Weights& a_weights, bool a_noExtraSaturation, bool a_smooth, float a_fraction)
	{
		Float3 lin[4];
		for (int k = 0; k < 4; ++k)
			lin[k] = KeyLinear(a_keys[k], a_gamma, a_noExtraSaturation);
		Float3 mix{};
		for (int k = 0; k < 4; ++k) {
			mix.r += a_weights.w[k] * lin[k].r;
			mix.g += a_weights.w[k] * lin[k].g;
			mix.b += a_weights.w[k] * lin[k].b;
		}
		if (!a_smooth)
			return mix;
		const float yn = std::max(Luminance(lin[kNight]), 1e-6f);
		const float yd = std::max(Luminance(lin[kDay]), 1e-6f);
		const float f = Clamp01(a_fraction);
		const float target = std::exp(std::log(yn) * (1.0f - f) + std::log(yd) * f);
		const float y = Luminance(mix);
		if (!(y > 1e-7f))
			return mix;
		return Scale(mix, target / y);
	}

	/// The game's emittance colour (gamma-space blend of the keys), for checking the reconstruction against the engine.
	inline Float3 EngineBlend(const Float3 a_keys[4], const Weights& a_weights)
	{
		Float3 mix{};
		for (int k = 0; k < 4; ++k) {
			mix.r += a_weights.w[k] * a_keys[k].r;
			mix.g += a_weights.w[k] * a_keys[k].g;
			mix.b += a_weights.w[k] * a_keys[k].b;
		}
		return mix;
	}

	/// Night factor of "Follow the Outdoor Weather" from the moons: 1 with a bright full moon up, down to kMoonlessFloor with no moon.
	inline constexpr float kMoonlessFloor = 0.25f;
	inline float MoonFactor(float a_moonStrength) { return kMoonlessFloor + (1.0f - kMoonlessFloor) * Clamp01(a_moonStrength); }
}
