#include "Utils/ReflectionCensus.h"

#include <array>
#include <fstream>
#include <mutex>

#include "Buffer.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Features/WetnessEffects.h"
#include "State.h"
#include "Util.h"

namespace ReflectionCensus
{
	namespace
	{
		constexpr uint32_t kCounters = 12;
		constexpr float kRoughStart = 0.30f;  // the 48d / 48f RT reflection roughness fade (RayTracing ReflectionsRoughStart / End defaults)
		constexpr float kRoughEnd = 0.65f;
		constexpr uint32_t kStagingCount = 3;

		struct alignas(16) CensusCB
		{
			uint32_t renderSize[2];
			uint32_t ssrtValid;
			uint32_t ssrtCapOn;
			float roughStart;
			float roughEnd;
			float ssrtCapEnd;
			float ssrtMaxRoughness;
			float4 cameraData;
		};
		static_assert(sizeof(CensusCB) == 48);

		/// What the request and the pass knew, written next to the counts.
		struct Context
		{
			std::filesystem::path json;
			uint32_t width = 0, height = 0;
			bool interior = false;
			float rain = 0.0f;
			bool ssrtSpecular = false;
			bool ssrtCap = false;
			float ssrtCapEndUnits = 0.0f;
			float ssrtMaxRoughness = 1.0f;
		};

		std::mutex g_mutex;
		bool g_requested = false;
		std::filesystem::path g_requestPath;
		bool g_inFlight = false;
		uint32_t g_inFlightFrame = 0;
		Context g_context;
		std::string g_summary;

		ID3D11ComputeShader* g_shader = nullptr;
		bool g_shaderFailed = false;
		winrt::com_ptr<ID3D11Buffer> g_counters;
		winrt::com_ptr<ID3D11UnorderedAccessView> g_countersUAV;
		winrt::com_ptr<ID3D11Buffer> g_staging;
		std::unique_ptr<ConstantBuffer> g_cb;

		bool EnsureResources()
		{
			if (g_shaderFailed)
				return false;
			if (!g_shader) {
				g_shader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ReflectionCensusCS.hlsl", {}, "cs_5_0"));
				if (!g_shader) {
					g_shaderFailed = true;
					logger::warn("[ReflectionCensus] ReflectionCensusCS.hlsl did not compile: no census this session");
					return false;
				}
			}
			auto* device = globals::d3d::device;
			if (!g_counters) {
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = kCounters * sizeof(uint32_t);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				uavDesc.Buffer.NumElements = kCounters;
				uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
				D3D11_BUFFER_DESC stagingDesc = desc;
				stagingDesc.Usage = D3D11_USAGE_STAGING;
				stagingDesc.BindFlags = 0;
				stagingDesc.MiscFlags = 0;
				stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (FAILED(device->CreateBuffer(&desc, nullptr, g_counters.put())) ||
					FAILED(device->CreateUnorderedAccessView(g_counters.get(), &uavDesc, g_countersUAV.put())) ||
					FAILED(device->CreateBuffer(&stagingDesc, nullptr, g_staging.put()))) {
					g_counters = nullptr;
					g_countersUAV = nullptr;
					g_staging = nullptr;
					logger::warn("[ReflectionCensus] could not make its buffers");
					return false;
				}
			}
			if (!g_cb)
				g_cb = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CensusCB>());
			return true;
		}

		double Share(uint32_t a_part, uint32_t a_total) { return a_total ? static_cast<double>(a_part) / a_total : 0.0; }

		void Write(const std::array<uint32_t, kCounters>& c, const Context& ctx)
		{
			const uint32_t pixels = c[0];
			const uint32_t surface = pixels - c[1];
			const double needsRay = pixels ? static_cast<double>(c[6]) / 1024.0 / pixels : 0.0;
			// The takeover plan: smooth pixels traced at full resolution, rough (semi + rough) at quarter resolution.
			const double smoothFull = c[3];
			const double roughQuarter = (static_cast<double>(c[4]) + c[5]) / 4.0;
			const double traced = smoothFull + roughQuarter;
			const double msLow = traced / 1e6 * 0.8, msHigh = traced / 1e6 * 1.15;
			nlohmann::json census = {
				{ "pixels", pixels },
				{ "render_resolution", { ctx.width, ctx.height } },
				{ "share_of_all", { { "sky", Share(c[1], pixels) }, { "no_specular", Share(c[2], pixels) }, { "smooth", Share(c[3], pixels) },
									  { "semi", Share(c[4], pixels) }, { "rough", Share(c[5], pixels) } } },
				{ "roughness_bands", { { "smooth_below", kRoughStart }, { "rough_above", kRoughEnd } } },
				{ "needs_a_ray_share", needsRay },
				{ "takeover_plan", { { "smooth_pixels_full_res", static_cast<uint64_t>(smoothFull) }, { "rough_pixels_quarter_res", static_cast<uint64_t>(roughQuarter) },
									   { "estimated_rt_ms_low", msLow }, { "estimated_rt_ms_high", msHigh },
									   { "note", "estimate: 0.8-1.15 ms per million traced pixels (measured slope)" } } },
				{ "context", { { "interior", ctx.interior }, { "rain", ctx.rain }, { "ssrt_specular", ctx.ssrtSpecular }, { "ssrt_distance_limit", ctx.ssrtCap },
								 { "ssrt_distance_limit_units", ctx.ssrtCapEndUnits }, { "ssrt_specular_max_roughness", ctx.ssrtMaxRoughness } } },
			};
			if (ctx.ssrtSpecular) {
				const uint32_t ssr = c[7] + c[8] + c[9] + c[10] + c[11];
				census["ssr_outcomes"] = { { "of_specular_pixels", ssr }, { "hit", Share(c[9], ssr) }, { "miss_to_cubemap_or_left_screen", Share(c[10], ssr) },
					{ "beyond_distance_limit", Share(c[7], ssr) }, { "skipped_by_max_roughness", Share(c[8], ssr) }, { "not_traced", Share(c[11], ssr) },
					{ "note", "SSRT writes the same 'no hit' for a miss and a ray that left the screen" } };
			}
			std::scoped_lock lock(g_mutex);
			g_summary = std::format("Reflection census: smooth {:.0f}% semi {:.0f}% rough {:.0f}% none {:.0f}% sky {:.0f}% | needs a ray {:.0f}% | RT takeover ~{:.1f}-{:.1f} ms (est.)",
				100.0 * Share(c[3], pixels), 100.0 * Share(c[4], pixels), 100.0 * Share(c[5], pixels), 100.0 * Share(c[2], pixels), 100.0 * Share(c[1], pixels),
				100.0 * needsRay, msLow, msHigh);
			(void)surface;
			const std::string summary = g_summary;
			try {
				nlohmann::json j;
				{
					std::ifstream in(ctx.json, std::ios::binary);
					if (in)
						j = nlohmann::json::parse(in, nullptr, false);
				}
				if (j.is_discarded() || !j.is_object())
					j = nlohmann::json::object();
				j["reflection_census"] = std::move(census);
				std::ofstream out(ctx.json, std::ios::binary | std::ios::trunc);
				if (out)
					out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
				logger::info("[ReflectionCensus] {} -> {}", summary, ctx.json.string());
			} catch (const std::exception& e) {
				logger::warn("[ReflectionCensus] could not write the census into {}: {}", ctx.json.string(), e.what());
			}
		}
	}

	void Request(const std::filesystem::path& a_jsonPath)
	{
		std::scoped_lock lock(g_mutex);
		g_requested = true;
		g_requestPath = a_jsonPath;
	}

	void Dispatch(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_normalRoughness, ID3D11ShaderResourceView* a_reflectance,
		ID3D11ShaderResourceView* a_depth)
	{
		std::filesystem::path path;
		{
			std::scoped_lock lock(g_mutex);
			if (!g_requested || g_inFlight)
				return;
			g_requested = false;
			path = g_requestPath;
		}
		if (!a_context || !a_normalRoughness || !a_reflectance || !a_depth || !EnsureResources())
			return;

		auto& ssrt = globals::features::screenSpaceRayTracing;
		const bool ssrtSpecular = ssrt.loaded && ssrt.settings.EnableSpecular && ssrt.texHitDistance;
		const float2 size = Util::ConvertToDynamic(globals::state->screenSize);

		Context ctx;
		ctx.json = path;
		ctx.width = static_cast<uint32_t>(size.x);
		ctx.height = static_cast<uint32_t>(size.y);
		ctx.interior = Util::IsInterior();
		ctx.rain = globals::features::wetnessEffects.loaded ? globals::features::wetnessEffects.GetCharacterRainIntensity() : 0.0f;
		ctx.ssrtSpecular = ssrtSpecular;
		ctx.ssrtCap = ssrtSpecular && ssrt.SpecularCapActive();
		ctx.ssrtCapEndUnits = ctx.ssrtCap ? ssrt.DistanceLimitUnits() : 0.0f;
		ctx.ssrtMaxRoughness = ssrt.loaded ? ssrt.settings.SpecularMaxRoughness : 1.0f;

		CensusCB cb{};
		cb.renderSize[0] = ctx.width;
		cb.renderSize[1] = ctx.height;
		cb.ssrtValid = ssrtSpecular ? 1u : 0u;
		cb.ssrtCapOn = ctx.ssrtCap ? 1u : 0u;
		cb.roughStart = kRoughStart;
		cb.roughEnd = kRoughEnd;
		cb.ssrtCapEnd = ctx.ssrtCapEndUnits;
		cb.ssrtMaxRoughness = ctx.ssrtMaxRoughness;
		cb.cameraData = Util::GetCameraData();
		g_cb->Update(cb);

		const UINT zero[4] = { 0, 0, 0, 0 };
		a_context->ClearUnorderedAccessViewUint(g_countersUAV.get(), zero);
		ID3D11ShaderResourceView* srvs[4] = { a_normalRoughness, a_reflectance, a_depth, ssrtSpecular ? ssrt.texHitDistance->srv.get() : nullptr };
		ID3D11UnorderedAccessView* uav = g_countersUAV.get();
		ID3D11Buffer* buffer = g_cb->CB();
		a_context->CSSetShader(g_shader, nullptr, 0);
		a_context->CSSetShaderResources(0, 4, srvs);
		a_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		a_context->CSSetConstantBuffers(0, 1, &buffer);
		a_context->Dispatch((ctx.width + 15) / 16, (ctx.height + 15) / 16, 1);
		ID3D11ShaderResourceView* nullSrvs[4]{};
		ID3D11UnorderedAccessView* nullUav = nullptr;
		ID3D11Buffer* nullBuffer = nullptr;
		a_context->CSSetShaderResources(0, 4, nullSrvs);
		a_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		a_context->CSSetConstantBuffers(0, 1, &nullBuffer);
		a_context->CSSetShader(nullptr, nullptr, 0);
		a_context->CopyResource(g_staging.get(), g_counters.get());

		std::scoped_lock lock(g_mutex);
		g_context = ctx;
		g_inFlight = true;
		g_inFlightFrame = globals::state->frameCount;
	}

	void Poll()
	{
		Context ctx;
		{
			std::scoped_lock lock(g_mutex);
			if (!g_inFlight || globals::state->frameCount - g_inFlightFrame < kStagingCount)
				return;
			ctx = g_context;
		}
		auto* context = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (!context || FAILED(context->Map(g_staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) || !mapped.pData)
			return;  // not ready yet: try next frame (never waits)
		std::array<uint32_t, kCounters> counts{};
		std::memcpy(counts.data(), mapped.pData, sizeof(uint32_t) * kCounters);
		context->Unmap(g_staging.get(), 0);
		{
			std::scoped_lock lock(g_mutex);
			g_inFlight = false;
		}
		Write(counts, ctx);
	}

	std::string Summary()
	{
		std::scoped_lock lock(g_mutex);
		return g_summary;
	}
}
