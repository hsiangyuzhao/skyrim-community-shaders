#include "GrassOptimizations.h"

#include "Features/PerformanceOverlay.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/GpuTimers.h"
#include "Utils/WinApi.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassOptimizations::Settings,
	Enabled,
	MinPixelSize,
	FullDetailPixelSize,
	MinDensity,
	MeshCostBias,
	CostBiasStartDistance,
	InvisibleFadeCull,
	RenderDistanceOverride,
	EdgeFadeStart,
	EnableOcclusionCulling,
	OcclusionBias,
	SimpleShadingPixelSize,
	CollisionDistance,
	EnableMeshLOD,
	EnableMidLOD,
	MidLODPixelSize,
	EnableFarLOD,
	FarLODPixelSize,
	MeshLODBandPixels)

namespace
{
	constexpr uint32_t kOptimizedFlag = static_cast<uint32_t>(SIE::ShaderCache::GrassShaderFlags::Optimized);

	/** @brief Flushes the engine's dirty render state before a draw. Our BSGraphics::SetDirtyStates hook also counts it as one draw call for the Performance Overlay. */
	void SetDirtyStates()
	{
		static REL::Relocation<void (*)(uint32_t)> setDirtyStates{ REL::RelocationID(75580, 77386) };
		setDirtyStates(0);
	}

	bool IsGrassProperty(const RE::BSShaderProperty* a_property)
	{
		return a_property && a_property->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get();
	}

	/** @brief Closes a GPU pass-timer interval on every exit path. */
	struct GpuTimerScope
	{
		GpuTimerScope() { Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::GrassOptimizations); }
		~GpuTimerScope() { Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::GrassOptimizations); }
		GpuTimerScope(const GpuTimerScope&) = delete;
		GpuTimerScope& operator=(const GpuTimerScope&) = delete;
	};
}

void GrassOptimizations::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassOptimizations::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassOptimizations::RestoreDefaultSettings()
{
	settings = {};
}

void GrassOptimizations::DrawSettings()
{
	ImGui::Checkbox("Enable Optimized Grass Rendering", &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Switches the optimized grass path on and off without restarting, for same-session A/B comparison.\n"
			"Off, every grass shape is culled and drawn by the game exactly as without this feature.\n"
			"Compare the Grass row and the Grass Optimizations block of the Performance Overlay in both states.");
	}

	if (settings.Enabled) {
		if (!ctx1)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Unavailable: ID3D11DeviceContext1 missing; grass is drawn by the game.");
		else if (cullCSFailed)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Unavailable: the culling shader failed to compile; grass is drawn by the game.");
		else if (!shadersReady)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Compiling optimized grass shaders; grass is drawn by the game until they are ready.");
	}

	ImGui::SeparatorText("Culling & LOD");

	ImGui::SliderFloat("Full-Detail Pixel Size", &settings.FullDetailPixelSize, 4.0f, 128.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Instances whose on-screen radius is above this render at full density. Below it, density is increasingly thinned down to Minimum Density at Min Pixel Size. Increasing this setting improves performance by removing closer grass.");
	}

	ImGui::SliderFloat("Min Pixel Size", &settings.MinPixelSize, 1.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Individual grass instances that visibly take up less space on the screen than this are dropped entirely. Higher values improve performance by removing far-away grass instances sooner.");
	}

	Util::PercentageSlider("Minimum Density", &settings.MinDensity);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("The percentage of grass that remains at the smallest (Min Pixel Size) LOD level before culling.");
	}

	Util::PercentageSlider("Mesh Cost Bias", &settings.MeshCostBias);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Culls or removes grass meshes based on their complexity (performance impact). At 0, removal is identical between all grass types regardless of complexity. At 1, heavier and more complex meshes are culled 2-6x sooner than simple ones. Only applies beyond the Cost Bias Start Distance, so nearby grass is never thinned.");
	}

	ImGui::SliderFloat("Cost Bias Start Distance", &settings.CostBiasStartDistance, 0.0f, 20000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ "Distance at which Mesh Cost Bias starts taking effect, ramping to full over the same distance again. Nearer than this, all grass types are treated identically no matter how complex. Zero applies the bias everywhere, including right in front of the player.",
			Util::Units::FormatDistance(settings.CostBiasStartDistance) });
	}

	ImGui::SliderFloat("Grass Render Distance", &settings.RenderDistanceOverride, 0.0f, 100000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Max grass render distance in units. 0 = use the game's INI cap (fGrassStartFadeDistance + fGrassFadeRange). Any grass beyond the vanilla range or this range will be removed.");
	}

	Util::PercentageSlider("Edge Fade Start", &settings.EdgeFadeStart);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ "Percent of the grass render distance at which grass starts fading out. The default of 85% fades over the last 15%. A lower value results in a longer, smoother fade out, while 100% disables the fade and grass pops out at the render distance.",
			Util::Units::FormatDistance(maxGrassDistance * settings.EdgeFadeStart) });
	}

	ImGui::SliderFloat("Invisible Fade Cull", &settings.InvisibleFadeCull, 0.0f, 0.5f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Skip drawing grass whose transparency is below this threshold. Grass with a fade value of zero is completely invisible and thus is removed early for performance reasons.");
	}

	ImGui::Checkbox("Occlusion Culling", &settings.EnableOcclusionCulling);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Skips grass hidden behind rocks, buildings and NPCs. Depending on how much grass is not visible, this may cost more than its benefits. If you see grass flickering when moving, try disabling this.");
	}

	ImGui::SliderFloat("Occlusion Bias", &settings.OcclusionBias, 0.0f, 0.05f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("How far behind an occluder grass must sit before Occlusion Culling removes it. Raise this if grass disappears around the edges of rocks and hills, lower it to reclaim more performance. Has no effect unless Occlusion Culling is enabled.");
	}

	ImGui::SliderFloat("Simple Shading Below", &settings.SimpleShadingPixelSize, 0.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Grass instances smaller than this size on screen will skip barely visible detail including contact shadows, specular highlights, and other complex grass visual elements. Zero disables this feature.");
	}

	ImGui::SliderFloat("Collision Distance", &settings.CollisionDistance, 0.0f, 8192.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ "Grass beyond this distance skips any collision detection. Zero disables collision on all grass. Requires the Grass Collision feature.",
			Util::Units::FormatDistance(settings.CollisionDistance) });
	}

	ImGui::SeparatorText("Mesh LOD");

	ImGui::Checkbox("Enable Mesh LOD", &settings.EnableMeshLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Improves performance by swapping distant grass instances for a simpler LOD mesh, in two bands. Requires an LOD .nif per grass type at meshes\\LOD\\Grass\\<source-mesh-name>_LOD0.nif, plus an optional _LOD1.nif for the far band. Grass with no LOD mesh keeps its full mesh.");
	}

	ImGui::BeginDisabled(!settings.EnableMeshLOD);

	ImGui::Checkbox("Enable Middle LOD", &settings.EnableMidLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Swaps mid-distance grass to the _LOD0.nif mesh. With this off, grass stays on its full mesh until the far band takes over.");
	}

	ImGui::SliderFloat("Middle LOD Pixel Size", &settings.MidLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Instances whose on-screen radius is below this but above the Far LOD Pixel Size swap to the _LOD0.nif mesh.");
	}

	ImGui::Checkbox("Enable Far LOD", &settings.EnableFarLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Swaps the most distant grass to the _LOD1.nif mesh. Grass types without that file reuse their _LOD0.nif, so the far band still gets its own brightness.");
	}

	ImGui::SliderFloat("Far LOD Pixel Size", &settings.FarLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Instances whose on-screen radius is below this but above Min Pixel Size swap to the _LOD1.nif mesh. Values above the Middle LOD Pixel Size are clamped to it, since the far band is always the more distant of the two.");
	}

	ImGui::SliderFloat("Mesh LOD Transition Band", &settings.MeshLODBandPixels, 0.0f, 16.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("The range of on-screen sizes over which a random amount of meshes are swapped out before completely transitioning to the next LOD. Applies to both transitions. A wider range results in a smoother transition.");
	}

	ImGui::EndDisabled();
}

void GrassOptimizations::PostPostLoad()
{
	// Upstream #2716: SexLabUtil.dll before 2.0 makes grass stutter with this feature. Upstream refuses to
	// load Community Shaders at all; here only this feature stands down, so the rest keeps working.
	if (auto sexLabUtil = Util::GetDllVersion(L"Data/SKSE/Plugins/SexLabUtil.dll"); sexLabUtil && sexLabUtil->major() < 2) {
		failedLoadedMessage = "Incompatible version of SexLabUtil.dll detected (causes grass stutter). Use SexLab P+ instead.";
		logger::error("[GRASS OPTIMIZATIONS] {}", failedLoadedMessage);
		loaded = false;
		return;
	}

	Hooks::Install();
}

void GrassOptimizations::ComputeFrustumPlanes(RE::NiFrustumPlanes& out, const RE::NiFrustum& viewFrustum, const RE::NiTransform& transform)
{
	const __m128 fwd = _mm_set_ps(0.0f, transform.rotate.entry[2][0], transform.rotate.entry[1][0], transform.rotate.entry[0][0]);
	const __m128 col1 = _mm_set_ps(0.0f, transform.rotate.entry[2][1], transform.rotate.entry[1][1], transform.rotate.entry[0][1]);
	const __m128 col2 = _mm_set_ps(0.0f, transform.rotate.entry[2][2], transform.rotate.entry[1][2], transform.rotate.entry[0][2]);
	const __m128 trans = _mm_set_ps(0.0f, transform.translate.z, transform.translate.y, transform.translate.x);

	const __m128 nearPt = _mm_add_ps(trans, _mm_mul_ps(fwd, _mm_set1_ps(viewFrustum.fNear)));
	const __m128 farPt = _mm_add_ps(trans, _mm_mul_ps(fwd, _mm_set1_ps(viewFrustum.fFar)));

	auto MakePlane = [&](int idx, __m128 normal, __m128 point) {
		alignas(16) float n[4];
		_mm_store_ps(n, normal);
		out.cullingPlanes[idx].normal = { n[0], n[1], n[2] };
		out.cullingPlanes[idx].constant = _mm_cvtss_f32(_mm_dp_ps(normal, point, 0x71));
	};

	MakePlane(0, fwd, nearPt);
	const __m128 negFwd = _mm_xor_ps(fwd, _mm_set1_ps(-0.0f));
	MakePlane(1, negFwd, farPt);

	if (viewFrustum.bOrtho) {
		__m128 leftVec = col2;
		MakePlane(2, leftVec, _mm_add_ps(trans, _mm_mul_ps(leftVec, _mm_set1_ps(viewFrustum.fLeft))));
		__m128 rightVec = _mm_xor_ps(col2, _mm_set1_ps(-0.0f));
		MakePlane(3, rightVec, _mm_add_ps(trans, _mm_mul_ps(rightVec, _mm_set1_ps(viewFrustum.fRight))));
		__m128 upVec = col1;
		MakePlane(4, upVec, _mm_add_ps(trans, _mm_mul_ps(upVec, _mm_set1_ps(viewFrustum.fTop))));
		__m128 botVec = _mm_xor_ps(col1, _mm_set1_ps(-0.0f));
		MakePlane(5, botVec, _mm_add_ps(trans, _mm_mul_ps(botVec, _mm_set1_ps(viewFrustum.fBottom))));
	} else {
		// SetFrustrumPlanes: s = 1/sqrt(slope²+1); n = fwd*(±slope*s) + axis*(±s)
		auto sidePlane = [&](int idx, __m128 axis, float slope, float fwdSign, float axisSign) {
			const float s = 1.0f / std::sqrt(slope * slope + 1.0f);
			__m128 n = _mm_add_ps(
				_mm_mul_ps(fwd, _mm_set1_ps(fwdSign * slope * s)),
				_mm_mul_ps(axis, _mm_set1_ps(axisSign * s)));
			MakePlane(idx, n, trans);
		};

		sidePlane(2, col2, viewFrustum.fLeft, -1.0f, +1.0f);
		sidePlane(3, col2, viewFrustum.fRight, +1.0f, -1.0f);
		sidePlane(4, col1, viewFrustum.fTop, +1.0f, -1.0f);
		sidePlane(5, col1, viewFrustum.fBottom, -1.0f, +1.0f);
	}

	out.activePlanes = RE::NiFrustumPlanes::ActivePlane(0x3F);

	constexpr float edgePadding = 128.0f;
	out.cullingPlanes[2].constant -= edgePadding;
	out.cullingPlanes[3].constant -= edgePadding;
	out.cullingPlanes[4].constant -= edgePadding;
	out.cullingPlanes[5].constant -= edgePadding;
}

bool GrassOptimizations::StatsWanted()
{
	auto& overlay = globals::features::performanceOverlay;
	return globals::menu && globals::menu->overlayVisible && overlay.loaded && overlay.IsOverlayVisible() && overlay.settings.ShowDrawCalls;
}

void GrassOptimizations::RollStats(uint32_t frame)
{
	if (currentStats.frame == frame)
		return;
	lastStats = currentStats;
	currentStats = {};
	currentStats.frame = frame;
}

void GrassOptimizations::NoteTechnique(RE::BSShader* shader, uint32_t vertexDescriptor, uint32_t pixelDescriptor, bool hasPixelShader)
{
	const TechniqueKey key{ vertexDescriptor, pixelDescriptor, hasPixelShader };
	if (key == lastTechnique && shader == grassShader)
		return;
	lastTechnique = key;

	std::scoped_lock lk(techniqueMutex);
	grassShader = shader;
	if (std::find(seenTechniques.begin(), seenTechniques.end(), key) != seenTechniques.end())
		return;
	if ((vertexDescriptor | pixelDescriptor) & kOptimizedFlag) {
		logger::error("[GRASS OPTIMIZATIONS] grass descriptor {:08X}/{:08X} already uses the optimized flag bit; technique left on the vanilla path", vertexDescriptor, pixelDescriptor);
		return;
	}
	seenTechniques.push_back(key);
	// A technique not seen before has no optimized permutation yet; stay on the vanilla path until it compiles.
	shadersReady = false;
}

bool GrassOptimizations::OptimizedShadersReady()
{
	auto* shaderCache = globals::shaderCache;
	std::scoped_lock lk(techniqueMutex);
	if (!shaderCache || !grassShader)
		return false;

	// Nothing drawn yet means nothing to compile; the first frame's techniques are noted while the vanilla path draws them.
	if (seenTechniques.empty())
		return false;

	bool ready = true;
	for (const auto& t : seenTechniques) {
		// Each lookup also queues the permutation for compilation when it is missing.
		if (!shaderCache->GetVertexShader(*grassShader, t.vertexDescriptor | kOptimizedFlag))
			ready = false;
		if (t.hasPixelShader && !shaderCache->GetPixelShader(*grassShader, t.pixelDescriptor | kOptimizedFlag))
			ready = false;
	}
	return ready;
}

void GrassOptimizations::UpdateGrass()
{
	Util::CpuPassScope cpuScope("GrassOptimizations");
	GpuTimerScope gpuScope;

	std::scoped_lock blk(bucketStore.bucketMutex);
	auto* device = globals::d3d::device;
	auto* ctx = globals::d3d::context;

	const bool stats = StatsWanted();
	if (stats) {
		RollStats(globals::game::graphicsState->frameCount);
		CollectDrawnInstanceCounts(ctx);
	} else {
		drawnInstancesValid = false;
		for (auto& slot : readbackSlots)
			slot.pending = false;
	}

	// Get vanilla wind timer values
	timeAccum += globals::game::smState->timerValues[1];
	prevTimeBase = timeBase;
	timeBase = globals::game::smState->timerValues[4] * 0.0016666667f * 6.2831802f;

	const auto iniFloat = [](const char* name, float fallback) {
		auto* setting = RE::GetINISetting(name);
		return setting ? setting->GetFloat() : fallback;
	};
	if (fadeInTimeRcp == 0.0f) {
		const float t = iniFloat("fGrassFadeInTime:Grass", 0.0f);
		fadeInTimeRcp = t > 0.0f ? 1.0f / t : 1e6f;
	}
	if (vanillaMaxDistance == 0.0f) {
		grassStartFadeDistance = iniFloat("fGrassStartFadeDistance:Grass", 6000.0f);
		vanillaMaxDistance = grassStartFadeDistance + iniFloat("fGrassFadeRange:Grass", 2000.0f);
	}

	maxGrassDistance = settings.RenderDistanceOverride > 0.0f ? settings.RenderDistanceOverride : vanillaMaxDistance;
	maxDistSq = maxGrassDistance * maxGrassDistance;

	bucketStore.BeginFrame({ settings.EnableMeshLOD, settings.EnableMidLOD, settings.EnableFarLOD, timeAccum });

	// Captures are folded in and uploaded whether or not the optimized path is on, so switching it on
	// mid-session finds every loaded cell already bucketed. This only does work on frames where grass
	// cells were loaded or unloaded.
	bucketStore.ApplyPending(device, ctx);

	for (auto& [key, b] : bucketStore.buckets)
		b.ResetCullState();

	if (stats) {
		currentStats.bucketsTotal = (uint32_t)bucketStore.buckets.size();
		for (const auto& [key, b] : bucketStore.buckets)
			currentStats.instancesResident += b.totalInstances;
	}

	// This frame's culling already ran with the previous decision, and the draws must honour it: a bucket
	// folded into one representative shape has to be culled and drawn here even if the path was just
	// switched off. The decision made below only applies to the next frame's culling.
	const bool collapsedThisFrame = collapseActive.load(std::memory_order_relaxed);
	const bool baseReady = ctx1 && cullParamsCB && globals::shaderCache && globals::shaderCache->IsEnabled();
	// The cull shader is only compiled once the optimized path is wanted, so leaving it off costs nothing.
	const bool wantOptimized = settings.Enabled && baseReady && GetCullCS();
	shadersReady = wantOptimized && OptimizedShadersReady();
	collapseActive.store(wantOptimized && shadersReady, std::memory_order_relaxed);
	if (stats)
		currentStats.waitingForShaders = wantOptimized && !shadersReady;

	if (!collapsedThisFrame || !baseReady || !GetCullCS()) {
		hiZ.Invalidate();
		return;
	}

	RE::NiCamera* cam = RE::Main::WorldRootCamera();
	if (!cam)
		return;

	// From here on this frame's representative shapes have cull results to draw from.
	optimizedDrawFrame = globals::game::graphicsState->frameCount;

	RE::NiFrustumPlanes frustum{};
	ComputeFrustumPlanes(frustum, cam->GetRuntimeData2().viewFrustum, cam->world);
	const RE::NiPoint3 camPos = cam->world.translate;
	const __m128 camPosV = _mm_setr_ps(camPos.x, camPos.y, camPos.z, 0.0f);
	FrustumSoA frustumSoA;
	BuildFrustumSoA(frustumSoA, frustum);

	if (settings.EnableOcclusionCulling)
		hiZ.Build(device, ctx);
	else
		hiZ.Invalidate();

	{
		CullParamsCB cp{};
		for (int i = 0; i < 6; ++i) {
			cp.frustumPlanes[i][0] = frustum.cullingPlanes[i].normal.x;
			cp.frustumPlanes[i][1] = frustum.cullingPlanes[i].normal.y;
			cp.frustumPlanes[i][2] = frustum.cullingPlanes[i].normal.z;
			cp.frustumPlanes[i][3] = frustum.cullingPlanes[i].constant;
		}

		cp.minPixelSize = settings.MinPixelSize;
		cp.fullDetailPixelSize = settings.FullDetailPixelSize;
		cp.lodMinKeep = settings.MinDensity;
		cp.lodFadeBand = 0.15f;

		const auto& vf = cam->GetRuntimeData2().viewFrustum;
		const float screenH = (float)globals::game::graphicsState->screenHeight;
		cp.meshCostBias = settings.MeshCostBias;
		cp.projScale = screenH / (2.0f * std::abs(vf.fTop));
		cp.maxDistSq = maxDistSq;
		cp.edgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 1.0f);

		cp.alphaParam1 = grassStartFadeDistance;
		cp.alphaParam2 = maxGrassDistance;
		cp.fadeNow = timeAccum;
		cp.fadeInTimeRcp = fadeInTimeRcp;

		const float collisionDist = std::max(0.0f, settings.CollisionDistance);
		cp.invisibleFadeCull = settings.InvisibleFadeCull;
		cp.simpleShadingPixelSize = std::max(0.0f, settings.SimpleShadingPixelSize);
		cp.collisionDistSq = collisionDist * collisionDist;
		cp.midLODPixelSize = settings.MidLODPixelSize;
		cp.farLODPixelSize = settings.EnableMidLOD ? std::min(settings.FarLODPixelSize, settings.MidLODPixelSize) : settings.FarLODPixelSize;

		cp.meshLODBandPx = std::max(0.0f, settings.MeshLODBandPixels);
		cp.hiZEnabled = hiZ.IsValid() ? 1.0f : 0.0f;
		cp.hiZSizeX = (float)hiZ.GetWidth();
		cp.hiZSizeY = (float)hiZ.GetHeight();

		cp.hiZTexelPixels = hiZ.GetTexelPixels();
		cp.hiZMipCount = (float)hiZ.GetMipCount();
		cp.occlusionBias = std::max(0.0f, settings.OcclusionBias);
		cp.costBiasStartDist = std::max(0.0f, settings.CostBiasStartDistance);

		cullParamsCB->Update(cp);
	}

	uint32_t visibleBuckets = 0;
	sliceTableCPU.clear();

	for (auto& [key, b] : bucketStore.buckets) {
		if (!b.totalInstances || !b.instanceSRV)
			continue;

		if (!b.coarseValid)
			bucketStore.UpdateCoarseBounds(b);

		CullBucketSlices(b, frustumSoA, camPosV);

		if (!b.cullVisible)
			continue;

		for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier)
			b.lodBins[tier].active = bucketStore.EnsureLODBin(b, (GrassMeshLibrary::LODTier)tier, device);
		++visibleBuckets;
	}

	UploadCullState(device, ctx, visibleBuckets);

	if (stats) {
		currentStats.bucketsVisible = 0;
		for (const auto& [key, b] : bucketStore.buckets) {
			if (b.cullVisible) {
				++currentStats.bucketsVisible;
				currentStats.instancesTested += b.visibleInstances;
			}
		}
		GatherDrawnInstanceCounts(ctx);
	}
}

void GrassOptimizations::MergeSlicesIntoRuns(GrassBucket& b)
{
	const auto cellOf = [](const RE::NiPoint3& origin) {
		constexpr float kCellSize = 4096.0f;
		const auto cellX = (int32_t)std::floor(origin.x / kCellSize);
		const auto cellY = (int32_t)std::floor(origin.y / kCellSize);
		return ((uint64_t)(uint32_t)cellX << 32) | (uint32_t)cellY;
	};

	const auto continuesRun = [&cellOf](const BucketSlice& slice, const GrassBucket::SliceRun& run, uint64_t cell) {
		return slice.bufferOffset != UINT32_MAX && slice.count != 0 &&
		       slice.bufferOffset == run.firstSliceOffset + run.instanceCount &&
		       cellOf(slice.origin) == cell;
	};

	b.sliceRuns.clear();
	const uint32_t sliceCount = (uint32_t)b.slices.size();

	for (uint32_t first = 0; first < sliceCount;) {
		if (b.slices[first].bufferOffset == UINT32_MAX || b.slices[first].count == 0) {
			++first;
			continue;
		}

		GrassBucket::SliceRun run;
		run.firstSliceOffset = b.slices[first].bufferOffset;
		run.instanceCount = b.slices[first].count;
		__m128 lo = _mm_load_ps(b.sliceBounds[first].lo);
		__m128 hi = _mm_load_ps(b.sliceBounds[first].hi);
		const uint64_t cell = cellOf(b.slices[first].origin);

		uint32_t next = first + 1;
		for (; next < sliceCount && continuesRun(b.slices[next], run, cell); ++next) {
			lo = _mm_min_ps(lo, _mm_load_ps(b.sliceBounds[next].lo));
			hi = _mm_max_ps(hi, _mm_load_ps(b.sliceBounds[next].hi));
			run.instanceCount += b.slices[next].count;
		}

		_mm_store_ps(run.bounds.lo, lo);
		_mm_store_ps(run.bounds.hi, hi);
		b.sliceRuns.push_back(run);
		first = next;
	}

	b.clustersValid = true;
}

void GrassOptimizations::CullBucketSlices(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV)
{
	b.sliceTableOffset = (uint32_t)sliceTableCPU.size();
	b.sliceTableCount = 0;
	b.visibleInstances = 0;
	b.cullVisible = false;
	for (GrassBucket::LODBin& bin : b.lodBins)
		bin.active = false;

	if (b.sliceBounds.size() != b.slices.size())
		return;

	const __m128 bucketLo = _mm_setr_ps(b.coarseMin.x, b.coarseMin.y, b.coarseMin.z, 0.0f);
	const __m128 bucketHi = _mm_setr_ps(b.coarseMax.x, b.coarseMax.y, b.coarseMax.z, 0.0f);
	if (!AabbVisible(frustumSoA, bucketLo, bucketHi))
		return;

	if (!b.clustersValid)
		MergeSlicesIntoRuns(b);

	const __m128 pad = _mm_set1_ps(b.modelRadius + 64.0f);
	for (const GrassBucket::SliceRun& run : b.sliceRuns) {
		const __m128 lo = _mm_sub_ps(_mm_load_ps(run.bounds.lo), pad);
		const __m128 hi = _mm_add_ps(_mm_load_ps(run.bounds.hi), pad);

		const __m128 beyond = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lo, camPosV), _mm_sub_ps(camPosV, hi)), _mm_setzero_ps());
		auto distanceSq = _mm_cvtss_f32(_mm_dp_ps(beyond, beyond, 0x71));

		const bool withinRenderDistance = distanceSq <= maxDistSq;
		if (!withinRenderDistance || !AabbVisible(frustumSoA, lo, hi))
			continue;

		sliceTableCPU.emplace_back(run.firstSliceOffset, b.visibleInstances);
		++b.sliceTableCount;
		b.visibleInstances += run.instanceCount;
	}

	b.cullVisible = b.sliceTableCount != 0;
	if (!b.cullVisible)
		sliceTableCPU.resize(b.sliceTableOffset);
}

void GrassOptimizations::UploadCullState(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t visibleBuckets)
{
	// One map fills every visible bucket's slot — replaces a Map/Unmap per bucket.
	bool cullStateUploaded = false;
	if (visibleBuckets && EnsureCullBucketCapacity(visibleBuckets, device)) {
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(cullBucketCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			auto* bytes = static_cast<uint8_t*>(m.pData);
			uint32_t slot = 0;
			for (auto& [key, b] : bucketStore.buckets) {
				if (!b.cullVisible)
					continue;
				b.cullSlot = slot;
				auto* cb = reinterpret_cast<CullBucketCB*>(bytes + (size_t)slot * kSlotBytes);
				cb->instanceCount = b.visibleInstances;
				cb->sliceTableOffset = b.sliceTableOffset;
				cb->sliceCount = b.sliceTableCount;
				cb->wavePeriod = b.wavePeriod;
				cb->timeBase = timeBase;
				cb->prevTimeBase = prevTimeBase;
				cb->boundCenter[0] = b.boundCenter.x;
				cb->boundCenter[1] = b.boundCenter.y;
				cb->boundCenter[2] = b.boundCenter.z;
				cb->modelRadius = b.modelRadius;
				cb->distScale = b.distScale;
				cb->minPixelScale = b.minPixelScale;
				// RunGrass.hlsl keeps its own per-pixel complex-grass test, so the per-instance flag is unused.
				cb->isComplex = 0.0f;
				cb->midLODEnabled = b.lodBins[(size_t)GrassMeshLibrary::LODTier::kMiddle].active ? 1.0f : 0.0f;
				cb->farLODEnabled = b.lodBins[(size_t)GrassMeshLibrary::LODTier::kFar].active ? 1.0f : 0.0f;
				cb->pad = 0.0f;
				++slot;
			}
			ctx->Unmap(cullBucketCB->CB(), 0);
			cullStateUploaded = true;
		}
	}

	// If the cull state failed to upload, skip all buckets to prevent the CS from running using garbage or out-of-date data.
	if (visibleBuckets && !cullStateUploaded) {
		for (auto& [key, b] : bucketStore.buckets)
			b.cullVisible = false;
	}

	ID3D11Buffer* paramsCB = cullParamsCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &paramsCB);
	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	ctx->CSSetConstantBuffers(12, 1, frameBuffers);

	bool sliceTableUploaded = sliceTableCPU.empty();
	if (!sliceTableCPU.empty()) {
		if (sliceTableCPU.size() > sliceTableCapacity) {
			sliceTable.reset();
			sliceTableCapacity = 0;

			uint32_t cap = 256;
			while (cap < sliceTableCPU.size())
				cap *= 2;

			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = cap * 2 * sizeof(uint32_t);
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = 2 * sizeof(uint32_t);
			try {
				sliceTable = std::make_unique<Buffer>(bd);
				Util::SetResourceName(sliceTable->resource.get(), "GrassOptimizations::SliceTable");
				D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
				sv.Format = DXGI_FORMAT_UNKNOWN;
				sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				sv.Buffer.NumElements = cap;
				sliceTable->CreateSRV(sv);
				sliceTableCapacity = cap;
			} catch (...) {
				logger::error("[GRASS OPTIMIZATIONS] slice table create failed elements={}", cap);
				sliceTable.reset();
			}
		}

		if (sliceTable && sliceTable->srv) {
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(ctx->Map(sliceTable->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, sliceTableCPU.data(), sliceTableCPU.size() * 2 * sizeof(uint32_t));
				ctx->Unmap(sliceTable->resource.get(), 0);
				sliceTableUploaded = true;
			}
		}
	}

	if (!sliceTableUploaded) {
		for (auto& [key, b] : bucketStore.buckets)
			b.cullVisible = false;
	}

	ctx->CSSetShader(cullCS, nullptr, 0);

	for (auto& [key, b] : bucketStore.buckets)
		if (b.cullVisible)
			CullBucket(b, ctx);

	ID3D11UnorderedAccessView* nullUAVs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	ctx->CSSetShaderResources(0, 4, nullSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);
}

void GrassOptimizations::BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& f)
{
	static constexpr RE::NiFrustumPlanes::ActivePlane kBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	// Pad unused and inactive slots with an always-pass plane (zero normal, constant -1), keeping the per-slice test branch-free.
	alignas(16) float nx[8], ny[8], nz[8], d[8];
	for (uint32_t i = 0; i < 8; ++i) {
		nx[i] = ny[i] = nz[i] = 0.0f;
		d[i] = -1.0f;
	}

	for (uint32_t i = 0; i < 6; ++i) {
		if (!f.activePlanes.any(kBits[i]))
			continue;
		const auto& pl = f.cullingPlanes[i];
		nx[i] = pl.normal.x;
		ny[i] = pl.normal.y;
		nz[i] = pl.normal.z;
		d[i] = pl.constant;
	}

	for (uint32_t g = 0; g < 2; ++g) {
		out.nx[g] = _mm_load_ps(nx + g * 4);
		out.ny[g] = _mm_load_ps(ny + g * 4);
		out.nz[g] = _mm_load_ps(nz + g * 4);
		out.d[g] = _mm_load_ps(d + g * 4);
	}
}

bool GrassOptimizations::AabbVisible(const FrustumSoA& f, __m128 lo, __m128 hi)
{
	const __m128 lx = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 ly = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 lz = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 hx = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 hy = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 hz = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(2, 2, 2, 2));

	for (uint32_t g = 0; g < 2; ++g) {
		// Positive vertex: the box corner furthest along each plane normal.
		// blendv keys off the normal's sign bit.
		const __m128 px = _mm_blendv_ps(hx, lx, f.nx[g]);
		const __m128 py = _mm_blendv_ps(hy, ly, f.ny[g]);
		const __m128 pz = _mm_blendv_ps(hz, lz, f.nz[g]);

		const __m128 dot = _mm_add_ps(
			_mm_add_ps(_mm_mul_ps(f.nx[g], px), _mm_mul_ps(f.ny[g], py)),
			_mm_mul_ps(f.nz[g], pz));

		// dot(n, p) - constant < 0 → outside
		if (_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(dot, f.d[g]), _mm_setzero_ps())))
			return false;
	}
	return true;
}

void GrassOptimizations::SetupResources()
{
	cullParamsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CullParamsCB>());
	hiZ.SetupResources();

	if (FAILED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) {
		logger::error("[GRASS OPTIMIZATIONS] ID3D11DeviceContext1 unavailable — optimized path disabled");
		ctx1 = nullptr;
	}
}

void GrassOptimizations::ClearShaderCache()
{
	if (cullCS)
		cullCS->Release();
	cullCS = nullptr;
	cullCSFailed = false;
	hiZ.ClearShaderCache();
}

ID3D11ComputeShader* GrassOptimizations::GetCullCS()
{
	if (!cullCS && !cullCSFailed) {
		cullCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", {}, "cs_5_0"));
		if (!cullCS) {
			cullCSFailed = true;
			logger::error("[GRASS OPTIMIZATIONS] cull CS load failed — optimized path disabled");
		}
	}
	return cullCS;
}

void GrassOptimizations::CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	static_assert(
		(size_t)GrassMeshLibrary::LODTier::kMiddle == 0 &&
			(size_t)GrassMeshLibrary::LODTier::kFar == 1 &&
			(size_t)GrassMeshLibrary::LODTier::kCount == 2,
		"GrassCullingCS LOD counter offsets must match LODTier");

	if (b.cullSlot == UINT32_MAX)
		return;

	// Clearing the args view allows the instance count to be directly reset to zero for the draw.
	const UINT zeros[4] = { 0, 0, 0, 0 };
	ctx->ClearUnorderedAccessViewUint(b.argsUAV, zeros);
	ctx->ClearUnorderedAccessViewUint(b.lodCounterUAV, zeros);

	// Main outputs, two outputs per LOD tier, then the shared LOD counter, matching u0-u7 in GrassCullingCS.
	ID3D11UnorderedAccessView* uavs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = { b.compactedUAV, b.extrasUAV, b.argsUAV };
	for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		const GrassBucket::LODBin& bin = b.lodBins[tier];
		uavs[3 + tier * 2 + 0] = bin.active ? bin.compactedUAV : nullptr;
		uavs[3 + tier * 2 + 1] = bin.active ? bin.extrasUAV : nullptr;
	}
	uavs[std::size(uavs) - 1] = b.lodCounterUAV;
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);

	ID3D11ShaderResourceView* sliceTableSRV = sliceTable ? sliceTable->srv.get() : nullptr;
	ID3D11ShaderResourceView* srvs[4] = { b.instanceSRV, b.originSRV,
		hiZ.GetSRV(), sliceTableSRV };
	ctx->CSSetShaderResources(0, 4, srvs);

	ID3D11Buffer* bucketCB = cullBucketCB->CB();
	UINT first = b.cullSlot * 16;
	UINT num = 16;
	ctx1->CSSetConstantBuffers1(1, 1, &bucketCB, &first, &num);

	// Skipping the dispatch keeps the instance count at zero for the draw.
	if (b.visibleInstances && b.sliceTableCount && sliceTableSRV)
		ctx->Dispatch((b.visibleInstances + 63) / 64, 1, 1);

	// The LOD counter UAV must be unbound before its values can be copied into the draw arguments.
	ID3D11UnorderedAccessView* nullUAVs[std::size(uavs)] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		const GrassBucket::LODBin& bin = b.lodBins[tier];
		if (!bin.active || !bin.argsBuf)
			continue;
		const UINT countOffset = (UINT)(tier * sizeof(uint32_t));
		const D3D11_BOX countBox{ countOffset, 0, 0, static_cast<UINT>(countOffset + sizeof(uint32_t)), 1, 1 };
		ctx->CopySubresourceRegion(bin.argsBuf, 0, instanceCountOffset, 0, 0, b.lodCounterBuf, 0, &countBox);
	}
}

bool GrassOptimizations::EnsureCullBucketCapacity(uint32_t slots, [[maybe_unused]] ID3D11Device* device)
{
	if (cullBucketCB && cullBucketCBSlots >= slots)
		return true;

	uint32_t cap = cullBucketCBSlots ? cullBucketCBSlots : 64;
	while (cap < slots)
		cap *= 2;

	cullBucketCB.reset();
	cullBucketCBSlots = 0;

	try {
		cullBucketCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc(cap * kSlotBytes));
	} catch (...) {
		logger::error("[GRASS OPTIMIZATIONS] cull bucket CB create failed slots={}", cap);
		return false;
	}
	cullBucketCBSlots = cap;
	return true;
}

void GrassOptimizations::GatherDrawnInstanceCounts(ID3D11DeviceContext* ctx)
{
	constexpr UINT kBytesPerBucket = kCountsPerBucket * sizeof(uint32_t);

	uint32_t buckets = 0;
	for (const auto& [key, b] : bucketStore.buckets)
		if (b.cullVisible && b.cullSlot != UINT32_MAX)
			++buckets;

	auto& slot = readbackSlots[readbackWrite];
	slot.count = 0;
	slot.pending = false;

	if (buckets) {
		auto* device = globals::d3d::device;
		const auto ensure = [device](winrt::com_ptr<ID3D11Buffer>& buffer, uint32_t& capacity, uint32_t needed, bool staging) {
			if (buffer && capacity >= needed)
				return true;
			uint32_t cap = capacity ? capacity : 64;
			while (cap < needed)
				cap *= 2;
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = cap * kBytesPerBucket;
			bd.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
			bd.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
			buffer = nullptr;
			capacity = 0;
			if (FAILED(device->CreateBuffer(&bd, nullptr, buffer.put())))
				return false;
			capacity = cap;
			return true;
		};

		if (!ensure(gatherBuffer, gatherCapacity, buckets, false) || !ensure(slot.staging, slot.capacity, buckets, true))
			return;

		// Each visible bucket contributes its full-mesh count (args[1]) and both LOD-tier counters.
		UINT offset = 0;
		const D3D11_BOX mainBox{ instanceCountOffset, 0, 0, instanceCountOffset + (UINT)sizeof(uint32_t), 1, 1 };
		const D3D11_BOX lodBox{ 0, 0, 0, (UINT)(2 * sizeof(uint32_t)), 1, 1 };
		for (const auto& [key, b] : bucketStore.buckets) {
			if (!b.cullVisible || b.cullSlot == UINT32_MAX)
				continue;
			ctx->CopySubresourceRegion(gatherBuffer.get(), 0, offset, 0, 0, b.argsBuf, 0, &mainBox);
			ctx->CopySubresourceRegion(gatherBuffer.get(), 0, offset + (UINT)sizeof(uint32_t), 0, 0, b.lodCounterBuf, 0, &lodBox);
			offset += kBytesPerBucket;
		}
		const D3D11_BOX allBox{ 0, 0, 0, offset, 1, 1 };
		ctx->CopySubresourceRegion(slot.staging.get(), 0, 0, 0, 0, gatherBuffer.get(), 0, &allBox);
		slot.count = buckets;
	}

	slot.pending = true;
	readbackWrite = (readbackWrite + 1) % kReadbackSlots;
}

void GrassOptimizations::CollectDrawnInstanceCounts(ID3D11DeviceContext* ctx)
{
	// Oldest slot first; the newest result that is ready wins.
	for (uint32_t i = 0; i < kReadbackSlots; ++i) {
		auto& slot = readbackSlots[(readbackWrite + i) % kReadbackSlots];
		if (!slot.pending)
			continue;
		if (!slot.count) {
			drawnInstances = 0;
			drawnInstancesValid = true;
			slot.pending = false;
			continue;
		}
		D3D11_MAPPED_SUBRESOURCE m{};
		if (ctx->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK)
			continue;
		uint64_t sum = 0;
		const auto* counts = static_cast<const uint32_t*>(m.pData);
		for (uint32_t c = 0; c < slot.count * kCountsPerBucket; ++c)
			sum += counts[c];
		ctx->Unmap(slot.staging.get(), 0);
		drawnInstances = sum;
		drawnInstancesValid = true;
		slot.pending = false;
	}
}

void GrassOptimizations::DrawOverlayStats()
{
	const uint32_t frame = globals::game::graphicsState ? globals::game::graphicsState->frameCount : 0;
	// No grass draw for a couple of frames (interiors, menus): the last numbers would be stale.
	const bool fresh = lastStats.frame != UINT32_MAX && frame - lastStats.frame <= 2;
	const FrameStats s = fresh ? lastStats : FrameStats{};

	ImGui::Spacing();
	ImGui::TextUnformatted("Grass Optimizations");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(
			"Per-frame grass counters for comparing the optimized path against the game's own.\n"
			"Grass draw calls in the table above count every submitted draw: with the optimized path each\n"
			"grass type is one instanced indirect draw (plus one per active mesh-LOD band); off, the game\n"
			"issues one instanced draw per visible group of every grass shape.");
	}

	const char* mode = !settings.Enabled ? "Off (game draws grass)" :
	                   s.optimizedPath   ? "On (optimized)" :
	                   s.waitingForShaders ? "On (compiling shaders, game draws grass)" :
	                                         "On (idle: no grass drawn by it this frame)";

	if (ImGui::BeginTable("GrassOptimizationsStats", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("##prop", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 11);
		ImGui::TableSetupColumn("##value");

		const auto row = [](const char* label, const std::string& value, const char* tooltip) {
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(value.c_str());
			if (tooltip && ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(tooltip);
			}
		};

		row("Mode:", mode, "Switch it in the Grass Optimizations settings (Enable Optimized Grass Rendering).");
		row("Indirect draws:", std::format("{}", s.indirectDraws),
			"Instanced indirect draws issued by the optimized path this frame, across every grass pass.");
		row("Game grass draws:", std::format("{} ({} instances)", s.engineDraws, s.engineInstances),
			"Instanced draws the game issued for grass this frame and the instances they submitted.\n"
			"With the optimized path on, only grass it could not take over is drawn this way.");
		row("Grass types:", std::format("{} visible / {} loaded", s.bucketsVisible, s.bucketsTotal),
			"Grass types (buckets) with captured instances, and how many survived the CPU cull this frame.");
		row("Instances:", std::format("{} loaded, {} GPU-tested", s.instancesResident, s.instancesTested),
			"Captured grass instances kept on the GPU, and how many were handed to the GPU culling pass.");
		row("Instances drawn:", drawnInstancesValid && s.optimizedPath ? std::format("{}", drawnInstances) : std::string("-"),
			"Instances that survived GPU culling and were drawn, read back a few frames late without stalling.\n"
			"Compare with the game's instance count above with the optimized path off.");

		ImGui::EndTable();
	}
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_dtor::thunk(RE::BSMultiStreamInstanceTriShape* shape)
{
	globals::features::grassOptimizations.bucketStore.StageRemoval(shape);
	func(shape);
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_OnVisible::thunk(RE::BSMultiStreamInstanceTriShape* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex)
{
	auto& self = globals::features::grassOptimizations;

	if (self.collapseActive.load(std::memory_order_relaxed) && IsGrassProperty(GrassRE::GetShaderProperty(This))) {
		switch (self.bucketStore.ClaimQueueSlot(This, globals::game::graphicsState->frameCount)) {
		case GrassBucketStore::QueueClaim::kClaimed:
			// One representative per bucket is queued; the frustum checks it skips are done by the slice cull and the cull CS.
			process->AppendVirtual(*This, static_cast<std::uint32_t>(alphaGroupIndex));
			return;
		case GrassBucketStore::QueueClaim::kAlreadyClaimed:
			return;
		case GrassBucketStore::QueueClaim::kNotBucketed:
			// Drawn by the game shape by shape, so it keeps the game's own culling.
			break;
		}
	}

	func(This, process, alphaGroupIndex);
}

void GrassOptimizations::Hooks::DoneAddingInstances::thunk(RE::BSMultiStreamInstanceTriShape* shape,
	RE::BSTArray<std::uint32_t>& a_instances)
{
	auto& self = globals::features::grassOptimizations;

	auto& rt = GrassRE::GetRuntimeData(shape);
	auto* prop = GrassRE::GetShaderProperty(shape);
	if (rt.groupAlloc && IsGrassProperty(prop)) {
		if (auto* tex = prop->GetBaseTexture()) {
			const uint64_t descVal = *reinterpret_cast<uint64_t*>(&shape->GetGeometryRuntimeData().vertexDesc);
			self.bucketStore.StageCapture(shape, rt.groupAlloc, rt.instanceCount,
				2u * rt.instanceSize, descVal, tex);
		}
	}
	func(shape, a_instances);
}

void GrassOptimizations::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags)
{
	auto& self = globals::features::grassOptimizations;

	const auto frame = globals::game::graphicsState->frameCount;
	if (self.lastFrame != frame) {
		self.UpdateGrass();
		self.lastFrame = frame;
	}

	func(This, a2, flags);
}

static size_t GIDGroupBytes(const GrassRE::GroupHeader* header)
{
	if (!header || !header->numShortsPerInstance)
		return 0;
	return (size_t)header->groupInstanceCount * header->numShortsPerInstance * sizeof(std::uint16_t);
}

std::uint32_t GrassOptimizations::Hooks::AddGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, GrassRE::GroupHeader* a2, std::uint16_t* a3)
{
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2));
	return func(a1, a2, a3);
}

std::uint32_t GrassOptimizations::Hooks::AddQueuedGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, GrassRE::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4)
{
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2));
	return func(a1, a2, a3, a4);
}

thread_local GrassRE::GroupHeader tl_lastFileGroupHeader{};
thread_local std::vector<uint16_t> tl_lastFileInstanceData;
thread_local bool tl_haveFileGroup = false;

void GrassOptimizations::Hooks::ReadGroupHeaderStreamTraits::thunk(RE::BSStreamHeader* streamHeader, GrassRE::GroupHeader* groupHeader, uint32_t size)
{
	func(streamHeader, groupHeader, size);
	std::memcpy(&tl_lastFileGroupHeader, groupHeader, std::min<uint32_t>(size, sizeof(tl_lastFileGroupHeader)));
}

void GrassOptimizations::Hooks::ReadInstanceGroupStreamTraits::thunk(RE::BSStreamHeader* streamHeader, uint16_t* instanceData, uint32_t size)
{
	func(streamHeader, instanceData, size);
	tl_lastFileInstanceData.resize((size + sizeof(uint16_t) - 1) / sizeof(uint16_t));
	std::memcpy(tl_lastFileInstanceData.data(), instanceData, size);
	tl_haveFileGroup = true;
}

void GrassOptimizations::Hooks::AddGroupQueuedGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3)
{
	tl_haveFileGroup = false;
	func(a1, a2, a3);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	tl_haveFileGroup = false;
	func(a1, a2);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

RE::BSMultiStreamInstanceTriShape* GrassOptimizations::Hooks::LoadGrassType::thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath)
{
	auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

	if (shape && modelPath)
		globals::features::grassOptimizations.bucketStore.meshLibrary.RecordModelPath(shape, modelPath->c_str());

	return shape;
}

/** @brief Reissues the engine's own per-instance-group draw loop that the hook replaced. */
static void VanillaDrawInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry, GrassOptimizations::FrameStats* stats)
{
	auto* ctx = globals::d3d::context;
	auto& groups = GrassRE::GetRuntimeData(geometry).instanceGroups;

	for (uint32_t i = 0; i < groups.size(); ++i) {
		auto* curInstanceGroup = groups[i];
		if (!curInstanceGroup || !curInstanceGroup->isVisible)
			continue;

		uint32_t indexCount = 0;
		uint32_t* indexCountPtr = &indexCount;
		static REL::Relocation<ID3D11Buffer** (*)(RE::BSGraphics::Renderer*, uint64_t, uint32_t**, uint32_t)> MapDynamicBuffer{ REL::RelocationID(75561, 77362) };
		auto buffer = MapDynamicBuffer(globals::game::renderer, 1, &indexCountPtr, 7);
		if (buffer && indexCountPtr) {
			*indexCountPtr = i;
			if (*buffer)
				ctx->Unmap(*buffer, 0);
			ctx->VSSetConstantBuffers(7u, 1u, buffer);
		}

		static REL::Relocation<void (*)(RE::BSGraphics::Renderer*, RE::BSGraphics::TriShape*, uint32_t, uint32_t, uint32_t, RE::BSGraphics::VertexDesc, GrassRE::VertexBuffer*)> DrawInstancedTriShape{ REL::RelocationID(75479, 77265) };
		DrawInstancedTriShape(globals::game::renderer, geometry->GetGeometryRuntimeData().rendererData, 0, geometry->GetTrishapeRuntimeData().triangleCount, curInstanceGroup->instanceCount, geometry->GetGeometryRuntimeData().vertexDesc, curInstanceGroup->vertexBuffer);

		if (stats) {
			++stats->engineDraws;
			stats->engineInstances += curInstanceGroup->instanceCount;
		}
	}
}

void GrassOptimizations::Hooks::DrawInstanceTriShape::thunk(RE::BSRenderPass* pass, RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto& self = globals::features::grassOptimizations;
	auto* ctx = globals::d3d::context;
	auto* state = globals::state;

	const uint32_t frame = globals::game::graphicsState->frameCount;
	FrameStats* stats = nullptr;
	if (StatsWanted()) {
		self.RollStats(frame);
		stats = &self.currentStats;
	}

	auto* shaderProperty = GrassRE::GetShaderProperty(geometry);
	if (!IsGrassProperty(shaderProperty) || !state->currentShader) {
		VanillaDrawInstanceTriShape(geometry, stats);
		return;
	}

	const bool hasPixelShader = globals::game::currentPixelShader && *globals::game::currentPixelShader;
	const uint32_t vertexDescriptor = state->modifiedVertexDescriptor;
	const uint32_t pixelDescriptor = state->modifiedPixelDescriptor;
	self.NoteTechnique(state->currentShader, vertexDescriptor, pixelDescriptor, hasPixelShader);

	// Frames culled the game's way (the optimized path off, or still compiling) skip the bucket lookup
	// entirely, so the vanilla path costs no more than a few comparisons per shape.
	if (self.optimizedDrawFrame != frame) {
		VanillaDrawInstanceTriShape(geometry, stats);
		return;
	}

	RE::NiSourceTexture* diffuseTexture = shaderProperty->GetBaseTexture();
	if (!diffuseTexture) {
		VanillaDrawInstanceTriShape(geometry, stats);
		return;
	}

	const uint64_t descVal = *reinterpret_cast<uint64_t*>(&geometry->GetGeometryRuntimeData().vertexDesc);

	GrassBucket* b = nullptr;
	{
		std::scoped_lock lk(self.bucketStore.bucketMutex);

		const uint32_t meshId = self.bucketStore.meshLibrary.ResolveMeshId(geometry);
		const uint32_t triCount = meshId ? 0u : (uint32_t)geometry->GetTrishapeRuntimeData().triangleCount;
		auto* material = static_cast<RE::BSGrassShaderProperty*>(shaderProperty)->material;
		auto it = self.bucketStore.buckets.find({ meshId, material, meshId ? nullptr : diffuseTexture, triCount, meshId ? 0u : descVal });
		// This frame was culled with buckets folded into one representative shape, so every bucketed shape
		// draws its whole bucket (once per pass); shapes without a bucket are drawn by the game.
		if (it != self.bucketStore.buckets.end() && it->second.totalInstances && it->second.instanceBuf) {
			b = &it->second;

			// Since draws are dispatch per shape, ensure each bucket is only drawn once per frame per technique.
			uint32_t descriptor = 0;
			if (hasPixelShader)
				descriptor = (*globals::game::currentPixelShader)->id;
			const uint64_t passKey = (static_cast<uint64_t>(pass->passEnum) << 32) | descriptor;

			if (b->drawnFrame == frame && b->drawnPassKey == passKey)
				return;
			b->drawnFrame = frame;
			b->drawnPassKey = passKey;
		}
	}

	if (!b) {
		VanillaDrawInstanceTriShape(geometry, stats);
		return;
	}

	if (!b->cullVisible)
		return;

	auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData)
		return;
	auto* meshVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	auto* indexB = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

	const UINT meshStride = VertexStrideFromDesc(descVal);
	if (!meshStride)
		return;

	auto* shaderCache = globals::shaderCache;
	RE::BSGraphics::VertexShader* optimizedVS = shaderCache->GetVertexShader(*state->currentShader, vertexDescriptor | kOptimizedFlag);
	RE::BSGraphics::PixelShader* optimizedPS = hasPixelShader ? shaderCache->GetPixelShader(*state->currentShader, pixelDescriptor | kOptimizedFlag) : nullptr;
	if (!optimizedVS || (hasPixelShader && !optimizedPS)) {
		// A technique whose optimized permutation is not compiled yet. The rest of this bucket was folded into
		// this shape by culling and cannot be recovered this frame; the next frame stays on the game's path
		// until the permutation is ready.
		self.shadersReady = false;
		self.collapseActive.store(false, std::memory_order_relaxed);
		VanillaDrawInstanceTriShape(geometry, stats);
		return;
	}

	if (stats)
		stats->optimizedPath = true;

	if (!b->argsIndexCountWritten) {
		const uint32_t indexCount = 3u * geometry->GetTrishapeRuntimeData().triangleCount;
		const D3D11_BOX argBox{ argsByteOffset, 0, 0, argsByteOffset + sizeof(uint32_t), 1, 1 };
		ctx->UpdateSubresource(b->argsBuf, 0, &argBox, &indexCount, 0, 0);
		b->argsIndexCountWritten = true;
	}

	// Replicate vanilla state setup
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	const auto setVertexDesc = [&shadowState](uint64_t desc) {
		if (shadowState.vertexDesc != desc) {
			shadowState.vertexDesc = desc;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
		}
	};
	setVertexDesc(descVal);
	if (shadowState.topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		shadowState.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}

	// The engine bound the technique's regular permutation; the next shape in this technique may be drawn
	// by the game, so the optimized pair is only bound around our own draws.
	winrt::com_ptr<ID3D11VertexShader> previousVS;
	winrt::com_ptr<ID3D11PixelShader> previousPS;
	ctx->VSGetShader(previousVS.put(), nullptr, nullptr);
	ctx->PSGetShader(previousPS.put(), nullptr, nullptr);

	const auto issue = [&](ID3D11Buffer* vertexBuffer, UINT vertexStride, ID3D11Buffer* indexBuffer, ID3D11Buffer* instances, ID3D11ShaderResourceView* extras, ID3D11Buffer* args) {
		// Flushes the engine's pending state (input layout for the vertex desc, topology, constants). Our
		// SetDirtyStates hook counts it as one Grass draw call, so the overlay sees each indirect draw once.
		SetDirtyStates();
		ctx->VSSetShader(reinterpret_cast<ID3D11VertexShader*>(optimizedVS->shader), nullptr, 0);
		if (optimizedPS)
			ctx->PSSetShader(reinterpret_cast<ID3D11PixelShader*>(optimizedPS->shader), nullptr, 0);
		ctx->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
		ID3D11Buffer* vbs[2] = { vertexBuffer, instances };
		const UINT strides[2] = { vertexStride, kGrassStride };
		const UINT offsets[2] = { 0, 0 };
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->VSSetShaderResources(2, 1, &extras);
		ctx->DrawIndexedInstancedIndirect(args, argsByteOffset);
		if (stats)
			++stats->indirectDraws;
	};

	issue(meshVB, meshStride, indexB, b->compactedBuf, b->extrasSRV, b->argsBuf);

	std::array<const GrassMeshLibrary::LODMesh*, (size_t)GrassMeshLibrary::LODTier::kCount> lodMeshes{};
	{
		std::scoped_lock lk(self.bucketStore.bucketMutex);
		for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier)
			lodMeshes[tier] = self.bucketStore.meshLibrary.GetLODMesh(b->meshId, (GrassMeshLibrary::LODTier)tier);
	}

	for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		GrassBucket::LODBin& bin = b->lodBins[tier];
		if (!bin.active)
			continue;

		const GrassMeshLibrary::LODMesh* lod = lodMeshes[tier];
		if (!lod || !lod->vertexBuffer || !lod->indexBuffer)
			continue;

		if (!bin.argsIndexCountWritten) {
			const D3D11_BOX argBox{ argsByteOffset, 0, 0, argsByteOffset + sizeof(uint32_t), 1, 1 };
			ctx->UpdateSubresource(bin.argsBuf, 0, &argBox, &lod->indexCount, 0, 0);
			bin.argsIndexCountWritten = true;
		}

		setVertexDesc(lod->descVal);
		issue(lod->vertexBuffer, lod->meshStride, lod->indexBuffer, bin.compactedBuf, bin.extrasSRV, bin.argsBuf);
	}

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->VSSetShaderResources(2, 1, &nullSRV);
	ctx->VSSetShader(previousVS.get(), nullptr, 0);
	ctx->PSSetShader(previousPS.get(), nullptr, 0);
}
