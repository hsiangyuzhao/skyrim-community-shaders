// Batch 39, items 5 and 6: table rows for dynamic snow accumulation and snow/mud footprints.
// Kept apart from Batch39.cpp so the rows of items 1-4 merge without conflicts.
#include "Utils/Batch39.h"

#include <format>
#include <string>

#include "Features/DynamicSnow.h"
#include "Globals.h"

namespace Batch39
{
	namespace
	{
		std::string OnOff(bool a_on) { return a_on ? "On" : "Off"; }

		Row Toggle(const char* a_group, const char* a_name, bool a_installed, bool& a_value, std::string a_nowText, const char* a_where)
		{
			Row r{ a_group, a_name, a_installed, a_value, OnOff(a_value), std::move(a_nowText), a_where };
			r.toggle = &a_value;
			return r;
		}

		Row Value(const char* a_group, const char* a_name, bool a_installed, bool a_own, std::string a_ownText, const char* a_where)
		{
			return Row{ a_group, a_name, a_installed, a_own, std::move(a_ownText), "", a_where };
		}

		constexpr const char* kWhere = "Landscape & Textures > Dynamic Snow";
	}

	void RowsSnowAccumulation(std::vector<Row>& a_rows)
	{
		auto& snow = globals::features::dynamicSnow;
		auto& s = snow.settings;
		const auto& st = snow.status;
		const char* group = "5. Dynamic snow accumulation";
		const bool installed = snow.loaded;

		std::string now;
		if (installed && s.EnableAccumulation) {
			if (!st.exterior)
				now = "Idle: indoors";
			else if (s.OverrideAmount)
				now = std::format("On (debug override {:.0f}%)", s.AmountOverride * 100.0f);
			else if (st.snowing)
				now = std::format("Snowing: {:.0f}% built up", st.amount * 100.0f);
			else if (st.amount > 0.0f)
				now = std::format("Melting: {:.0f}% left", st.amount * 100.0f);
			else
				now = "Idle: no snow";
		}
		a_rows.push_back(Toggle(group, "Snow builds up while it snows, melts after", installed, s.EnableAccumulation, now, kWhere));

		static constexpr const char* kConditions[] = { "Snow weather, any region", "Snow weather, cold region", "Snow weather, or rain/snow in cold region" };
		std::string condition = kConditions[std::clamp(s.Condition, 0, 2)];
		if (s.Condition != 0)
			condition += std::format(" (here: {})", st.coldRegion ? "cold" : "not cold");
		a_rows.push_back(Value(group, "  When it builds up", installed, s.EnableAccumulation, condition, kWhere));
		a_rows.push_back(Value(group, "  Build-up / melt time", installed, s.EnableAccumulation,
			std::format("{:.1f} h / {:.1f} h (game time)", s.AccumulationHours, s.MeltHours), kWhere));
		a_rows.push_back(Toggle(group, "  Even cover on roofs and slopes (39c)", installed, s.SlopeCoverage, "", kWhere));
		a_rows.push_back(Value(group, s.SlopeCoverage ? "  Max coverage / slope start, full" : "  Max coverage / normal threshold", installed, s.EnableAccumulation,
			s.SlopeCoverage ? std::format("{:.2f} / {:.2f}, {:.2f}", s.MaxCoverage, s.SlopeStart, s.SlopeFull) : std::format("{:.2f} / {:.2f}", s.MaxCoverage, s.NormalThreshold), kWhere));
		a_rows.push_back(Toggle(group, "  Snow on trees and bushes (39c)", installed, s.SnowOnTrees, "", kWhere));
		a_rows.push_back(Toggle(group, "  Snow on grass (39c)", installed, s.SnowOnGrass, "", kWhere));
		a_rows.push_back(Toggle(group, "  Snow on distant trees (39c)", installed, s.SnowOnLodTrees, "", kWhere));
		a_rows.push_back(Toggle(group, "  Snow on characters (39b, default Off)", installed, s.SnowOnCharacters, "", kWhere));
		a_rows.push_back(Toggle(group, "  Recognise snowy ground: land texture, PBR terrain (39b)", installed, s.DetectAuthoredSnow, "", kWhere));
		a_rows.push_back(Toggle(group, "  ... also guess from colour (39b, default Off)", installed, s.AlbedoSnowGuess, "", kWhere));
	}

	void RowsSnowTrails(std::vector<Row>& a_rows)
	{
		auto& snow = globals::features::dynamicSnow;
		auto& s = snow.settings;
		const auto& st = snow.status;
		const char* group = "6. Snow / mud footprints";
		const bool installed = snow.loaded;

		std::string now;
		if (installed && s.EnableTrails) {
			if (!st.exterior)
				now = "Idle: indoors";
			else if (!st.trailsDrawn)
				now = "Starting";
			else
				now = std::format("On: {} actors, {} prints", st.actorsTracked, st.stamps);
		}
		a_rows.push_back(Toggle(group, "Footprints from the player and nearby actors", installed, s.EnableTrails, now, kWhere));

		std::string surfaces;
		auto add = [&surfaces](bool a_on, const char* a_name) {
			if (!a_on)
				return;
			if (!surfaces.empty())
				surfaces += ", ";
			surfaces += a_name;
		};
		add(s.TrailsOnSnow, "snowy ground");
		add(s.TrailsOnAccumulated, "built-up snow");
		add(s.MudTrails, "mud/dirt");
		a_rows.push_back(Value(group, "  Surfaces", installed, s.EnableTrails, surfaces.empty() ? "none" : surfaces, kWhere));
		a_rows.push_back(Value(group, "  Size / depth / refill", installed, s.EnableTrails,
			std::format("{:.2f}x / {:.1f} units / {:.0f} s", s.TrailSize, s.TrailDepth, s.TrailRefillSeconds), kWhere));
		const uint32_t size = 1024u << std::clamp(s.TrailResolution, 0, 2);
		a_rows.push_back(Value(group, "  Trail map", installed, s.EnableTrails,
			std::format("{0}x{0}, {1:.0f} units per texel, 58 m across", size, DynamicSnow::kTrailWindowUnits / static_cast<float>(size)), kWhere));
		a_rows.push_back(Toggle(group, "  Smooth footprints (bicubic, 39b)", installed, s.SmoothTrails, "", kWhere));
		a_rows.push_back(Toggle(group, "  Use installed footprint textures (39c)", installed, s.UseModFootprintShapes,
			st.shapesLoaded ? std::format("{} shapes ({})", st.shapesLoaded, st.shapeSource) : "", kWhere));
		a_rows.push_back(Toggle(group, "  Leave snowy ground to the Footprints mod (39c)", installed, s.YieldToFootprintsMod,
			!st.footprintsMod ? "Footprints mod not loaded" : st.yieldingToMod ? "Ours: built-up snow only" : "Both draw", kWhere));
		a_rows.push_back(Toggle(group, "  Trenches from bodies and objects (39c)", installed, s.BodyAndObjectTrails,
			st.bodyStamps ? std::format("{} this frame", st.bodyStamps) : "", kWhere));
	}
}
