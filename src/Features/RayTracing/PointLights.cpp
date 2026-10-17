#include "PointLights.h"

#include "Features/LightLimitFix.h"
#include "State.h"
#include "Utils/LightCensus.h"

#include <DirectXPackedVector.h>
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
			// (46s) Steady position: the light's local offset (the engine's flicker moves it) and its parent node's world position.
			float local[3]{};
			float parentPos[3]{};
			bool hasParent = false;
			// (46x) Light Limit Fix's room rule (IsGlobalLight): portal-strict with a portal graph = only the geometry of its rooms.
			bool portalStrict = false;
			bool portalGraph = false;
		};

		/// (46x) The rooms (BSMultiBoundRoom) and portal shared nodes a portal-strict light lights (Light Limit Fix's room nodes).
		struct RoomRead
		{
			RE::NiNode* nodes[32]{};
			std::uint32_t count = 0;
			bool overflow = false;
		};

		__declspec(noinline) void ReadRoomsUnsafe(RE::BSLight* a_light, RoomRead& o)
		{
			for (auto* room : a_light->rooms) {
				if (!room)
					continue;
				if (o.count >= std::size(o.nodes)) {
					o.overflow = true;
					return;
				}
				// BSMultiBoundRoom : BSMultiBoundNode : NiNode (single inheritance; CommonLib only declares the class).
				o.nodes[o.count++] = reinterpret_cast<RE::NiNode*>(room);
			}
			for (auto* portal : a_light->portals) {
				auto* shared = portal ? portal->portalSharedNode.get() : nullptr;
				if (!shared)
					continue;
				if (o.count >= std::size(o.nodes)) {
					o.overflow = true;
					return;
				}
				o.nodes[o.count++] = shared;
			}
		}

		bool ReadRooms(RE::BSLight* a_light, RoomRead* o_read)
		{
			__try {
				ReadRoomsUnsafe(a_light, *o_read);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// (46x) A room node's world box (axis-aligned, conservative): its multi-bound (AABB, or OBB: the box around it for either
		/// reading of the rotation), else the node's bounding sphere. Empty = no bound at all (a portal without geometry): ignored.
		struct RoomBox
		{
			const RE::NiNode* node = nullptr;
			double lo[3]{}, hi[3]{};
			enum : std::uint8_t
			{
				kInvalid = 0,
				kValid,
				kEmpty
			} state = kInvalid;
		};

		bool IsKindOf(const RE::NiRTTI* a_rtti, const RE::NiRTTI* a_base)
		{
			for (int depth = 0; a_rtti && a_base && depth < 16; ++depth, a_rtti = a_rtti->GetBaseRTTI())
				if (a_rtti == a_base)
					return true;
			return false;
		}

		__declspec(noinline) void RoomBoxUnsafe(RE::NiNode* a_node, RoomBox& o)
		{
			static const auto* multiBoundNodeRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundNode }.get();
			static const auto* aabbRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundAABB }.get();
			static const auto* obbRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundOBB }.get();
			double center[3]{}, extent[3]{};
			bool have = false;
			if (IsKindOf(a_node->GetRTTI(), multiBoundNodeRtti)) {
				auto* multiBound = static_cast<RE::BSMultiBoundNode*>(a_node)->GetRuntimeData().multiBound.get();
				auto* shape = multiBound ? multiBound->data.get() : nullptr;
				const auto* rtti = shape ? shape->GetRTTI() : nullptr;
				if (rtti && (rtti == aabbRtti || rtti == obbRtti)) {
					const auto* box = static_cast<const RE::BSMultiBoundAABB*>(shape);
					const double size[3] = { std::abs(box->size.x), std::abs(box->size.y), std::abs(box->size.z) };
					center[0] = box->center.x;
					center[1] = box->center.y;
					center[2] = box->center.z;
					if (rtti == obbRtti) {
						const auto& m = static_cast<const RE::BSMultiBoundOBB*>(shape)->rotation;
						for (int i = 0; i < 3; ++i) {
							double rows = 0.0, cols = 0.0;
							for (int j = 0; j < 3; ++j) {
								rows += std::abs(static_cast<double>(m.entry[i][j])) * size[j];
								cols += std::abs(static_cast<double>(m.entry[j][i])) * size[j];
							}
							extent[i] = std::max(rows, cols);
						}
					} else {
						std::copy_n(size, 3, extent);
					}
					have = true;
				}
			}
			if (!have) {
				const auto& bound = a_node->worldBound;
				if (!(bound.radius > 0.0f)) {
					o.state = std::isfinite(bound.radius) ? RoomBox::kEmpty : RoomBox::kInvalid;
					return;
				}
				center[0] = bound.center.x;
				center[1] = bound.center.y;
				center[2] = bound.center.z;
				extent[0] = extent[1] = extent[2] = bound.radius;
			}
			for (int i = 0; i < 3; ++i) {
				if (!std::isfinite(center[i]) || !std::isfinite(extent[i]) || std::abs(center[i]) > 1e7 || extent[i] > 65536.0)
					return;  // invalid
				o.lo[i] = center[i] - extent[i];
				o.hi[i] = center[i] + extent[i];
			}
			o.state = RoomBox::kValid;
		}

		bool ReadRoomBox(RE::NiNode* a_node, RoomBox* o_box)
		{
			__try {
				RoomBoxUnsafe(a_node, *o_box);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				o_box->state = RoomBox::kInvalid;
				return false;
			}
		}

		bool InBox(const double a_lo[3], const double a_hi[3], const double a_p[3], double a_margin)
		{
			for (int i = 0; i < 3; ++i)
				if (a_p[i] < a_lo[i] - a_margin || a_p[i] > a_hi[i] + a_margin)
					return false;
			return true;
		}

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
			o.portalStrict = a_light->portalStrict;          // (46x)
			o.portalGraph = a_light->portalGraph != nullptr;
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
			// (46s) Flicker moves the light's local translate around its rest point (a placed light's root node, a Light Placer node).
			const auto& l = niLight->local.translate;
			o.local[0] = l.x;
			o.local[1] = l.y;
			o.local[2] = l.z;
			if (const auto* parent = niLight->parent) {
				const auto& pp = parent->world.translate;
				o.parentPos[0] = pp.x;
				o.parentPos[1] = pp.y;
				o.parentPos[2] = pp.z;
				o.hasParent = std::isfinite(pp.x) && std::isfinite(pp.y) && std::isfinite(pp.z) && std::isfinite(l.x) && std::isfinite(l.y) && std::isfinite(l.z);
			}
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
			// (46s)
			Util::LightCensus::Category category;
			Util::LightCensus::Traits traits;
			void* hist;            // Selector::History of the light (null = none)
			bool steady;           // pos is the steady position
			bool meshless;         // a meshless placed light named like a fixture (Config::meshless applies)
			std::uint32_t anchor;  // Snap: index (sorted order) of the visible fixture it is tagged to (UINT32_MAX = none)
			// (46x)
			bool forced;           // its shadow map was not drawn (always a source)
			bool direct;           // a direct fill light (a source of its own)
			bool aimed;            // ... aimed at a window spot (aim = its world position)
			double aim[3];
			bool boxed;            // ranked only inside its rooms' box (lo / hi, world)
			bool dropped;          // left out by the portal rule
			double boxLo[3], boxHi[3];
		};

		/// (46s) Categories of static world lights (Steady Flickering Lights): the rest move with someone or are not traced.
		bool SteadyCategory(Util::LightCensus::Category a_category)
		{
			using C = Util::LightCensus::Category;
			return a_category == C::Fixture || a_category == C::Standalone || a_category == C::MeshAttached || a_category == C::FixtureShadow;
		}

		double DistSq(const double a[3], const double b[3])
		{
			const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
			return x * x + y * y + z * z;
		}

		struct Group
		{
			double pos[3]{};
			double weight = 0.0;
			float radius = 0.0f;
			// (46t) The reach of the source (coverage + the trace's ranking range): the radius, extended to cover every member's
			// sphere around the group centre (snapped lights; merged ones with Merge Hysteresis). The clearance keeps radius.
			float reach = 0.0f;
			float intensity = 0.0f;
			float score = 0.0f;
			const Candidate* strongest = nullptr;
			bool native = false;
			bool spot = false;  // (46c) never merged with anything
			std::uint8_t kind = 0;  // (46d) merged only with lights of the same kind
			std::uint32_t owner = 0;  // (46e) merged only with lights of the same carrier (or none)
			std::uint32_t first = 0;  // first member in the member list
			std::uint32_t slot = UINT32_MAX;
			// (46x) A direct fill (never merged), its aim; the rooms' box (every member boxed: the union; else none).
			bool direct = false;
			bool aimed = false;
			double aim[3]{};
			bool boxed = false;
			double boxLo[3]{}, boxHi[3]{};
		};

		/// (46x) Grows a group's room box by a member's (a member without one: the group has none).
		void JoinBox(Group& a_group, const Candidate& a_member)
		{
			if (!a_group.boxed || !a_member.boxed) {
				a_group.boxed = false;
				return;
			}
			for (int k = 0; k < 3; ++k) {
				a_group.boxLo[k] = std::min(a_group.boxLo[k], a_member.boxLo[k]);
				a_group.boxHi[k] = std::max(a_group.boxHi[k], a_member.boxHi[k]);
			}
		}

		/// (46x) Float to half (IEEE binary16), rounded toward -infinity (a_down) or +infinity, finite range (+-65504).
		std::uint16_t HalfDirected(float a_value, bool a_down)
		{
			const float v = std::clamp(a_value, -65504.0f, 65504.0f);
			std::uint16_t h = DirectX::PackedVector::XMConvertFloatToHalf(v);
			const float back = DirectX::PackedVector::XMConvertHalfToFloat(h);
			// One step outward when the rounding went the wrong way (|h| grows: +1 on the magnitude bits keeps the sign).
			const bool wrong = a_down ? back > v : back < v;
			if (wrong) {
				const bool negative = (h & 0x8000u) != 0;
				const bool magnitudeUp = a_down != negative ? false : true;  // down on a positive value shrinks the magnitude
				if ((h & 0x7FFFu) == 0) {
					h = a_down ? 0x8001u : 0x0001u;  // the smallest subnormal past zero
				} else if (magnitudeUp) {
					if ((h & 0x7FFFu) < 0x7BFFu)
						++h;
				} else {
					--h;
				}
			}
			return h;
		}

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

	const char* DecisionName(Decision a_decision)
	{
		static constexpr const char* kNames[] = { "none", "negative", "fill-ambient", "fill-direct-aimed", "fill-direct-point", "fill-fixture",
			"window-beam-wider", "portal-boxed", "portal-dropped" };
		static_assert(std::size(kNames) == static_cast<std::size_t>(Decision::Count));
		const auto i = static_cast<std::size_t>(a_decision);
		return i < std::size(kNames) ? kNames[i] : "none";
	}

	const char* DecisionWhy(Decision a_decision, std::uint8_t a_why)
	{
		switch (a_decision) {
		case Decision::Negative:
			return a_why == 0 ? "negative light (LIGH flag)" : (a_why == 1 ? "negative colour" : "negative fade");
		case Decision::FillAmbient:
			switch (a_why) {
			case 1:
				return "named / placed like direct light but dimmer than 1.0 before the fade";
			case 2:
				return "named / placed like direct light but tinted (brightest / dimmest channel > 1.3)";
			case 3:
				return "Trace Direct Fill Lights is off";
			default:
				return "bounce / omni / ambient fill: not named like direct light, no window spot above it";
			}
		case Decision::FillDirectAimed:
			return a_why == 0 ? "named like direct light; rays aimed at the window spot shadow light above it" :
			                    (a_why == 1 ? "on the axis of the window spot shadow light above it; rays aimed at that spot" :
			                                  "named like direct light and on the axis of the window spot above it; rays aimed at that spot");
		case Decision::FillDirectPoint:
			return "named like direct light (sun / shaft / beam / ray), no window spot above it: a soft point light";
		case Decision::FillFixture:
			return a_why == 0 ? "a fill only by its 'ambient' plugin, named like a fire / candle / torch: a fixture's light" :
			                    "a fill only by its 'ambient' plugin, within 128 units of a fixture light: that fixture's light";
		case Decision::WiderBeam:
			return "window spot of radius 4096 - 10000 (or a Window Shadows Ultimate spot by its plugin): a beam by the wider rule";
		case Decision::PortalBoxed:
			return a_why == 0 ? "portal-strict, its rooms hold the camera: ranked only inside its rooms' box" :
			                    "portal-strict behind a portal (its rooms do not hold the camera): ranked only inside its rooms' box";
		case Decision::PortalDropped:
			return a_why == 0 ? "portal-strict behind a portal, its rooms' box is off screen" : "portal-strict behind a portal (Per-Pixel Room Box off)";
		default:
			return "";
		}
	}

	void Selector::Clear()
	{
		slots_ = {};
		motion_.fill(255);
		slotBeams_.fill(nullptr);
		tags_.clear();
		maskEntries_.clear();
		history_.clear();    // (46g)
		reach_.clear();      // (46s)
		decisions_.clear();  // (46x)
		emitterSlots_ = 0;
		published_ = false;
	}

	std::uint16_t Selector::DecisionFor(const RE::BSLight* a_light) const
	{
		if (decisions_.empty() || !a_light)
			return 0;
		const auto it = std::lower_bound(decisions_.begin(), decisions_.end(), a_light, [](const auto& a_entry, const RE::BSLight* a_key) { return a_entry.first < a_key; });
		return (it != decisions_.end() && it->first == a_light) ? it->second : 0u;
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
		reach_.clear();  // (46s)
		decisions_.clear();  // (46x)
		emitterSlots_ = 0;
		stats_ = {};
		std::fill_n(o_sources, kMaxSources, GpuSource{});

		// (46s) Steady Flickering Lights: the light's rest point (its parent node) when its local offset is within the LIGH's flicker
		// movement (2 x amplitude + 1); else an average of its position (EMA 0.05 per frame, snapping on a jump over 128 units).
		const auto updateSteady = [a_frame](History& h, const LightRead& r, const Util::LightCensus::Traits& t) {
			const double limit = 2.0 * static_cast<double>(t.moveAmplitude) + 1.0;
			const double local2 = static_cast<double>(r.local[0]) * r.local[0] + static_cast<double>(r.local[1]) * r.local[1] +
			                      static_cast<double>(r.local[2]) * r.local[2];
			if (r.hasParent && local2 <= limit * limit) {
				for (int k = 0; k < 3; ++k)
					h.steady[k] = r.parentPos[k];
				h.steadyParent = true;
			} else {
				const double raw[3] = { r.pos[0], r.pos[1], r.pos[2] };
				if (!h.steadyValid || DistSq(raw, h.steady) > 128.0 * 128.0) {
					std::copy_n(raw, 3, h.steady);
				} else {
					for (int k = 0; k < 3; ++k)
						h.steady[k] += (raw[k] - h.steady[k]) * 0.05;
				}
				h.steadyParent = false;
			}
			h.steadyValid = true;
			h.steadyFrame = a_frame;
		};

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
		using C = Util::LightCensus::Category;
		// (46x) The census's view of the 46x rules: one decision per light (a later one replaces an earlier one).
		const auto decide = [this](const RE::BSLight* a_light, Decision a_decision, std::uint8_t a_why) {
			const auto code = static_cast<std::uint16_t>(static_cast<std::uint32_t>(a_decision) | (static_cast<std::uint32_t>(a_why) << 8));
			for (auto& e : decisions_) {
				if (e.first == a_light) {
					e.second = code;
					return;
				}
			}
			decisions_.emplace_back(a_light, code);
		};
		// (46x) This frame's window spot shadow lights (world positions): a direct fill under one aims its rays at it.
		struct WindowSpot
		{
			double pos[3];
		};
		static thread_local std::vector<WindowSpot> windowSpots;
		windowSpots.clear();
		const auto addWindowSpot = [](const LightRead& r) { windowSpots.push_back({ { r.pos[0], r.pos[1], r.pos[2] } }); };
		// (46x) Fill lights past the generic rules: decided after the loop, once the frame's fixtures and window spots are known.
		struct DeferredFill
		{
			RE::BSLight* light;
			LightRead read;
			double pos[3];
			History* hist;
			Util::LightCensus::Traits traits;
			float intensity;
			float intensityBeforeFade;
			bool wasTagged;
		};
		static thread_local std::vector<DeferredFill> fills;
		fills.clear();

		// A light that passed every rule becomes a candidate (46x: also the deferred fills). Null: none (Meshless Lights Near Fires: Skip).
		const auto addCandidate = [&](RE::BSLight* light, const LightRead& r, const double (&a_pos)[3], History* hist, C category, const Util::LightCensus::Traits& traits,
									  float intensity, float intensityBeforeFade, bool wasTagged, bool forced, bool native, bool spot, bool beam) -> Candidate* {
			double pos[3] = { a_pos[0], a_pos[1], a_pos[2] };
			Candidate c;
			c.light = light;
			c.read = r;
			c.intensity = intensity;
			c.native = native;
			c.spot = spot;
			c.beam = beam;
			c.kind = (forced || beam) ? 0 : (category == C::Player ? 1 : (category == C::Effect ? 2 : 0));
			c.category = category;
			c.traits = traits;
			c.hist = hist;
			c.steady = false;
			c.meshless = false;
			c.anchor = UINT32_MAX;
			c.forced = forced;  // (46x)
			c.direct = false;
			c.aimed = false;
			std::fill_n(c.aim, 3, 0.0);
			c.boxed = false;
			c.dropped = false;
			std::fill_n(c.boxLo, 3, 0.0);
			std::fill_n(c.boxHi, 3, 0.0);
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
			// (46s) Meshless placed lights named like a fixture (a vanilla "torch" light that is at no visible fire).
			c.meshless = traits.meshlessNamedFixture && !native && !forced && !spot && category == C::Fixture && c.kind == 0 && c.owner == 0;
			if (c.meshless && a_config.meshless == kMeshlessSkip) {
				++stats_.meshlessSkipped;
				++stats_.byReason[kRejectCategory];
				return nullptr;
			}
			// (46s) Steady Flickering Lights: static world lights take their steady position.
			if (a_config.steady && hist && !spot && !beam && c.kind == 0 && c.owner == 0 && SteadyCategory(category)) {
				updateSteady(*hist, r, traits);
				std::copy_n(hist->steady, 3, pos);
				c.steady = true;
				stats_.steadyParent += hist->steadyParent ? 1 : 0;
				stats_.steadyAverage += hist->steadyParent ? 0 : 1;
			} else if (hist) {
				hist->steadyValid = false;
			}
			std::copy_n(pos, 3, c.pos);
			// Screen influence: brightness x the light sphere's share of the view (capped where it fills it).
			const float dist = static_cast<float>(std::sqrt(DistSq(pos, a_camera)));
			const float cover = r.radius / std::max(dist, 1.0f);
			c.score = intensity * std::min(cover * cover, 4.0f);
			if (forced)
				c.score = FLT_MAX;  // never capped
			if (wasTagged && (r.radius < a_config.minRadius || intensityBeforeFade < a_config.minIntensity))
				++stats_.kept;
			candidates.push_back(c);
			stats_.native += native ? 1 : 0;
			stats_.forced += forced ? 1 : 0;
			return &candidates.back();
		};

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
			// (46x) Negative lights take light away: never a shadow source (their LIGH flag is tested with the category below).
			if (r.fade < 0.0f || r.color[0] < 0.0f || r.color[1] < 0.0f || r.color[2] < 0.0f) {
				++stats_.negative;
				++stats_.byReason[kRejectCategory];
				decide(light, Decision::Negative, r.fade < 0.0f ? 2 : 1);
				continue;
			}
			// (46c) An engine shadow light whose shadow map was not drawn this frame: always a source (no other rule).
			const bool forced = a_config.forced && r.shadow &&
			                    std::binary_search(a_config.forced->begin(), a_config.forced->end(), static_cast<const RE::BSLight*>(light));
			// (46c) Spot shadow lights: only indoors and only with a readable cone (outdoors they keep the engine's).
			const bool spot = r.shadow && r.spot;
			if (spot && a_config.interior)
				LogSpot(light, r, r.radius > a_config.beamRadius);
			if (spot && (!a_config.interior || !r.spotValid)) {
				stats_.spotsInvalid += a_config.interior ? 1 : 0;
				++stats_.byReason[kRejectCategory];
				continue;
			}
			// (46c) Window beams (Window Shadows Ultimate): huge spots outside the windows. Classified first: the
			// radius / distance / area rules below do not apply to them.
			Util::LightCensus::Category category = Util::LightCensus::Category::Unknown;
			Util::LightCensus::Traits traits;
			bool classified = false;
			bool beam = false;
			if (spot && r.radius > std::min(a_config.maxRadius, a_config.beamRadius)) {
				if (!Util::LightCensus::ClassifyCached(light, r.radius, a_frame, classifyBudget, category, &traits)) {
					++stats_.pending;
					continue;
				}
				classified = true;
				beam = category == C::WindowBeam;
				// (46x) A window spot shadow light: the aim of the direct fill under it.
				if (a_config.directFills && (beam || category == C::WindowFill || category == C::WindowGlow || traits.wsu))
					addWindowSpot(r);
				if (!beam || !a_config.beams) {
					++stats_.byReason[kRejectCategory];
					continue;
				}
				if (a_config.excludedBeams && std::binary_search(a_config.excludedBeams->begin(), a_config.excludedBeams->end(), static_cast<const RE::BSLight*>(light))) {
					++stats_.byReason[kRejectCategory];  // (46g) implausible: the engine's shadow map stays
					continue;
				}
				// (46x) A beam only by the wider rule (46w: radius over 10000 only).
				if (r.radius <= 10000.0f) {
					++stats_.beamsWider;
					decide(light, Decision::WiderBeam, 0);
				}
			} else if (spot && a_config.directFills) {
				// (46x) The other spot shadow lights indoors: the window ones are aims of direct fills (classified now; cached).
				if (Util::LightCensus::ClassifyCached(light, r.radius, a_frame, classifyBudget, category, &traits)) {
					classified = true;
					if (category == C::WindowBeam || category == C::WindowFill || category == C::WindowGlow || traits.wsu)
						addWindowSpot(r);
				}
			}
			// (46g) Stable selection: the smoothed intensity / radius decide (flickering candles, Inverse Square Lighting's radius
			// following the fade), with hysteresis for a light that was tagged last frame.
			const float rawIntensity = std::max({ r.color[0], r.color[1], r.color[2] }) * r.fade;
			float smoothIntensity = rawIntensity;
			bool wasTagged = false;
			// (46s) The per-light history serves the stable selection, the steady position, the merge hysteresis and the snap.
			History* hist = nullptr;
			if (!beam && (a_config.stable || a_config.steady || a_config.mergeHysteresis || a_config.meshless == kMeshlessSnap || a_config.directFills)) {
				hist = &history_[light];
				if (hist->niLight != r.niLight) {
					*hist = History{};  // a new light at the address of an old one
					hist->niLight = r.niLight;
				}
				hist->seen = a_frame;
			}
			if (a_config.stable && hist) {
				auto& h = *hist;
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
			// (46s) The position the rules test: last frame's steady position while there is one (this frame's comes with the category).
			double pos[3] = { r.pos[0], r.pos[1], r.pos[2] };
			if (a_config.steady && hist && hist->steadyValid && a_frame - hist->steadyFrame <= 2u)
				std::copy_n(hist->steady, 3, pos);
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
			const double dx = pos[0] - a_camera[0], dy = pos[1] - a_camera[1], dz = pos[2] - a_camera[2];
			const double distSq = dx * dx + dy * dy + dz * dz;
			if (limits && distSq > maxDistSq) {
				++stats_.byReason[kRejectDistance];
				continue;
			}
			if (limits && (pos[0] < a_rect[0] || pos[1] < a_rect[1] || pos[0] > a_rect[2] || pos[1] > a_rect[3])) {
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
			if (!classified && !Util::LightCensus::ClassifyCached(light, r.radius, a_frame, classifyBudget, category, &traits)) {
				if (!forced) {
					++stats_.pending;
					continue;
				}
				category = Util::LightCensus::Category::Unknown;
			}
			// (46x) A negative LIGH: never traced.
			if (traits.negative && !forced) {
				++stats_.negative;
				++stats_.byReason[kRejectCategory];
				decide(light, Decision::Negative, 0);
				continue;
			}
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
				// (46x) Fill lights: decided after the loop (direct light, a fixture's light, or left as they are).
				if (category == C::Fill && !native && !forced) {
					if (a_config.directFills || (a_config.pluginFillFixtures && traits.fillPluginOnly)) {
						fills.push_back({ light, r, { pos[0], pos[1], pos[2] }, hist, traits, intensity, intensityBeforeFade, wasTagged });
						continue;
					}
					++stats_.fillAmbient;
					decide(light, Decision::FillAmbient, 3);
				}
				++stats_.byReason[kRejectCategory];
				continue;
			}
			addCandidate(light, r, pos, hist, category, traits, intensity, intensityBeforeFade, wasTagged, forced, native, spot, beam);
		}

		// (46x) Fill lights, in the order of the rules: (b) a fill only by its "ambient" plugin, named like a fire / candle / torch or
		// within 128 units of a fixture light, is that fixture's light (the meshless rules); (c) a fill named like direct light or on
		// the axis of a window spot shadow light, bright and near-neutral, is direct light: traced (aimed at the spot when there is
		// one); (d) every other fill stays untraced.
		for (auto& f : fills) {
			auto traits = f.traits;
			if (a_config.pluginFillFixtures && traits.fillPluginOnly) {
				bool fixture = traits.fillFixtureWord;
				std::uint8_t why = 0;
				if (!fixture) {
					for (const auto& c : candidates) {
						const bool fixtureLight = c.kind == 0 && c.owner == 0 && !c.spot && !c.beam && !c.direct && !c.meshless &&
						                          (c.category == C::Fixture || c.category == C::FixtureShadow || c.traits.visibleFixture);
						if (fixtureLight && DistSq(c.pos, f.pos) <= 128.0 * 128.0) {
							fixture = true;
							why = 1;
							break;
						}
					}
				}
				if (fixture) {
					traits.meshlessNamedFixture = true;
					traits.nearMeshDistance = -1.0f;
					if (addCandidate(f.light, f.read, f.pos, f.hist, C::Fixture, traits, f.intensity, f.intensityBeforeFade, f.wasTagged, false, false, false, false))
						++stats_.fillFixture;
					decide(f.light, Decision::FillFixture, why);
					continue;
				}
			}
			std::uint8_t ambientWhy = a_config.directFills ? 0 : 3;
			if (a_config.directFills) {
				// The window spot above it: within 128 units horizontally, at least 64 units higher (the nearest horizontally).
				const WindowSpot* spot = nullptr;
				double bestSq = 128.0 * 128.0;
				for (const auto& s : windowSpots) {
					const double ex = s.pos[0] - f.pos[0], ey = s.pos[1] - f.pos[1];
					const double h2 = ex * ex + ey * ey;
					if (s.pos[2] > f.pos[2] + 64.0 && h2 <= bestSq) {
						bestSq = h2;
						spot = &s;
					}
				}
				double aim[3]{};
				bool paired = false;
				if (spot) {
					std::copy_n(spot->pos, 3, aim);
					paired = true;
					if (f.hist) {
						std::copy_n(spot->pos, 3, f.hist->aim);
						f.hist->aimFrame = a_frame;
					}
				} else if (f.hist && f.hist->aimFrame && a_frame - f.hist->aimFrame <= 120u) {
					std::copy_n(f.hist->aim, 3, aim);  // its spot is not in this frame's lights: the last one seen
					paired = true;
				}
				if (traits.fillDirectName || paired) {
					const float mx = std::max({ f.read.color[0], f.read.color[1], f.read.color[2] });
					const float mn = std::min({ f.read.color[0], f.read.color[1], f.read.color[2] });
					if (f.intensityBeforeFade < 1.0f) {
						ambientWhy = 1;
					} else if (!(mn > 1e-4f) || mx > mn * 1.3f) {
						ambientWhy = 2;
					} else if (auto* c = addCandidate(f.light, f.read, f.pos, f.hist, C::Fill, traits, f.intensity, f.intensityBeforeFade, f.wasTagged, false, false, false,
								   false)) {
						c->direct = true;
						c->aimed = paired;
						std::copy_n(aim, 3, c->aim);
						if (paired) {
							++stats_.fillDirectAimed;
							decide(f.light, Decision::FillDirectAimed, traits.fillDirectName ? 2 : 1);
						} else {
							++stats_.fillDirectPoint;
							decide(f.light, Decision::FillDirectPoint, 0);
						}
						continue;
					}
				}
			}
			++stats_.fillAmbient;
			++stats_.byReason[kRejectCategory];
			decide(f.light, Decision::FillAmbient, ambientWhy);
		}

		// (46x) Skip Lights Behind Portals: Light Limit Fix lights a portal-strict light's pixels only on the geometry of its rooms (and
		// portals). Each such light's rooms' box (world, conservative); a light whose rooms do not hold the camera is behind a portal.
		// Only when its rooms read well (every room box valid, the light inside them) and the camera is in some known room.
		if (a_config.portals && a_config.interior && !candidates.empty()) {
			static thread_local std::vector<RoomBox> roomBoxes;
			roomBoxes.clear();
			const auto boxOf = [](RE::NiNode* a_node) -> RoomBox {
				for (const auto& b : roomBoxes)
					if (b.node == a_node)
						return b;
				RoomBox b;
				b.node = a_node;
				ReadRoomBox(a_node, &b);
				if (roomBoxes.size() < 512)
					roomBoxes.push_back(b);
				return b;
			};
			struct PortalState
			{
				bool rooms = false;     // portal-strict with rooms / portals (Light Limit Fix's room rule applies)
				bool readable = false;  // every room box valid, the light inside them
				bool reaches = false;   // one of its rooms holds the camera
				double lo[3]{}, hi[3]{};
			};
			static thread_local std::vector<PortalState> states;
			states.assign(candidates.size(), PortalState{});
			bool cameraInRoom = false;
			for (std::size_t i = 0; i < candidates.size(); ++i) {
				const auto& c = candidates[i];
				if (c.forced || c.spot || c.beam || !c.read.portalStrict || !c.read.portalGraph)
					continue;
				RoomRead rooms;
				const bool read = ReadRooms(c.light, &rooms);
				if (read && rooms.count == 0)
					continue;  // no rooms: not limited by Light Limit Fix's room rule here
				auto& s = states[i];
				s.rooms = true;
				s.readable = read && !rooms.overflow;
				bool any = false;
				for (std::uint32_t k = 0; k < rooms.count && s.readable; ++k) {
					const RoomBox b = boxOf(rooms.nodes[k]);
					if (b.state == RoomBox::kEmpty)
						continue;
					if (b.state != RoomBox::kValid) {
						s.readable = false;
						break;
					}
					for (int a = 0; a < 3; ++a) {
						s.lo[a] = any ? std::min(s.lo[a], b.lo[a]) : b.lo[a];
						s.hi[a] = any ? std::max(s.hi[a], b.hi[a]) : b.hi[a];
					}
					any = true;
					if (InBox(b.lo, b.hi, a_camera, 32.0))
						s.reaches = true;
				}
				const double lightPos[3] = { c.read.pos[0], c.read.pos[1], c.read.pos[2] };
				if (s.readable && (!any || !InBox(s.lo, s.hi, lightPos, 64.0)))
					s.readable = false;
				if (s.readable && s.reaches)
					cameraInRoom = true;
			}
			stats_.cameraInRoom = cameraInRoom;
			if (cameraInRoom) {
				for (std::size_t i = 0; i < candidates.size(); ++i) {
					auto& c = candidates[i];
					const auto& s = states[i];
					if (!s.rooms)
						continue;
					if (!s.readable) {
						++stats_.portalBadRooms;
						continue;
					}
					++stats_.portalStrict;
					if (!s.reaches) {
						++stats_.portalAway;
						bool drop = !a_config.roomBoxes;
						std::uint8_t why = 1;
						if (!drop) {
							// Behind a portal and its rooms' box wholly outside the view: no source.
							for (const auto& p : planes) {
								double d = p[3];
								for (int a = 0; a < 3; ++a)
									d += static_cast<double>(p[a]) * ((p[a] >= 0.0f ? s.hi[a] : s.lo[a]) - a_camera[a]);
								if (d < 0.0) {
									drop = true;
									why = 0;
									break;
								}
							}
						}
						if (drop) {
							c.dropped = true;
							++stats_.portalDropped;
							stats_.native -= c.native ? 1 : 0;
							decide(c.light, Decision::PortalDropped, why);
							continue;
						}
					}
					if (a_config.roomBoxes && !c.direct) {
						c.boxed = true;
						std::copy_n(s.lo, 3, c.boxLo);
						std::copy_n(s.hi, 3, c.boxHi);
						++stats_.portalBoxed;
						decide(c.light, Decision::PortalBoxed, s.reaches ? 0 : 1);
					}
				}
				std::erase_if(candidates, [](const Candidate& a_candidate) { return a_candidate.dropped; });
			}
		}
		std::sort(decisions_.begin(), decisions_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		stats_.candidates = static_cast<std::uint32_t>(candidates.size());
		Util::LightCensus::TrimClassifierCache(a_frame, 600);

		// Merge: brightest first; a light within the merge distance of a group's centre joins it.
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
			return a.intensity > b.intensity || (a.intensity == b.intensity && a.light < b.light);  // (46g) a stable seed order
		});
		const double mergeSq = static_cast<double>(a_config.mergeDistance) * a_config.mergeDistance;
		memberGroup.assign(candidates.size(), UINT32_MAX);

		// (46s) Snap: a meshless light named like a fixture takes the source of the nearest visible fixture (Light Placer, a light in a
		// fixture's model, a LIGH with a mesh) within the snap distance; last frame's choice is kept while it stays within 1.25 x.
		if (a_config.meshless == kMeshlessSnap) {
			const double snapSq = static_cast<double>(a_config.snapDistance) * a_config.snapDistance;
			const double keepSq = snapSq * 1.5625;
			const auto anchorable = [](const Candidate& a) {
				return a.traits.visibleFixture && !a.meshless && a.kind == 0 && a.owner == 0 && !a.spot && !a.beam && !a.direct;
			};
			for (std::uint32_t i = 0; i < candidates.size(); ++i) {
				auto& c = candidates[i];
				if (!c.meshless)
					continue;
				// A fixture mesh (a sconce, a candle) clearly nearer than any lit fixture: the light is that mesh's own, it keeps its
				// source. The snap only reaches as far as the nearest fixture mesh + 64 units (the flame sits above the mesh's origin).
				double limitSq = snapSq, keepLimitSq = keepSq;
				if (c.traits.nearMeshDistance >= 0.0f) {
					const double reach = static_cast<double>(c.traits.nearMeshDistance) + 64.0;
					limitSq = std::min(limitSq, reach * reach);
					keepLimitSq = std::min(keepLimitSq, reach * reach * 1.5625);
				}
				auto* h = static_cast<History*>(c.hist);
				std::uint32_t best = UINT32_MAX;
				if (h && h->anchor && a_frame - h->anchorFrame <= 30u) {
					for (std::uint32_t j = 0; j < candidates.size(); ++j) {
						if (candidates[j].light != h->anchor)
							continue;
						if (anchorable(candidates[j]) && DistSq(candidates[j].pos, c.pos) <= keepLimitSq)
							best = j;
						break;
					}
				}
				if (best == UINT32_MAX) {
					double bestSq = limitSq;
					for (std::uint32_t j = 0; j < candidates.size(); ++j) {
						if (j == i || !anchorable(candidates[j]))
							continue;
						const double d = DistSq(candidates[j].pos, c.pos);
						if (d < bestSq || (d == bestSq && best != UINT32_MAX && candidates[j].light < candidates[best].light)) {
							bestSq = d;
							best = j;
						}
					}
				}
				c.anchor = best;
				if (h) {
					h->anchor = best != UINT32_MAX ? candidates[best].light : nullptr;
					h->anchorFrame = a_frame;
				}
			}
		}

		const double keepMergeSq = mergeSq * 2.25;  // (46s) merge hysteresis: 1.5 x the merge distance
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			const auto& c = candidates[i];
			if (c.anchor != UINT32_MAX)
				continue;  // (46s) snapped: after the groups exist
			std::uint32_t found = UINT32_MAX;
			// (46s) Merge hysteresis: last frame's group (the same seed light) while within 1.5 x the merge distance.
			if (a_config.mergeHysteresis && !c.spot && !c.direct) {
				const auto* h = static_cast<const History*>(c.hist);
				if (h && h->seed && a_frame - h->seedFrame <= 2u) {
					for (std::uint32_t g = 0; g < groups.size(); ++g) {
						const auto& group = groups[g];
						if (!group.strongest || group.strongest->light != h->seed)
							continue;
						if (!group.spot && !group.direct && group.kind == c.kind && group.owner == c.owner && DistSq(group.pos, c.pos) <= keepMergeSq) {
							found = g;
							stats_.mergeKept += DistSq(group.pos, c.pos) > mergeSq ? 1 : 0;
						}
						break;
					}
				}
			}
			// (46c) Spot lights keep a source of their own (their cone and direction are theirs). (46x) Direct fills too (their aim).
			for (std::uint32_t g = 0; g < groups.size() && !c.spot && !c.direct && found == UINT32_MAX; ++g) {
				const auto& group = groups[g];
				if (group.spot || group.direct || group.kind != c.kind || group.owner != c.owner)
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
				group.direct = c.direct;  // (46x)
				group.aimed = c.aimed;
				std::copy_n(c.aim, 3, group.aim);
				group.boxed = c.boxed;
				std::copy_n(c.boxLo, 3, group.boxLo);
				std::copy_n(c.boxHi, 3, group.boxHi);
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
				JoinBox(group, c);  // (46x)
				memberGroup[i] = found;
				++stats_.merged;
			}
		}
		// (46s) Snapped lights join their fixture's group: their intensity counts, the centre stays the fixture's.
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			const auto& c = candidates[i];
			if (c.anchor == UINT32_MAX)
				continue;
			const std::uint32_t g = memberGroup[c.anchor];
			if (g == UINT32_MAX)
				continue;  // cannot happen (an anchor is never snapped itself)
			groups[g].intensity += c.intensity;
			groups[g].score += c.score;
			JoinBox(groups[g], c);  // (46x)
			memberGroup[i] = g;
			++stats_.snapped;
		}
		// (46t) Reach, once the centres are final: a snapped light (up to the snap distance away, its own radius) lit pixels
		// outside its fixture's sphere, which then got no traced shadow -- the shadow ended in a hard line at the fixture's
		// radius. Each snapped member, and with Merge Hysteresis each merged member (the centre moves off the seed), widens
		// the source's reach to cover its sphere. All 46s switches off: reach = radius (46r).
		for (auto& group : groups)
			group.reach = group.radius;
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			const std::uint32_t g = memberGroup[i];
			if (g == UINT32_MAX)
				continue;
			const auto& c = candidates[i];
			auto& group = groups[g];
			if (group.spot || !(c.anchor != UINT32_MAX || a_config.mergeHysteresis))
				continue;
			const float cover = static_cast<float>(std::sqrt(DistSq(group.pos, c.pos))) + c.read.radius;
			if (std::isfinite(cover))
				group.reach = std::max(group.reach, cover);
		}
		for (std::uint32_t i = 0; i < candidates.size(); ++i) {
			const auto& c = candidates[i];
			if (c.meshless && c.anchor == UINT32_MAX)
				++stats_.meshlessOwn;
			// (46s) Merge hysteresis: the seed of the group each light joined (none for seeds and snapped lights).
			if (auto* h = static_cast<History*>(c.hist)) {
				const std::uint32_t g = memberGroup[i];
				const bool joined = g != UINT32_MAX && c.anchor == UINT32_MAX && groups[g].strongest && groups[g].strongest != &c;
				h->seed = joined ? groups[g].strongest->light : nullptr;
				h->seedFrame = a_frame;
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
		// (46y) Non-RT casters mode: SSPLS for the occluders the trace does not see, its contact length in bits 16-23.
		const std::uint32_t contactBits = a_config.ssplsNonRT ? static_cast<std::uint32_t>(std::lround(std::clamp(std::isfinite(a_config.ssplsContact) ? a_config.ssplsContact : 0.0f, 0.0f, 31.875f) * 8.0f)) : 0u;
		const std::uint32_t tagFlags = (a_config.ssplsNonRT ? (kTagSSPLSNonRT | (contactBits << kTagContactShift)) : (a_config.keepSSPLS ? kTagKeepSSPLS : 0u)) |
		                               (a_config.split ? kTagSplit : 0u) | (a_config.waterBed ? kTagWaterBed : 0u);
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
			// (46t) The reach: the trace's per-pixel source test and its ranking range (the shading of the engine's lights is
			// their own; the trace only picks which sources to shadow-test at a pixel and returns a visibility mask).
			out.posRadius[3] = group.reach;
			out.atten[0] = 1.0f / std::max(group.reach, 1.0f);
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
			} else if (group.direct) {
				// (46x) A direct fill: aimed at its window spot (beam-like rays, no clearance: the spot is outside), or a larger soft light.
				if (group.aimed) {
					out.info[0] |= kSourceAim;
					for (int k = 0; k < 3; ++k)
						out.spot[k] = static_cast<float>(group.aim[k] - a_camera[k]);
					clearance = 0.0f;
				} else {
					out.info[0] |= kSourceEmitter;
					out.spot[3] = std::clamp(a_config.directEmitter, 0.0f, 64.0f);
					emitterSlots_ |= 1ull << group.slot;
				}
			} else if (group.boxed) {
				// (46x) Ranked only inside its rooms' box (camera-relative, half floats rounded outward, 48 units more on each side).
				std::uint16_t h[6];
				for (int k = 0; k < 3; ++k) {
					h[k] = HalfDirected(static_cast<float>(group.boxLo[k] - a_camera[k] - 48.0), true);
					h[3 + k] = HalfDirected(static_cast<float>(group.boxHi[k] - a_camera[k] + 48.0), false);
				}
				const std::uint32_t packed[3] = { h[0] | (static_cast<std::uint32_t>(h[1]) << 16), h[2] | (static_cast<std::uint32_t>(h[3]) << 16),
					h[4] | (static_cast<std::uint32_t>(h[5]) << 16) };
				std::memcpy(out.spot, packed, sizeof(packed));
				out.info[0] |= kSourceRoomBox;
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
		// (46s) Diagnostics: the sources whose range covers the player.
		if (a_config.reachPoint) {
			const double* p = a_config.reachPoint;
			for (const std::uint32_t g : order) {
				const auto& group = groups[g];
				if (group.slot == UINT32_MAX || !group.strongest)
					continue;
				const double d = std::sqrt(DistSq(group.pos, p));
				if (!(d <= group.reach))  // (46t) the reach (radius before)
					continue;
				const auto& s = *group.strongest;
				Reach e;
				e.slot = group.slot;
				e.kind = group.kind;
				e.category = static_cast<std::uint8_t>(s.category);
				e.native = group.native;
				e.spot = group.spot;
				e.steady = s.steady;
				e.distance = static_cast<float>(d);
				e.radius = group.reach;  // (46t)
				e.intensity = group.intensity;
				std::memcpy(e.label, s.traits.label, sizeof(e.label));
				e.label[sizeof(e.label) - 1] = '\0';
				for (std::uint32_t i = 0; i < candidates.size(); ++i) {
					if (memberGroup[i] != g)
						continue;
					++e.members;
					e.snapped += candidates[i].anchor != UINT32_MAX ? 1 : 0;
				}
				reach_.push_back(e);
			}
			std::sort(reach_.begin(), reach_.end(), [](const Reach& a, const Reach& b) { return a.distance < b.distance || (a.distance == b.distance && a.slot < b.slot); });
			if (reach_.size() > 8)
				reach_.resize(8);
		}
		// (46g) Lights not seen for a while leave the history. (46s) By the last frame any rule looked at them.
		if (a_frame % 300u == 0u)
			std::erase_if(history_, [&](const auto& a_entry) { return a_frame - a_entry.second.seen > 600u; });
		if (a_config.forced) {
			for (const auto* light : *a_config.forced)
				if (std::none_of(maskEntries_.begin(), maskEntries_.end(), [&](const MaskEntry& a_entry) { return a_entry.light == light; }))
					++stats_.forcedMissing;
		}
		stats_.cpuMs = static_cast<float>(QpcToMs(Qpc() - start));
		return scan;
	}
}
