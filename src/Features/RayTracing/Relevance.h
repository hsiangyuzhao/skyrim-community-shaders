#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * @brief (47u) "RT: Skip Updates That Cannot Affect the Image": the space every ray of a frame can reach, as convex supersets
 * (intersections of half-spaces), and whether a world box meets it.
 *
 * Every ray the frame traces starts at a visible pixel (inside the view frustum: the side planes through the camera and the camera's
 * own plane; the reconstructed point plus its start offset, covered by a widened frustum and a margin) or at the water surface along a
 * pixel's view ray (also inside the frustum), and is one of:
 *  - a sun ray: from a pixel inside the sun trace's rect toward the sun, at most kMaxRayLength long. The set of such segments is the
 *    frustum cut by the rect's four sides, extruded toward the sun; its superset here moves every plane out by the extrusion's support
 *    along its normal (max(0, length x n . toSun));
 *  - a short ray (RTAO <= 210 units, surface / coverage probes <= 96, character self-shadow offsets): inside the frustum moved out by
 *    the near reach;
 *  - a point-light ray: from a pixel (or the water above it) to a point on its source's emitter (a ball around the light, or around
 *    its aim): inside the convex hull of the frustum and that ball, whose superset moves each frustum plane out to the ball's support.
 * A box that meets none of these supersets is met by no ray of the frame: whatever pose its BLAS holds cannot change any result.
 * Beam / aimed sources (interiors only) make the region "everything".
 */
namespace RayTracingRelevance
{
	struct Plane
	{
		double n[3];  // unit normal, pointing out of the region
		double d;     // inside: n . p <= d (world coordinates)
	};

	/// One frame's ray sources (world coordinates).
	struct Inputs
	{
		bool valid = false;
		std::uint32_t frame = 0;
		double camera[3]{};
		float invViewProj[16]{};  // camera-relative clip -> position (FrameBuffer::CameraViewProjInverse, row-major as the shaders read it)
		bool sun = false;
		double toSun[3]{};
		double sunLength = 0.0;
		double sunSpread = 0.0;   // largest |ray direction - toSun| (the sun disc's cone: about its tangent)
		double rect[4]{};         // world xy min / max of the sun trace's area
		double nearReach = 0.0;   // short rays (RTAO, probes)
		bool all = false;         // a source the region cannot bound: everything is reachable
		std::vector<std::array<double, 4>> targets;  // point sources: world xyz of the ray target, radius of its emitter ball
	};

	class Region
	{
	public:
		[[nodiscard]] bool Valid() const { return valid_; }
		[[nodiscard]] bool All() const { return all_; }

		/// a_widen: the frustum's corner tangents scaled by it (>= 1); a_margin: units every plane moves out (camera movement, start
		/// offsets, jitter); a_targetGrow: added to every point source's emitter radius.
		void Build(const Inputs& a_in, double a_widen, double a_margin, double a_targetGrow)
		{
			valid_ = false;
			all_ = false;
			sun_.clear();
			near_.clear();
			points_.clear();
			if (!a_in.valid)
				return;
			valid_ = true;
			if (a_in.all) {
				all_ = true;
				return;
			}
			// Corner directions (camera-relative) of the frustum, widened.
			const double sx[4] = { -1.0, 1.0, 1.0, -1.0 }, sy[4] = { -1.0, -1.0, 1.0, 1.0 };
			double dir[4][3];
			for (int c = 0; c < 4; ++c) {
				const double clip[4] = { sx[c] * a_widen, sy[c] * a_widen, 0.5, 1.0 };
				double p[4];
				for (int r = 0; r < 4; ++r) {
					p[r] = 0.0;
					for (int k = 0; k < 4; ++k)
						p[r] += static_cast<double>(a_in.invViewProj[r * 4 + k]) * clip[k];
				}
				if (!(std::abs(p[3]) > 1e-12) || !std::isfinite(p[3])) {
					all_ = true;
					return;
				}
				double len = 0.0;
				for (int a = 0; a < 3; ++a) {
					dir[c][a] = p[a] / p[3];
					len += dir[c][a] * dir[c][a];
				}
				len = std::sqrt(len);
				if (!(len > 1e-9) || !std::isfinite(len)) {
					all_ = true;
					return;
				}
				for (int a = 0; a < 3; ++a)
					dir[c][a] /= len;
			}
			double fwd[3] = { 0.0, 0.0, 0.0 };
			for (int c = 0; c < 4; ++c)
				for (int a = 0; a < 3; ++a)
					fwd[a] += dir[c][a];
			if (!Normalize(fwd)) {
				all_ = true;
				return;
			}
			for (int c = 0; c < 4; ++c) {
				if (Dot(dir[c], fwd) <= 0.05) {  // a frustum wider than ~174 degrees: not bounded here
					all_ = true;
					return;
				}
			}
			// Frustum planes through the camera (camera-relative d = 0), then the margin; world d = d + n . camera.
			std::vector<Plane> frustum;
			for (int c = 0; c < 4; ++c) {
				const double* a = dir[c];
				const double* b = dir[(c + 1) % 4];
				Plane plane{ { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }, 0.0 };
				if (!Normalize(plane.n)) {
					all_ = true;
					return;
				}
				if (Dot(plane.n, fwd) > 0.0)
					for (double& v : plane.n)
						v = -v;
				frustum.push_back(plane);
			}
			frustum.push_back(Plane{ { -fwd[0], -fwd[1], -fwd[2] }, 0.0 });  // the camera's own plane (everything visible is in front)
			for (auto& plane : frustum)
				plane.d = Dot(plane.n, a_in.camera) + a_margin;

			// Short rays.
			near_ = frustum;
			for (auto& plane : near_)
				plane.d += a_in.nearReach;

			// Sun rays: the frustum and the rect's sides, each moved out by the extrusion's support.
			if (a_in.sun) {
				sun_ = frustum;
				sun_.push_back(Plane{ { -1.0, 0.0, 0.0 }, -a_in.rect[0] + a_margin });
				sun_.push_back(Plane{ { 0.0, -1.0, 0.0 }, -a_in.rect[1] + a_margin });
				sun_.push_back(Plane{ { 1.0, 0.0, 0.0 }, a_in.rect[2] + a_margin });
				sun_.push_back(Plane{ { 0.0, 1.0, 0.0 }, a_in.rect[3] + a_margin });
				// A ray direction r within the sun's cone: n . r <= n . toSun + |r - toSun|.
				for (auto& plane : sun_)
					plane.d += std::max(0.0, a_in.sunLength * (Dot(plane.n, a_in.toSun) + a_in.sunSpread));
			}

			// Point-light rays: the hull of the frustum and each target ball.
			points_.reserve(a_in.targets.size());
			for (const auto& target : a_in.targets) {
				const double radius = std::max(target[3], 0.0) + a_targetGrow;
				std::vector<Plane> hull = frustum;
				for (auto& plane : hull) {
					const double support = plane.n[0] * target[0] + plane.n[1] * target[1] + plane.n[2] * target[2] + radius + a_margin;
					plane.d = std::max(plane.d, support);
				}
				points_.push_back(std::move(hull));
			}
		}

		/// Whether a world box meets the region (conservative: true when unsure).
		[[nodiscard]] bool Meets(const double a_lo[3], const double a_hi[3]) const
		{
			if (!valid_ || all_)
				return true;
			for (int a = 0; a < 3; ++a)
				if (!std::isfinite(a_lo[a]) || !std::isfinite(a_hi[a]) || a_lo[a] > a_hi[a])
					return true;
			if (MeetsPolytope(near_, a_lo, a_hi))
				return true;
			if (!sun_.empty() && MeetsPolytope(sun_, a_lo, a_hi))
				return true;
			for (const auto& hull : points_)
				if (MeetsPolytope(hull, a_lo, a_hi))
					return true;
			return false;
		}

	private:
		static double Dot(const double a_a[3], const double a_b[3]) { return a_a[0] * a_b[0] + a_a[1] * a_b[1] + a_a[2] * a_b[2]; }
		static bool Normalize(double a_v[3])
		{
			const double len = std::sqrt(Dot(a_v, a_v));
			if (!(len > 1e-12) || !std::isfinite(len))
				return false;
			for (int a = 0; a < 3; ++a)
				a_v[a] /= len;
			return true;
		}
		static bool MeetsPolytope(const std::vector<Plane>& a_planes, const double a_lo[3], const double a_hi[3])
		{
			for (const auto& plane : a_planes) {
				// The box's lowest value of n . p: separated when even that is outside.
				double low = 0.0;
				for (int a = 0; a < 3; ++a)
					low += plane.n[a] * (plane.n[a] >= 0.0 ? a_lo[a] : a_hi[a]);
				if (low > plane.d)
					return false;
			}
			return true;
		}

		bool valid_ = false;
		bool all_ = false;
		std::vector<Plane> sun_;
		std::vector<Plane> near_;
		std::vector<std::vector<Plane>> points_;
	};

	/// Box (world) of a mesh-space box under a rotation x scale (row-major, row r = world axis r) and a world translation.
	inline void TransformBox(const float a_m[3][3], const double a_t[3], const float a_lo[3], const float a_hi[3], double o_lo[3], double o_hi[3])
	{
		double c[3], h[3];
		for (int a = 0; a < 3; ++a) {
			c[a] = 0.5 * (static_cast<double>(a_lo[a]) + static_cast<double>(a_hi[a]));
			h[a] = 0.5 * (static_cast<double>(a_hi[a]) - static_cast<double>(a_lo[a]));
		}
		for (int r = 0; r < 3; ++r) {
			double center = a_t[r], half = 0.0;
			for (int k = 0; k < 3; ++k) {
				center += static_cast<double>(a_m[r][k]) * c[k];
				half += std::abs(static_cast<double>(a_m[r][k])) * h[k];
			}
			o_lo[r] = center - half;
			o_hi[r] = center + half;
		}
	}
}
