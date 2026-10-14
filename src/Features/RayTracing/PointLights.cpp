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
		};

		__declspec(noinline) void ReadLightUnsafe(RE::BSLight* a_light, LightRead& o)
		{
			auto* niLight = a_light->light.get();
			if (!niLight || niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return;
			o.niLight = niLight;
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
			if (o.shadow)
				o.directional = static_cast<RE::BSShadowLight*>(a_light)->IsDirectionalLight();
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
			std::uint32_t first = 0;  // first member in the member list
			std::uint32_t slot = UINT32_MAX;
		};

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
		tags_.clear();
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
			++stats_.used;
			if (r.lodDimmer < a_config.minLodDimmer) {
				++stats_.byReason[kRejectFaded];
				continue;
			}
			if (r.radius < a_config.minRadius || r.radius > a_config.maxRadius) {
				++stats_.byReason[kRejectRadius];
				continue;
			}
			const float intensity = std::max({ r.color[0], r.color[1], r.color[2] }) * r.fade;
			const float intensityBeforeFade = r.lodDimmer > 1e-4f ? intensity / r.lodDimmer : intensity;
			if (intensityBeforeFade < a_config.minIntensity) {
				++stats_.byReason[kRejectIntensity];
				continue;
			}
			const double dx = r.pos[0] - a_camera[0], dy = r.pos[1] - a_camera[1], dz = r.pos[2] - a_camera[2];
			const double distSq = dx * dx + dy * dy + dz * dz;
			if (distSq > maxDistSq) {
				++stats_.byReason[kRejectDistance];
				continue;
			}
			if (r.pos[0] < a_rect[0] || r.pos[1] < a_rect[1] || r.pos[0] > a_rect[2] || r.pos[1] > a_rect[3]) {
				++stats_.byReason[kRejectOutside];
				continue;
			}
			// Off screen: its whole sphere outside one of the side / near planes.
			{
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
			Util::LightCensus::Category category = Util::LightCensus::Category::Unknown;
			if (!Util::LightCensus::ClassifyCached(light, r.radius, a_frame, classifyBudget, category)) {
				++stats_.pending;
				continue;
			}
			using C = Util::LightCensus::Category;
			const bool window = category == C::WindowBeam || category == C::WindowGlow || category == C::WindowFill;
			if (native)
				include = a_config.native && !window && category != C::Player && category != C::Effect;  // omni / hemi / spot shadow lights
			else
				include = IncludedCategory(category);
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
			c.native = native;
			candidates.push_back(c);
			stats_.native += native ? 1 : 0;
		}
		stats_.candidates = static_cast<std::uint32_t>(candidates.size());
		Util::LightCensus::TrimClassifierCache(a_frame, 600);

		// Merge: brightest first; a light within the merge distance of a group's centre joins it.
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.intensity > b.intensity; });
		const double mergeSq = static_cast<double>(a_config.mergeDistance) * a_config.mergeDistance;
		memberGroup.assign(candidates.size(), UINT32_MAX);
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			const auto& c = candidates[i];
			std::uint32_t found = UINT32_MAX;
			for (std::uint32_t g = 0; g < groups.size(); ++g) {
				const auto& group = groups[g];
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
		std::array<bool, kMaxSources> taken{};
		for (const std::uint32_t g : order) {
			auto& group = groups[g];
			double best = mergeSq;
			std::uint32_t bestSlot = UINT32_MAX;
			for (std::uint32_t s = 0; s < kMaxSources; ++s) {
				if (!slots_[s].live || taken[s])
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
			const float clearance = std::min(a_config.clearance, group.radius * 0.5f);
			std::memcpy(&clearanceBits, &clearance, sizeof(clearanceBits));
			out.info[1] = clearanceBits;
			scan = std::max(scan, group.slot + 1);
			++stats_.sources;
		}
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			// Capped groups never got a slot.
			const std::uint32_t g = memberGroup[i];
			if (g == UINT32_MAX || groups[g].slot == UINT32_MAX)
				continue;
			const std::uint32_t tag = ((groups[g].slot + 1) & kTagSourceMask) | (candidates[i].native ? kTagNative : 0u) | tagFlags;
			tags_.emplace_back(candidates[i].light, tag);
		}
		std::sort(tags_.begin(), tags_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		stats_.cpuMs = static_cast<float>(QpcToMs(Qpc() - start));
		return scan;
	}
}
