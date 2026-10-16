#include "PhysicalSkyModel.h"

#include <numbers>

namespace PhysicalSkyModel
{
	namespace
	{
		constexpr double kPi = std::numbers::pi;
		constexpr double kHorizonCos = -0.414;  // TrLutUv / LUTGEN 0-1: cos of the lowest zenith angle stored

		double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
		Vec3 Scale(const Vec3& a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
		Vec3 Add(const Vec3& a, const Vec3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
		Vec3 Mul(const Vec3& a, const Vec3& b) { return { a[0] * b[0], a[1] * b[1], a[2] * b[2] }; }
		double Luminance(const Vec3& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
		bool Finite(const Vec3& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

		// Common.hlsli RayIntersectSphere (centre at the origin)
		double IntersectSphere(const Vec3& a_origin, const Vec3& a_dir, double a_radius)
		{
			const double b = Dot(a_origin, a_dir);
			const double c = Dot(a_origin, a_origin) - a_radius * a_radius;
			const double discr = b * b - c;
			if (discr < 0.0)
				return -1.0;
			return discr > b * b ? (-b + std::sqrt(discr)) : (-b - std::sqrt(discr));
		}

		double PhaseRayleigh(double a_cos) { return 0.1875 / kPi * (1.0 + a_cos * a_cos); }
		double PhaseCornetteShanks(double a_cos, double a_g)
		{
			const double g2 = a_g * a_g;
			const double num = (1.0 - g2) * (1.0 + a_cos * a_cos);
			const double denom = (2.0 + g2) * std::pow(std::abs(1.0 + g2 - 2.0 * a_g * a_cos), 1.5);
			return 0.375 / kPi * num / denom;
		}
	}

	bool Model::Update(const Atmosphere& a_atmosphere)
	{
		if (everBuilt && a_atmosphere == atmo)
			return ready;
		atmo = a_atmosphere;
		everBuilt = true;
		ready = false;
		const bool sane = std::isfinite(atmo.rPlanet) && std::isfinite(atmo.rAtmosphere) && atmo.rPlanet > 1.0 && atmo.rAtmosphere > atmo.rPlanet + 1e-3 &&
		                  Finite(atmo.rayleighScatter) && Finite(atmo.aerosolScatter) && Finite(atmo.aerosolAbsorption) && Finite(atmo.ozoneAbsorption) &&
		                  Finite(atmo.groundAlbedo) && std::isfinite(atmo.rayleighFalloff) && std::isfinite(atmo.aerosolFalloff) &&
		                  std::isfinite(atmo.aerosolPhaseG) && std::abs(atmo.aerosolPhaseG) < 1.0 && std::isfinite(atmo.ozoneAltitude) && std::isfinite(atmo.ozoneThickness);
		if (!sane)
			return false;
		BuildTr();
		BuildMs();
		ready = true;
		return true;
	}

	void Model::Densities(double a_altitude, double& a_rayleigh, double& a_aerosol, double& a_ozone) const
	{
		a_rayleigh = std::exp(-a_altitude * atmo.rayleighFalloff);
		a_aerosol = std::exp(-a_altitude * atmo.aerosolFalloff);
		const double halfThickness = atmo.ozoneThickness * 0.5;
		a_ozone = halfThickness > 0.0 ? std::max(0.0, 1.0 - std::abs(a_altitude - atmo.ozoneAltitude) / halfThickness) : 0.0;
	}

	Vec3 Model::Extinction(double a_rayleigh, double a_aerosol, double a_ozone) const
	{
		Vec3 e;
		for (int c = 0; c < 3; ++c)
			e[c] = a_rayleigh * atmo.rayleighScatter[c] + a_aerosol * (atmo.aerosolScatter[c] + atmo.aerosolAbsorption[c]) + a_ozone * atmo.ozoneAbsorption[c];
		return e;
	}

	void Model::TrLutUv(double a_r, double a_cosZenith, double& a_u, double& a_v) const
	{
		a_u = std::clamp((a_cosZenith - kHorizonCos) / (1.0 - kHorizonCos), 0.0, 1.0);
		a_v = std::clamp((a_r - atmo.rPlanet) / (atmo.rAtmosphere - atmo.rPlanet), 0.0, 1.0);
		if (atmo.trLutEdgeFix) {
			a_u = std::clamp(a_u, 0.5 / 256.0, 1.0 - 0.5 / 256.0);
			a_v = std::clamp(a_v, 0.5 / 64.0, 1.0 - 0.5 / 64.0);
		}
	}

	// Bilinear, clamp addressing (Physical Sky's SampTr).
	Vec3 Model::Sample(const std::vector<Vec3>& a_lut, int a_w, int a_h, double a_u, double a_v) const
	{
		const double fx = a_u * a_w - 0.5, fy = a_v * a_h - 0.5;
		const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
		const double tx = fx - x0, ty = fy - y0;
		const int xa = std::clamp(x0, 0, a_w - 1), xb = std::clamp(x0 + 1, 0, a_w - 1);
		const int ya = std::clamp(y0, 0, a_h - 1), yb = std::clamp(y0 + 1, 0, a_h - 1);
		const Vec3& c00 = a_lut[ya * a_w + xa];
		const Vec3& c10 = a_lut[ya * a_w + xb];
		const Vec3& c01 = a_lut[yb * a_w + xa];
		const Vec3& c11 = a_lut[yb * a_w + xb];
		Vec3 o;
		for (int c = 0; c < 3; ++c)
			o[c] = (c00[c] * (1.0 - tx) + c10[c] * tx) * (1.0 - ty) + (c01[c] * (1.0 - tx) + c11[c] * tx) * ty;
		return o;
	}

	Vec3 Model::TrAt(const Vec3& a_pos, const Vec3& a_dir) const
	{
		const double r = Length(a_pos);
		double u, v;
		TrLutUv(r, r > 0.0 ? Dot(a_pos, a_dir) / r : 1.0, u, v);
		return Sample(tr, TrW, TrH, u, v);
	}

	Vec3 Model::MsAt(const Vec3& a_pos, const Vec3& a_dir) const
	{
		const double r = Length(a_pos);
		double u, v;
		TrLutUv(r, r > 0.0 ? Dot(a_pos, a_dir) / r : 1.0, u, v);
		return Sample(ms, MsW, MsH, u, v);
	}

	// LUTGEN 0
	void Model::BuildTr()
	{
		tr.assign(static_cast<size_t>(TrW) * TrH, Vec3{ 0.0, 0.0, 0.0 });
		constexpr int steps = 40;
		for (int y = 0; y < TrH; ++y) {
			const double v = (y + 0.5) / TrH;
			const Vec3 pos = { 0.0, 0.0, std::lerp(atmo.rPlanet, atmo.rAtmosphere, v) };
			for (int x = 0; x < TrW; ++x) {
				const double u = (x + 0.5) / TrW;
				const double zc = std::lerp(kHorizonCos, 1.0, u);
				const Vec3 dir = { 0.0, std::sqrt(std::max(0.0, 1.0 - zc * zc)), zc };
				if (IntersectSphere(pos, dir, atmo.rPlanet) > 0.0)
					continue;  // 0: the sun is behind the planet
				const double dt = IntersectSphere(pos, dir, atmo.rAtmosphere) / steps;
				Vec3 depth = { 0.0, 0.0, 0.0 };
				for (int i = 0; i < steps; ++i) {
					const double t = dt * (atmo.midpoint ? i + 0.5 : i + 1.0);
					const Vec3 p = Add(pos, Scale(dir, t));
					double dr, da, dO;
					Densities(std::max(0.0, Length(p) - atmo.rPlanet), dr, da, dO);
					depth = Add(depth, Scale(Extinction(dr, da, dO), dt));
				}
				tr[y * TrW + x] = { std::exp(-depth[0]), std::exp(-depth[1]), std::exp(-depth[2]) };
			}
		}
	}

	// LUTGEN 1
	void Model::BuildMs()
	{
		ms.assign(static_cast<size_t>(MsW) * MsH, Vec3{ 0.0, 0.0, 0.0 });
		constexpr int steps = 20, sqrtSamples = 4;
		const double azimuthRange = atmo.multiScatterFix ? 2.0 * kPi : kPi;
		for (int y = 0; y < MsH; ++y) {
			const double v = (y + 0.5) / MsH;
			const Vec3 pos = { 0.0, 0.0, std::lerp(atmo.rPlanet, atmo.rAtmosphere, v) };
			for (int x = 0; x < MsW; ++x) {
				const double u = (x + 0.5) / MsW;
				const double zc = std::lerp(kHorizonCos, 1.0, u);
				const Vec3 sunDir = { 0.0, std::sqrt(std::max(0.0, 1.0 - zc * zc)), zc };
				Vec3 lumTotal = { 0.0, 0.0, 0.0 }, fMs = { 0.0, 0.0, 0.0 };
				for (int i = 0; i < sqrtSamples; ++i)
					for (int j = 0; j < sqrtSamples; ++j) {
						const double theta = (i + 0.5) * azimuthRange / sqrtSamples;
						const double phi = std::acos(1.0 - 2.0 * (j + 0.5) / sqrtSamples);
						const Vec3 dir = { std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi) };
						const double tGround = IntersectSphere(pos, dir, atmo.rPlanet);
						const double tMax = tGround > 0.0 ? tGround : IntersectSphere(pos, dir, atmo.rAtmosphere);
						const double dt = tMax / steps;
						double phaseR, phaseA;
						if (atmo.multiScatterFix) {
							phaseR = phaseA = 0.25 / kPi;
						} else {
							const double uSun = Dot(dir, sunDir);
							phaseR = PhaseRayleigh(uSun);
							phaseA = PhaseCornetteShanks(uSun, atmo.aerosolPhaseG);
						}
						Vec3 trv = { 1.0, 1.0, 1.0 }, lum = { 0.0, 0.0, 0.0 }, lumFactor = { 0.0, 0.0, 0.0 };
						for (int k = 0; k < steps; ++k) {
							const double t = dt * (atmo.midpoint ? k + 0.5 : k + 1.0);
							const Vec3 p = Add(pos, Scale(dir, t));
							double dr, da, dO;
							Densities(std::max(0.0, Length(p) - atmo.rPlanet), dr, da, dO);
							const Vec3 ext = Extinction(dr, da, dO);
							const Vec3 trSun = TrAt(p, sunDir);
							for (int c = 0; c < 3; ++c) {
								const double muR = dr * atmo.rayleighScatter[c], muA = da * atmo.aerosolScatter[c];
								const double trSample = std::exp(-dt * ext[c]);
								const double scatterFactor = ext[c] > 0.0 ? (1.0 - trSample) / ext[c] : dt;
								lumFactor[c] += trv[c] * (muR + muA) * scatterFactor;
								lum[c] += (muR * phaseR + muA * phaseA) * trSun[c] * scatterFactor * trv[c];
								trv[c] *= trSample;
							}
						}
						if (tGround > 0.0) {
							const Vec3 hit = Add(pos, Scale(dir, tGround));
							const double hitLength = Length(hit);
							if (hitLength > 0.0) {
								const Vec3 up = Scale(hit, 1.0 / hitLength);
								if (atmo.groundFix) {
									const double cosSun = Dot(up, sunDir);
									if (cosSun > 0.0) {
										const Vec3 trGround = TrAt(Scale(up, atmo.rPlanet), sunDir);
										for (int c = 0; c < 3; ++c)
											lum[c] += trv[c] * atmo.groundAlbedo[c] * trGround[c] * (cosSun / kPi);
									}
								} else if (Dot(pos, sunDir) > 0.0) {
									const Vec3 trGround = TrAt(Scale(up, atmo.rPlanet), sunDir);
									for (int c = 0; c < 3; ++c)
										lum[c] += trv[c] * atmo.groundAlbedo[c] * trGround[c];
								}
							}
						}
						fMs = Add(fMs, lumFactor);
						lumTotal = Add(lumTotal, lum);
					}
				constexpr double rcpSamples = 1.0 / (sqrtSamples * sqrtSamples);
				Vec3 out;
				for (int c = 0; c < 3; ++c) {
					const double denom = 1.0 - fMs[c] * rcpSamples;
					out[c] = denom > 1e-6 ? lumTotal[c] * rcpSamples / denom : 0.0;
				}
				ms[y * MsW + x] = out;
			}
		}
	}

	// LUTGEN 2 (without the night base light)
	Vec3 Model::SkyRay(double a_cameraRadius, const Vec3& a_dir, const Light* a_lights, int a_lightCount) const
	{
		Vec3 lum = { 0.0, 0.0, 0.0 };
		if (!ready || a_lightCount <= 0)
			return lum;
		constexpr int steps = 30;
		const Vec3 pos = { 0.0, 0.0, std::max(a_cameraRadius, atmo.rPlanet + 1e-4) };
		const double tGround = IntersectSphere(pos, a_dir, atmo.rPlanet);
		const double tMax = tGround > 0.0 ? tGround : IntersectSphere(pos, a_dir, atmo.rAtmosphere);
		if (!(tMax > 0.0))
			return lum;
		const double dt = tMax / steps;

		constexpr int maxLights = 3;
		const int lightCount = std::min(a_lightCount, maxLights);
		double phaseR[maxLights], phaseA[maxLights];
		for (int l = 0; l < lightCount; ++l) {
			const double u = Dot(a_dir, a_lights[l].dir);
			phaseR[l] = PhaseRayleigh(u);
			phaseA[l] = PhaseCornetteShanks(u, atmo.aerosolPhaseG);
		}

		Vec3 trv = { 1.0, 1.0, 1.0 };
		for (int k = 0; k < steps; ++k) {
			const double t = dt * (atmo.midpoint ? k + 0.5 : k + 1.0);
			const Vec3 p = Add(pos, Scale(a_dir, t));
			double dr, da, dO;
			Densities(std::max(0.0, Length(p) - atmo.rPlanet), dr, da, dO);
			const Vec3 ext = Extinction(dr, da, dO);
			Vec3 trBody[maxLights], psiMs = { 0.0, 0.0, 0.0 };
			for (int l = 0; l < lightCount; ++l) {
				trBody[l] = TrAt(p, a_lights[l].dir);
				psiMs = Add(psiMs, Mul(MsAt(p, a_lights[l].dir), a_lights[l].color));
			}
			for (int c = 0; c < 3; ++c) {
				const double muR = dr * atmo.rayleighScatter[c], muA = da * atmo.aerosolScatter[c];
				const double trSample = std::exp(-dt * ext[c]);
				const double scatterFactor = ext[c] > 0.0 ? (1.0 - trSample) / ext[c] : dt;
				double inscatter = (muR + muA) * psiMs[c];
				for (int l = 0; l < lightCount; ++l)
					inscatter += (muR * phaseR[l] + muA * phaseA[l]) * trBody[l][c] * a_lights[l].color[c];
				lum[c] += inscatter * scatterFactor * trv[c];
				trv[c] *= trSample;
			}
		}
		return lum;
	}

	double Model::RingLuminance(double a_cameraRadius, double a_elevationDeg, const Light* a_lights, int a_lightCount) const
	{
		const double e = a_elevationDeg * (kPi / 180.0);
		double sum = 0.0;
		constexpr int count = 16;
		for (int j = 0; j < count; ++j) {
			const double a = j * (2.0 * kPi / count);
			sum += Luminance(SkyRay(a_cameraRadius, { std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e) }, a_lights, a_lightCount));
		}
		return sum / count;
	}
}
