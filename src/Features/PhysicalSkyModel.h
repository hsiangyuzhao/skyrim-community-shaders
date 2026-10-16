#pragma once

#include <array>
#include <vector>

/// (47a) CPU copy of Physical Sky's scattering model (PhysicalSky/LutGen.cs.hlsl), used only by
/// "Sky Fixes" to measure the sky's zenith brightness each frame without reading anything back from
/// the GPU. Same LUT sizes, same mappings, same step counts and sampling as the shaders, in km.
namespace PhysicalSkyModel
{
	using Vec3 = std::array<double, 3>;

	struct Atmosphere
	{
		double rPlanet = 6360.0;  // km
		double rAtmosphere = 6420.0;
		Vec3 rayleighScatter = {};  // km^-1
		double rayleighFalloff = 0.0;
		Vec3 aerosolScatter = {};
		Vec3 aerosolAbsorption = {};
		double aerosolFalloff = 0.0;
		double aerosolPhaseG = 0.0;
		double ozoneAltitude = 0.0;
		double ozoneThickness = 0.0;
		Vec3 ozoneAbsorption = {};
		Vec3 groundAlbedo = {};
		bool midpoint = false;         // ray-march samples in the middle of each step
		bool groundFix = false;        // multiscatter ground bounce x cos / pi, sun tested at the ground point
		bool multiScatterFix = true;   // isotropic multiscatter phase over the full sphere
		bool trLutEdgeFix = true;      // transmittance/multiscatter lookups clamped to texel centres

		bool operator==(const Atmosphere&) const = default;
	};

	struct Light
	{
		Vec3 dir = { 0.0, 0.0, 1.0 };  // unit, towards the body
		Vec3 color = {};               // as uploaded (exposure and glow strength included)
	};

	class Model
	{
	public:
		/// Rebuilds the transmittance and multiscatter LUTs when the atmosphere changed. Returns false
		/// when the parameters are unusable (the caller then leaves the sky alone).
		bool Update(const Atmosphere& a_atmosphere);
		bool Ready() const { return ready; }

		/// In-scattered radiance of one sky-view ray (LUTGEN 2 without the night base light) from a
		/// camera a_cameraRadius km from the planet centre.
		Vec3 SkyRay(double a_cameraRadius, const Vec3& a_dir, const Light* a_lights, int a_lightCount) const;

		/// Mean luminance of SkyRay over 16 directions a_elevationDeg above the horizon.
		double RingLuminance(double a_cameraRadius, double a_elevationDeg, const Light* a_lights, int a_lightCount) const;

	private:
		static constexpr int TrW = 256, TrH = 64, MsW = 32, MsH = 32;

		Atmosphere atmo{};
		bool ready = false;
		bool everBuilt = false;
		std::vector<Vec3> tr;
		std::vector<Vec3> ms;

		void Densities(double a_altitude, double& a_rayleigh, double& a_aerosol, double& a_ozone) const;
		Vec3 Extinction(double a_rayleigh, double a_aerosol, double a_ozone) const;
		void TrLutUv(double a_r, double a_cosZenith, double& a_u, double& a_v) const;
		Vec3 Sample(const std::vector<Vec3>& a_lut, int a_w, int a_h, double a_u, double a_v) const;
		Vec3 TrAt(const Vec3& a_pos, const Vec3& a_dir) const;
		Vec3 MsAt(const Vec3& a_pos, const Vec3& a_dir) const;
		void BuildTr();
		void BuildMs();
	};
}
