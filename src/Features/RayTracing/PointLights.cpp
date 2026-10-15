#include "PointLights.h"

#include "Features/LightLimitFix.h"
#include "State.h"
#include "Utils/LightCensus.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RayTracingPointLights
{
	namespace
	{
		enum Reject : std::uint32_t
		{
			kRejectFaded = 0,
			kRejectRadius,
			kRejectIntensity,
			kRejectDistance,
			kRejectOutside,
			kRejectCategory,
			kRejectFrustum,
			kRejectUnused
		};

		/// What the selection needs of one light (plain data: filled under SEH).
		struct LightRead
		{
			RE::NiLight* niLight = nullptr;
			const void* owner = nullptr;  // (46e) the light's reference (GetUserData: its own, or a parent node's)
			float pos[3]{};
			float color[3]{};
			float fade = 0.0f;  // with lodDimmer, as LLF uploads it
			float radius = 0.0f, invRadius = 0.0f, fadeZone = 0.0f, sizeBias = 0.0f;
			float lodDimmer = 0.0f;
			std::uint32_t flags = 0;  // LightLimitFix::LightFlags
			std::uint32_t shadowIndex = 0;
			bool shadow = false;
			bool directional = false;
			bool used = false;  // LLF uploads it
			// (46c) Spot shadow lights: the cone of their shadow map (the engine's spot mask: 1 - r^falloff, r = the
			// projected distance from the axis, 1 at the frustum's half width).
			bool spot = false;
			bool spotValid = false;
			bool spotFromCamera = false;
			float dir[3]{}, right[3]{}, up[3]{};
			float semiWidth = 0.0f, semiHeight = 0.0f, falloff = 1.0f;
		};

		bool Normalize3(float (&v)[3])
		{
			const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (!std::isfinite(len) || len < 1e-4f)
				return false;
			for (auto& c : v)
				c /= len;
			return true;
		}

		/// (46c) The cone of a spot shadow light (BSShadowFrustumLight). Preferred: the camera its shadow map was
		/// rendered with (axes: Gamebryo's x forward, y up, z right; frustum extents at unit distance); otherwise
		/// the light's own axes and BSShadowFrustumLight's semi width / height. Falloff: BSShadowFrustumLight::falloff
		/// (0x568, VR 0x5C8), 1 when unusable. SE/AE layout only (Ray Tracing is not available in VR).
		void ReadSpotUnsafe(RE::BSShadowLight* a_light, RE::NiLight* a_niLight, LightRead& o)
		{
			const RE::NiMatrix3* rotate = &a_niLight->world.rotate;
			float semiW = REL::RelocateMember<float>(a_light, 0x560, 0x5C0);
			float semiH = REL::RelocateMember<float>(a_light, 0x564, 0x5C4);
			const float falloff = REL::RelocateMember<float>(a_light, 0x568, 0x5C8);
			if (!REL::Module::IsVR()) {
				auto& descriptors = a_light->GetRuntimeData().shadowmapDescriptors;
				if (!descriptors.empty()) {
					if (auto* camera = descriptors[0].camera.get()) {
						const auto& frustum = camera->GetRuntimeData2().viewFrustum;
						const float w = std::max(std::abs(frustum.fLeft), std::abs(frustum.fRight));
						const float h = std::max(std::abs(frustum.fTop), std::abs(frustum.fBottom));
						if (!frustum.bOrtho && std::isfinite(w) && std::isfinite(h) && w > 0.01f && h > 0.01f && w < 50.0f && h < 50.0f) {
							semiW = w;
							semiH = h;
							rotate = &camera->world.rotate;
							o.spotFromCamera = true;
						}
					}
				}
			}
			for (int r = 0; r < 3; ++r) {
				o.dir[r] = rotate->entry[r][0];
				o.up[r] = rotate->entry[r][1];
				o.right[r] = rotate->entry[r][2];
			}
			o.semiWidth = semiW;
			o.semiHeight = semiH;
			o.falloff = (std::isfinite(falloff) && falloff > 0.0f && falloff <= 64.0f) ? falloff : 1.0f;
			o.spotValid = std::isfinite(semiW) && std::isfinite(semiH) && semiW > 0.01f && semiH > 0.01f && semiW < 50.0f && semiH < 50.0f &&
			              Normalize3(o.dir) && Normalize3(o.up) && Normalize3(o.right);
		}

		__declspec(noinline) void ReadLightUnsafe(RE::BSLight* a_light, LightRead& o)
		{
			auto* niLight = a_light->light.get();
			if (!niLight || niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return;
			o.niLight = niLight;
			o.owner = niLight->GetUserData();
			LightLimitFix::LightData d{};
			LightLimitFix::FillLightData(a_light, niLight, d);
			o.color[0] = d.color.x;
			o.color[1] = d.color.y;
			o.color[2] = d.color.z;
			o.fade = d.fade;
			o.radius = d.radius;
			o.invRadius = d.invRadius;
			o.fadeZone = d.fadeZone;
			o.sizeBias = d.sizeBias;
			o.flags = d.lightFlags.underlying();
			o.shadowIndex = d.shadowMaskIndex;
			o.lodDimmer = a_light->lodDimmer;
			o.shadow = a_light->IsShadowLight();
			if (o.shadow) {
				auto* shadowLight = static_cast<RE::BSShadowLight*>(a_light);
				o.directional = shadowLight->IsDirectionalLight();
				o.spot = !o.directional && shadowLight->IsSpotLight();
				if (o.spot)
					ReadSpotUnsafe(shadowLight, niLight, o);
			}
			const auto& p = niLight->world.translate;
			o.pos[0] = p.x;
			o.pos[1] = p.y;
			o.pos[2] = p.z;
			// Light Limit Fix's own test (UpdateLights): an active shadow slot, some colour, some radius.
			o.used = o.shadowIndex != 255 && (o.color[0] + o.color[1] + o.color[2]) * o.fade > 1e-4f && o.radius > 1e-4f;
		}

		bool ReadLight(RE::BSLight* a_light, LightRead* o_read)
		{
			__try {
				ReadLightUnsafe(a_light, *o_read);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// Copies the two light lists (NiPointer: the lights stay alive while we hold them this frame).
		__declspec(noinline) void CollectUnsafe(RE::ShadowSceneNode* a_node, std::vector<RE::BSLight*>& o_lights)
		{
			for (auto& e : a_node->GetRuntimeData().activeLights)
				if (e)
					o_lights.push_back(e.get());
			for (auto& e : a_node->GetRuntimeData().activeShadowLights)
				if (e)
					o_lights.push_back(e.get());
		}

		bool Collect(RE::ShadowSceneNode* a_node, std::vector<RE::BSLight*>* o_lights)
		{
			__try {
				CollectUnsafe(a_node, *o_lights);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool IncludedCategory(Util::LightCensus::Category a_category)
		{
			using C = Util::LightCensus::Category;
			switch (a_category) {
			case C::Fixture:
			case C::Actor:
			case C::Standalone:
			case C::MeshAttached:
			case C::FixtureShadow:
				return true;
			default:
				return false;  // windows, fill, effect, player (46d), unknown
			}
		}

		struct Candidate
		{
			RE::BSLight* light;
			double pos[3];
			LightRead read;
			float intensity;  // brightest channel x fade (with lodDimmer)
			float score;
			bool native;
			bool spot;   // (46c) a spot shadow light (own group, its cone)
			bool beam;   // (46c) a window beam
			std::uint8_t kind;  // (46d) 0 world light, 1 the player's, 2 spell / effect
			std::uint32_t owner;  // (46e) key of the traced character carrying it (0 = none)
		};

		struct Group
		{
			double pos[3]{};
			double weight = 0.0;
			float radius = 0.0f;
			float intensity = 0.0f;
			float score = 0.0f;
			const Candidate* strongest = nullptr;
			bool native = false;
			bool spot = false;  // (46c) never merged with anything
			std::uint8_t kind = 0;  // (46d) merged only with lights of the same kind
			std::uint32_t owner = 0;  // (46e) merged only with lights of the same carrier (or none)
			std::uint32_t first = 0;  // first member in the member list
			std::uint32_t slot = UINT32_MAX;
		};

		/// (46c) Spot parameters logged once per light (the first 32): the cone has to match the engine's.
		void LogSpot(const RE::BSLight* a_light, const LightRead& a_read, bool a_beam)
		{
			static std::vector<const RE::BSLight*> logged;
			if (logged.size() >= 32 || std::find(logged.begin(), logged.end(), a_light) != logged.end())
				return;
			logged.push_back(a_light);
			logger::info("[RayTracing] Spot shadow light {}{}: position ({:.0f} {:.0f} {:.0f}), radius {:.0f}, direction ({:.3f} {:.3f} {:.3f}), half widths tan {:.3f} x {:.3f} ({:.1f} x {:.1f} deg), "
						 "falloff {:.2f}, from {}{}",
				static_cast<const void*>(a_light), a_beam ? " (window beam)" : "", a_read.pos[0], a_read.pos[1], a_read.pos[2], a_read.radius, a_read.dir[0], a_read.dir[1], a_read.dir[2], a_read.semiWidth,
				a_read.semiHeight, std::atan(a_read.semiWidth) * 57.29578f, std::atan(a_read.semiHeight) * 57.29578f, a_read.falloff,
				a_read.spotFromCamera ? "its shadow-map camera" : "the light's axes", a_read.spotValid ? "" : " -- invalid, left to the engine");
		}

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcToMs(std::int64_t a_ticks)
		{
			static const double toMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
			}();
			return static_cast<double>(a_ticks) * toMs;
		}
	}

	void Selector::Clear()
	{
		slots_ = {};
		motion_.fill(255);
		slotBeams_.fill(nullptr);
		tags_.clear();
		maskEntries_.clear();
		history_.clear();  // (46g)
		published_ = false;
	}

	std::uint32_t Selector::TagFor(const RE::BSLight* a_light) const
	{
		if (!published_ || tags_.empty() || !a_light)
			return 0;
		const auto it = std::lower_bound(tags_.begin(), tags_.end(), a_light, [](const auto& a_entry, const RE::BSLight* a_key) { return a_entry.first < a_key; });
		return (it != tags_.end() && it->first == a_light) ? it->second : 0u;
	}

	std::uint32_t Selector::Select(const Config& a_config, const double a_camera[3], const float a_viewProj[16], const float a_rect[4], std::uint32_t a_frame,
		GpuSource* o_sources)
	{
		const std::int64_t start = Qpc();
		published_ = false;
		tags_.clear();
		maskEntries_.clear();
		stats_ = {};
		std::fill_n(o_sources, kMaxSources, GpuSource{});

		auto* smState = globals::game::smState;
		auto* node = smState ? smState->shadowSceneNode[0] : nullptr;
		static thread_local std::vector<RE::BSLight*> lights;
		static thread_local std::vector<Candidate> candidates;
		static thread_local std::vector<Group> groups;
		static thread_local std::vector<std::uint32_t> members;  // candidate index per group, chained by group order
		static thread_local std::vector<std::uint32_t> memberGroup;
		lights.clear();
		candidates.clear();
		groups.clear();
		members.clear();
		memberGroup.clear();
		if (!node || !Collect(node, &lights)) {
			stats_.readErrors += node ? 1 : 0;
			for (auto& slot : slots_)
				if (slot.live) {
					slot.live = false;
					slot.freedFrame = a_frame;
				}
			stats_.cpuMs = static_cast<float>(QpcToMs(Qpc() - start));
			return 0;
		}
		stats_.lights = static_cast<std::uint32_t>(lights.size());

		// Frustum planes (camera-relative): rows of the view-projection (clip = M * p, row-major).
		const float* m = a_viewProj;
		float planes[5][4];
		for (int i = 0; i < 4; ++i) {
			planes[0][i] = m[12 + i] + m[0 + i];  // left
			planes[1][i] = m[12 + i] - m[0 + i];  // right
			planes[2][i] = m[12 + i] + m[4 + i];  // bottom
			planes[3][i] = m[12 + i] - m[4 + i];  // top
			planes[4][i] = m[8 + i];              // near (D3D: z >= 0)
		}
		for (auto& p : planes) {
			const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
			if (len > 1e-12f)
				for (auto& v : p)
					v /= len;
		}

		std::uint32_t classifyBudget = a_config.classifyBudget;
		const double maxDistSq = static_cast<double>(a_config.maxDistance) * a_config.maxDistance;
		for (auto* light : lights) {
			LightRead r;
			if (!ReadLight(light, &r)) {
				++stats_.readErrors;
				continue;
			}
			if (!r.niLight || !r.used || r.directional) {
				++stats_.byReason[kRejectUnused];
				continue;
			}
			// (46e) Engine data that is not a number (a broken light) never reaches the ranking: NaN breaks std::sort.
			if (!std::isfinite(r.pos[0]) || !std::isfinite(r.pos[1]) || !std::isfinite(r.pos[2]) || !std::isfinite(r.radius) || !std::isfinite(r.fade) ||
				!std::isfinite(r.lodDimmer) || !std::isfinite(r.color[0]) || !std::isfinite(r.color[1]) || !std::isfinite(r.color[2])) {
				++stats_.readErrors;
				continue;
			}
			++stats_.used;
			// (46c) An engine shadow light whose shadow map was not drawn this frame: always a source (no other rule).
			const bool forced = a_config.forced && r.shadow &&
			                    std::binary_search(a_config.forced->begin(), a_config.forced->end(), static_cast<const RE::BSLight*>(light));
			// (46c) Spot shadow lights: only indoors and only with a readable cone (outdoors they keep the engine's).
			const bool spot = r.shadow && r.spot;
			if (spot && a_config.interior)
				LogSpot(light, r, r.radius > 10000.0f);
			if (spot && (!a_config.interior || !r.spotValid)) {
				stats_.spotsInvalid += a_config.interior ? 1 : 0;
				++stats_.byReason[kRejectCategory];
				continue;
			}
			// (46c) Window beams (Window Shadows Ultimate): huge spots outside the windows. Classified first: the
			// radius / distance / area rules below do not apply to them.
			Util::LightCensus::Category category = Util::LightCensus::Category::Unknown;
			bool classified = false;
			bool beam = false;
			if (spot && r.radius > a_config.maxRadius) {
				if (!Util::LightCensus::ClassifyCached(light, r.radius, a_frame, classifyBudget, category)) {
					++stats_.pending;
					continue;
				}
				classified = true;
				beam = category == Util::LightCensus::Category::WindowBeam;
				if (!beam || !a_config.beams) {
					++stats_.byReason[kRejectCategory];
					continue;
				}
				if (a_config.excludedBeams && std::binary_search(a_config.excludedBeams->begin(), a_config.excludedBeams->end(), static_cast<const RE::BSLight*>(light))) {
					++stats_.byReason[kRejectCategory];  // (46g) implausible: the engine's shadow map stays
					continue;
				}
			}
			// (46g) Stable selection: the smoothed intensity / radius decide (flickering candles, Inverse Square Lighting's radius
			// following the fade), with hysteresis for a light that was tagged last frame.
			const float rawIntensity = std::max({ r.color[0], r.color[1], r.color[2] }) * r.fade;
			float smoothIntensity = rawIntensity;
			bool wasTagged = false;
			if (a_config.stable && !beam) {
				auto& h = history_[light];
				if (h.frame == 0 || a_frame - h.frame > 30u) {
					h.intensity = rawIntensity;
					h.radius = r.radius;
					h.tagged = false;
				} else if (h.frame != a_frame) {
					h.intensity += (rawIntensity - h.intensity) * 0.1f;
					h.radius = std::max(r.radius, h.radius * 0.98f);
				}
				wasTagged = h.tagged && a_frame - h.frame <= 2u;
				h.frame = a_frame;
				h.tagged = false;
				smoothIntensity = h.intensity;
				r.radius = h.radius;
				r.invRadius = 1.0f / std::max(r.radius, 1.0f);
			}
			const float cutScale = wasTagged ? 0.8f : 1.0f;
			const bool limits = !forced && !beam;
			if (limits && r.lodDimmer < a_config.minLodDimmer) {
				++stats_.byReason[kRejectFaded];
				continue;
			}
			if (limits && (r.radius < a_config.minRadius * cutScale || r.radius > a_config.maxRadius)) {
				++stats_.byReason[kRejectRadius];
				continue;
			}
			const float intensity = smoothIntensity;
			const float intensityBeforeFade = r.lodDimmer > 1e-4f ? intensity / r.lodDimmer : intensity;
			if (!forced && intensityBeforeFade < a_config.minIntensity * cutScale) {
				++stats_.byReason[kRejectIntensity];
				continue;
			}
			const double dx = r.pos[0] - a_camera[0], dy = r.pos[1] - a_camera[1], dz = r.pos[2] - a_camera[2];
			const double distSq = dx * dx + dy * dy + dz * dz;
			if (limits && distSq > maxDistSq) {
				++stats_.byReason[kRejectDistance];
				continue;
			}
			if (limits && (r.pos[0] < a_rect[0] || r.pos[1] < a_rect[1] || r.pos[0] > a_rect[2] || r.pos[1] > a_rect[3])) {
				++stats_.byReason[kRejectOutside];
				continue;
			}
			// Off screen: its whole sphere outside one of the side / near planes.
			if (limits) {
				bool outside = false;
				for (const auto& p : planes) {
					const float d = p[0] * static_cast<float>(dx) + p[1] * static_cast<float>(dy) + p[2] * static_cast<float>(dz) + p[3];
					if (d < -r.radius) {
						outside = true;
						break;
					}
				}
				if (outside) {
					++stats_.byReason[kRejectFrustum];
					continue;
				}
			}
			// The census category (cached per light; a few new lights read per frame, nearest first is not needed:
			// the filters above leave only lit fixtures near the camera).
			const bool native = r.shadow;
			bool include = false;
			if (!classified && !Util::LightCensus::ClassifyCached(light, r.radius, a_frame, classifyBudget, category)) {
				if (!forced) {
					++stats_.pending;
					continue;
				}
				category = Util::LightCensus::Category::Unknown;
			}
			using C = Util::LightCensus::Category;
			const bool window = category == C::WindowBeam || category == C::WindowGlow || category == C::WindowFill;
			// (46d) The player's own lights and ownerless spell / effect lights, each with its switch.
			const bool own = category == C::Player || category == C::Effect;
			const bool ownWanted = category == C::Player ? a_config.player : (category == C::Effect ? a_config.effects : false);
			if (forced || beam)
				include = true;
			else if (native)
				include = a_config.native && !window && (!own || ownWanted);  // omni / hemi / spot shadow lights
			else
				include = own ? ownWanted : IncludedCategory(category);
			if (!include) {
				++stats_.byReason[kRejectCategory];
				continue;
			}
			Candidate c;
			c.light = light;
			c.pos[0] = r.pos[0];
			c.pos[1] = r.pos[1];
			c.pos[2] = r.pos[2];
			c.read = r;
			c.intensity = intensity;
			// Screen influence: brightness x the light sphere's share of the view (capped where it fills it).
			const float dist = static_cast<float>(std::sqrt(distSq));
			const float cover = r.radius / std::max(dist, 1.0f);
			c.score = intensity * std::min(cover * cover, 4.0f);
			if (forced)
				c.score = FLT_MAX;  // never capped
			c.native = native;
			c.spot = spot;
			c.beam = beam;
			c.kind = (forced || beam) ? 0 : (category == C::Player ? 1 : (category == C::Effect ? 2 : 0));
			// (46e) Carried by a traced character (an NPC's torch): its rays do not test that character (the player's own
			// lights skip the player's body through kSourceNoPlayer already).
			c.owner = 0;
			if (a_config.owners && r.owner && c.kind != 1) {
				const auto it = std::lower_bound(a_config.owners->begin(), a_config.owners->end(), r.owner,
					[](const auto& a_entry, const void* a_key) { return a_entry.first < a_key; });
				if (it != a_config.owners->end() && it->first == r.owner)
					c.owner = it->second;
			} else if (a_config.owners && c.kind == 1 && a_config.playerCarrier) {
				// (46q) The player's own light in third person: the player is its carrier (by the player reference; a light whose
				// reference is another node of the player is still the player's). Not traced: as 46p (kSourceNoPlayer).
				const void* player = RE::PlayerCharacter::GetSingleton();
				const auto it = std::lower_bound(a_config.owners->begin(), a_config.owners->end(), player,
					[](const auto& a_entry, const void* a_key) { return a_entry.first < a_key; });
				if (player && it != a_config.owners->end() && it->first == player)
					c.owner = it->second;
			}
			if (wasTagged && (r.radius < a_config.minRadius || intensityBeforeFade < a_config.minIntensity))
				++stats_.kept;
			candidates.push_back(c);
			stats_.native += native ? 1 : 0;
			stats_.forced += forced ? 1 : 0;
		}
		stats_.candidates = static_cast<std::uint32_t>(candidates.size());
		Util::LightCensus::TrimClassifierCache(a_frame, 600);

		// Merge: brightest first; a light within the merge distance of a group's centre joins it.
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
			return a.intensity > b.intensity || (a.intensity == b.intensity && a.light < b.light);  // (46g) a stable seed order
		});
		const double mergeSq = static_cast<double>(a_config.mergeDistance) * a_config.mergeDistance;
		memberGroup.assign(candidates.size(), UINT32_MAX);
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			const auto& c = candidates[i];
			std::uint32_t found = UINT32_MAX;
			// (46c) Spot lights keep a source of their own (their cone and direction are theirs).
			for (std::uint32_t g = 0; g < groups.size() && !c.spot; ++g) {
				const auto& group = groups[g];
				if (group.spot || group.kind != c.kind || group.owner != c.owner)
					continue;
				const double ex = group.pos[0] - c.pos[0], ey = group.pos[1] - c.pos[1], ez = group.pos[2] - c.pos[2];
				if (ex * ex + ey * ey + ez * ez <= mergeSq) {
					found = g;
					break;
				}
			}
			if (found == UINT32_MAX) {
				Group group;
				std::copy_n(c.pos, 3, group.pos);
				group.weight = std::max(c.intensity, 1e-4f);
				group.radius = c.read.radius;
				group.intensity = c.intensity;
				group.score = c.score;
				group.strongest = &c;
				group.native = c.native;
				group.spot = c.spot;
				group.kind = c.kind;
				group.owner = c.owner;
				groups.push_back(group);
				memberGroup[i] = static_cast<std::uint32_t>(groups.size() - 1);
			} else {
				auto& group = groups[found];
				const double w = std::max(c.intensity, 1e-4f);
				for (int k = 0; k < 3; ++k)
					group.pos[k] = (group.pos[k] * group.weight + c.pos[k] * w) / (group.weight + w);
				group.weight += w;
				group.radius = std::max(group.radius, c.read.radius);
				group.intensity += c.intensity;
				group.score += c.score;
				group.native = group.native || c.native;
				memberGroup[i] = found;
				++stats_.merged;
			}
		}

		// Cap by screen influence.
		static thread_local std::vector<std::uint32_t> order;
		order.resize(groups.size());
		for (std::uint32_t g = 0; g < groups.size(); ++g)
			order[g] = g;
		std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return groups[a].score > groups[b].score; });
		const std::uint32_t cap = std::clamp<std::uint32_t>(a_config.maxSources, 1u, kMaxSources);
		if (order.size() > cap) {
			stats_.capped = static_cast<std::uint32_t>(order.size() - cap);
			order.resize(cap);
		}

		// Slots: a source keeps the slot of last frame's source nearest to it (within the merge distance);
		// new ones take a slot freed at least two frames ago when there is one (the denoiser's history of the
		// old source is gone by then).
		// (46d) Only a slot of the same kind (a carried torch never takes over a lantern's history), and the distance the
		// source moved since last frame goes to the denoiser (SlotMotion): a moving light drops its history in proportion.
		std::array<bool, kMaxSources> taken{};
		motion_.fill(255);
		for (const std::uint32_t g : order) {
			auto& group = groups[g];
			double best = mergeSq;
			std::uint32_t bestSlot = UINT32_MAX;
			for (std::uint32_t s = 0; s < kMaxSources; ++s) {
				if (!slots_[s].live || taken[s] || slots_[s].kind != group.kind)
					continue;
				const double ex = slots_[s].pos[0] - group.pos[0], ey = slots_[s].pos[1] - group.pos[1], ez = slots_[s].pos[2] - group.pos[2];
				const double d = ex * ex + ey * ey + ez * ez;
				if (d <= best) {
					best = d;
					bestSlot = s;
				}
			}
			if (bestSlot != UINT32_MAX) {
				group.slot = bestSlot;
				taken[bestSlot] = true;
				const double moved = std::sqrt(best);
				motion_[bestSlot] = static_cast<std::uint8_t>(std::clamp(moved / kMotionFullUnits * 255.0, 0.0, 255.0));
			}
		}
		// Last frame's slots that found no source are free from now on.
		for (std::uint32_t s = 0; s < kMaxSources; ++s) {
			if (slots_[s].live && !taken[s]) {
				slots_[s].live = false;
				slots_[s].freedFrame = a_frame;
			}
		}
		for (const std::uint32_t g : order) {
			auto& group = groups[g];
			if (group.slot != UINT32_MAX)
				continue;
			std::uint32_t pick = UINT32_MAX, fallback = UINT32_MAX;
			for (std::uint32_t s = 0; s < kMaxSources; ++s) {
				if (taken[s] || slots_[s].live)
					continue;
				if (a_frame - slots_[s].freedFrame >= 2u || slots_[s].freedFrame == 0) {
					pick = s;
					break;
				}
				if (fallback == UINT32_MAX)
					fallback = s;
			}
			if (pick == UINT32_MAX)
				pick = fallback;
			if (pick == UINT32_MAX)
				continue;  // cannot happen (cap <= slots)
			group.slot = pick;
			taken[pick] = true;
			++stats_.slotChanges;
		}

		// GPU records and tags.
		slotBeams_.fill(nullptr);  // (46g)
		std::uint32_t scan = 0;
		const std::uint32_t tagFlags = (a_config.keepSSPLS ? kTagKeepSSPLS : 0u) | (a_config.split ? kTagSplit : 0u);
		std::uint32_t clearanceBits;
		for (const std::uint32_t g : order) {
			const auto& group = groups[g];
			if (group.slot == UINT32_MAX || !group.strongest)
				continue;
			auto& slot = slots_[group.slot];
			slot.live = true;
			std::copy_n(group.pos, 3, slot.pos);
			slot.kind = group.kind;
			const auto& s = *group.strongest;
			auto& out = o_sources[group.slot];
			out.posRadius[0] = static_cast<float>(group.pos[0] - a_camera[0]);
			out.posRadius[1] = static_cast<float>(group.pos[1] - a_camera[1]);
			out.posRadius[2] = static_cast<float>(group.pos[2] - a_camera[2]);
			out.posRadius[3] = group.radius;
			out.atten[0] = 1.0f / std::max(group.radius, 1.0f);
			out.atten[1] = s.read.fadeZone;
			out.atten[2] = s.read.sizeBias;
			out.atten[3] = group.intensity;
			const bool isl = (s.read.flags & static_cast<std::uint32_t>(LightLimitFix::LightFlags::InverseSquare)) != 0;
			out.info[0] = (isl ? kSourceISL : 0u) | (group.native ? kSourceNative : 0u);
			float clearance = std::min(a_config.clearance, group.radius * 0.5f);
			if (group.kind != 0) {
				// (46d) The player's lights: the player's body is not tested (the torch, the arm, the torso); their own
				// fixture is part of the player (excluded) or does not exist (spells): a small clearance only.
				if (group.kind == 1 && group.owner == 0)  // (46q) with a carrier key the body blocks except near the light
					out.info[0] |= kSourceNoPlayer;
				clearance = std::min(a_config.ownClearance, group.radius * 0.5f);
				stats_.player += group.kind == 1 ? 1 : 0;
				stats_.effects += group.kind == 2 ? 1 : 0;
			}
			if (group.spot) {
				// (46c) Spot: its cone (the trace takes the larger half width: the engine's mask is 0 beyond it).
				out.info[0] |= kSourceSpot | (s.beam ? kSourceBeam : 0u);
				std::copy_n(s.read.dir, 3, out.spot);
				out.spot[3] = std::max(s.read.semiWidth, s.read.semiHeight);
				std::memcpy(&out.info[2], &s.read.falloff, sizeof(float));
				if (s.beam)
					slotBeams_[group.slot] = s.light;  // (46g)
				if (s.beam)
					clearance = 0.0f;  // the light is outside the interior: nothing of its own is in the way
				++stats_.spots;
				stats_.beams += s.beam ? 1 : 0;
			}
			std::memcpy(&clearanceBits, &clearance, sizeof(clearanceBits));
			out.info[1] = clearanceBits;
			out.info[3] = group.owner & 0xFFFFFFu;  // (46e) TracePointLightsCS: instances of this character are not tested
			stats_.carried += group.owner ? 1 : 0;
			scan = std::max(scan, group.slot + 1);
			++stats_.sources;
		}
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			// Capped groups never got a slot.
			const std::uint32_t g = memberGroup[i];
			if (g == UINT32_MAX || groups[g].slot == UINT32_MAX)
				continue;
			if (a_config.stable) {
				if (const auto it = history_.find(candidates[i].light); it != history_.end())
					it->second.tagged = true;  // (46g) hysteresis next frame
			}
			const auto& c = candidates[i];
			// (46c) Spot lights are not tagged: Lighting reads their shadow mask channel, which Ray Tracing writes (with the cone).
			if (!c.spot) {
				const std::uint32_t tag = ((groups[g].slot + 1) & kTagSourceMask) | (c.native ? kTagNative : 0u) | tagFlags;
				tags_.emplace_back(c.light, tag);
			}
			// (46c) Interiors: every engine shadow light with a source gets its mask channel written.
			if (a_config.interior && c.native && c.read.shadowIndex < 4) {
				MaskEntry e;
				e.light = c.light;
				e.channel = c.read.shadowIndex;
				e.source = groups[g].slot + 1;
				e.spot = c.spot;
				for (int k = 0; k < 3; ++k) {
					e.pos[k] = static_cast<float>(c.pos[k] - a_camera[k]);
					e.world[k] = c.pos[k];
				}
				std::copy_n(c.read.dir, 3, e.dir);
				std::copy_n(c.read.right, 3, e.right);
				std::copy_n(c.read.up, 3, e.up);
				e.semiWidth = c.read.semiWidth;
				e.semiHeight = c.read.semiHeight;
				e.falloff = c.read.falloff;
				maskEntries_.push_back(e);
			}
		}
		std::sort(tags_.begin(), tags_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		// (46g) Lights not seen for a while leave the history.
		if (a_frame % 300u == 0u)
			std::erase_if(history_, [&](const auto& a_entry) { return a_frame - a_entry.second.frame > 600u; });
		if (a_config.forced) {
			for (const auto* light : *a_config.forced)
				if (std::none_of(maskEntries_.begin(), maskEntries_.end(), [&](const MaskEntry& a_entry) { return a_entry.light == light; }))
					++stats_.forcedMissing;
		}
		stats_.cpuMs = static_cast<float>(QpcToMs(Qpc() - start));
		return scan;
	}
}
