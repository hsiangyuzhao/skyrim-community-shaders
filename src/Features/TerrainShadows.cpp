#include "TerrainShadows.h"

#include <DirectXTex.h>
#include <pystring/pystring.h>

#include "State.h"
#include "Util.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainShadows::Settings,
	EnableTerrainShadow,
	StablePenumbrae,
	RefreshOnSunJump,
	UseParentHeightmap)

void TerrainShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainShadows::DrawSettings()
{
	ImGui::Checkbox("Enable Terrain Shadow", &settings.EnableTerrainShadow);

	// (batch 40) Upstream fixes, each switchable for A/B; off = the batch 39 behaviour.
	ImGui::SeparatorText("Fixes (Batch 40)");
	ImGui::Checkbox("Stable Soft Edges", &settings.StablePenumbrae);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(
			"Upstream fix: the soft edge of mountain shadows on distant ground no longer crawls or flickers\n"
			"as the sun moves, and lines up with the terrain (it sat half a heightmap texel off).\n"
			"The edge is a little tighter than before. Off = the batch 39 terrain shadows.\n"
			"Switching rebuilds the shadow map (one frame).");
	ImGui::Checkbox("Instant Refresh After Time Jumps", &settings.RefreshOnSunJump);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(
			"After waiting, sleeping, fast travel or loading, the mountain shadows are rebuilt at once.\n"
			"Off = they sweep across the map over a second or two, showing the old sun angle meanwhile.");
	ImGui::Checkbox("Use Parent Worldspace Heightmap", &settings.UseParentHeightmap);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(
			"Worldspaces that borrow their parent's terrain also get the parent's terrain shadows.\n"
			"Off = no terrain shadows there.");

	if (ImGui::CollapsingHeader("Debug")) {
		std::string curr_worldspace = "N/A";
		std::string curr_worldspace_name = "N/A";
		auto tes = RE::TES::GetSingleton();
		if (tes) {
			auto worldspace = tes->GetRuntimeData2().worldSpace;
			if (worldspace) {
				curr_worldspace = worldspace->GetFormEditorID();
				curr_worldspace_name = worldspace->GetName();
			}
		}
		ImGui::Text(fmt::format("Current worldspace: {} ({})", curr_worldspace, curr_worldspace_name).c_str());
		ImGui::Text(fmt::format("Has height map: {}", heightmaps.contains(curr_worldspace)).c_str());
		ImGui::Text(fmt::format("Height map in use: {}", cachedHeightmap ? cachedHeightmap->worldspace : std::string("none")).c_str());
		ImGui::Text(fmt::format("Full refreshes: {}", fullRefreshCount).c_str());

		ImGui::Separator();

		ImGui::BulletText("shadowUpdateCBData");
		ImGui::Indent();
		{
			ImGui::Text(fmt::format("LightPxDir: ({}, {})", shadowUpdateCBData.LightPxDir.x, shadowUpdateCBData.LightPxDir.y).c_str());
			ImGui::Text(fmt::format("LightDeltaZ: ({}, {})", shadowUpdateCBData.LightDeltaZ.x, shadowUpdateCBData.LightDeltaZ.y).c_str());
			ImGui::Text(fmt::format("StartPxCoord: {}", shadowUpdateCBData.StartPxCoord).c_str());
			ImGui::Text(fmt::format("PxSize: ({}, {})", shadowUpdateCBData.PxSize.x, shadowUpdateCBData.PxSize.y).c_str());
		}
		ImGui::Unindent();

		if (ImGui::TreeNode("Buffer Viewer")) {
			static float debugRescale = .1f;
			ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

			if (texShadowHeight) {
				BUFFER_VIEWER_NODE_BULLET(texShadowHeight, debugRescale)
			}
			ImGui::TreePop();
		}
	}
}

void TerrainShadows::ClearShaderCache()
{
	// com_ptr: assigning nullptr releases (the old explicit Release() followed by = nullptr
	// released twice).
	shadowUpdateProgram = nullptr;
	shadowUpdateProgramLegacy = nullptr;

	CompileComputeShaders();
}

void TerrainShadows::ParseHeightmapPath(std::filesystem::path p, bool xlodgen_style)
{
	auto filename = p.filename();
	if (filename.extension() != ".dds")
		return;
	logger::debug("Found dds: {}", filename.string());

	auto splitstr = pystring::split(filename.stem().string(), ".");
	if (splitstr.size() != (xlodgen_style ? 9 : 10)) {
		logger::debug("{} has incorrect number ({}) of fields", filename.string(), splitstr.size());
		return;
	}

	bool middle_check = xlodgen_style ? ((splitstr[1] == "Terrain") && (splitstr[2] == "HeightMap")) : (splitstr[1] == "HeightMap");
	if (middle_check) {
		HeightMapMetadata metadata;
		try {
			if (xlodgen_style) {
				metadata.worldspace = splitstr[0];
				metadata.pos0.x = std::stoi(splitstr[3]) * 4096.f;
				metadata.pos1.y = std::stoi(splitstr[4]) * 4096.f;
				metadata.pos1.x = (std::stoi(splitstr[5]) + 1) * 4096.f;
				metadata.pos0.y = (std::stoi(splitstr[6]) + 1) * 4096.f;
				metadata.pos0.z = -32767 * 8.f;
				metadata.pos1.z = 32767 * 8.f;
				metadata.zRange.x = std::stoi(splitstr[7]) * 8.f;
				metadata.zRange.y = std::stoi(splitstr[8]) * 8.f;
			} else {
				metadata.worldspace = splitstr[0];
				metadata.pos0.x = std::stoi(splitstr[2]) * 4096.f;
				metadata.pos1.y = std::stoi(splitstr[3]) * 4096.f;
				metadata.pos1.x = (std::stoi(splitstr[4]) + 1) * 4096.f;
				metadata.pos0.y = (std::stoi(splitstr[5]) + 1) * 4096.f;
				metadata.pos0.z = std::stoi(splitstr[6]) * 8.f;
				metadata.pos1.z = std::stoi(splitstr[7]) * 8.f;
				metadata.zRange.x = std::stoi(splitstr[8]) * 8.f;
				metadata.zRange.y = std::stoi(splitstr[9]) * 8.f;
			}
		} catch (std::exception& e) {
			logger::debug("Failed to parse {}. Error: {}", filename.string(), e.what());
			return;
		}

		metadata.dir = p.parent_path().wstring();
		metadata.filename = filename.string();

		if (heightmaps.contains(metadata.worldspace))
			logger::warn("{} has more than one height maps!", metadata.worldspace);
		heightmaps[metadata.worldspace] = metadata;

		logger::info("{} loaded.", filename.string());
	} else
		logger::debug("{} has unknown type ({})", filename.string(), splitstr[1]);
}

void TerrainShadows::SetupResources()
{
	logger::debug("Listing xLODGen height maps...");
	{
		std::filesystem::path texture_dir{ L"Data\\textures\\Terrain\\" };
		std::error_code ec;
		for (auto const& dir_entry : std::filesystem::directory_iterator{ texture_dir, ec }) {
			auto dir_path = dir_entry.path();
			if (!std::filesystem::is_directory(dir_path))
				continue;

			for (auto const& sub_dir_entry : std::filesystem::directory_iterator{ dir_path })
				ParseHeightmapPath(sub_dir_entry.path(), true);
		}
	}

	logger::debug("Listing height maps...");
	{
		std::filesystem::path texture_dir{ L"Data\\textures\\heightmaps\\" };
		std::error_code ec;
		for (auto const& dir_entry : std::filesystem::directory_iterator{ texture_dir, ec })
			ParseHeightmapPath(dir_entry.path(), false);
	}

	logger::debug("Creating constant buffers...");
	{
		shadowUpdateCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShadowUpdateCB>());
	}

	CompileComputeShaders();
}

void TerrainShadows::CompileComputeShaders()
{
	logger::debug("Compiling shaders...");
	{
		auto program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\ShadowUpdate.cs.hlsl", {}, "cs_5_0"));
		if (program_ptr)
			shadowUpdateProgram.attach(program_ptr);
		// (batch 40) The batch 39 kernel, for Stable Soft Edges off.
		auto legacy_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\ShadowUpdate.cs.hlsl", { { "LEGACY_UPDATE", "" } }, "cs_5_0"));
		if (legacy_ptr)
			shadowUpdateProgramLegacy.attach(legacy_ptr);
	}
}

RE::TESWorldSpace* TerrainShadows::GetHeightmapWorldspace() const
{
	auto tes = RE::TES::GetSingleton();
	if (!tes)
		return nullptr;
	auto worldspace = tes->GetRuntimeData2().worldSpace;
	// (batch 40) Upstream fc46f1a66: a worldspace that uses its parent's land data has the
	// parent's terrain, so the parent's heightmap is the right one. Upstream walked the chain
	// only when loading and still compared the child's name here, which left it switched off.
	if (settings.UseParentHeightmap) {
		while (worldspace && worldspace->parentWorld && worldspace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
			worldspace = worldspace->parentWorld;
	}
	return worldspace;
}

bool TerrainShadows::IsHeightMapReady()
{
	if (auto worldspace = GetHeightmapWorldspace())
		return cachedHeightmap && texHeightMap && cachedHeightmap->worldspace == worldspace->GetFormEditorID();
	return false;
}

TerrainShadows::PerFrame TerrainShadows::GetCommonBufferData()
{
	bool isHeightmapReady = IsHeightMapReady();

	PerFrame data = {
		.EnableTerrainShadow = settings.EnableTerrainShadow && isHeightmapReady,
	};

	// (batch 40) Follows the kernel the current map was built with (builtStable), so the
	// sampling and the map always match, also on the frame the switch flips.
	data.StablePenumbrae = builtStable;
	data.SelfShadowBias = builtStable ? 256.f : 1024.f;
	data.ZBlur = 0.f;

	if (isHeightmapReady) {
		auto invScale = cachedHeightmap->pos1 - cachedHeightmap->pos0;
		data.Scale = float3(1.f, 1.f, 1.f) / invScale;
		data.Offset = -cachedHeightmap->pos0 * float2{ data.Scale.x, data.Scale.y };
		data.ZRange = cachedHeightmap->zRange;
		if (builtStable) {
			// Upstream #2729: texel centres lie on terrain vertices anchored at the south-west
			// corner (the old mapping sat half a texel off) ...
			const float2 halfTexel = { 0.5f / texHeightMap->desc.Width, -0.5f / texHeightMap->desc.Height };
			data.Offset = data.Offset + halfTexel;
			// ... and the z blur spans about one heightmap step of light descent.
			const float stepDescent = -0.5f * (shadowUpdateCBData.LightDeltaZ.x + shadowUpdateCBData.LightDeltaZ.y) * (data.ZRange.y - data.ZRange.x);
			data.ZBlur = std::max(stepDescent, 0.f);
		}
	}

	return data;
}

void TerrainShadows::LoadHeightmap()
{
	auto worldspace = GetHeightmapWorldspace();
	if (!worldspace)
		return;
	std::string worldspace_name = worldspace->GetFormEditorID();
	if (!heightmaps.contains(worldspace_name))  // no height map for that, but we don't remove cache
		return;
	if (cachedHeightmap && cachedHeightmap->worldspace == worldspace_name)  // already cached
		return;

	auto device = globals::d3d::device;

	logger::debug("Loading height map...");
	{
		auto& target_heightmap = heightmaps[worldspace_name];

		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ target_heightmap.dir };
			path /= target_heightmap.filename;

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		// reset(), not release(): unique_ptr::release() hands off ownership WITHOUT
		// destroying, so every worldspace change orphaned the previous heightmap
		// (Tamriel 21.8 MiB, Solstheim 72 MiB) for the rest of the process.
		texHeightMap.reset();
		texHeightMap = std::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texHeightMap->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texHeightMap->CreateSRV(srvDesc);

		cachedHeightmap = &heightmaps[worldspace_name];
	}

	shadowUpdateIdx = 0;
	needPrecompute = true;
}

void TerrainShadows::Precompute()
{
	if (!cachedHeightmap || !texHeightMap)
		return;

	// (batch 40) The map is built for the kernel selected now; switching rebuilds it.
	builtStable = settings.StablePenumbrae;

	logger::info("Creating shadow texture...");
	{
		if (texShadowHeight) {
			auto context = globals::d3d::context;

			std::array<ID3D11ShaderResourceView*, 1> srvs = { nullptr };
			context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
			context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		}

		// reset(), not release(): see LoadHeightmap(). Freeing here rather than
		// letting operator= do it also keeps the peak at one texture, not two.
		texShadowHeight.reset();

		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = texHeightMap->desc.Width,
			.Height = texHeightMap->desc.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			// (batch 40) Upstream #2729 stores the normalised heights as UNORM: even steps of
			// 1/65535 of the height range, where half floats near 1.0 step 32 times coarser.
			// Heights below 0 clamp to 0, which is below all terrain anyway.
			.Format = builtStable ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16G16_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texShadowHeight = std::make_unique<Texture2D>(texDesc);
		texShadowHeight->CreateSRV(srvDesc);
		texShadowHeight->CreateUAV(uavDesc);
	}

	needPrecompute = false;
}

bool TerrainShadows::UpdateShadow(bool a_refreshImmediately)
{
	if (!IsHeightMapReady() || !texShadowHeight)
		return false;

	auto program = builtStable ? shadowUpdateProgram.get() : shadowUpdateProgramLegacy.get();
	if (!program)
		return false;

	// don't forget to change NTHREADS in shader!
	constexpr uint updateLength = 128u;
	constexpr uint logUpdateLength = std::bit_width(128u) - 1;  // integer log2, https://stackoverflow.com/questions/994593/how-to-do-an-integer-log2-in-c

	auto context = globals::d3d::context;

	// (batch 40) Upstream #2238 / #2833: the world's shadow scene node instead of the current
	// accumulator's, which this early in the frame can still belong to a menu (for example the
	// local map), and which was dereferenced without any null check.
	auto shadowSceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
	if (!shadowSceneNode)
		return false;
	auto shadowSunLight = shadowSceneNode->GetRuntimeData().sunLight;
	if (!shadowSunLight || !shadowSunLight->light)
		return false;
	auto sunLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSunLight->light.get());
	if (!sunLight)
		return false;

	{
		std::array<ID3D11ShaderResourceView*, 1> srvs = { nullptr };
		context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Occlusion - Update Shadows");

	/* ---- UPDATE CB ---- */
	uint width = texHeightMap->desc.Width;
	uint height = texHeightMap->desc.Height;

	auto direction = sunLight->GetWorldDirection();
	float3 dirLightDir = { direction.x, direction.y, direction.z };
	if (dirLightDir.z > 0)
		dirLightDir = -dirLightDir;

	// (batch 40) A sun that moved more than about a degree since this sweep started has jumped:
	// waiting, sleeping, fast travel, a console time change, or the sun/moon switch. Its normal
	// drift over one whole sweep (about 30 frames) is a few hundredths of a degree.
	if (settings.RefreshOnSunJump && sweepLightDirValid) {
		constexpr float cosJump = 0.99985f;  // cos(1 degree)
		float3 a = dirLightDir;
		float3 b = sweepLightDir;
		a.Normalize();
		b.Normalize();
		if (a.Dot(b) < cosJump)
			a_refreshImmediately = true;
	}

	// only update direction at the start of each cycle
	static uint edgePxCoord;
	static int signDir;
	static uint maxUpdates;
	if (a_refreshImmediately)
		shadowUpdateIdx = 0;
	if (shadowUpdateIdx == 0) {
		sweepLightDir = dirLightDir;
		sweepLightDirValid = true;

		// in UV
		float3 invScale = cachedHeightmap->pos1 - cachedHeightmap->pos0;
		invScale.z = cachedHeightmap->zRange.y - cachedHeightmap->zRange.x;

		if (builtStable) {
			// Upstream #2729: the same stepping, plus a guard for a sun straight overhead, a 1 degree
			// soft angle instead of 4 (the z blur in the sampling widens the edge back a little),
			// and both angles clamped to the valid range.
			float2 dirLightPxDir = { dirLightDir.x / invScale.x * width, dirLightDir.y / invScale.y * height };
			if (dirLightPxDir.x == 0.f && dirLightPxDir.y == 0.f)
				dirLightPxDir = { 1.f, 0.f };

			if (abs(dirLightPxDir.x) >= abs(dirLightPxDir.y)) {
				edgePxCoord = dirLightPxDir.x > 0 ? 0 : (width - 1);
				signDir = dirLightPxDir.x > 0 ? 1 : -1;
				dirLightPxDir.y /= abs(dirLightPxDir.x);
				dirLightPxDir.x = static_cast<float>(signDir);
				maxUpdates = (width + updateLength - 1) >> logUpdateLength;
			} else {
				edgePxCoord = dirLightPxDir.y > 0 ? 0 : height - 1;
				signDir = dirLightPxDir.y > 0 ? 1 : -1;
				dirLightPxDir.x /= abs(dirLightPxDir.y);
				dirLightPxDir.y = static_cast<float>(signDir);
				maxUpdates = (height + updateLength - 1) >> logUpdateLength;
			}

			shadowUpdateCBData.LightPxDir = dirLightPxDir;

			float lenUV = float2{ dirLightDir.x, dirLightDir.y }.Length();
			float dirLightAngle = atan2(-dirLightDir.z, lenUV);
			float shadowSofteningRadiusAngle = RE::NI_PI / 180.f;
			float maxAngle = RE::NI_HALF_PI - 1e-2f;
			float upperAngle = std::clamp(dirLightAngle - shadowSofteningRadiusAngle, 0.f, maxAngle);
			float lowerAngle = std::clamp(dirLightAngle + shadowSofteningRadiusAngle, 0.f, maxAngle);
			float stepLength = float2{ dirLightPxDir.x * invScale.x / width, dirLightPxDir.y * invScale.y / height }.Length();

			shadowUpdateCBData.LightDeltaZ = -(stepLength / invScale.z) * float2{ std::tan(upperAngle), std::tan(lowerAngle) };
		} else {
			float3 dirLightPxDir = dirLightDir / invScale;
			dirLightPxDir.x *= width;
			dirLightPxDir.y *= height;

			float stepMult;
			if (abs(dirLightPxDir.x) >= abs(dirLightPxDir.y)) {
				stepMult = 1.f / abs(dirLightPxDir.x);
				edgePxCoord = dirLightPxDir.x > 0 ? 0 : (width - 1);
				signDir = dirLightPxDir.x > 0 ? 1 : -1;
				maxUpdates = (width + updateLength - 1) >> logUpdateLength;
			} else {
				stepMult = 1.f / abs(dirLightPxDir.y);
				edgePxCoord = dirLightPxDir.y > 0 ? 0 : height - 1;
				signDir = dirLightPxDir.y > 0 ? 1 : -1;
				maxUpdates = (height + updateLength - 1) >> logUpdateLength;
			}
			dirLightPxDir *= stepMult;

			shadowUpdateCBData.LightPxDir = { dirLightPxDir.x, dirLightPxDir.y };

			// soft shadow angles
			float lenUV = float2{ dirLightDir.x, dirLightDir.y }.Length();
			float dirLightAngle = atan2(-dirLightDir.z, lenUV);
			float shadowSofteningRadiusAngle = 4.f * RE::NI_PI / 180.f;
			float upperAngle = std::max(0.f, dirLightAngle - shadowSofteningRadiusAngle);
			float lowerAngle = std::min(RE::NI_HALF_PI - 1e-2f, dirLightAngle + shadowSofteningRadiusAngle);

			shadowUpdateCBData.LightDeltaZ = -(lenUV / invScale.z * stepMult) * float2{ std::tan(upperAngle), std::tan(lowerAngle) };
		}
	}

	shadowUpdateCBData.PxSize = { 1.f / texHeightMap->desc.Width, 1.f / texHeightMap->desc.Height };
	shadowUpdateCBData.PosRange = { cachedHeightmap->pos0.z, cachedHeightmap->pos1.z };
	shadowUpdateCBData.ZRange = cachedHeightmap->zRange;
	// A full refresh writes every texel at full weight, so nothing of the old sun angle (or of
	// the empty texture after a load) survives; a normal step keeps the old 50/50 blend.
	shadowUpdateCBData.BlendWeight = a_refreshImmediately ? 1.0f : 0.5f;

	/* ---- BACKUP ---- */
	struct ShaderState
	{
		ID3D11ShaderResourceView* srvs[1] = { nullptr };
		ID3D11ComputeShader* shader = nullptr;
		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		ID3D11Buffer* buffer = nullptr;
	} old, newer;

	/* ---- DISPATCH ---- */

	newer.srvs[0] = texHeightMap->srv.get();
	newer.uavs[0] = texShadowHeight->uav.get();
	newer.buffer = shadowUpdateCB->CB();

	auto timers = Util::GpuPassTimers::GetSingleton();
	timers->Begin(Util::GpuBucket::TerrainShadows);

	context->CSSetShaderResources(0, ARRAYSIZE(newer.srvs), newer.srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(newer.uavs), newer.uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &newer.buffer);
	context->CSSetShader(program, nullptr, 0);

	// One 128-texel band per frame normally; on a full refresh every band this frame, in sweep
	// order, so each band continues from the one the light came through.
	const uint updateCount = a_refreshImmediately ? maxUpdates : 1u;
	for (uint update = 0; update < updateCount; ++update) {
		shadowUpdateCBData.StartPxCoord = edgePxCoord + signDir * shadowUpdateIdx * updateLength;
		shadowUpdateCB->Update(shadowUpdateCBData);
		context->Dispatch(abs(shadowUpdateCBData.LightPxDir.x) >= abs(shadowUpdateCBData.LightPxDir.y) ? height : width, 1, 1);
		shadowUpdateIdx = (shadowUpdateIdx + 1) % maxUpdates;
	}
	if (a_refreshImmediately)
		++fullRefreshCount;

	timers->End(Util::GpuBucket::TerrainShadows);

	/* ---- RESTORE ---- */
	context->CSSetShaderResources(0, ARRAYSIZE(old.srvs), old.srvs);
	context->CSSetShader(old.shader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(old.uavs), old.uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &old.buffer);
	return true;
}

void TerrainShadows::ReflectionsPrepass()
{
	if (texShadowHeight) {
		auto context = globals::d3d::context;

		std::array<ID3D11ShaderResourceView*, 1> srvs = { texShadowHeight->srv.get() };
		context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	}
}

void TerrainShadows::EarlyPrepass()
{
	LoadHeightmap();

	if (!settings.EnableTerrainShadow)
		return;

	// (batch 40) Switching Stable Soft Edges rebuilds the map with the other kernel and format,
	// and always fills it in one frame so the A/B shows no sweep-in.
	const bool rebuildForSwitch = texShadowHeight && builtStable != settings.StablePenumbrae;
	if (rebuildForSwitch)
		needPrecompute = true;

	bool refreshImmediately = false;
	if (needPrecompute) {
		Precompute();
		// A new map starts empty. Upstream #2617 fills it in one go; with Instant Refresh off it
		// sweeps in as before.
		refreshImmediately = settings.RefreshOnSunJump || rebuildForSwitch;
	}

	UpdateShadow(refreshImmediately);

	if (texShadowHeight) {
		auto context = globals::d3d::context;

		std::array<ID3D11ShaderResourceView*, 1> srvs = { texShadowHeight->srv.get() };
		context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	}
}
