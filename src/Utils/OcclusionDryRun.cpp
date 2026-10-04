#include "Utils/OcclusionDryRun.h"

#include "Features/GrassOptimizations/HiZPyramid.h"
#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"
#include "Utils/UI.h"

#include <DirectXMath.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <imgui.h>

using namespace DirectX;

namespace Util::OcclusionDryRun
{
	namespace
	{
		// ---------------------------------------------------------------------------------
		// State shared with the culling threads
		// ---------------------------------------------------------------------------------
		Settings g_settings;
		Report g_report;
		bool g_installed = false;
		const char* g_notInstalledReason = "not installed yet";

		std::atomic<bool> g_active{ false };  // test this frame (render thread decides at frame end)
		std::atomic<uint64_t> g_frame{ 1 };   // frame id, bumped at frame end
		uint64_t g_settleUntil = 0;           // render thread only
		bool g_wasLoading = false;

		// (batch 37b, D) The frame id of the last Deferred::EndDeferred that reached us, i.e. the
		// last frame the world was actually rendered. Replaces State::inWorld as the "in game"
		// test: inWorld is only true INSIDE Main_RenderWorld (Deferred.cpp sets it on entry and
		// clears it on exit), so at Present -- where the next frame's activity is decided -- it
		// is always false, and the dry run could never switch itself on. EndDeferred itself runs
		// only with inWorld set, so this is the same condition, sampled where it is true.
		uint64_t g_lastWorldFrame = 0;  // render thread only
		constexpr uint64_t kWorldGraceFrames = 3;

		// (batch 37b, D) Pipeline counters since launch, for the panel line that says which step
		// a stuck dry run is waiting on. Render thread only, except the two culling-side ones.
		struct PipelineCounters
		{
			uint64_t worldFrames = 0;   // EndDeferred calls (world rendered)
			uint64_t activeFrames = 0;  // EndDeferred calls while the test was running
			uint64_t builds = 0;        // Hi-Z built and its 1/16 level queued for readback
			uint64_t buildFails = 0;    // Hi-Z could not be built (reason below)
			uint64_t readTries = 0;     // Map(DO_NOT_WAIT) attempts on a pending slot
			uint64_t readOk = 0;        // ... that returned data (a snapshot was published)
			uint64_t readBusy = 0;      // ... that the GPU had not finished yet
			uint64_t readErrors = 0;    // ... that failed for any other reason (slot dropped)
			uint64_t ringFull = 0;      // frames skipped because the oldest slot was still busy
			const char* lastFail = "";
			std::atomic<uint64_t> process1Calls{ 0 };  // Process1 detour hits while running
			std::atomic<uint64_t> process1Main{ 0 };   // ... of them for the main render camera
		};
		PipelineCounters g_pipe;

		constexpr uint32_t kAccumulatedBit = 1u << 26;  // NiAVObject flags +0xF4, set/cleared by Process1 (1.5.97)
		constexpr int kRing = 3;
		constexpr int kSnapshots = 3;  // > 2, so a reader's snapshot is never the one being refilled
		constexpr int kHiZMip = 2;     // 1/4 base * 1/4 = 1/16 of the rendered depth
		constexpr int kCellPixels = 16;
		constexpr float kDepthBias = 1e-4f;

		// World render camera: Main::spWorldRoot (SE ID 517006) -> BSSceneGraph::camera.
		// The exact pointer every main-view cull carries (doodlum, IDA-verified).
		RE::NiCamera* GetMainCamera()
		{
			static REL::Relocation<RE::NiNode**> worldRoot{ REL::ID(517006) };
			RE::NiNode* root = *worldRoot;
			if (!root)
				return nullptr;
			return static_cast<RE::BSSceneGraph*>(root)->GetRuntimeData().camera.get();
		}

#pragma warning(push)
#pragma warning(disable: 4324)  // padding from XMMATRIX alignment is intended
		struct Snapshot
		{
			std::vector<float> grid;  // gridW * gridH, farthest depth per 16x16 rendered pixels
			uint32_t gridW = 0, gridH = 0;
			uint32_t renderW = 0, renderH = 0;  // the rendered sub-rectangle the grid covers
			XMMATRIX view{};
			XMMATRIX viewProj{};
			RE::NiPoint3 pos{};
			RE::NiPoint3 dir{};
			uint64_t captureFrame = 0;
		};

		struct RingSlot
		{
			winrt::com_ptr<ID3D11Texture2D> staging;
			XMMATRIX view{};
			XMMATRIX viewProj{};
			RE::NiPoint3 pos{};
			RE::NiPoint3 dir{};
			uint32_t gridW = 0, gridH = 0, renderW = 0, renderH = 0;
			uint64_t frame = 0;
			bool pending = false;
		};

		/// What every test of one frame shares, built once by the first test of the frame.
		struct FrameContext
		{
			XMMATRIX view{};
			XMMATRIX viewProj{};
			XMVECTOR snapPosV{};
			RE::NiPoint3 camPos{};
			const Snapshot* snap = nullptr;
			float margin = 0.0f;
			bool guard = false;
			float move = 0.0f;
			float turn = 0.0f;
			int age = -1;
		};
#pragma warning(pop)

		Snapshot g_snaps[kSnapshots];
		std::atomic<const Snapshot*> g_front{ nullptr };
		int g_snapWrite = 0;

		RingSlot g_ring[kRing];
		int g_ringWrite = 0;
		uint32_t g_stagingW = 0, g_stagingH = 0;
		std::unique_ptr<HiZPyramid> g_pyramid;

		FrameContext g_ctx;
		std::atomic<uint64_t> g_ctxClaim{ 0 };
		std::atomic<uint64_t> g_ctxReady{ 0 };
		std::atomic<bool> g_ctxValid{ false };

		// Per-frame counters, written from the culling threads.
		struct AtomicCounts
		{
			std::atomic<uint32_t> objectsTested{ 0 }, containersTested{ 0 };
			std::atomic<uint32_t> occludedRaw{ 0 }, occludedGuarded{ 0 }, occludedEmpty{ 0 };
			std::atomic<uint32_t> containersOccludedRaw{ 0 }, containersOccludedGuarded{ 0 };
			std::atomic<uint32_t> geomsRaw{ 0 }, geomsGuarded{ 0 }, visibleGeoms{ 0 };
			std::atomic<uint32_t> keptActor{ 0 }, keptLod{ 0 }, keptGrass{ 0 }, keptNear{ 0 }, keptMotion{ 0 };
			std::atomic<int64_t> testTicks{ 0 };
		};
		AtomicCounts g_counts;

		// Objects already counted as culled this frame, per verdict: a would-be-culled object
		// under one of them is part of that one's subtree and must not be counted again.
		std::mutex g_countedLock;
		std::unordered_set<const void*> g_countedRaw;
		std::unordered_set<const void*> g_countedGuarded;

		// The object whose Process1 is running on this thread (innermost).
		thread_local RE::NiAVObject* t_current = nullptr;

		int64_t QpcNow()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcToMs()
		{
			static const double toMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
			}();
			return toMs;
		}

		bool OverlayOnScreen()
		{
			auto& overlay = globals::features::performanceOverlay;
			return globals::menu && globals::menu->overlayVisible && overlay.loaded && overlay.IsOverlayVisible();
		}

		// ---------------------------------------------------------------------------------
		// Camera matrices (doodlum's port of Nukem's NiCamera::CalculateViewProjection).
		// Camera-relative: the view has no translation, positions are offset by the camera's.
		// ---------------------------------------------------------------------------------
		void CalculateViewProjection(RE::NiCamera* a_camera, XMMATRIX& a_view, XMMATRIX& a_viewProj)
		{
			const RE::NiMatrix3& R = a_camera->world.rotate;
			const RE::NiPoint3 dir{ R.entry[0][0], R.entry[1][0], R.entry[2][0] };
			const RE::NiPoint3 up{ R.entry[0][1], R.entry[1][1], R.entry[2][1] };
			const RE::NiPoint3 right{ R.entry[0][2], R.entry[1][2], R.entry[2][2] };

			a_view.r[0] = XMVectorSet(right.x, up.x, dir.x, 0.0f);
			a_view.r[1] = XMVectorSet(right.y, up.y, dir.y, 0.0f);
			a_view.r[2] = XMVectorSet(right.z, up.z, dir.z, 0.0f);
			a_view.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

			const RE::NiFrustum& fr = a_camera->GetRuntimeData2().viewFrustum;
			const float rightLeftDiff = fr.fRight - fr.fLeft;
			const float rightLeftRatio = -((1.0f / rightLeftDiff) * (fr.fRight + fr.fLeft));
			const float topBottomDiff = fr.fTop - fr.fBottom;
			const float topBottomRatio = -((1.0f / topBottomDiff) * (fr.fTop + fr.fBottom));
			const float invNearFarDiff = 1.0f / (fr.fFar - fr.fNear);

			XMFLOAT4X4 p{};
			p.m[0][0] = (1.0f / rightLeftDiff) * 2.0f;
			p.m[1][1] = (1.0f / topBottomDiff) * 2.0f;
			if (!fr.bOrtho) {
				p.m[2][0] = rightLeftRatio;
				p.m[2][1] = topBottomRatio;
				p.m[2][2] = invNearFarDiff * fr.fFar;
				p.m[2][3] = 1.0f;
				p.m[3][2] = -((fr.fNear * fr.fFar) * invNearFarDiff);
			} else {
				p.m[2][2] = invNearFarDiff;
				p.m[3][0] = rightLeftRatio;
				p.m[3][1] = topBottomRatio;
				p.m[3][2] = -(invNearFarDiff * fr.fNear);
				p.m[3][3] = 1.0f;
			}
			a_viewProj = XMMatrixMultiply(a_view, XMLoadFloat4x4(&p));
		}

		RE::NiPoint3 CameraForward(RE::NiCamera* a_camera)
		{
			const RE::NiMatrix3& R = a_camera->world.rotate;
			return { R.entry[0][0], R.entry[1][0], R.entry[2][0] };
		}

		// ---------------------------------------------------------------------------------
		// The test. Occluded iff the object's NEAREST depth is behind the FARTHEST depth of every
		// cell its screen rectangle touches (max-reduced grid, standard Z: near 0, far 1).
		// ---------------------------------------------------------------------------------
		bool RectKeep(const Snapshot& s, float a_minX, float a_minY, float a_maxX, float a_maxY, float a_zMin)
		{
			if (s.grid.empty() || !s.gridW || !s.gridH)
				return true;
			if (a_maxX < -1.0f || a_minX > 1.0f || a_maxY < -1.0f || a_minY > 1.0f)
				return true;  // off-screen in the snapshot's view: the engine's frustum cull owns that
			if (a_zMin > 0.9995f)
				return true;  // sky / far plane: depth is compressed to ~1 there
			// NDC -> UV (y flips) -> RENDERED pixels -> cell. (batch 37a fix: the original used the
			// full depth-texture size here, which is wrong whenever DLSS/DRS renders a sub-rectangle.)
			const float u0 = (a_minX + 1.0f) * 0.5f, u1 = (a_maxX + 1.0f) * 0.5f;
			const float v0 = (1.0f - a_maxY) * 0.5f, v1 = (1.0f - a_minY) * 0.5f;
			const int gw = static_cast<int>(s.gridW), gh = static_cast<int>(s.gridH);
			const int cx0 = std::clamp(static_cast<int>(std::floor(u0 * s.renderW)) / kCellPixels, 0, gw - 1);
			const int cx1 = std::clamp(static_cast<int>(std::floor(u1 * s.renderW)) / kCellPixels, 0, gw - 1);
			const int cy0 = std::clamp(static_cast<int>(std::floor(v0 * s.renderH)) / kCellPixels, 0, gh - 1);
			const int cy1 = std::clamp(static_cast<int>(std::floor(v1 * s.renderH)) / kCellPixels, 0, gh - 1);
			if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 256)
				return true;  // huge on screen: almost surely visible, skip the scan
			float farthest = 0.0f;
			for (int y = cy0; y <= cy1; ++y) {
				const float* row = &s.grid[static_cast<size_t>(y) * s.gridW];
				for (int x = cx0; x <= cx1; ++x)
					farthest = std::max(farthest, row[x]);
			}
			return a_zMin <= farthest + kDepthBias;
		}

		bool AabbKeep(const FrameContext& c, const RE::NiPoint3& a_center, const RE::NiPoint3& a_half, float a_margin)
		{
			const __m128 vCenter = _mm_sub_ps(_mm_setr_ps(a_center.x, a_center.y, a_center.z, 0.0f), c.snapPosV);
			const __m128 vHalf = _mm_setr_ps(std::abs(a_half.x) + a_margin, std::abs(a_half.y) + a_margin, std::abs(a_half.z) + a_margin, 0.0f);
			const __m128 vMin = _mm_sub_ps(vCenter, vHalf);
			const __m128 vMax = _mm_add_ps(vCenter, vHalf);

			__m128 xRow[2], yRow[2], zRow[2];
			xRow[0] = _mm_mul_ps(_mm_shuffle_ps(vMin, vMin, 0x00), c.viewProj.r[0]);
			xRow[1] = _mm_mul_ps(_mm_shuffle_ps(vMax, vMax, 0x00), c.viewProj.r[0]);
			yRow[0] = _mm_mul_ps(_mm_shuffle_ps(vMin, vMin, 0x55), c.viewProj.r[1]);
			yRow[1] = _mm_mul_ps(_mm_shuffle_ps(vMax, vMax, 0x55), c.viewProj.r[1]);
			zRow[0] = _mm_mul_ps(_mm_shuffle_ps(vMin, vMin, 0xaa), c.viewProj.r[2]);
			zRow[1] = _mm_mul_ps(_mm_shuffle_ps(vMax, vMax, 0xaa), c.viewProj.r[2]);

			const __m128 minVert = _mm_add_ps(c.viewProj.r[3],
				_mm_add_ps(_mm_add_ps(_mm_min_ps(xRow[0], xRow[1]), _mm_min_ps(yRow[0], yRow[1])), _mm_min_ps(zRow[0], zRow[1])));
			if (XMVectorGetW(minVert) < 0.00000001f)
				return true;  // straddles the near plane: close to the camera, keep

			static const uint32_t sx[8] = { 1, 0, 0, 1, 1, 1, 0, 0 };
			static const uint32_t sy[8] = { 1, 1, 1, 1, 0, 0, 0, 0 };
			static const uint32_t sz[8] = { 1, 1, 0, 0, 0, 1, 1, 0 };

			__m128 screenMin = _mm_set1_ps(FLT_MAX);
			__m128 screenMax = _mm_set1_ps(-FLT_MAX);
			for (uint32_t i = 0; i < 8; ++i) {
				__m128 vert = c.viewProj.r[3];
				vert = _mm_add_ps(vert, xRow[sx[i]]);
				vert = _mm_add_ps(vert, yRow[sy[i]]);
				vert = _mm_add_ps(vert, zRow[sz[i]]);
				const __m128 w = _mm_shuffle_ps(vert, vert, 0xff);
				const __m128 ndc = _mm_div_ps(vert, w);
				screenMin = _mm_min_ps(screenMin, ndc);
				screenMax = _mm_max_ps(screenMax, ndc);
			}
			return RectKeep(*c.snap, XMVectorGetX(screenMin), XMVectorGetY(screenMin), XMVectorGetX(screenMax), XMVectorGetY(screenMax),
				XMVectorGetZ(screenMin));
		}

		bool SphereKeep(const FrameContext& c, const RE::NiPoint3& a_center, float a_radius)
		{
			XMVECTOR bounds = _mm_sub_ps(_mm_setr_ps(a_center.x, a_center.y, a_center.z, 1.0f), c.snapPosV);
			bounds = XMVectorSetW(bounds, 1.0f);
			const float dist = XMVectorGetX(XMVector3Length(bounds));
			if (dist <= a_radius)
				return true;  // camera inside the sphere

			XMVECTOR closest = XMVectorAdd(bounds, XMVectorScale(XMVector3Normalize(XMVectorNegate(bounds)), a_radius));
			closest = XMVector4Transform(XMVectorSetW(closest, 1.0f), c.viewProj);
			const float closestW = XMVectorGetW(closest);
			if (closestW < 0.000001f)
				return true;  // straddles the near plane

			// Eye at the origin (camera-relative view); expand the sphere on the view plane,
			// compensating perspective, and take the rectangle of its four corners.
			const XMVECTOR viewUp = XMVectorSet(XMVectorGetY(c.view.r[0]), XMVectorGetY(c.view.r[1]), XMVectorGetY(c.view.r[2]), 0.0f);
			const XMVECTOR toEye = XMVectorNegate(XMVectorSetW(bounds, 0.0f));
			const XMVECTOR viewRight = XMVector3Normalize(XMVector3Cross(toEye, viewUp));
			const float r = dist * std::tan(std::asin(std::min(a_radius / dist, 0.999f)));
			const XMVECTOR vUp = XMVectorScale(viewUp, r);
			const XMVECTOR vRight = XMVectorScale(viewRight, r);

			const XMVECTOR corners[4] = {
				XMVectorSubtract(XMVectorAdd(bounds, vUp), vRight),
				XMVectorAdd(XMVectorAdd(bounds, vUp), vRight),
				XMVectorSubtract(XMVectorSubtract(bounds, vUp), vRight),
				XMVectorAdd(XMVectorSubtract(bounds, vUp), vRight),
			};
			XMVECTOR mn = XMVectorReplicate(FLT_MAX), mx = XMVectorReplicate(-FLT_MAX);
			for (const auto& corner : corners) {
				const XMVECTOR cs = XMVector4Transform(XMVectorSetW(corner, 1.0f), c.viewProj);
				const XMVECTOR ndc = XMVectorDivide(cs, XMVectorSplatW(cs));
				mn = XMVectorMin(mn, ndc);
				mx = XMVectorMax(mx, ndc);
			}
			return RectKeep(*c.snap, XMVectorGetX(mn), XMVectorGetY(mn), XMVectorGetX(mx), XMVectorGetY(mx), XMVectorGetZ(closest) / closestW);
		}

		/// Builds the frame's shared test context once; other threads wait the few microseconds.
		const FrameContext* GetContext()
		{
			const uint64_t frame = g_frame.load(std::memory_order_acquire);
			if (g_ctxReady.load(std::memory_order_acquire) == frame)
				return g_ctxValid.load(std::memory_order_relaxed) ? &g_ctx : nullptr;

			uint64_t claimed = g_ctxClaim.load(std::memory_order_relaxed);
			if (claimed != frame && g_ctxClaim.compare_exchange_strong(claimed, frame, std::memory_order_acq_rel)) {
				bool valid = false;
				const Snapshot* snap = g_front.load(std::memory_order_acquire);
				RE::NiCamera* cam = GetMainCamera();
				if (snap && cam && !snap->grid.empty()) {
					FrameContext& c = g_ctx;
					c.snap = snap;
					c.view = snap->view;
					c.viewProj = snap->viewProj;
					c.snapPosV = _mm_setr_ps(snap->pos.x, snap->pos.y, snap->pos.z, 0.0f);
					c.camPos = cam->world.translate;
					c.move = c.camPos.GetDistance(snap->pos);
					const RE::NiPoint3 fwd = CameraForward(cam);
					const float d = std::clamp(fwd.Dot(snap->dir), -1.0f, 1.0f);
					c.turn = std::acos(d) * (180.0f / 3.14159265f);
					c.guard = c.move > g_settings.MotionGuardDistance || c.turn > g_settings.MotionGuardAngle;
					c.margin = std::max(0.0f, g_settings.BoundsMargin) + c.move * std::max(0.0f, g_settings.MotionMarginScale);
					c.age = static_cast<int>(frame - snap->captureFrame);
					valid = true;
				}
				g_ctxValid.store(valid, std::memory_order_relaxed);
				g_ctxReady.store(frame, std::memory_order_release);
				return valid ? &g_ctx : nullptr;
			}
			// Another thread is building it right now.
			for (int spin = 0; spin < 200000; ++spin) {
				if (g_ctxReady.load(std::memory_order_acquire) == frame)
					return g_ctxValid.load(std::memory_order_relaxed) ? &g_ctx : nullptr;
				_mm_pause();
			}
			return nullptr;
		}

		bool IsActorObject(RE::NiAVObject* a_object)
		{
			if (auto* ref = a_object->GetUserData())
				return ref->formType == RE::FormType::ActorCharacter;
			return false;
		}

		bool HasActorAncestor(RE::NiAVObject* a_object)
		{
			int guard = 0;
			for (RE::NiAVObject* o = a_object; o && guard < 64; o = o->parent, ++guard) {
				if (IsActorObject(o))
					return true;
			}
			return false;
		}

		enum class GeomKind
		{
			Drawable,
			Lod,
			Grass
		};

		GeomKind ClassifyGeometry(RE::BSGeometry* a_geom)
		{
			auto* prop = netimmerse_cast<RE::BSShaderProperty*>(a_geom->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect].get());
			if (!prop)
				return GeomKind::Drawable;
			using enum RE::BSShaderProperty::EShaderPropertyFlag;
			if (prop->flags.any(kLODObjects, kHDLODObjects, kLODLandscape))
				return GeomKind::Lod;
			if (netimmerse_cast<RE::BSGrassShaderProperty*>(prop))
				return GeomKind::Grass;
			if (netimmerse_cast<RE::BSDistantTreeShaderProperty*>(prop))
				return GeomKind::Lod;
			return GeomKind::Drawable;
		}

		/// Drawable geometry in a subtree (not LOD, not grass, not hidden). Capped walk.
		uint32_t CountDrawableGeoms(RE::NiAVObject* a_root)
		{
			uint32_t count = 0;
			RE::NiAVObject* stack[256];
			int top = 0;
			int visits = 0;
			stack[top++] = a_root;
			while (top > 0 && visits < 4096) {
				RE::NiAVObject* o = stack[--top];
				++visits;
				if (!o || o->GetAppCulled())
					continue;
				if (auto* geom = o->AsGeometry()) {
					if (ClassifyGeometry(geom) == GeomKind::Drawable)
						++count;
					continue;
				}
				if (auto* node = o->AsNode()) {
					for (auto& child : node->GetChildren()) {
						if (child && top < 256)
							stack[top++] = child.get();
					}
				}
			}
			return count;
		}

		bool AncestorCounted(const std::unordered_set<const void*>& a_set, RE::NiAVObject* a_object)
		{
			if (a_set.empty())
				return false;
			int guard = 0;
			for (RE::NiAVObject* o = a_object; o && guard < 64; o = o->parent, ++guard) {
				if (a_set.contains(o))
					return true;
			}
			return false;
		}

		RE::BSMultiBoundAABB* GetAABBNode(RE::NiAVObject* a_object)
		{
			auto* mbn = netimmerse_cast<RE::BSMultiBoundNode*>(a_object);
			if (!mbn)
				return nullptr;
			auto* mb = mbn->GetRuntimeData().multiBound.get();
			if (!mb || !mb->data)
				return nullptr;
			return netimmerse_cast<RE::BSMultiBoundAABB*>(mb->data.get());
		}

		/// Records a would-be-culled object (raw and/or guarded verdict). Applies the exclusions.
		/// Returns nothing: this is the dry run, the caller always lets the engine proceed.
		void CountOccluded(RE::NiAVObject* a_object, bool a_guardedOccluded, bool a_container)
		{
			if (HasActorAncestor(a_object)) {
				g_counts.keptActor.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (auto* geom = a_object->AsGeometry()) {
				const GeomKind kind = ClassifyGeometry(geom);
				if (kind == GeomKind::Lod) {
					g_counts.keptLod.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				if (kind == GeomKind::Grass) {
					g_counts.keptGrass.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}

			bool countRaw = false, countGuarded = false;
			{
				std::lock_guard lock(g_countedLock);
				countRaw = !AncestorCounted(g_countedRaw, a_object);
				countGuarded = a_guardedOccluded && !AncestorCounted(g_countedGuarded, a_object);
				if (!countRaw && !countGuarded)
					return;
			}
			const uint32_t geoms = CountDrawableGeoms(a_object);
			if (geoms == 0) {
				if (countRaw && !a_container)
					g_counts.occludedEmpty.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			{
				std::lock_guard lock(g_countedLock);
				if (countRaw)
					g_countedRaw.insert(a_object);
				if (countGuarded)
					g_countedGuarded.insert(a_object);
			}
			if (countRaw) {
				if (!a_container)
					g_counts.occludedRaw.fetch_add(1, std::memory_order_relaxed);
				g_counts.geomsRaw.fetch_add(geoms, std::memory_order_relaxed);
			}
			if (countGuarded) {
				if (!a_container)
					g_counts.occludedGuarded.fetch_add(1, std::memory_order_relaxed);
				g_counts.geomsGuarded.fetch_add(geoms, std::memory_order_relaxed);
			}
		}

		/// The guarded verdict, given the raw one. Bigger bounds only ever keep more, so a raw
		/// keep is a guarded keep.
		template <class Retest>
		bool GuardedKeep(const FrameContext& c, bool a_rawKeep, const RE::NiPoint3& a_center, float a_radius, Retest&& a_retest)
		{
			if (a_rawKeep)
				return true;
			if (c.guard) {
				g_counts.keptMotion.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			if (c.camPos.GetDistance(a_center) - a_radius < g_settings.NearNoCullDistance) {
				g_counts.keptNear.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			return a_retest(c.margin);
		}

		void EvaluateObject(RE::NiAVObject* a_object)
		{
			const FrameContext* c = GetContext();
			if (!c)
				return;
			const auto& wb = a_object->worldBound;
			if (wb.radius < g_settings.MinTestRadius || a_object->GetAppCulled())
				return;

			const int64_t t0 = QpcNow();
			g_counts.objectsTested.fetch_add(1, std::memory_order_relaxed);
			auto* aabb = GetAABBNode(a_object);
			bool rawKeep, guardedKeep;
			if (aabb) {
				rawKeep = AabbKeep(*c, aabb->center, aabb->size, 0.0f);
				guardedKeep = GuardedKeep(*c, rawKeep, aabb->center, aabb->size.Length(),
					[&](float m) { return AabbKeep(*c, aabb->center, aabb->size, m); });
			} else {
				// Spheres of 5 units or less are never tested (the original's rule).
				rawKeep = wb.radius <= 5.0f || SphereKeep(*c, wb.center, wb.radius);
				guardedKeep = GuardedKeep(*c, rawKeep, wb.center, wb.radius,
					[&](float m) { return SphereKeep(*c, wb.center, wb.radius + m); });
			}
			if (!rawKeep)
				CountOccluded(a_object, !guardedKeep, false);
			g_counts.testTicks.fetch_add(QpcNow() - t0, std::memory_order_relaxed);
		}

		void EvaluateContainer(RE::BSMultiBound* a_bound)
		{
			const FrameContext* c = GetContext();
			if (!c)
				return;
			auto* aabb = netimmerse_cast<RE::BSMultiBoundAABB*>(a_bound->data.get());
			if (!aabb || aabb->size.z <= 1.0f)
				return;

			const int64_t t0 = QpcNow();
			g_counts.containersTested.fetch_add(1, std::memory_order_relaxed);
			const bool rawKeep = AabbKeep(*c, aabb->center, aabb->size, 0.0f);
			const bool guardedKeep = GuardedKeep(*c, rawKeep, aabb->center, aabb->size.Length(),
				[&](float m) { return AabbKeep(*c, aabb->center, aabb->size, m); });
			if (!rawKeep) {
				g_counts.containersOccludedRaw.fetch_add(1, std::memory_order_relaxed);
				if (!guardedKeep)
					g_counts.containersOccludedGuarded.fetch_add(1, std::memory_order_relaxed);
				// The node that owns this bound is the one whose Process1 is running here; its
				// contents are what a real cull would drop.
				auto* node = netimmerse_cast<RE::BSMultiBoundNode*>(t_current);
				if (node && node->GetRuntimeData().multiBound.get() == a_bound)
					CountOccluded(node, !guardedKeep, true);
			}
			g_counts.testTicks.fetch_add(QpcNow() - t0, std::memory_order_relaxed);
		}

		bool IsMainView(const RE::NiCullingProcess* a_process)
		{
			return a_process && a_process->camera && a_process->camera == GetMainCamera();
		}

		// ---------------------------------------------------------------------------------
		// Detours. Every one calls the original with its own arguments and returns the
		// original's result: the test result is only counted.
		// ---------------------------------------------------------------------------------
		template <class Self>
		void Process1Impl(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg)
		{
			if (!g_active.load(std::memory_order_relaxed) || !a_object || a_object == t_current) {
				Self::func(a_self, a_object, a_arg);
				return;
			}
			g_pipe.process1Calls.fetch_add(1, std::memory_order_relaxed);
			if (!IsMainView(a_self)) {
				Self::func(a_self, a_object, a_arg);
				return;
			}
			g_pipe.process1Main.fetch_add(1, std::memory_order_relaxed);
			RE::NiAVObject* previous = t_current;
			t_current = a_object;
			EvaluateObject(a_object);
			Self::func(a_self, a_object, a_arg);
			// Geometry the engine accepted (Process1 sets the accumulated bit when it keeps an
			// object and the process tracks it): the denominator of "draws per geometry".
			if (a_object->AsGeometry()) {
				const bool tracked = a_self->updateAccumulateFlag;
				if (!tracked || (a_object->GetFlags().underlying() & kAccumulatedBit))
					g_counts.visibleGeoms.fetch_add(1, std::memory_order_relaxed);
			}
			t_current = previous;
		}

		struct Process1_Base
		{
			static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				Process1Impl<Process1_Base>(a_self, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Process1_Parabolic
		{
			static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				Process1Impl<Process1_Parabolic>(a_self, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <class Self>
		bool TestBaseVisibility1Impl(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
		{
			const bool visible = Self::func(a_self, a_bound);
			if (visible && a_bound && g_active.load(std::memory_order_relaxed) && IsMainView(a_self))
				EvaluateContainer(a_bound);
			return visible;
		}

		struct TestBaseVis1_Base
		{
			static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
			{
				return TestBaseVisibility1Impl<TestBaseVis1_Base>(a_self, a_bound);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct TestBaseVis1_Parabolic
		{
			static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
			{
				return TestBaseVisibility1Impl<TestBaseVis1_Parabolic>(a_self, a_bound);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void DropSnapshots()
		{
			g_front.store(nullptr, std::memory_order_release);
			for (auto& slot : g_ring)
				slot.pending = false;
		}

		// ---------------------------------------------------------------------------------
		// Hi-Z build + readback (render thread)
		// ---------------------------------------------------------------------------------
		struct ComputeStateBackup
		{
			ID3D11ComputeShader* shader = nullptr;
			ID3D11Buffer* cb = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			ID3D11UnorderedAccessView* uavs[6]{};

			void Save(ID3D11DeviceContext* a_ctx)
			{
				a_ctx->CSGetShader(&shader, nullptr, nullptr);
				a_ctx->CSGetConstantBuffers(0, 1, &cb);
				a_ctx->CSGetShaderResources(0, 1, &srv);
				a_ctx->CSGetUnorderedAccessViews(0, 6, uavs);
			}
			void Restore(ID3D11DeviceContext* a_ctx)
			{
				a_ctx->CSSetShader(shader, nullptr, 0);
				a_ctx->CSSetConstantBuffers(0, 1, &cb);
				a_ctx->CSSetShaderResources(0, 1, &srv);
				a_ctx->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);
				if (shader)
					shader->Release();
				if (cb)
					cb->Release();
				if (srv)
					srv->Release();
				for (auto* u : uavs) {
					if (u)
						u->Release();
				}
			}
		};

		bool EnsureStaging(ID3D11Device* a_device, uint32_t a_w, uint32_t a_h)
		{
			if (a_w == g_stagingW && a_h == g_stagingH && g_ring[0].staging)
				return true;
			DropSnapshots();
			g_stagingW = g_stagingH = 0;
			D3D11_TEXTURE2D_DESC sd{};
			sd.Width = a_w;
			sd.Height = a_h;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.Format = DXGI_FORMAT_R32_FLOAT;
			sd.SampleDesc.Count = 1;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; i < kRing; ++i) {
				g_ring[i].staging = nullptr;
				if (FAILED(a_device->CreateTexture2D(&sd, nullptr, g_ring[i].staging.put()))) {
					logger::error("[OcclusionDryRun] staging texture create failed");
					return false;
				}
				Util::SetResourceName(g_ring[i].staging.get(), "OcclusionDryRun::HiZStaging%d", i);
			}
			g_stagingW = a_w;
			g_stagingH = a_h;
			logger::info("[OcclusionDryRun] Hi-Z readback ring {}x{} x{}", a_w, a_h, kRing);
			return true;
		}

		void Harvest(ID3D11DeviceContext* a_ctx, RingSlot& a_slot)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			++g_pipe.readTries;
			if (const HRESULT hr = a_ctx->Map(a_slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); FAILED(hr)) {
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
					++g_pipe.readBusy;
				} else {
					// Not "busy": this slot will never map. Drop it so the ring keeps moving.
					++g_pipe.readErrors;
					a_slot.pending = false;
				}
				return;  // still in flight: try again next frame, never wait
			}
			++g_pipe.readOk;
			Snapshot& snap = g_snaps[g_snapWrite];
			snap.grid.resize(static_cast<size_t>(a_slot.gridW) * a_slot.gridH);
			for (uint32_t y = 0; y < a_slot.gridH; ++y)
				std::memcpy(&snap.grid[static_cast<size_t>(y) * a_slot.gridW], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
					static_cast<size_t>(a_slot.gridW) * sizeof(float));
			a_ctx->Unmap(a_slot.staging.get(), 0);
			snap.gridW = a_slot.gridW;
			snap.gridH = a_slot.gridH;
			snap.renderW = a_slot.renderW;
			snap.renderH = a_slot.renderH;
			snap.view = a_slot.view;
			snap.viewProj = a_slot.viewProj;
			snap.pos = a_slot.pos;
			snap.dir = a_slot.dir;
			snap.captureFrame = a_slot.frame;
			g_front.store(&snap, std::memory_order_release);
			g_snapWrite = (g_snapWrite + 1) % kSnapshots;
			a_slot.pending = false;
		}
	}

	Settings& GetSettings() { return g_settings; }
	const Report& GetReport() { return g_report; }

	void Install()
	{
		static bool once = false;
		if (once)
			return;
		once = true;

		if (REL::Module::IsVR()) {
			g_notInstalledReason = "Not available in VR";
			logger::info("[OcclusionDryRun] VR: not installed");
			return;
		}
		if (!REL::Module::IsSE()) {
			g_notInstalledReason = "Only on Skyrim SE 1.5.97 (engine addresses for this version are not verified)";
			logger::info("[OcclusionDryRun] not SE 1.5.97: not installed");
			return;
		}
		stl::detour_thunk<Process1_Base>(REL::RelocationID(74804, 74804));
		stl::detour_thunk<Process1_Parabolic>(REL::RelocationID(101597, 101597));
		stl::detour_thunk<TestBaseVis1_Base>(REL::RelocationID(74816, 74816));
		stl::detour_thunk<TestBaseVis1_Parabolic>(REL::RelocationID(101605, 101605));
		g_installed = true;
		logger::info("[OcclusionDryRun] culling detours installed (dry run: nothing is ever culled)");
	}

	void OnEndDeferred()
	{
		// (batch 37b, D) Record that the world was rendered this frame, before anything can return.
		g_lastWorldFrame = g_frame.load(std::memory_order_relaxed);
		++g_pipe.worldFrames;

		if (!g_active.load(std::memory_order_relaxed))
			return;
		++g_pipe.activeFrames;
		auto* device = globals::d3d::device;
		auto* ctx = globals::d3d::context;
		auto* cam = GetMainCamera();
		if (!device || !ctx || !cam) {
			++g_pipe.buildFails;
			g_pipe.lastFail = !cam ? "no main camera" : "no D3D device";
			return;
		}

		if (!g_pyramid) {
			g_pyramid = std::make_unique<HiZPyramid>();
			g_pyramid->SetupResources();
		}

		// 1. The oldest slot: read it back if the GPU is done with it.
		RingSlot& slot = g_ring[g_ringWrite];
		if (slot.pending && slot.staging)
			Harvest(ctx, slot);
		if (slot.pending) {
			++g_pipe.ringFull;
			return;  // still in flight; this frame's capture is skipped, the ring catches up
		}

		// 2. Reduce this frame's depth (opaque pass complete) and queue its 1/16 level.
		ComputeStateBackup backup;
		backup.Save(ctx);
		const bool built = g_pyramid->Build(device, ctx);
		backup.Restore(ctx);
		ID3D11Texture2D* tex = g_pyramid->GetTexture();
		if (!built || !tex || g_pyramid->GetMipCount() <= static_cast<uint32_t>(kHiZMip)) {
			++g_pipe.buildFails;
			g_pipe.lastFail = !built ? "Hi-Z build failed (depth or shader unavailable)" : "Hi-Z has too few mip levels";
			return;
		}

		D3D11_TEXTURE2D_DESC td{};
		tex->GetDesc(&td);
		const uint32_t mipW = std::max(1u, td.Width >> kHiZMip), mipH = std::max(1u, td.Height >> kHiZMip);
		if (!EnsureStaging(device, mipW, mipH)) {
			++g_pipe.buildFails;
			g_pipe.lastFail = "readback texture could not be created";
			return;
		}

		const uint32_t gridW = std::min(mipW, (g_pyramid->GetWidth() + 3) / 4);
		const uint32_t gridH = std::min(mipH, (g_pyramid->GetHeight() + 3) / 4);
		const D3D11_BOX box{ 0, 0, 0, gridW, gridH, 1 };
		RingSlot& target = g_ring[g_ringWrite];
		ctx->CopySubresourceRegion(target.staging.get(), 0, 0, 0, 0, tex, D3D11CalcSubresource(kHiZMip, 0, td.MipLevels), &box);

		XMMATRIX v{}, vp{};
		CalculateViewProjection(cam, v, vp);
		target.view = v;
		target.viewProj = vp;
		target.pos = cam->world.translate;
		target.dir = CameraForward(cam);
		target.gridW = gridW;
		target.gridH = gridH;
		target.renderW = g_pyramid->GetSourceWidth();
		target.renderH = g_pyramid->GetSourceHeight();
		target.frame = g_frame.load(std::memory_order_relaxed);
		target.pending = true;
		g_ringWrite = (g_ringWrite + 1) % kRing;
		++g_pipe.builds;
	}

	void OnFrameEnd(uint32_t a_mainViewDraws, bool a_drawsValid)
	{
		// ---- close this frame's counts ----
		const bool wasActive = g_active.load(std::memory_order_relaxed);
		Counts n;
		n.objectsTested = g_counts.objectsTested.exchange(0);
		n.containersTested = g_counts.containersTested.exchange(0);
		n.occludedRaw = g_counts.occludedRaw.exchange(0);
		n.occludedGuarded = g_counts.occludedGuarded.exchange(0);
		n.occludedEmpty = g_counts.occludedEmpty.exchange(0);
		n.containersOccludedRaw = g_counts.containersOccludedRaw.exchange(0);
		n.containersOccludedGuarded = g_counts.containersOccludedGuarded.exchange(0);
		n.geomsRaw = g_counts.geomsRaw.exchange(0);
		n.geomsGuarded = g_counts.geomsGuarded.exchange(0);
		n.visibleGeoms = g_counts.visibleGeoms.exchange(0);
		n.keptActor = g_counts.keptActor.exchange(0);
		n.keptLod = g_counts.keptLod.exchange(0);
		n.keptGrass = g_counts.keptGrass.exchange(0);
		n.keptNear = g_counts.keptNear.exchange(0);
		n.keptMotion = g_counts.keptMotion.exchange(0);
		n.testMs = static_cast<double>(g_counts.testTicks.exchange(0)) * QpcToMs();
		{
			std::lock_guard lock(g_countedLock);
			g_countedRaw.clear();
			g_countedGuarded.clear();
		}

		Report& r = g_report;
		const bool ctxValid = g_ctxReady.load(std::memory_order_relaxed) == g_frame.load(std::memory_order_relaxed) && g_ctxValid.load(std::memory_order_relaxed);
		if (wasActive && ctxValid && n.objectsTested > 0) {
			r.last = n;
			r.motionGuard = g_ctx.guard;
			r.cameraMove = g_ctx.move;
			r.cameraTurn = g_ctx.turn;
			r.latencyFrames = g_ctx.age;
			if (g_ctx.snap) {
				r.gridW = g_ctx.snap->gridW;
				r.gridH = g_ctx.snap->gridH;
				r.renderW = g_ctx.snap->renderW;
				r.renderH = g_ctx.snap->renderH;
			}
			r.mainViewDraws = a_mainViewDraws;
			const float measured = (a_drawsValid && n.visibleGeoms >= 50) ? static_cast<float>(a_mainViewDraws) / static_cast<float>(n.visibleGeoms) : 0.0f;
			r.drawsPerGeomMeasured = measured >= 1.0f && measured <= 8.0f;
			r.drawsPerGeom = r.drawsPerGeomMeasured ? measured : 2.0f;  // assumed: depth prepass + G-buffer

			const bool first = r.frames == 0;
			const float a = first ? 1.0f : 0.05f;
			const auto blend = [a](float& o_avg, float v) { o_avg += a * (v - o_avg); };
			blend(r.objectsTested, static_cast<float>(n.objectsTested));
			blend(r.occludedRaw, static_cast<float>(n.occludedRaw));
			blend(r.occludedGuarded, static_cast<float>(n.occludedGuarded));
			blend(r.geomsRaw, static_cast<float>(n.geomsRaw));
			blend(r.geomsGuarded, static_cast<float>(n.geomsGuarded));
			blend(r.containersTested, static_cast<float>(n.containersTested));
			blend(r.containersOccludedRaw, static_cast<float>(n.containersOccludedRaw));
			blend(r.containersOccludedGuarded, static_cast<float>(n.containersOccludedGuarded));
			blend(r.estDrawsRaw, static_cast<float>(n.geomsRaw) * r.drawsPerGeom);
			blend(r.estDrawsGuarded, static_cast<float>(n.geomsGuarded) * r.drawsPerGeom);
			blend(r.testMs, static_cast<float>(n.testMs));
			blend(r.motionGuardShare, g_ctx.guard ? 1.0f : 0.0f);
			++r.frames;
		}

		// ---- decide the next frame ----
		g_frame.fetch_add(1, std::memory_order_acq_rel);

		// (batch 37b, D) "In the world" = the world was rendered within the last few frames (see
		// g_lastWorldFrame). The 37a test read State::inWorld here, which is false at Present on
		// every frame, so `active` never became true and the status sat on "Waiting for the first
		// depth readback" (the status chain below never checked it).
		const bool inWorld = g_pipe.worldFrames > 0 && g_frame.load(std::memory_order_relaxed) - g_lastWorldFrame <= kWorldGraceFrames;
		bool active = g_installed && g_settings.Enabled && OverlayOnScreen() && inWorld;
		const bool loading = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
		const uint64_t frame = g_frame.load(std::memory_order_relaxed);
		if (loading) {
			g_wasLoading = true;
			active = false;
		} else if (g_wasLoading) {
			g_wasLoading = false;
			g_settleUntil = frame + static_cast<uint64_t>(std::max(0, g_settings.SettleFrames));
		}
		const bool settling = frame < g_settleUntil;
		if (settling)
			active = false;
		if (!active && wasActive) {
			DropSnapshots();
			r.frames = 0;
		}
		g_active.store(active, std::memory_order_relaxed);

		r.installed = g_installed;
		r.active = active;
		if (!g_installed)
			r.status = g_notInstalledReason;
		else if (!g_settings.Enabled)
			r.status = "Off";
		else if (!OverlayOnScreen())
			r.status = "Paused while the overlay is hidden";
		else if (loading)
			r.status = "Paused during loading";
		else if (settling)
			r.status = std::format("Waiting after loading ({} frames)", g_settleUntil - frame);
		else if (!inWorld)
			r.status = "Paused: the world is not being rendered";
		else if (!g_front.load(std::memory_order_relaxed)) {
			if (g_pipe.builds == 0 && g_pipe.buildFails > 0)
				r.status = std::format("Waiting: depth pyramid not built ({})", g_pipe.lastFail);
			else if (g_pipe.builds == 0)
				r.status = "Waiting: depth pyramid not built yet";
			else
				r.status = "Waiting for the first depth readback";
		} else if (r.frames == 0 && g_pipe.process1Main.load(std::memory_order_relaxed) == 0)
			r.status = "Waiting: no main-view culling seen yet";
		else
			r.status = "Running (counting only, nothing is skipped)";

		r.pipeline = {
			.worldFrames = g_pipe.worldFrames,
			.activeFrames = g_pipe.activeFrames,
			.builds = g_pipe.builds,
			.buildFails = g_pipe.buildFails,
			.readTries = g_pipe.readTries,
			.readOk = g_pipe.readOk,
			.readBusy = g_pipe.readBusy,
			.readErrors = g_pipe.readErrors,
			.ringFull = g_pipe.ringFull,
			.process1Calls = g_pipe.process1Calls.load(std::memory_order_relaxed),
			.process1Main = g_pipe.process1Main.load(std::memory_order_relaxed),
			.lastFail = g_pipe.lastFail,
		};
	}

	void Load(const nlohmann::json& a_json)
	{
		if (!a_json.is_object())
			return;
		Settings d;
		const auto num = [&a_json](const char* a_key, float a_def, float a_lo, float a_hi) {
			if (a_json.contains(a_key) && a_json[a_key].is_number())
				return std::clamp(a_json[a_key].get<float>(), a_lo, a_hi);
			return a_def;
		};
		if (a_json.contains("Enabled") && a_json["Enabled"].is_boolean())
			d.Enabled = a_json["Enabled"].get<bool>();
		d.MotionGuardDistance = num("MotionGuardDistance", d.MotionGuardDistance, 0.0f, 1000.0f);
		d.MotionGuardAngle = num("MotionGuardAngle", d.MotionGuardAngle, 0.0f, 90.0f);
		d.BoundsMargin = num("BoundsMargin", d.BoundsMargin, 0.0f, 500.0f);
		d.MotionMarginScale = num("MotionMarginScale", d.MotionMarginScale, 0.0f, 10.0f);
		d.NearNoCullDistance = num("NearNoCullDistance", d.NearNoCullDistance, 0.0f, 10000.0f);
		d.MinTestRadius = num("MinTestRadius", d.MinTestRadius, 0.0f, 1000.0f);
		d.SettleFrames = static_cast<int>(num("SettleFrames", static_cast<float>(d.SettleFrames), 0.0f, 2000.0f));
		g_settings = d;
	}

	void Save(nlohmann::json& a_json)
	{
		a_json = nlohmann::json{
			{ "Enabled", g_settings.Enabled },
			{ "MotionGuardDistance", g_settings.MotionGuardDistance },
			{ "MotionGuardAngle", g_settings.MotionGuardAngle },
			{ "BoundsMargin", g_settings.BoundsMargin },
			{ "MotionMarginScale", g_settings.MotionMarginScale },
			{ "NearNoCullDistance", g_settings.NearNoCullDistance },
			{ "MinTestRadius", g_settings.MinTestRadius },
			{ "SettleFrames", g_settings.SettleFrames },
		};
	}

	void DrawPanel(bool a_menuOpen)
	{
		const Report& r = g_report;
		const auto tooltip = [](const char* a_text) {
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(a_text);
			}
		};

		ImGui::TextWrapped("%s", r.status.c_str());
		tooltip(
			"A test of how many objects an occlusion cull could skip: every object in the main view is checked against "
			"the depth of a frame a few frames ago, and the result is only counted. Nothing is hidden; the picture is "
			"exactly what it is without this.");

		if (r.installed) {
			// (batch 37b, D) One line per pipeline step, so a stall shows where it is.
			const auto& p = r.pipeline;
			ImGui::TextDisabled("Steps: world %llu (running %llu) | depth built %llu, failed %llu | readback ok %llu, busy %llu, error %llu | culling %llu, main view %llu",
				p.worldFrames, p.activeFrames, p.builds, p.buildFails, p.readOk, p.readBusy, p.readErrors, p.process1Calls, p.process1Main);
			tooltip(
				"Counters since the game started, in pipeline order. world = frames the world was drawn; running = of those, "
				"with this test on; depth built = depth snapshots queued; readback ok = snapshots that came back from the GPU "
				"(busy = not finished yet, tried again next frame); culling = engine culling calls seen while running, main "
				"view = those for the player's camera. The first number that stays at 0 is where it is stuck.");
			if (p.buildFails > 0 && p.lastFail && *p.lastFail)
				ImGui::TextDisabled("Last depth failure: %s", p.lastFail);
		}

		if (r.installed && r.frames > 0 && ImGui::BeginTable("OcclusionDryRun", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("##what");
			ImGui::TableSetupColumn("Raw");
			ImGui::TableSetupColumn("Guarded");
			ImGui::TableHeadersRow();
			const auto pct = [](float a_part, float a_whole) { return a_whole > 0.0f ? a_part / a_whole * 100.0f : 0.0f; };

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Objects tested");
			tooltip("Objects of the main view the test looked at this frame (smoothed). Containers (rooms, cells, building shells) in brackets.");
			ImGui::TableNextColumn();
			ImGui::Text("%.0f (+%.0f)", r.objectsTested, r.containersTested);
			ImGui::TableNextColumn();
			ImGui::TextDisabled("same");

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Could be culled");
			tooltip(
				"Objects that are hidden behind something nearer and hold something drawable.\n"
				"Raw: the original rules. Guarded: with the camera-motion guard, extra bound margin and near no-cull distance below.");
			ImGui::TableNextColumn();
			ImGui::Text("%.0f (%.1f%%)", r.occludedRaw, pct(r.occludedRaw, r.objectsTested));
			ImGui::TableNextColumn();
			ImGui::Text("%.0f (%.1f%%)", r.occludedGuarded, pct(r.occludedGuarded, r.objectsTested));

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Draws saved (est.)");
			tooltip(
				"Drawable pieces under the cullable objects, times how many draws one piece costs in the main view "
				"(depth prepass + solid pass). An estimate, not a count.");
			ImGui::TableNextColumn();
			ImGui::Text("~%.0f", r.estDrawsRaw);
			ImGui::TableNextColumn();
			ImGui::Text("~%.0f", r.estDrawsGuarded);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Containers culled");
			ImGui::TableNextColumn();
			ImGui::Text("%.0f", r.containersOccludedRaw);
			ImGui::TableNextColumn();
			ImGui::Text("%.0f", r.containersOccludedGuarded);
			ImGui::EndTable();

			ImGui::Text("Depth is %d frames old | camera moved %.0f, turned %.1f deg%s", r.latencyFrames, r.cameraMove, r.cameraTurn,
				r.motionGuard ? " | motion guard ON" : "");
			tooltip(
				"How old the depth used for the test is (readback delay), and how far the camera moved since then. "
				"While the motion guard is on, the guarded column culls nothing.");
			ImGui::Text("Kept by rules: actors %u, LOD %u, grass %u, near %u, motion %u", r.last.keptActor, r.last.keptLod, r.last.keptGrass,
				r.last.keptNear, r.last.keptMotion);
			tooltip("Objects the raw test would cull but a rule keeps (last frame). Actors, LOD and grass are never culled.");
			ImGui::Text("Test cost %.2f ms CPU (all threads) | %.2f draws per piece%s", r.testMs, r.drawsPerGeom,
				r.drawsPerGeomMeasured ? "" : " (assumed)");
		}

		if (a_menuOpen && r.installed) {
			ImGui::PushID("OcclusionDryRunSettings");
			ImGui::Checkbox("Run the test", &g_settings.Enabled);
			tooltip("Counting only. Off removes even the counting work.");
			ImGui::SliderFloat("Motion guard distance", &g_settings.MotionGuardDistance, 0.0f, 300.0f, "%.0f units");
			tooltip("If the camera moved more than this since the depth was taken, the guarded column culls nothing that frame.");
			ImGui::SliderFloat("Motion guard angle", &g_settings.MotionGuardAngle, 0.0f, 30.0f, "%.1f deg");
			ImGui::SliderFloat("Bound margin", &g_settings.BoundsMargin, 0.0f, 200.0f, "%.0f units");
			tooltip("Every object is made this much bigger before the guarded test. Bigger = safer, fewer culls.");
			ImGui::SliderFloat("Margin per unit moved", &g_settings.MotionMarginScale, 0.0f, 4.0f, "%.2f");
			ImGui::SliderFloat("Near no-cull distance", &g_settings.NearNoCullDistance, 0.0f, 3000.0f, "%.0f units");
			tooltip("The guarded column never culls anything closer than this.");
			ImGui::PopID();
		}
	}

	nlohmann::json ToJson()
	{
		const Report& r = g_report;
		const Counts& n = r.last;
		const auto pct = [](float a_part, float a_whole) { return a_whole > 0.0f ? a_part / a_whole * 100.0f : 0.0f; };
		nlohmann::json j;
		j["installed"] = r.installed;
		j["active"] = r.active;
		j["status"] = r.status;
		j["frames_counted"] = r.frames;
		{
			const auto& p = r.pipeline;
			j["pipeline"] = {
				{ "world_frames", p.worldFrames },
				{ "active_frames", p.activeFrames },
				{ "hiz_builds", p.builds },
				{ "hiz_build_fails", p.buildFails },
				{ "last_fail", p.lastFail ? p.lastFail : "" },
				{ "readback_tries", p.readTries },
				{ "readback_ok", p.readOk },
				{ "readback_busy", p.readBusy },
				{ "readback_errors", p.readErrors },
				{ "ring_full_skips", p.ringFull },
				{ "culling_calls", p.process1Calls },
				{ "culling_calls_main_view", p.process1Main },
			};
		}
		j["settings"] = nlohmann::json::object();
		Save(j["settings"]);
		j["snapshot"] = { { "latency_frames", r.latencyFrames }, { "grid", { r.gridW, r.gridH } }, { "render", { r.renderW, r.renderH } },
			{ "camera_move_units", r.cameraMove }, { "camera_turn_deg", r.cameraTurn }, { "motion_guard", r.motionGuard } };
		j["smoothed"] = {
			{ "objects_tested", r.objectsTested },
			{ "occluded_raw", r.occludedRaw },
			{ "occluded_guarded", r.occludedGuarded },
			{ "occluded_raw_pct", pct(r.occludedRaw, r.objectsTested) },
			{ "occluded_guarded_pct", pct(r.occludedGuarded, r.objectsTested) },
			{ "geoms_raw", r.geomsRaw },
			{ "geoms_guarded", r.geomsGuarded },
			{ "est_draws_saved_raw", r.estDrawsRaw },
			{ "est_draws_saved_guarded", r.estDrawsGuarded },
			{ "containers_tested", r.containersTested },
			{ "containers_occluded_raw", r.containersOccludedRaw },
			{ "containers_occluded_guarded", r.containersOccludedGuarded },
			{ "test_cpu_ms", r.testMs },
			{ "motion_guard_share", r.motionGuardShare },
		};
		j["last_frame"] = {
			{ "objects_tested", n.objectsTested },
			{ "containers_tested", n.containersTested },
			{ "occluded_raw", n.occludedRaw },
			{ "occluded_guarded", n.occludedGuarded },
			{ "occluded_nothing_drawable", n.occludedEmpty },
			{ "containers_occluded_raw", n.containersOccludedRaw },
			{ "containers_occluded_guarded", n.containersOccludedGuarded },
			{ "geoms_raw", n.geomsRaw },
			{ "geoms_guarded", n.geomsGuarded },
			{ "visible_geoms_main_view", n.visibleGeoms },
			{ "main_view_draws", r.mainViewDraws },
			{ "draws_per_geom", r.drawsPerGeom },
			{ "draws_per_geom_measured", r.drawsPerGeomMeasured },
			{ "est_draws_saved_raw", static_cast<float>(n.geomsRaw) * r.drawsPerGeom },
			{ "est_draws_saved_guarded", static_cast<float>(n.geomsGuarded) * r.drawsPerGeom },
			{ "kept_actor", n.keptActor },
			{ "kept_lod", n.keptLod },
			{ "kept_grass", n.keptGrass },
			{ "kept_near", n.keptNear },
			{ "kept_motion", n.keptMotion },
			{ "test_cpu_ms", n.testMs },
		};
		return j;
	}
}
