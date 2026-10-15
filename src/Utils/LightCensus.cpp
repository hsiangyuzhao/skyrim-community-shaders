#include "Utils/LightCensus.h"

#include "BuildLabel.h"
#include "Features/InteriorSun.h"
#include "Features/InverseSquareLighting.h"
#include "Features/InverseSquareLighting/Common.h"
#include "Features/LightLimitFix.h"
#include "Features/RayTracing.h"
#include "Features/ScreenSpacePointLightShadows.h"
#include "Menu.h"
#include "Utils/FrameTrace.h"
#include "Utils/GpuPhaseTimeline.h"
#include "Utils/GpuTimers.h"

#include <thread>

namespace Util::LightCensus
{
	namespace
	{
		// ---- tuning --------------------------------------------------------------------------------
		// (46a) No radius rule for fill lights any more: True Light's campfire ISL lights are ~540 units.
		constexpr float kWindowBeamRadius = 10000.0f;   ///< (46a) a window spot this big = sun beam (Window Shadows Ultimate)
		constexpr float kWindowGlowRadius = 128.0f;     ///< (46a) a window light this small = the glow of a lit pane
		constexpr float kStableIdLocalGrid = 16.0f;     ///< (46a) local offset grid of the stable ID (flicker moves lights)
		constexpr double kStaleSampleMs = 1000.0;       ///< (46a) an engine-pass GPU sample older than this is not used
		constexpr float kFixtureNearDistance = 200.0f;  ///< a placed light this close to a candle / torch / ... mesh belongs to it
		constexpr float kFixtureScanRadius = 8192.0f;   ///< references searched for fixture meshes around the player
		constexpr std::size_t kMaxFixtureRefs = 8192;
		constexpr int kPixelStride = 4;          ///< depth samples every 4th pixel in x and y
		constexpr int kBlockingAfterFrames = 6;  ///< then the read-back waits for the GPU
		constexpr int kTraceRing = 4;
		constexpr int kChainDepth = 10;
		constexpr int kEmissiveVisitLimit = 96;

		constexpr std::string_view kFixtureWords[] = { "candle", "torch", "brazier", "lantern", "sconce", "chandelier", "campfire", "firepit", "fireplace",
			"hearth", "fire", "flame", "lamp", "forge", "smelter", "embers", "cookpot" };
		constexpr std::string_view kFillWords[] = { "fill", "ambient", "ambi", "bounce" };
		constexpr std::string_view kWindowWords[] = { "window", "sunbeam", "shaft", "godray", "lightray", "lightbeam" };

		// ---- records ---------------------------------------------------------------------------------
		struct LightRecord
		{
			int bufferIndex = -1;  ///< index in LLF's light buffer, -1 = skipped
			DropReason dropped = DropReason::None;
			bool readError = false;

			std::string niType, kind;
			bool shadow = false;
			int shadowIndex = -1;
			int shadowMaps = -1;
			bool pointLight = false, ambientLight = false, dynamic = false, portalStrict = false, affectLand = false, affectWater = false, neverFades = false;
			bool hidden = false, portalGraph = false;
			int rooms = 0, portals = 0;
			float luminance = 0, lodDimmer = 0;
			float rawDiffuse[3]{}, rawRadius = 0, rawFade = 0;
			// as LLF uploads it (ISL applied)
			float color[3]{}, radius = 0, fade = 0;
			// (46a) Brightest channel x fade before and after lodDimmer (the distance / room fade).
			float intensityBeforeFade = 0, intensityAfterFade = 0;
			uint32_t llfFlags = 0;
			bool isl = false, islLinear = false, islInitialised = false, islDisabled = false;
			float islSize = 0, islCutoff = 0;
			uint32_t islLighId = 0;
			float pos[3]{}, local[3]{}, distance = 0;

			std::vector<std::string> chain;  ///< light first, then its parents up to the owning reference's root
			std::string objectNode;
			bool playerNode = false;

			uint32_t refId = 0, baseId = 0;
			std::string refEdid, refPlugin, refLastPlugin, baseType, baseEdid, basePlugin, baseModel;
			bool refDisabled = false, isActor = false, isPlayer = false;
			bool baseIsLight = false;

			uint32_t lighId = 0, lighFlags = 0;
			std::string lighEdid, lighPlugin, lighModel;
			float lighRadius = 0, lighFade = 0, lighFov = 0;
			float lighMoveAmplitude = 0;  ///< (46s) flicker movement amplitude (0 when the LIGH does not flicker / pulse)

			bool emissiveNearby = false;
			std::string emissiveShape;
			bool lightPlacer = false;
			std::string lightPlacerNode;

			std::string fixtureWord, nearFixture;
			float nearFixtureDistance = -1;

			std::string category;
			std::vector<std::string> reasons;
			uint64_t stableId = 0;
			int duplicate = 0;

			uint32_t clusters = 0;
			uint64_t pixelHits = 0;

			uint32_t rtTag = 0;  ///< (46s) Ray Tracing's point-light shadow tag this frame (source slot + 1 in the low bits; 0 = none)
		};

		struct FixtureRef
		{
			RE::NiPoint3 pos;
			std::string label;
			std::string word;
		};

		struct FillContext
		{
			RE::TESObjectREFR* player = nullptr;
			RE::NiAVObject* player3D[2]{};
			RE::NiPoint3 camera;
		};

		struct Capture
		{
			std::vector<LightRecord> lights;
			nlohmann::json scene, cost, distribution;
			std::string tag;
			int shadowListSize = 0;
			int fixtureRefs = 0;
			double cpuMs = 0;

			// GPU read-back
			winrt::com_ptr<ID3D11Buffer> grid, indexList, counter;
			winrt::com_ptr<ID3D11Texture2D> depth;
			DXGI_FORMAT depthViewFormat = DXGI_FORMAT_UNKNOWN;
			uint32_t gridCount = 0, indexCapacity = 0;
			uint32_t clusterSize[3]{};
			float renderW = 0, renderH = 0;
			float4 cameraData{};
			uint32_t lightCount = 0;
			bool copied = false;
			int framesWaited = 0;
			std::string gpuNote;
		};

		std::atomic<bool> g_requested{ false };
		std::atomic<bool> g_busy{ false };
		std::unique_ptr<Capture> g_capture;  // render thread only
		bool g_captureThisFrame = false;
		std::mutex g_statusMutex;
		std::string g_status;

		// F7 trace ring
		struct TraceSlot
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			bool pending = false;
			uint64_t frame = 0;
			uint32_t count = 0;
			uint32_t sizeX = 0, sizeY = 0, sizeZ = 0;
		};
		TraceSlot g_ring[kTraceRing];
		uint32_t g_ringBytes = 0;
		int g_ringWrite = 0;
		uint64_t g_frameCounter = 0;
		FrameTrace::LightStats g_traceStats;
		int g_lastMaxCluster = -1;
		float g_lastOver4 = -1.0f;

		void SetStatus(std::string a_text)
		{
			std::scoped_lock lk(g_statusMutex);
			g_status = std::move(a_text);
		}

		std::string Lower(std::string_view a_text)
		{
			std::string s(a_text);
			for (char& c : s)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		template <std::size_t N>
		std::string_view FindWord(const std::string& a_lower, const std::string_view (&a_words)[N])
		{
			for (const auto w : a_words)
				if (a_lower.find(w) != std::string::npos)
					return w;
			return {};
		}

		std::string PluginOf(const RE::TESForm* a_form)
		{
			if (!a_form)
				return {};
			if ((a_form->GetFormID() >> 24) == 0xFF)
				return "(runtime)";
			const auto* file = a_form->GetFile(0);
			return file ? std::string(file->GetFilename()) : std::string();
		}

		std::string LastPluginOf(const RE::TESForm* a_form)
		{
			if (!a_form || (a_form->GetFormID() >> 24) == 0xFF)
				return {};
			const auto* file = a_form->GetDescriptionOwnerFile();
			return file ? std::string(file->GetFilename()) : std::string();
		}

		std::string EdidOf(const RE::TESForm* a_form)
		{
			if (!a_form)
				return {};
			return clib_util::editorID::get_editorID(a_form);
		}

		const char* SafeName(const RE::NiObjectNET* a_object)
		{
			if (!a_object)
				return "";
			const char* n = a_object->name.c_str();
			return n ? n : "";
		}

		/// This node's own userData (CommonLib's GetUserData() falls back to the parents).
		RE::TESObjectREFR* NodeOwner(RE::NiAVObject* a_node)
		{
			return REL::RelocateMember<RE::TESObjectREFR*>(a_node, 0x0F8, 0x110);
		}

		uint64_t Fnv(uint64_t a_hash, const void* a_data, std::size_t a_bytes)
		{
			const auto* p = static_cast<const uint8_t*>(a_data);
			for (std::size_t i = 0; i < a_bytes; ++i) {
				a_hash ^= p[i];
				a_hash *= 1099511628211ull;
			}
			return a_hash;
		}

		bool IsEmissive(RE::BSGeometry* a_geometry)
		{
			auto* property = a_geometry->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect].get();
			if (!property)
				return false;
			if (netimmerse_cast<RE::BSEffectShaderProperty*>(property))
				return true;
			if (auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property)) {
				using F = RE::BSShaderProperty::EShaderPropertyFlag;
				return lighting->flags.any(F::kOwnEmit, F::kGlowMap, F::kExternalEmittance);
			}
			return false;
		}

		/// Looks for a glowing / effect shape among the siblings of the light (two levels up, one level down).
		void FindEmissive(RE::NiAVObject* a_light, LightRecord& a_record)
		{
			int visits = 0;
			RE::NiNode* anchor = a_light->parent;
			for (int level = 0; level < 2 && anchor && visits < kEmissiveVisitLimit; ++level, anchor = anchor->parent) {
				if (a_record.refId && NodeOwner(anchor) && level > 0)
					break;  // do not leave the reference's own model
				for (auto& child : anchor->GetChildren()) {
					auto* object = child.get();
					if (!object || object == a_light || ++visits > kEmissiveVisitLimit)
						continue;
					if (auto* geometry = object->AsGeometry()) {
						if (IsEmissive(geometry)) {
							a_record.emissiveNearby = true;
							a_record.emissiveShape = SafeName(geometry);
							return;
						}
					} else if (auto* node = object->AsNode()) {
						for (auto& grandChild : node->GetChildren()) {
							auto* g = grandChild.get();
							if (!g || ++visits > kEmissiveVisitLimit)
								continue;
							if (auto* inner = g->AsGeometry(); inner && IsEmissive(inner)) {
								a_record.emissiveNearby = true;
								a_record.emissiveShape = SafeName(inner);
								return;
							}
						}
					}
				}
			}
		}

		/// Everything about one light, read from the engine. May fault on a bad pointer a mod left
		/// behind: always called through FillRecordSafe.
		__declspec(noinline) void FillRecordUnsafe(RE::BSLight* a_light, LightRecord& r, const FillContext& a_ctx)
		{
			auto* niLight = a_light->light.get();
			if (!niLight) {
				r.kind = "no-nilight";
				return;
			}

			r.luminance = a_light->luminance;
			r.lodDimmer = a_light->lodDimmer;
			r.pointLight = a_light->pointLight;
			r.ambientLight = a_light->ambientLight;
			r.dynamic = a_light->dynamic;
			r.portalStrict = a_light->portalStrict;
			r.affectLand = a_light->affectLand;
			r.affectWater = a_light->affectWater;
			r.neverFades = a_light->neverFades;
			r.portalGraph = a_light->portalGraph != nullptr;
			r.rooms = static_cast<int>(a_light->rooms.size());
			r.portals = static_cast<int>(a_light->portals.size());
			r.hidden = niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden);
			if (auto* object = a_light->objectNode.get())
				r.objectNode = SafeName(object);

			if (const auto* rtti = niLight->GetRTTI())
				r.niType = rtti->GetName() ? rtti->GetName() : "";

			r.shadow = a_light->IsShadowLight();
			if (r.shadow) {
				auto* shadowLight = static_cast<RE::BSShadowLight*>(a_light);
				if (shadowLight->IsDirectionalLight())
					r.kind = "shadow-directional";
				else if (shadowLight->IsSpotLight())
					r.kind = "shadow-spot";
				else if (shadowLight->IsParabolicLight())
					r.kind = "shadow-hemi";
				else if (shadowLight->IsOmnidirectionalLight())
					r.kind = "shadow-omni";
				else
					r.kind = "shadow-other";
				GET_INSTANCE_MEMBER(shadowLightIndex, shadowLight);
				r.shadowIndex = static_cast<int>(shadowLightIndex);
				if (!REL::Module::IsVR())
					r.shadowMaps = static_cast<int>(shadowLight->GetRuntimeData().shadowmapDescriptors.size());
			} else if (r.ambientLight) {
				r.kind = "ambient";
			} else if (r.niType == "NiSpotLight") {
				r.kind = "spot";
			} else if (r.pointLight) {
				r.kind = "point";
			} else {
				r.kind = r.niType.empty() ? "unknown" : r.niType;
			}

			const auto& raw = niLight->GetLightRuntimeData();
			r.rawDiffuse[0] = raw.diffuse.red;
			r.rawDiffuse[1] = raw.diffuse.green;
			r.rawDiffuse[2] = raw.diffuse.blue;
			r.rawRadius = raw.radius.x;
			r.rawFade = raw.fade;

			// ISL keeps its data in the light's ambient colour slot (flags, cutoff, LIGH form ID).
			const auto* ext = ISLCommon::RuntimeLightDataExt::Get(niLight);
			r.islInitialised = ext->flags.any(LightLimitFix::LightFlags::Initialised);
			r.isl = ext->flags.any(LightLimitFix::LightFlags::InverseSquare);
			r.islLinear = ext->flags.any(LightLimitFix::LightFlags::Linear);
			r.islDisabled = ext->flags.any(LightLimitFix::LightFlags::Disabled);
			r.islSize = ext->size;
			r.islCutoff = ext->cutoffOverride;
			r.islLighId = r.islInitialised ? ext->lighFormId : 0;

			const auto& world = niLight->world.translate;
			r.pos[0] = world.x;
			r.pos[1] = world.y;
			r.pos[2] = world.z;
			r.local[0] = niLight->local.translate.x;
			r.local[1] = niLight->local.translate.y;
			r.local[2] = niLight->local.translate.z;
			r.distance = world.GetDistance(a_ctx.camera);

			// Owner and node chain.
			RE::TESObjectREFR* ref = niLight->GetUserData();
			r.chain.push_back(SafeName(niLight));
			RE::NiAVObject* node = niLight->parent;
			for (int depth = 0; node && depth < kChainDepth; ++depth, node = node->parent) {
				r.chain.push_back(SafeName(node));
				if (node == a_ctx.player3D[0] || node == a_ctx.player3D[1])
					r.playerNode = true;
				if (ref && NodeOwner(node) == ref && (!node->parent || NodeOwner(node->parent) != ref))
					break;  // the reference's root node
			}
			for (const auto& name : r.chain) {
				if (name.starts_with("LP_")) {
					r.lightPlacer = true;
					r.lightPlacerNode = name;
					break;
				}
			}

			if (ref) {
				r.refId = ref->GetFormID();
				r.refEdid = EdidOf(ref);
				r.refPlugin = PluginOf(ref);
				r.refLastPlugin = LastPluginOf(ref);
				r.refDisabled = ref->IsDisabled();
				r.isPlayer = ref == a_ctx.player;
				r.isActor = ref->GetFormType() == RE::FormType::ActorCharacter;
				if (auto* base = ref->GetBaseObject()) {
					r.baseId = base->GetFormID();
					r.baseType = std::string(RE::FormTypeToString(base->GetFormType()));
					r.baseEdid = EdidOf(base);
					r.basePlugin = PluginOf(base);
					if (auto* model = base->As<RE::TESModel>()) {
						const char* path = model->GetModel();
						r.baseModel = path ? path : "";
					}
					r.baseIsLight = base->GetFormType() == RE::FormType::Light;
				}
			}
			if (r.playerNode)
				r.isPlayer = true;

			RE::TESObjectLIGH* ligh = nullptr;
			if (r.baseIsLight)
				ligh = static_cast<RE::TESObjectLIGH*>(ref->GetBaseObject());
			else if (r.islLighId)
				if (auto* form = RE::TESForm::LookupByID(r.islLighId); form && form->GetFormType() == RE::FormType::Light)
					ligh = static_cast<RE::TESObjectLIGH*>(form);
			if (ligh) {
				r.lighId = ligh->GetFormID();
				r.lighEdid = EdidOf(ligh);
				r.lighPlugin = PluginOf(ligh);
				const char* path = ligh->GetModel();
				r.lighModel = path ? path : "";
				r.lighFlags = ligh->data.flags.underlying();
				r.lighRadius = static_cast<float>(ligh->data.radius);
				r.lighFade = ligh->fade;
				r.lighFov = ligh->data.fov;
				const float amplitude = ligh->data.flickerMovementAmplitude;
				r.lighMoveAmplitude = (!ligh->GetNoFlicker() && std::isfinite(amplitude)) ? std::clamp(amplitude, 0.0f, 1024.0f) : 0.0f;
			}

			// Glowing shapes next to a light that lives inside a mesh (not for placed lights: their node is a marker).
			if (!r.baseIsLight)
				FindEmissive(niLight, r);
		}

		bool FillRecordSafe(RE::BSLight* a_light, LightRecord* a_record, const FillContext* a_ctx)
		{
			__try {
				FillRecordUnsafe(a_light, *a_record, *a_ctx);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// Light refs of fixture meshes around the player (candles, torches, braziers, ...).
		__declspec(noinline) void CollectFixturesUnsafe(std::vector<FixtureRef>& o_refs, RE::TESObjectREFR* a_player)
		{
			auto* tes = RE::TES::GetSingleton();
			if (!tes || !a_player)
				return;
			tes->ForEachReferenceInRange(a_player, kFixtureScanRadius, [&](RE::TESObjectREFR* a_ref) {
				if (o_refs.size() >= kMaxFixtureRefs)
					return RE::BSContainer::ForEachResult::kStop;
				if (!a_ref || a_ref->IsDisabled() || !a_ref->Get3D())
					return RE::BSContainer::ForEachResult::kContinue;
				auto* base = a_ref->GetBaseObject();
				if (!base || base->GetFormType() == RE::FormType::Light)
					return RE::BSContainer::ForEachResult::kContinue;
				auto* model = base->As<RE::TESModel>();
				const char* path = model ? model->GetModel() : nullptr;
				if (!path || !*path)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto lower = Lower(path);
				if (lower.find("marker") != std::string::npos)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto word = FindWord(lower, kFixtureWords);
				if (!word.empty()) {
					auto edid = EdidOf(base);
					o_refs.push_back({ a_ref->GetPosition(), edid.empty() ? std::string(path) : edid, std::string(word) });
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		/// (46s) Distance from a placed light's reference to the nearest fixture mesh (candle, torch, brazier, fire ...) within
		/// a_radius (-1 = none): a meshless "torch" light right at a sconce belongs to it, one far from any is at no visible fire.
		__declspec(noinline) void NearestFixtureMeshUnsafe(RE::TESObjectREFR* a_origin, float a_radius, float& o_distance)
		{
			auto* tes = RE::TES::GetSingleton();
			if (!tes || !a_origin)
				return;
			const RE::NiPoint3 origin = a_origin->GetPosition();
			int visited = 0;
			tes->ForEachReferenceInRange(a_origin, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (++visited > 4096)
					return RE::BSContainer::ForEachResult::kStop;
				if (!a_ref || a_ref == a_origin || a_ref->IsDisabled() || !a_ref->Get3D())
					return RE::BSContainer::ForEachResult::kContinue;
				auto* base = a_ref->GetBaseObject();
				if (!base || base->GetFormType() == RE::FormType::Light)
					return RE::BSContainer::ForEachResult::kContinue;
				auto* model = base->As<RE::TESModel>();
				const char* path = model ? model->GetModel() : nullptr;
				if (!path || !*path)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto lower = Lower(path);
				if (lower.find("marker") != std::string::npos || FindWord(lower, kFixtureWords).empty())
					return RE::BSContainer::ForEachResult::kContinue;
				const float d = a_ref->GetPosition().GetDistance(origin);
				if (d <= a_radius && (o_distance < 0.0f || d < o_distance))
					o_distance = d;
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		bool NearestFixtureMeshSafe(RE::TESObjectREFR* a_origin, float a_radius, float* o_distance)
		{
			__try {
				NearestFixtureMeshUnsafe(a_origin, a_radius, *o_distance);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool CollectFixturesSafe(std::vector<FixtureRef>* o_refs, RE::TESObjectREFR* a_player)
		{
			__try {
				CollectFixturesUnsafe(*o_refs, a_player);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// (46a) Fill light by name: the LIGH editor ID says omni / ambient / fill / bounce, or a plugin it comes
		/// from is an "ambient" one (True Light - Shadows and Ambient). Never by radius. Lower-case inputs.
		bool FillByName(const std::string& a_lighEdidLower, const std::string& a_pluginsLower)
		{
			return a_lighEdidLower.find("omni") != std::string::npos || !FindWord(a_lighEdidLower, kFillWords).empty() ||
			       a_pluginsLower.find("ambient") != std::string::npos;
		}

		/// (46a) Case-insensitive "marker" in a model path, without allocating.
		bool IsMarkerPath(const char* a_path)
		{
			static constexpr char kWord[] = "marker";
			for (const char* p = a_path; p && *p; ++p) {
				int i = 0;
				while (kWord[i] && p[i] && std::tolower(static_cast<unsigned char>(p[i])) == kWord[i])
					++i;
				if (!kWord[i])
					return true;
			}
			return false;
		}

		struct CheapInfo
		{
			bool player = false;
			bool placedNoMesh = false;  ///< a placed LIGH reference whose light object has no mesh (or a marker)
			RE::TESObjectREFR* ref = nullptr;
			RE::TESForm* ligh = nullptr;
			RE::FormID refId = 0;
		};

		/// Per-frame trace: who owns a light. No strings, no allocation.
		bool CheapRead(RE::BSLight* a_light, const RE::TESObjectREFR* a_player, CheapInfo* o_info)
		{
			__try {
				auto* niLight = a_light ? a_light->light.get() : nullptr;
				if (!niLight)
					return false;
				RE::TESObjectREFR* ref = niLight->GetUserData();
				if (!ref)
					return true;
				if (ref == a_player) {
					o_info->player = true;
					return true;
				}
				auto* base = ref->GetBaseObject();
				if (base && base->GetFormType() == RE::FormType::Light) {
					const char* model = static_cast<RE::TESObjectLIGH*>(base)->GetModel();
					o_info->placedNoMesh = !model || !*model || IsMarkerPath(model);
					o_info->ref = ref;
					o_info->ligh = base;
					o_info->refId = ref->GetFormID();
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// (46a) Fill-by-name per placed reference, cached (render thread; names are read once per reference).
		std::unordered_map<RE::FormID, bool> g_fillByRef;

		/// Per-frame trace classification: bit 0 = player light, bit 1 = fill estimate (the census rule:
		/// placed, no mesh, no shadow, named / from a plugin like a fill light; 46a: not by radius).
		int CheapClassify(RE::BSLight* a_light, const RE::TESObjectREFR* a_player, bool a_shadow)
		{
			CheapInfo info;
			if (!CheapRead(a_light, a_player, &info))
				return 0;
			if (info.player)
				return 1;
			if (a_shadow || !info.placedNoMesh || !info.refId)
				return 0;
			auto it = g_fillByRef.find(info.refId);
			if (it == g_fillByRef.end()) {
				if (g_fillByRef.size() > 16384)
					g_fillByRef.clear();
				const std::string plugins = Lower(PluginOf(info.ligh) + "/" + PluginOf(info.ref) + "/" + LastPluginOf(info.ref));
				it = g_fillByRef.emplace(info.refId, FillByName(Lower(EdidOf(info.ligh)), plugins)).first;
			}
			return it->second ? 2 : 0;
		}
		void Classify(LightRecord& r, const std::vector<FixtureRef>& a_fixtures)
		{
			auto because = [&](std::string a_reason) { r.reasons.push_back(std::move(a_reason)); };
			if (r.readError) {
				r.category = "unknown";
				because("engine data could not be read");
				return;
			}
			if (r.kind == "no-nilight") {
				r.category = "unknown";
				because("BSLight without NiLight");
				return;
			}
			const auto chainLower = [&] {
				std::string all;
				for (const auto& n : r.chain) {
					all += Lower(n);
					all += '/';
				}
				return all;
			}();
			const auto idsLower = Lower(r.lighEdid + "/" + r.baseEdid + "/" + r.refEdid);
			const auto modelLower = Lower(r.baseModel + "/" + r.lighModel);

			if (r.isPlayer) {
				r.category = "player";
				because(r.playerNode ? "under the player's 3D" : "owned by the player");
				return;
			}
			if (r.isActor) {
				r.category = "actor";
				because("owned by an actor (carried torch, creature or spell light)");
				return;
			}
			if (!r.refId) {
				r.category = "effect";
				because("no owning reference (magic, projectile, explosion or other effect)");
				if (r.lighId)
					because("made from LIGH " + (r.lighEdid.empty() ? std::format("{:08X}", r.lighId) : r.lighEdid));
				return;
			}
			if (r.baseType == "PROJ" || r.baseType == "EXPL" || r.baseType == "HAZD" || r.baseType == "ARMA" || r.baseType == "MGEF") {
				r.category = "effect";
				because("owner is a " + r.baseType);
				return;
			}

			if (auto w = FindWord(idsLower + "/" + chainLower, kWindowWords); !w.empty()) {
				// (46a) Three different lights: the sun beam through the window (Window Shadows Ultimate's
				// shadowed spot), the glow of a lit pane seen from outside, and the light a window spreads indoors.
				const bool spot = r.kind == "spot" || r.kind == "shadow-spot";
				if (spot && r.radius > kWindowBeamRadius) {
					r.category = "window-beam";
					because(std::format("name contains '{}', spot light, radius {:.0f} > {:.0f}", w, r.radius, kWindowBeamRadius));
				} else if (r.radius < kWindowGlowRadius) {
					r.category = "window-glow";
					because(std::format("name contains '{}', radius {:.0f} < {:.0f}", w, r.radius, kWindowGlowRadius));
				} else {
					r.category = "window-fill";
					because(std::format("name contains '{}', radius {:.0f}", w, r.radius));
				}
				return;
			}

			if (r.baseIsLight) {
				// A placed light (LIGH reference): vanilla, True Light, Window Shadows ...
				// (46s) The nearest fixture mesh is measured first, so a capture shows it for every placed light (also the ones
				// the rules below decide by name: is a vanilla "torch" light really at a fire?). The decision is unchanged.
				float best = -1;
				const FixtureRef* nearest = nullptr;
				{
					const RE::NiPoint3 p{ r.pos[0], r.pos[1], r.pos[2] };
					for (const auto& f : a_fixtures) {
						const float d = f.pos.GetDistance(p);
						if (best < 0 || d < best) {
							best = d;
							nearest = &f;
						}
					}
					if (nearest) {
						r.nearFixture = nearest->label;
						r.nearFixtureDistance = best;
					}
				}
				// (46a) No mesh and named like a fill light (LIGH editor ID omni / ambient / fill, or an "ambient"
				// plugin): fill, before the nearby-fixture test (True Light's DwarvenOmniLight went either way by it).
				// Shadow-casting ones stand in for a fixture's shadow.
				const bool noMesh = r.lighModel.empty() || Lower(r.lighModel).find("marker") != std::string::npos;
				if (noMesh && FillByName(Lower(r.lighEdid), Lower(r.lighPlugin + "/" + r.refPlugin + "/" + r.refLastPlugin))) {
					if (r.shadow) {
						r.category = "fixture-shadow";
						because("placed shadow-casting light without a mesh, named / from a plugin like a fill light: a fixture's shadow stand-in");
					} else {
						r.category = "fill";
						because("placed light without a mesh, LIGH editor ID or plugin names it a fill / omni / ambient light");
					}
					return;
				}
				if (auto w = FindWord(idsLower, kFillWords); !w.empty()) {
					r.category = "fill";
					because(std::format("editor ID contains '{}'", w));
					return;
				}
				if (auto w = FindWord(idsLower + "/" + modelLower, kFixtureWords); !w.empty()) {
					r.category = "fixture";
					r.fixtureWord = w;
					because(std::format("light's editor ID / model contains '{}'", w));
					return;
				}
				if (!r.lighModel.empty() && Lower(r.lighModel).find("marker") == std::string::npos) {
					r.category = "fixture";
					because("the light object has its own mesh: " + r.lighModel);
					return;
				}
				if (nearest && best <= kFixtureNearDistance) {
					r.category = "fixture";
					r.fixtureWord = nearest->word;
					because(std::format("{:.0f} units from a '{}' mesh ({})", best, nearest->word, nearest->label));
					return;
				}
				// (46a) No "radius >= 512 = fill" rule: True Light's campfires are ~540 units.
				r.category = "standalone";
				because(std::format("placed light without a mesh, radius {:.0f}{}", r.radius,
					nearest ? std::format(", nearest fixture {:.0f} units away", best) : std::string(", no fixture mesh nearby")));
				return;
			}

			// A light inside a model (Light Placer, or a light node in the NIF).
			if (r.lightPlacer)
				because("Light Placer node " + r.lightPlacerNode);
			if (auto w = FindWord(modelLower + "/" + chainLower + "/" + idsLower, kFixtureWords); !w.empty()) {
				r.category = "fixture";
				r.fixtureWord = w;
				because(std::format("model / node / editor ID contains '{}'", w));
				return;
			}
			if (r.emissiveNearby) {
				r.category = "fixture";
				because("glowing shape next to the light: " + r.emissiveShape);
				return;
			}
			if (auto w = FindWord(idsLower + "/" + chainLower, kFillWords); !w.empty()) {
				r.category = "fill";
				because(std::format("name contains '{}'", w));
				return;
			}
			r.category = "mesh-attached";
			because("inside the model of " + (r.baseEdid.empty() ? r.baseType : r.baseEdid) + ", no fixture word or glowing shape found");
		}

		// ---- (46b) shared per-frame classifier -------------------------------------------------------

		Category CategoryFromName(const std::string& a_name)
		{
			static const std::pair<std::string_view, Category> kNames[] = { { "player", Category::Player }, { "actor", Category::Actor },
				{ "effect", Category::Effect }, { "window-beam", Category::WindowBeam }, { "window-glow", Category::WindowGlow },
				{ "window-fill", Category::WindowFill }, { "fixture-shadow", Category::FixtureShadow }, { "fill", Category::Fill },
				{ "fixture", Category::Fixture }, { "standalone", Category::Standalone }, { "mesh-attached", Category::MeshAttached } };
			for (const auto& [name, category] : kNames)
				if (a_name == name)
					return category;
			return Category::Unknown;
		}

		struct ClassEntry
		{
			RE::NiLight* niLight = nullptr;      ///< validation: the same BSLight address with another NiLight is another light
			RE::TESObjectREFR* owner = nullptr;  ///< ... or another owner
			Category category = Category::Unknown;
			bool window = false;                 ///< a window light: re-split by the radius on every call
			bool spot = false;
			std::uint32_t lastFrame = 0;
			Traits traits;                       ///< (46s)
		};
		std::unordered_map<const RE::BSLight*, ClassEntry> g_classCache;
		std::uint64_t g_classReads = 0;
		std::uint32_t g_classLastTrim = 0;

		/// The light's NiLight and owner (validation of a cache entry), SEH-protected.
		bool ReadIdentity(RE::BSLight* a_light, RE::NiLight** o_niLight, RE::TESObjectREFR** o_owner)
		{
			__try {
				auto* niLight = a_light ? a_light->light.get() : nullptr;
				*o_niLight = niLight;
				*o_owner = niLight ? niLight->GetUserData() : nullptr;
				return niLight != nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		Category SplitWindow(bool a_spot, float a_radius)
		{
			if (a_spot && a_radius > kWindowBeamRadius)
				return Category::WindowBeam;
			if (a_radius < kWindowGlowRadius)
				return Category::WindowGlow;
			return Category::WindowFill;
		}

		uint64_t StableId(const LightRecord& r)
		{
			uint64_t h = 14695981039346656037ull;
			h = Fnv(h, &r.refId, sizeof(r.refId));
			h = Fnv(h, &r.lighId, sizeof(r.lighId));
			for (const auto& n : r.chain)
				h = Fnv(h, n.data(), n.size() + 1);
			// (46a) Flickering lights move (Light Placer / True Light flicker animates the node): Light Placer
			// lights leave the local offset out (their node name and owner identify them), the rest use it on a
			// 16-unit grid. 45o hashed the offset rounded to 1 unit: 33 of 49 Whiterun fires did not match.
			if (!r.lightPlacer) {
				const int32_t local[3] = { static_cast<int32_t>(std::lround(r.local[0] / kStableIdLocalGrid)),
					static_cast<int32_t>(std::lround(r.local[1] / kStableIdLocalGrid)), static_cast<int32_t>(std::lround(r.local[2] / kStableIdLocalGrid)) };
				h = Fnv(h, local, sizeof(local));
			}
			// Without an owner the world position (64-unit grid) tells same-named effect lights apart; player lights move.
			if (!r.refId && !r.isPlayer) {
				const int32_t grid[3] = { static_cast<int32_t>(std::floor(r.pos[0] / 64.0f)), static_cast<int32_t>(std::floor(r.pos[1] / 64.0f)),
					static_cast<int32_t>(std::floor(r.pos[2] / 64.0f)) };
				h = Fnv(h, grid, sizeof(grid));
			}
			return h;
		}

		const char* DropName(DropReason a_reason)
		{
			switch (a_reason) {
			case DropReason::NoNiLight:
				return "no-nilight";
			case DropReason::Hidden:
				return "hidden";
			case DropReason::InactiveShadow:
				return "inactive-shadow-slot";
			case DropReason::FadedOut:
				return "faded out (lodDimmer 0)";
			case DropReason::OverLimit:
				return "over-1024-limit";
			default:
				return "";
			}
		}

		nlohmann::json Rgb(const RE::Color& a_color) { return { a_color.red, a_color.green, a_color.blue }; }

		nlohmann::json FormJson(const RE::TESForm* a_form)
		{
			if (!a_form)
				return nullptr;
			return { { "id", std::format("{:08X}", a_form->GetFormID()) }, { "edid", EdidOf(a_form) }, { "plugin", PluginOf(a_form) } };
		}

		nlohmann::json InteriorJson(const RE::INTERIOR_DATA& d)
		{
			return {
				{ "ambient", Rgb(d.ambient) },
				{ "directional", Rgb(d.directional) },
				{ "directionalRotXY", d.directionalXY },
				{ "directionalRotZ", d.directionalZ },
				{ "directionalFade", d.directionalFade },
				{ "fogColorNear", Rgb(d.fogColorNear) },
				{ "fogColorFar", Rgb(d.fogColorFar) },
				{ "fogNear", d.fogNear },
				{ "fogFar", d.fogFar },
				{ "fogPower", d.fogPower },
				{ "fogClamp", d.fogClamp },
				{ "clipDist", d.clipDist },
				{ "lightFadeStart", d.lightFadeStart },
				{ "lightFadeEnd", d.lightFadeEnd },
			};
		}

		/// Where, when, weather, interior lighting, sun light and the relevant CS settings. Render thread.
		nlohmann::json SceneJson(const FrameTrace::Location& a_location, const RE::NiPoint3& a_camera)
		{
			nlohmann::json j;
			{
				const auto now = std::chrono::system_clock::now();
				const std::time_t t = std::chrono::system_clock::to_time_t(now);
				std::tm local{};
				localtime_s(&local, &t);
				char text[32];
				std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
				j["capturedAt"] = text;
				j["build"] = BuildLabel::kBuild;
			}
			j["location"] = { { "world", a_location.world }, { "cell", a_location.cell }, { "interior", a_location.interior },
				{ "player", { a_location.x, a_location.y, a_location.z } }, { "camera", { a_camera.x, a_camera.y, a_camera.z } } };

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (auto* cell = player ? player->GetParentCell() : nullptr) {
				auto& c = j["cell"];
				c = FormJson(cell);
				const char* name = cell->GetFullName();
				c["name"] = name ? name : "";
				c["interior"] = cell->IsInteriorCell();
				if (cell->IsInteriorCell()) {
					if (auto* lighting = cell->GetLighting())
						c["lighting"] = InteriorJson(*lighting);
					auto* tmpl = cell->GetRuntimeData().lightingTemplate;
					c["lightingTemplate"] = FormJson(tmpl);
					if (tmpl)
						c["lightingTemplateData"] = InteriorJson(tmpl->data);
				}
				if (auto* extra = cell->extraList.GetByType<RE::ExtraCellImageSpace>())
					c["imageSpace"] = FormJson(extra->imageSpace);
			}
			if (auto* world = player ? player->GetWorldspace() : nullptr)
				j["worldspace"] = FormJson(world);

			auto& time = j["time"];
			time["gameHour"] = a_location.hour;
			if (auto* sky = RE::Sky::GetSingleton()) {
				const float sunriseBegin = sky->GetSunriseBegin(), sunriseEnd = sky->GetSunriseEnd();
				const float sunsetBegin = sky->GetSunsetBegin(), sunsetEnd = sky->GetSunsetEnd();
				time["sunriseBegin"] = sunriseBegin;
				time["sunriseEnd"] = sunriseEnd;
				time["sunsetBegin"] = sunsetBegin;
				time["sunsetEnd"] = sunsetEnd;
				const float h = a_location.hour;
				const char* phase = h < 0 ? "?" : (h >= sunriseEnd && h < sunsetBegin) ? "day" :
				                              (h >= sunriseBegin && h < sunriseEnd)    ? "dawn" :
				                              (h >= sunsetBegin && h < sunsetEnd)      ? "dusk" :
				                                                                         "night";
				time["phase"] = phase;
				time["skyMode"] = static_cast<uint32_t>(sky->mode.underlying());

				auto weatherJson = [](RE::TESWeather* a_weather) -> nlohmann::json {
					if (!a_weather)
						return nullptr;
					auto w = FormJson(a_weather);
					const auto flags = a_weather->data.flags;
					w["pleasant"] = flags.any(RE::TESWeather::WeatherDataFlag::kPleasant);
					w["cloudy"] = flags.any(RE::TESWeather::WeatherDataFlag::kCloudy);
					w["rainy"] = flags.any(RE::TESWeather::WeatherDataFlag::kRainy);
					w["snow"] = flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
					return w;
				};
				j["weather"] = { { "current", weatherJson(sky->currentWeather) }, { "last", weatherJson(sky->lastWeather) },
					{ "transition", sky->currentWeatherPct } };
			}

			// The directional light the engine uses (sun by day, moon by night; the interior directional inside).
			if (auto* ssn = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr) {
				if (auto* sun = ssn->GetRuntimeData().sunLight) {
					if (auto* niSun = sun->light.get()) {
						const auto& d = niSun->GetLightRuntimeData();
						const auto& m = niSun->world.rotate;
						j["sunLight"] = { { "diffuse", { d.diffuse.red, d.diffuse.green, d.diffuse.blue } },
							{ "ambient", { d.ambient.red, d.ambient.green, d.ambient.blue } }, { "fade", d.fade },
							{ "direction", { m.entry[0][0], m.entry[1][0], m.entry[2][0] } }, { "shadow", sun->IsShadowLight() } };
					}
				}
			}

			auto& cs = j["communityShaders"];
			{
				auto& llf = globals::features::lightLimitFix;
				cs["lightLimitFix"] = { { "loaded", llf.loaded }, { "visualisation", llf.settings.EnableLightsVisualisation },
					{ "clusters", { llf.clusterSize[0], llf.clusterSize[1], llf.clusterSize[2] } } };
				cs["inverseSquareLighting"] = { { "loaded", globals::features::inverseSquareLighting.loaded } };
				auto& sun = globals::features::interiorSun;
				cs["interiorSun"] = { { "loaded", sun.loaded }, { "interiorWithSun", sun.isInteriorWithSun },
					{ "forceDoubleSided", sun.settings.ForceDoubleSidedRendering }, { "shadowDistance", sun.settings.InteriorShadowDistance } };
				auto& sspls = globals::features::screenSpacePointLightShadows;
				cs["screenSpacePointLightShadows"] = { { "loaded", sspls.loaded }, { "enable", sspls.settings.Enable != 0 },
					{ "strength", sspls.settings.Strength }, { "stepLimit", sspls.settings.StepLimit }, { "rayLength", sspls.settings.RayLength },
					{ "maxDistance", sspls.settings.MaxDistance }, { "softShadows", sspls.settings.EnableSoftShadows != 0 } };
				auto& rt = globals::features::rayTracing;
				cs["rayTracing"] = { { "loaded", rt.loaded }, { "enabled", rt.settings.Enabled }, { "profile", rt.loaded ? rt.DescribeForProfile() : std::string() } };
				// (46s) The ray-traced point-light shadow sources whose range covers the player this frame.
				if (rt.loaded)
					cs["rayTracing"]["pointSourcesReachingPlayer"] = rt.PointSourcesReachingPlayerJson();
			}
			return j;
		}

		nlohmann::json CostJson(int a_shadowLights)
		{
			nlohmann::json j;
			auto* passes = Util::GpuPassTimers::GetSingleton();
			const float llfSmoothed = passes->SmoothedMs(Util::GpuBucket::LightLimitFix);
			const float llfLast = passes->LastSampleMs(Util::GpuBucket::LightLimitFix);
			j["llfGpuMs"] = { { "smoothed", llfSmoothed > 0 ? llfSmoothed : -1.0f }, { "last", llfLast },
				{ "note", "cluster build + light culling; -1 = not measured (open the Performance Overlay or record an F7 trace)" } };
			j["ssplsGpuMs"] = { { "value", -1 }, { "note", "SSPLS runs inside the Lighting shader; it has no timer of its own" } };

			const auto& report = Util::GpuPhaseTimeline::GetSingleton()->Get();
			auto ms = [&](Util::GpuPhase p) { return report.ms[static_cast<size_t>(p)]; };
			// (46a) The timeline keeps its last sample after the overlay closes: a value from minutes ago is
			// not this scene's. Age in ms (-1 = never measured); older than kStaleSampleMs = -1 everywhere.
			double ageMs = -1.0;
			if (report.hasSample && report.sampleTick != 0) {
				const auto nowTick = std::chrono::steady_clock::now().time_since_epoch().count();
				ageMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(nowTick - report.sampleTick)).count();
			}
			const bool fresh = ageMs >= 0.0 && ageMs <= kStaleSampleMs;
			j["engineShadows"] = {
				{ "measured", fresh },
				{ "sampleAgeMs", ageMs >= 0.0 ? std::round(ageMs) : -1.0 },
				{ "localLightShadowMapsMs", fresh ? ms(Util::GpuPhase::ShadowLocalLights) : -1.0f },
				{ "sunCascadesMs", fresh ? ms(Util::GpuPhase::ShadowSun1) + ms(Util::GpuPhase::ShadowSun2) + ms(Util::GpuPhase::ShadowSun3) : -1.0f },
				{ "focusShadowsMs", fresh ? ms(Util::GpuPhase::ShadowFocus) : -1.0f },
				{ "shadowMaskMs", fresh ? ms(Util::GpuPhase::ShadowMask) : -1.0f },
				{ "localShadowMapsRendered", fresh && report.shadow.valid ? static_cast<int>(report.shadow.localShadowMaps) : -1 },
				{ "note", "measured only while the Performance Overlay's Engine passes (GPU) table is on screen; -1 = not measured or the "
						  "last sample is older than 1 s (sampleAgeMs)" },
			};
			j["activeShadowLights"] = a_shadowLights;
			const auto smoothed = FrameTrace::GetSmoothed();
			j["frameMs"] = smoothed.valid ? smoothed.frameMs : -1.0f;
			const auto gpu = Util::GpuFrameTimer::GetSingleton()->Get();
			j["gpuWorkMs"] = gpu.hasSample ? gpu.workSpanMs : -1.0f;
			return j;
		}

		nlohmann::json RecordJson(const LightRecord& r)
		{
			nlohmann::json j;
			j["id"] = std::format("{:016X}", r.stableId);
			if (r.duplicate)
				j["duplicateOf"] = r.duplicate;
			j["bufferIndex"] = r.bufferIndex;
			if (r.dropped != DropReason::None)
				j["skipped"] = (r.dropped == DropReason::FadedOut && r.lodDimmer > 1e-4f) ? "zero colour or radius" : DropName(r.dropped);
			j["category"] = r.category;
			j["why"] = r.reasons;
			j["kind"] = r.kind;
			j["niType"] = r.niType;
			if (r.readError)
				j["readError"] = true;
			j["shadow"] = r.shadow;
			if (r.shadow) {
				j["shadowIndex"] = r.shadowIndex;
				j["shadowMaps"] = r.shadowMaps;
			}
			j["radius"] = r.radius;
			j["color"] = { r.color[0], r.color[1], r.color[2] };
			j["fade"] = r.fade;
			// (46a) After the distance / room fade (lodDimmer), as the light is drawn, and before it (what the
			// light would give at full strength). 45o wrote the pre-fade value for skipped lights.
			j["intensity"] = r.intensityAfterFade;
			j["intensityBeforeFade"] = r.intensityBeforeFade;
			j["raw"] = { { "diffuse", { r.rawDiffuse[0], r.rawDiffuse[1], r.rawDiffuse[2] } }, { "radius", r.rawRadius }, { "fade", r.rawFade },
				{ "luminance", r.luminance }, { "lodDimmer", r.lodDimmer } };
			if (r.islInitialised)
				j["isl"] = { { "inverseSquare", r.isl }, { "linear", r.islLinear }, { "size", r.islSize }, { "cutoff", r.islCutoff },
					{ "disabledByEditor", r.islDisabled } };
			j["flags"] = { { "portalStrict", r.portalStrict }, { "neverFades", r.neverFades }, { "affectLand", r.affectLand },
				{ "affectWater", r.affectWater }, { "dynamic", r.dynamic }, { "pointLight", r.pointLight }, { "ambientLight", r.ambientLight },
				{ "hidden", r.hidden }, { "portalGraph", r.portalGraph }, { "rooms", r.rooms }, { "portals", r.portals },
				{ "llf", r.llfFlags } };
			j["pos"] = { std::round(r.pos[0]), std::round(r.pos[1]), std::round(r.pos[2]) };
			j["distance"] = std::round(r.distance);
			j["nodes"] = r.chain;
			if (!r.objectNode.empty())
				j["objectNode"] = r.objectNode;
			if (r.lightPlacer)
				j["lightPlacerNode"] = r.lightPlacerNode;
			if (r.refId) {
				j["ref"] = { { "id", std::format("{:08X}", r.refId) }, { "edid", r.refEdid }, { "plugin", r.refPlugin }, { "lastPlugin", r.refLastPlugin },
					{ "disabled", r.refDisabled } };
				j["base"] = { { "id", std::format("{:08X}", r.baseId) }, { "type", r.baseType }, { "edid", r.baseEdid }, { "plugin", r.basePlugin },
					{ "model", r.baseModel } };
			}
			if (r.lighId)
				j["ligh"] = { { "id", std::format("{:08X}", r.lighId) }, { "edid", r.lighEdid }, { "plugin", r.lighPlugin }, { "model", r.lighModel },
					{ "flags", std::format("{:04X}", r.lighFlags) }, { "radius", r.lighRadius }, { "fade", r.lighFade }, { "fov", r.lighFov },
					{ "moveAmplitude", r.lighMoveAmplitude } };  // (46s)
			// (46s) The ray-traced shadow source this light uses (its slot; lights sharing a slot share one shadow).
			if (r.rtTag & RayTracingPointLights::kTagSourceMask)
				j["rtSource"] = static_cast<int>((r.rtTag & RayTracingPointLights::kTagSourceMask) - 1);
			if (r.emissiveNearby)
				j["glowingShape"] = r.emissiveShape;
			if (!r.nearFixture.empty())
				j["nearestFixture"] = { { "what", r.nearFixture }, { "distance", std::round(r.nearFixtureDistance) } };
			if (r.bufferIndex >= 0)
				j["screen"] = { { "clusters", r.clusters }, { "pixelHits", r.pixelHits } };
			return j;
		}

		struct Histogram
		{
			// 0, 1, 2, 3, 4, 5-8, 9-16, >16
			uint64_t bins[8]{};
			uint64_t total = 0;
			uint64_t sum = 0;
			uint32_t max = 0;
			uint64_t over2 = 0, over4 = 0;

			void Add(uint32_t a_count, uint64_t a_weight = 1)
			{
				const int bin = a_count <= 4 ? static_cast<int>(a_count) : a_count <= 8 ? 5 :
				                                                       a_count <= 16    ? 6 :
				                                                                          7;
				bins[bin] += a_weight;
				total += a_weight;
				sum += static_cast<uint64_t>(a_count) * a_weight;
				max = std::max(max, a_count);
				if (a_count > 2)
					over2 += a_weight;
				if (a_count > 4)
					over4 += a_weight;
			}

			nlohmann::json ToJson() const
			{
				static constexpr const char* kNames[] = { "0", "1", "2", "3", "4", "5-8", "9-16", ">16" };
				nlohmann::json bj;
				for (int i = 0; i < 8; ++i)
					bj[kNames[i]] = bins[i];
				const double t = total ? static_cast<double>(total) : 1.0;
				return { { "samples", total }, { "bins", bj }, { "max", max }, { "mean", static_cast<double>(sum) / t },
					{ "shareOver2", static_cast<double>(over2) / t }, { "shareOver4", static_cast<double>(over4) / t } };
			}
		};

		float DecodeDepth(const uint8_t* a_texel, DXGI_FORMAT a_viewFormat)
		{
			switch (a_viewFormat) {
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
				{
					uint32_t v;
					std::memcpy(&v, a_texel, 4);
					return static_cast<float>(v & 0xFFFFFF) / 16777215.0f;
				}
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
				{
					float v;
					std::memcpy(&v, a_texel, 4);
					return v;
				}
			case DXGI_FORMAT_R16_UNORM:
				{
					uint16_t v;
					std::memcpy(&v, a_texel, 2);
					return static_cast<float>(v) / 65535.0f;
				}
			default:
				return -1.0f;
			}
		}

		uint32_t TexelBytes(DXGI_FORMAT a_viewFormat)
		{
			switch (a_viewFormat) {
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
				return 4;
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
				return 8;
			case DXGI_FORMAT_R16_UNORM:
				return 2;
			default:
				return 0;
			}
		}

		winrt::com_ptr<ID3D11Buffer> StagingCopy(ID3D11DeviceContext* a_context, ID3D11Buffer* a_source)
		{
			if (!a_source)
				return nullptr;
			D3D11_BUFFER_DESC desc{};
			a_source->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			winrt::com_ptr<ID3D11Buffer> staging;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
				return nullptr;
			a_context->CopyResource(staging.get(), a_source);
			return staging;
		}

		/// Turns the read-back grid, index list and depth into the distribution and per-light screen numbers.
		void ComputeDistribution(Capture& c, const uint32_t* a_grid, const uint32_t* a_indices, uint32_t a_usedIndices, const uint8_t* a_depth,
			uint32_t a_depthPitch, uint32_t a_depthW, uint32_t a_depthH)
		{
			const uint32_t sx = c.clusterSize[0], sy = c.clusterSize[1], sz = c.clusterSize[2];
			const uint32_t clusterCount = sx * sy * sz;
			std::vector<int> bufferToRecord(c.lightCount, -1);
			for (int i = 0; i < static_cast<int>(c.lights.size()); ++i)
				if (c.lights[i].bufferIndex >= 0 && static_cast<uint32_t>(c.lights[i].bufferIndex) < c.lightCount)
					bufferToRecord[c.lights[i].bufferIndex] = i;

			auto countOf = [&](uint32_t a_cluster) { return a_grid[a_cluster * 4 + 1]; };
			auto offsetOf = [&](uint32_t a_cluster) { return a_grid[a_cluster * 4 + 0]; };

			Histogram perCluster, perTile, perPixel;
			uint64_t nonEmptyClusters = 0;
			for (uint32_t i = 0; i < clusterCount; ++i) {
				perCluster.Add(countOf(i));
				nonEmptyClusters += countOf(i) ? 1 : 0;
			}
			for (uint32_t y = 0; y < sy; ++y)
				for (uint32_t x = 0; x < sx; ++x) {
					uint32_t m = 0;
					for (uint32_t z = 0; z < sz; ++z)
						m = std::max(m, countOf(x + y * sx + z * sx * sy));
					perTile.Add(m);
				}

			// Pixels: cluster of every sampled pixel, as the Lighting shader picks it (log depth slices).
			std::vector<uint32_t> pixelsPerCluster(clusterCount, 0);
			uint64_t sky = 0, sampled = 0;
			const float farZ = c.cameraData.x, nearZ = c.cameraData.y;
			const bool depthOk = a_depth && farZ > nearZ && nearZ > 0 && TexelBytes(c.depthViewFormat) != 0;
			if (depthOk) {
				const uint32_t texel = TexelBytes(c.depthViewFormat);
				const uint32_t w = std::min(a_depthW, static_cast<uint32_t>(c.renderW));
				const uint32_t h = std::min(a_depthH, static_cast<uint32_t>(c.renderH));
				const float logRange = std::log(farZ / nearZ);
				for (uint32_t y = kPixelStride / 2; y < h; y += kPixelStride) {
					const uint8_t* row = a_depth + static_cast<size_t>(y) * a_depthPitch;
					for (uint32_t x = kPixelStride / 2; x < w; x += kPixelStride) {
						const float d = DecodeDepth(row + static_cast<size_t>(x) * texel, c.depthViewFormat);
						++sampled;
						if (d < 0.0f || d >= 0.999999f) {
							++sky;
							continue;
						}
						const float z = std::max(c.cameraData.w / (-d * c.cameraData.z + c.cameraData.x), nearZ);
						const uint32_t cz = static_cast<uint32_t>(std::log(z / nearZ) * static_cast<float>(sz) / logRange);
						const uint32_t cx = static_cast<uint32_t>((static_cast<float>(x) + 0.5f) / c.renderW * static_cast<float>(sx));
						const uint32_t cy = static_cast<uint32_t>((static_cast<float>(y) + 0.5f) / c.renderH * static_cast<float>(sy));
						if (cx >= sx || cy >= sy || cz >= sz) {
							++sky;
							continue;
						}
						const uint32_t index = cx + cy * sx + cz * sx * sy;
						perPixel.Add(countOf(index));
						++pixelsPerCluster[index];
					}
				}
			}

			// Per light: clusters touched and sampled pixels lit (before the room / shadow-mask tests of the shader).
			if (a_indices) {
				for (uint32_t i = 0; i < clusterCount; ++i) {
					const uint32_t offset = offsetOf(i), count = countOf(i);
					if (!count || offset >= a_usedIndices)
						continue;
					const uint32_t end = std::min(offset + count, a_usedIndices);
					for (uint32_t k = offset; k < end; ++k) {
						const uint32_t light = a_indices[k];
						if (light >= c.lightCount || bufferToRecord[light] < 0)
							continue;
						auto& r = c.lights[bufferToRecord[light]];
						++r.clusters;
						r.pixelHits += pixelsPerCluster[i];
					}
				}
			}

			auto& d = c.distribution;
			d["clusterGrid"] = { sx, sy, sz };
			d["note"] =
				"Counts are lights whose sphere touches the cluster (LLF culling), before the Lighting shader's portal-room and shadow-mask tests, "
				"so indoors with portal-strict lights they are upper bounds. perTile = the busiest depth slice of each 64x64 screen tile; "
				"perPixel = the cluster each sampled pixel (every 4th in x and y, sky excluded) actually reads.";
			d["perCluster"] = perCluster.ToJson();
			d["perCluster"]["nonEmpty"] = nonEmptyClusters;
			d["perTile"] = perTile.ToJson();
			if (depthOk) {
				d["perPixel"] = perPixel.ToJson();
				d["perPixel"]["sampledPixels"] = sampled;
				d["perPixel"]["skyOrOutside"] = sky;
			} else {
				d["perPixel"] = { { "unavailable", c.gpuNote.empty() ? "depth not read" : c.gpuNote } };
			}
			d["indexListUsed"] = a_usedIndices;
			d["indexListCapacity"] = c.indexCapacity;

			// Top 20 by screen influence.
			const uint64_t geometryPixels = sampled > sky ? sampled - sky : 0;
			std::vector<int> order;
			for (int i = 0; i < static_cast<int>(c.lights.size()); ++i)
				if (c.lights[i].bufferIndex >= 0)
					order.push_back(i);
			std::ranges::sort(order, [&](int a, int b) {
				const auto& la = c.lights[a];
				const auto& lb = c.lights[b];
				return la.pixelHits != lb.pixelHits ? la.pixelHits > lb.pixelHits : la.clusters > lb.clusters;
			});
			auto& top = d["top20"];
			top = nlohmann::json::array();
			for (int i = 0; i < std::min<int>(20, static_cast<int>(order.size())); ++i) {
				const auto& r = c.lights[order[i]];
				std::string name = !r.lighEdid.empty() ? r.lighEdid : !r.baseEdid.empty() ? r.baseEdid :
				                                                                            (!r.chain.empty() ? r.chain.front() : std::string());
				top.push_back({ { "id", std::format("{:016X}", r.stableId) }, { "bufferIndex", r.bufferIndex }, { "category", r.category },
					{ "kind", r.kind }, { "name", name }, { "radius", r.radius }, { "distance", std::round(r.distance) },
					{ "screenShare", geometryPixels ? static_cast<double>(r.pixelHits) / static_cast<double>(geometryPixels) : 0.0 },
					{ "clusters", r.clusters } });
			}
		}

		void WriteFile(std::unique_ptr<Capture> a_capture)
		{
			try {
				auto& c = *a_capture;
				nlohmann::json root;
				root["format"] = "cs-light-census-2";  // (46a) window split, fill rule, faded-out, sample age, ID grid
				root["scene"] = std::move(c.scene);
				root["cost"] = std::move(c.cost);
				root["distribution"] = std::move(c.distribution);

				// Totals by category, kind and flags (lights in the buffer only).
				nlohmann::json byCategory = nlohmann::json::object(), byKind = nlohmann::json::object(), byPlugin = nlohmann::json::object();
				int used = 0, skipped = 0, shadow = 0, portalStrict = 0, isl = 0, lightPlacer = 0, duplicates = 0;
				for (const auto& r : c.lights) {
					if (r.bufferIndex < 0) {
						++skipped;
						continue;
					}
					++used;
					byCategory[r.category] = byCategory.value(r.category, 0) + 1;
					byKind[r.kind] = byKind.value(r.kind, 0) + 1;
					const std::string plugin = !r.lighPlugin.empty() ? r.lighPlugin : !r.refPlugin.empty() ? r.refPlugin :
					                                                                                         "(none)";
					byPlugin[plugin] = byPlugin.value(plugin, 0) + 1;
					shadow += r.shadow ? 1 : 0;
					portalStrict += r.portalStrict ? 1 : 0;
					isl += r.isl ? 1 : 0;
					lightPlacer += r.lightPlacer ? 1 : 0;
					duplicates += r.duplicate ? 1 : 0;
				}
				root["totals"] = { { "inBuffer", used }, { "skipped", skipped }, { "shadowCasting", shadow }, { "portalStrict", portalStrict },
					{ "inverseSquare", isl }, { "lightPlacer", lightPlacer }, { "duplicateIds", duplicates }, { "byCategory", byCategory },
					{ "byKind", byKind }, { "byPlugin", byPlugin }, { "fixtureMeshesScanned", c.fixtureRefs }, { "captureCpuMs", c.cpuMs } };
				root["categories"] = {
					{ "player", "attached to the player (Quick Light, magic hand light, carried torch)" },
					{ "actor", "attached to another actor (NPC torch, creature, NPC spell)" },
					{ "fixture", "belongs to a visible light source: candle, torch, brazier, lantern, fire ... (mesh word, glowing shape, or a placed light within 200 units of such a mesh)" },
					{ "fill", "placed light without a mesh whose LIGH editor ID says omni / ambient / fill / bounce or that comes from an 'ambient' plugin "
							  "(never by radius): lights the room, no visible source" },
					{ "fixture-shadow", "like fill, but shadow-casting: stands in for a fixture's shadow (True Light - Shadows and Ambient)" },
					{ "window-beam", "window light (name says window / sunbeam / shaft), spot with radius > 10000: the sun beam through a window (Window Shadows Ultimate)" },
					{ "window-glow", "window light with radius < 128: the glow of a lit pane seen from outside" },
					{ "window-fill", "other window lights: the light a window spreads into a room" },
					{ "standalone", "placed light without a mesh nearby and not named like a fill light" },
					{ "mesh-attached", "inside some model (Light Placer or a NIF light) with no fixture evidence" },
					{ "effect", "no owning reference, or owned by a projectile / explosion / hazard: magic and effects" },
				};
				auto& lights = root["lights"];
				lights = nlohmann::json::array();
				for (const auto& r : c.lights)
					lights.push_back(RecordJson(r));

				auto dir = logger::log_directory();
				if (!dir) {
					SetStatus("Light Census: no log directory");
					g_busy = false;
					return;
				}
				std::filesystem::path path = *dir / "CommunityShaders" / "Perf";
				std::filesystem::create_directories(path);
				const auto now = std::chrono::system_clock::now();
				const std::time_t t = std::chrono::system_clock::to_time_t(now);
				std::tm local{};
				localtime_s(&local, &t);
				char stamp[32];
				std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
				const std::string name = c.tag.empty() ? std::format("light-census-{}.json", stamp) : std::format("light-census-{}-{}.json", stamp, c.tag);
				path /= name;
				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out) {
					SetStatus("Light Census: could not write " + name);
					g_busy = false;
					return;
				}
				out << root.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
				out.close();
				SetStatus(std::format("Last census: {} ({} lights, {} skipped)", name, used, skipped));
				logger::info("[LightCensus] Wrote {} ({} lights in LLF's buffer, {} skipped)", path.string(), used, skipped);
			} catch (const std::exception& e) {
				SetStatus(std::string("Light Census failed: ") + e.what());
				logger::error("[LightCensus] Writing failed: {}", e.what());
			}
			g_busy = false;
		}

		void ReleaseRing()
		{
			for (auto& s : g_ring)
				s = {};
			g_ringBytes = 0;
			g_ringWrite = 0;
		}

		/// F7 trace: copy this frame's grid into the ring, read the oldest finished one (never waits).
		void TraceRing(ID3D11DeviceContext* a_context, const ClusterInputs& a_in)
		{
			if (!a_in.grid)
				return;
			D3D11_BUFFER_DESC desc{};
			a_in.grid->GetDesc(&desc);
			if (desc.ByteWidth != g_ringBytes) {
				ReleaseRing();
				g_ringBytes = desc.ByteWidth;
			}
			++g_frameCounter;
			// Read back finished slots (two or more frames old).
			for (auto& s : g_ring) {
				if (!s.pending || g_frameCounter - s.frame < 2)
					continue;
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (a_context->Map(s.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
					continue;
				const auto* grid = static_cast<const uint32_t*>(mapped.pData);
				uint32_t maxCount = 0, tilesOver4 = 0;
				const uint32_t clusters = std::min(s.count, g_ringBytes / 16);
				for (uint32_t i = 0; i < clusters; ++i)
					maxCount = std::max(maxCount, grid[i * 4 + 1]);
				if (s.sizeX && s.sizeY && s.sizeZ && s.sizeX * s.sizeY * s.sizeZ <= clusters) {
					for (uint32_t y = 0; y < s.sizeY; ++y)
						for (uint32_t x = 0; x < s.sizeX; ++x) {
							uint32_t m = 0;
							for (uint32_t z = 0; z < s.sizeZ; ++z)
								m = std::max(m, grid[(x + y * s.sizeX + z * s.sizeX * s.sizeY) * 4 + 1]);
							tilesOver4 += m > 4 ? 1 : 0;
						}
					g_lastOver4 = static_cast<float>(tilesOver4) / static_cast<float>(s.sizeX * s.sizeY);
				}
				a_context->Unmap(s.staging.get(), 0);
				g_lastMaxCluster = static_cast<int>(maxCount);
				s.pending = false;
			}
			auto& slot = g_ring[g_ringWrite];
			if (slot.pending)
				return;  // the GPU is far behind; skip this frame
			if (!slot.staging) {
				D3D11_BUFFER_DESC sd = desc;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				sd.StructureByteStride = 0;
				if (FAILED(globals::d3d::device->CreateBuffer(&sd, nullptr, slot.staging.put())))
					return;
			}
			a_context->CopyResource(slot.staging.get(), a_in.grid);
			slot.pending = true;
			slot.frame = g_frameCounter;
			slot.count = a_in.clusterSize[0] * a_in.clusterSize[1] * a_in.clusterSize[2];
			slot.sizeX = a_in.clusterSize[0];
			slot.sizeY = a_in.clusterSize[1];
			slot.sizeZ = a_in.clusterSize[2];
			g_ringWrite = (g_ringWrite + 1) % kTraceRing;
		}
	}

	void RequestCapture()
	{
		if (g_busy.exchange(true))
			return;
		g_requested = true;
		SetStatus("Light Census: capturing...");
	}

	bool Busy() { return g_busy.load(); }

	std::string StatusLine()
	{
		std::scoped_lock lk(g_statusMutex);
		return g_status;
	}

	const char* CategoryName(Category a_category)
	{
		static constexpr const char* kNames[] = { "unknown", "player", "actor", "effect", "window-beam", "window-glow", "window-fill", "fixture-shadow",
			"fill", "fixture", "standalone", "mesh-attached" };
		static_assert(std::size(kNames) == static_cast<std::size_t>(Category::Count));
		const auto i = static_cast<std::size_t>(a_category);
		return i < std::size(kNames) ? kNames[i] : "unknown";
	}

	bool ClassifyCached(RE::BSLight* a_light, float a_radius, std::uint32_t a_frame, std::uint32_t& a_budget, Category& o_category, Traits* o_traits)
	{
		o_category = Category::Unknown;
		if (o_traits)
			*o_traits = {};
		RE::NiLight* niLight = nullptr;
		RE::TESObjectREFR* owner = nullptr;
		if (!a_light || !ReadIdentity(a_light, &niLight, &owner))
			return false;
		if (auto it = g_classCache.find(a_light); it != g_classCache.end() && it->second.niLight == niLight && it->second.owner == owner) {
			auto& e = it->second;
			e.lastFrame = a_frame;
			o_category = e.window ? SplitWindow(e.spot, a_radius) : e.category;
			if (o_traits)
				*o_traits = e.traits;
			return true;
		}
		if (a_budget == 0)
			return false;
		--a_budget;
		++g_classReads;
		// The capture's own read and decision (without its nearby-fixture scan).
		LightRecord r;
		FillContext ctx;
		ctx.player = RE::PlayerCharacter::GetSingleton();
		if (ctx.player) {
			ctx.player3D[0] = ctx.player->Get3D(false);
			ctx.player3D[1] = ctx.player->Get3D(true);
		}
		const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust(0);
		ctx.camera = { eye.x, eye.y, eye.z };
		if (!FillRecordSafe(a_light, &r, &ctx))
			r.readError = true;
		r.radius = a_radius;
		static const std::vector<FixtureRef> kNoFixtures;
		Classify(r, kNoFixtures);
		ClassEntry e;
		e.niLight = niLight;
		e.owner = owner;
		e.category = CategoryFromName(r.category);
		e.window = e.category == Category::WindowBeam || e.category == Category::WindowGlow || e.category == Category::WindowFill;
		e.spot = r.kind == "spot" || r.kind == "shadow-spot";
		e.lastFrame = a_frame;
		// (46s) What the shadow selection needs beyond the category.
		{
			auto& t = e.traits;
			t.lighResolved = r.lighId != 0;
			t.moveAmplitude = r.lighMoveAmplitude;
			t.lightPlacer = r.lightPlacer;
			const bool lighNoMesh = r.lighModel.empty() || Lower(r.lighModel).find("marker") != std::string::npos;
			t.meshlessNamedFixture = !r.readError && r.baseIsLight && lighNoMesh && !r.shadow && e.category == Category::Fixture;
			if (t.meshlessNamedFixture) {
				float d = -1.0f;
				if (NearestFixtureMeshSafe(owner, kMeshScanRadius, &d))
					t.nearMeshDistance = d;
			}
			t.visibleFixture = !r.readError && ((!r.baseIsLight && (e.category == Category::Fixture || e.category == Category::MeshAttached)) ||
			                                       (r.baseIsLight && !lighNoMesh && e.category == Category::Fixture));
			const std::string& name = !r.lighEdid.empty() ? r.lighEdid : r.baseEdid;
			const std::size_t n = std::min(name.size(), sizeof(t.label) - 1);
			std::memcpy(t.label, name.data(), n);
			t.label[n] = '\0';
		}
		g_classCache[a_light] = e;
		o_category = e.window ? SplitWindow(e.spot, a_radius) : e.category;
		if (o_traits)
			*o_traits = e.traits;
		return true;
	}

	void TrimClassifierCache(std::uint32_t a_frame, std::uint32_t a_maxAge)
	{
		// Every 64 frames is enough: the entries are small and validated on every lookup.
		if (a_frame - g_classLastTrim < 64u && g_classCache.size() < 8192)
			return;
		g_classLastTrim = a_frame;
		std::erase_if(g_classCache, [&](const auto& a_entry) { return a_frame - a_entry.second.lastFrame > a_maxAge; });
	}

	std::uint32_t ClassifierCacheSize() { return static_cast<std::uint32_t>(g_classCache.size()); }
	std::uint64_t ClassifierReads() { return g_classReads; }

	bool WantsSources()
	{
		g_captureThisFrame = !g_capture && g_requested.load(std::memory_order_relaxed);
		return g_captureThisFrame || FrameTrace::IsRecording();
	}

	void OnLightsGathered(const std::vector<RE::BSLight*>& a_used, const std::vector<std::pair<RE::BSLight*, DropReason>>& a_dropped, const void* a_data,
		std::size_t a_dataStride, std::uint32_t a_uploaded)
	{
		if (a_dataStride != sizeof(LightLimitFix::LightData))
			return;
		const auto* data = static_cast<const LightLimitFix::LightData*>(a_data);
		auto* player = RE::PlayerCharacter::GetSingleton();

		if (FrameTrace::IsRecording()) {
			FrameTrace::LightStats s;
			s.total = static_cast<int>(a_uploaded);
			s.shadow = s.fill = s.player = 0;
			for (std::uint32_t i = 0; i < a_uploaded && i < a_used.size(); ++i) {
				const bool shadow = data[i].lightFlags.any(LightLimitFix::LightFlags::Shadow);
				s.shadow += shadow ? 1 : 0;
				const int c = CheapClassify(a_used[i], player, shadow);
				s.player += (c & 1) ? 1 : 0;
				s.fill += (c & 2) ? 1 : 0;
			}
			g_traceStats = s;
		}

		if (!g_captureThisFrame)
			return;
		g_requested = false;
		const auto start = std::chrono::steady_clock::now();

		auto capture = std::make_unique<Capture>();
		auto& c = *capture;
		FillContext ctx;
		ctx.player = player;
		if (player) {
			ctx.player3D[0] = player->Get3D(false);
			ctx.player3D[1] = player->Get3D(true);
		}
		const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust(0);
		ctx.camera = { eye.x, eye.y, eye.z };

		std::vector<FixtureRef> fixtures;
		if (!CollectFixturesSafe(&fixtures, player))
			logger::warn("[LightCensus] Fixture scan stopped on a bad reference; {} found before it", fixtures.size());
		c.fixtureRefs = static_cast<int>(fixtures.size());

		c.lights.reserve(a_used.size() + a_dropped.size());
		auto add = [&](RE::BSLight* a_light, int a_index, DropReason a_reason) {
			LightRecord r;
			r.bufferIndex = a_index;
			r.dropped = a_reason;
			if (!a_light || !FillRecordSafe(a_light, &r, &ctx))
				r.readError = true;
			if (a_index >= 0) {
				const auto& d = data[a_index];
				r.color[0] = d.color.x;
				r.color[1] = d.color.y;
				r.color[2] = d.color.z;
				r.radius = d.radius;
				r.fade = d.fade;
				r.llfFlags = d.lightFlags.underlying();
				// LLF's fade already carries lodDimmer.
				r.intensityAfterFade = std::max({ r.color[0], r.color[1], r.color[2] }) * r.fade;
				r.intensityBeforeFade = r.lodDimmer > 1e-4f ? r.intensityAfterFade / r.lodDimmer : r.intensityAfterFade;
			} else {
				r.color[0] = r.rawDiffuse[0];
				r.color[1] = r.rawDiffuse[1];
				r.color[2] = r.rawDiffuse[2];
				r.radius = r.rawRadius;
				r.fade = r.rawFade;
				// Raw engine values (ISL not applied): before the fade, then x lodDimmer as LLF would.
				r.intensityBeforeFade = std::max({ r.color[0], r.color[1], r.color[2] }) * r.fade;
				r.intensityAfterFade = r.intensityBeforeFade * r.lodDimmer;
			}
			Classify(r, fixtures);
			r.stableId = StableId(r);
			if (a_light && globals::features::rayTracing.loaded)
				r.rtTag = globals::features::rayTracing.PointLightTag(a_light);  // (46s)
			c.lights.push_back(std::move(r));
		};
		for (std::size_t i = 0; i < a_used.size(); ++i) {
			if (i < a_uploaded)
				add(a_used[i], static_cast<int>(i), DropReason::None);
			else
				add(a_used[i], -1, DropReason::OverLimit);
		}
		for (const auto& [light, reason] : a_dropped)
			add(light, -1, reason);

		// The same ID twice (e.g. two identical Light Placer lights on one node): number the repeats.
		{
			std::unordered_map<uint64_t, int> seen;
			for (auto& r : c.lights) {
				const int n = seen[r.stableId]++;
				if (n) {
					r.duplicate = n;
					r.stableId = Fnv(r.stableId, &n, sizeof(n));
				}
			}
		}

		int shadowLights = 0;
		if (auto* ssn = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr)
			shadowLights = static_cast<int>(ssn->GetRuntimeData().activeShadowLights.size());

		const auto location = FrameTrace::CurrentLocation();
		c.scene = SceneJson(location, ctx.camera);
		c.cost = CostJson(shadowLights);
		c.tag = FrameTrace::LocationTag(location);
		if (location.hour >= 0)
			c.tag += std::format("-h{:02d}", static_cast<int>(location.hour));
		c.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		g_capture = std::move(capture);
	}

	void OnClustersCulled(const ClusterInputs& a_in)
	{
		auto* context = globals::d3d::context;

		if (FrameTrace::IsRecording()) {
			TraceRing(context, a_in);
			auto s = g_traceStats;
			s.maxCluster = g_lastMaxCluster;
			s.over4Share = g_lastOver4;
			s.llfGpuMs = Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::LightLimitFix);
			FrameTrace::OnLights(s);
		} else if (g_ringBytes) {
			ReleaseRing();
			g_lastMaxCluster = -1;
			g_lastOver4 = -1.0f;
		}

		if (!g_capture)
			return;
		auto& c = *g_capture;

		if (!c.copied) {
			c.copied = true;
			c.lightCount = a_in.lightCount;
			std::copy(a_in.clusterSize, a_in.clusterSize + 3, c.clusterSize);
			c.renderW = a_in.renderWidth;
			c.renderH = a_in.renderHeight;
			c.cameraData = Util::GetCameraData();
			c.gridCount = a_in.clusterSize[0] * a_in.clusterSize[1] * a_in.clusterSize[2];
			c.grid = StagingCopy(context, a_in.grid);
			c.indexList = StagingCopy(context, a_in.indexList);
			c.counter = StagingCopy(context, a_in.counter);
			if (a_in.indexList) {
				D3D11_BUFFER_DESC d{};
				a_in.indexList->GetDesc(&d);
				c.indexCapacity = d.ByteWidth / 4;
			}
			// Depth after the engine's depth prepass (the SRV's resource: Terrain Blending may swap in its own R32 depth).
			auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			if (depth.depthSRV) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				depth.depthSRV->GetDesc(&srvDesc);
				winrt::com_ptr<ID3D11Resource> resource;
				depth.depthSRV->GetResource(resource.put());
				auto texture = resource.try_as<ID3D11Texture2D>();
				D3D11_TEXTURE2D_DESC td{};
				if (texture)
					texture->GetDesc(&td);
				if (!texture || td.SampleDesc.Count != 1 || TexelBytes(srvDesc.Format) == 0) {
					c.gpuNote = std::format("depth format {} / {} samples not supported", static_cast<int>(srvDesc.Format), td.SampleDesc.Count);
				} else {
					td.Usage = D3D11_USAGE_STAGING;
					td.BindFlags = 0;
					td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					td.MiscFlags = 0;
					if (SUCCEEDED(globals::d3d::device->CreateTexture2D(&td, nullptr, c.depth.put()))) {
						context->CopyResource(c.depth.get(), texture.get());
						c.depthViewFormat = srvDesc.Format;
					} else {
						c.gpuNote = "depth staging texture could not be created";
					}
				}
			} else {
				c.gpuNote = "no depth after the prepass this frame";
			}
			return;
		}

		// Poll the copies; after a few frames wait for them.
		++c.framesWaited;
		const UINT flags = c.framesWaited >= kBlockingAfterFrames ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT;
		D3D11_MAPPED_SUBRESOURCE grid{}, indices{}, counter{}, depth{};
		bool mappedGrid = false, mappedIndices = false, mappedCounter = false, mappedDepth = false;
		bool ready = true;
		if (c.grid) {
			mappedGrid = context->Map(c.grid.get(), 0, D3D11_MAP_READ, flags, &grid) == S_OK;
			ready &= mappedGrid;
		}
		if (ready && c.counter) {
			mappedCounter = context->Map(c.counter.get(), 0, D3D11_MAP_READ, flags, &counter) == S_OK;
			ready &= mappedCounter;
		}
		if (ready && c.indexList) {
			mappedIndices = context->Map(c.indexList.get(), 0, D3D11_MAP_READ, flags, &indices) == S_OK;
			ready &= mappedIndices;
		}
		if (ready && c.depth) {
			mappedDepth = context->Map(c.depth.get(), 0, D3D11_MAP_READ, flags, &depth) == S_OK;
			ready &= mappedDepth;
		}
		auto unmapAll = [&] {
			if (mappedGrid)
				context->Unmap(c.grid.get(), 0);
			if (mappedCounter)
				context->Unmap(c.counter.get(), 0);
			if (mappedIndices)
				context->Unmap(c.indexList.get(), 0);
			if (mappedDepth)
				context->Unmap(c.depth.get(), 0);
		};
		if (!ready && flags != 0) {
			unmapAll();
			return;
		}

		if (mappedGrid) {
			const uint32_t used = mappedCounter ? std::min(*static_cast<const uint32_t*>(counter.pData), c.indexCapacity) : 0;
			D3D11_TEXTURE2D_DESC td{};
			if (c.depth)
				c.depth->GetDesc(&td);
			ComputeDistribution(c, static_cast<const uint32_t*>(grid.pData), mappedIndices ? static_cast<const uint32_t*>(indices.pData) : nullptr, used,
				mappedDepth ? static_cast<const uint8_t*>(depth.pData) : nullptr, depth.RowPitch, td.Width, td.Height);
			c.distribution["readBackFrames"] = c.framesWaited;
		} else {
			c.distribution = { { "unavailable", "the cluster grid could not be read back" } };
		}
		unmapAll();
		c.grid = nullptr;
		c.indexList = nullptr;
		c.counter = nullptr;
		c.depth = nullptr;
		std::thread(WriteFile, std::move(g_capture)).detach();
		g_capture.reset();
	}

	void DrawSettings(std::uint32_t& a_key, bool& a_capturingKey)
	{
		ImGui::BeginDisabled(Busy());
		if (ImGui::Button("Light Census: capture snapshot"))
			RequestCapture();
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(
				"Writes every light the renderer uses this frame to a JSON in\n"
				"Documents/My Games/Skyrim Special Edition/SKSE/CommunityShaders/Perf (light-census-<time>-<place>.json):\n"
				"type, radius, colour, flags, which mod / reference / mesh it belongs to, fixture vs fill vs player light,\n"
				"how many lights hit each part of the screen, and the light pass cost. Takes a few ms once; nothing runs otherwise.\n"
				"Tip: start an F7 frame trace first, so the GPU times are measured too.");
		}

		ImGui::PushID("LightCensusKey");
		ImGui::TextUnformatted("Capture key:");
		ImGui::SameLine();
		const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
		if (a_capturingKey) {
			ImGui::TextColored(palette.CurrentHotkey, "press a key... (Esc = keep)");
		} else {
			ImGui::TextColored(palette.CurrentHotkey, "%s", a_key ? Util::Input::KeyIdToString(a_key) : "none");
			ImGui::SameLine();
			if (ImGui::SmallButton("Change"))
				a_capturingKey = true;
			ImGui::SameLine();
			if (ImGui::SmallButton("None"))
				a_key = 0;
		}
		ImGui::PopID();

		if (const auto status = StatusLine(); !status.empty())
			ImGui::TextDisabled("%s", status.c_str());
	}
}
