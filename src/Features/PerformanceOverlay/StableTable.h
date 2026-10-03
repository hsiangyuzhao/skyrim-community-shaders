#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>
#include <vector>

/**
 * @brief (batch 36e) Display layer for the Performance Overlay tables: time-based smoothing,
 *        a stable row set and a row order that does not flicker.
 *
 * The timers behind the overlay are unchanged. This sits between them and ImGui and only
 * decides what number to print and in which row:
 *
 * - Smoothing is by TIME, not by frame count, so it means the same with frame generation
 *   on or off: a window average weights every sample by the wall time it was on screen.
 * - Rows never disappear. A row with no data in the current window prints "-" and keeps
 *   its place; a row seen for the first time is appended (or slotted into its fixed position).
 * - Ordering by cost uses hysteresis: a row only moves above the row ranked above it once it
 *   has been more than `swapThreshold` (10%) more expensive for `swapHoldSeconds` (0.5 s)
 *   straight. Two rows that cost about the same never trade places, and a real change still
 *   shows within half a second. Re-sorting every N seconds was the alternative; it was not
 *   chosen because every re-sort would still jump many rows at once, and between re-sorts the
 *   order can be visibly wrong for N seconds.
 */
namespace PerfView
{
	enum class SortMode : int
	{
		Smoothed = 0,  ///< by smoothed cost, with hysteresis (default)
		Fixed = 1,     ///< fixed order (the table's natural order)
		Live = 2       ///< by the current reading every frame (old behaviour, for debugging)
	};

	enum class SmoothMode : int
	{
		Off = 0,
		Window = 1,  ///< time-weighted average over the window (default)
		Ema = 2      ///< exponential average with a time constant equal to the window
	};

	struct ViewConfig
	{
		SortMode sort = SortMode::Smoothed;
		SmoothMode smooth = SmoothMode::Window;
		float windowSeconds = 0.5f;
		bool showLive = false;  ///< print the live reading instead of the smoothed one
		int topN = 0;           ///< 0 = all rows
		float swapThreshold = 0.10f;
		float swapHoldSeconds = 0.5f;
	};

	struct RowValues
	{
		float live = 0.0f;
		float smoothed = 0.0f;
		float peak = 0.0f;
		bool present = false;  ///< has a live reading this frame
		bool hasData = false;  ///< has any reading inside the window
	};

	template <class Row>
	class StableTable
	{
	public:
		struct Input
		{
			int id;
			Row row;
			float value;
			int fixedOrder;
		};

		/**
		 * @brief Feeds one frame. `a_now` is seconds on a monotonic clock.
		 * Rows not in `a_inputs` are kept, with no live reading this frame.
		 */
		void Update(double a_now, std::vector<Input>&& a_inputs, const ViewConfig& a_cfg)
		{
			double dt = (lastUpdate > 0.0) ? (a_now - lastUpdate) : 0.0;
			lastUpdate = a_now;
			// Long gaps (overlay hidden, loading screen) carry no information about the
			// rows; weight the first sample after one like a normal frame instead.
			if (dt <= 0.0 || dt > 0.25)
				dt = 1.0 / 60.0;

			for (auto& [id, e] : entries)
				e.values.present = false;

			for (auto& in : a_inputs) {
				auto [it, inserted] = entries.try_emplace(in.id);
				Entry& e = it->second;
				if (inserted) {
					e.seq = nextSeq++;
					rank.push_back(in.id);
				}
				e.row = std::move(in.row);
				e.fixedOrder = in.fixedOrder;
				e.values.live = in.value;
				e.values.present = true;
				e.samples.push_back(Sample{ a_now, static_cast<float>(dt), in.value });
				if (!e.emaPrimed) {
					e.ema = in.value;
					e.emaPrimed = true;
				} else {
					const double tau = std::max(0.05, static_cast<double>(a_cfg.windowSeconds));
					const float alpha = static_cast<float>(1.0 - std::exp(-dt / tau));
					e.ema += alpha * (in.value - e.ema);
				}
			}

			const double window = std::max(0.05, static_cast<double>(a_cfg.windowSeconds));
			for (auto& [id, e] : entries) {
				while (!e.samples.empty() && e.samples.front().t < a_now - window)
					e.samples.pop_front();
				RowValues& v = e.values;
				v.hasData = !e.samples.empty();
				if (!v.hasData) {
					e.emaPrimed = false;  // restart instead of resuming from a stale value
					v.smoothed = v.peak = 0.0f;
					continue;
				}
				double wsum = 0.0, vsum = 0.0;
				float peak = -std::numeric_limits<float>::max();
				for (const auto& s : e.samples) {
					wsum += s.weight;
					vsum += static_cast<double>(s.value) * s.weight;
					peak = std::max(peak, s.value);
				}
				v.peak = peak;
				switch (a_cfg.smooth) {
				case SmoothMode::Off:
					v.smoothed = v.present ? v.live : e.samples.back().value;
					break;
				case SmoothMode::Ema:
					v.smoothed = e.ema;
					break;
				default:
					v.smoothed = wsum > 0.0 ? static_cast<float>(vsum / wsum) : e.samples.back().value;
					break;
				}
			}

			UpdateRank(a_now, a_cfg);
		}

		/// @brief Row ids in display order, after Top N. Stable between Update() calls.
		std::vector<int> DisplayOrder(const ViewConfig& a_cfg) const
		{
			std::vector<int> order;
			const size_t limit = (a_cfg.topN > 0) ? std::min(rank.size(), static_cast<size_t>(a_cfg.topN)) : rank.size();

			if (a_cfg.sort == SortMode::Live) {
				order = rank;
				std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
					return LiveKey(a) > LiveKey(b);
				});
				order.resize(limit);
				return order;
			}

			// Top N is always chosen by smoothed cost with hysteresis, so the selection is
			// as steady as the order.
			order.assign(rank.begin(), rank.begin() + limit);
			if (a_cfg.sort == SortMode::Fixed) {
				std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
					const Entry& ea = entries.at(a);
					const Entry& eb = entries.at(b);
					if (ea.fixedOrder != eb.fixedOrder)
						return ea.fixedOrder < eb.fixedOrder;
					return ea.seq < eb.seq;
				});
			}
			return order;
		}

		const Row* GetRow(int a_id) const
		{
			auto it = entries.find(a_id);
			return it == entries.end() ? nullptr : &it->second.row;
		}

		RowValues GetValues(int a_id) const
		{
			auto it = entries.find(a_id);
			return it == entries.end() ? RowValues{} : it->second.values;
		}

		/// @brief The number printed for a row under the current config, or nullopt-like
		///        `false` return when there is nothing to print ("-").
		bool DisplayValue(int a_id, const ViewConfig& a_cfg, float& o_value) const
		{
			const RowValues v = GetValues(a_id);
			if (a_cfg.showLive) {
				o_value = v.live;
				return v.present;
			}
			o_value = v.smoothed;
			return v.hasData;
		}

		size_t RowCount() const { return rank.size(); }

		void Clear()
		{
			entries.clear();
			rank.clear();
			challenge.clear();
			lastUpdate = 0.0;
			nextSeq = 0;
		}

	private:
		struct Sample
		{
			double t;
			float weight;
			float value;
		};

		struct Entry
		{
			Row row{};
			std::deque<Sample> samples;
			RowValues values;
			float ema = 0.0f;
			bool emaPrimed = false;
			int fixedOrder = 0;
			int seq = 0;
		};

		float RankKey(int a_id) const
		{
			const RowValues& v = entries.at(a_id).values;
			return v.hasData ? v.smoothed : -1.0f;
		}

		float LiveKey(int a_id) const
		{
			const RowValues& v = entries.at(a_id).values;
			return v.present ? v.live : -1.0f;
		}

		static bool Beats(float a_lower, float a_upper, float a_threshold)
		{
			// "-" rows (negative key) sink below every row that has data, and never swap
			// among themselves.
			if (a_lower < 0.0f)
				return false;
			if (a_upper < 0.0f)
				return true;
			constexpr float kAbsoluteEpsilonMs = 0.005f;
			return a_lower > a_upper * (1.0f + a_threshold) + kAbsoluteEpsilonMs;
		}

		static uint64_t PairKey(int a_upper, int a_lower)
		{
			return (static_cast<uint64_t>(static_cast<uint32_t>(a_upper)) << 32) | static_cast<uint32_t>(a_lower);
		}

		/// Hysteresis ranking. For every pair (upper, lower) of the current order, remember
		/// since when `lower` has clearly beaten `upper`; an insertion pass then lets a row
		/// climb past exactly those rows it has beaten for long enough.
		void UpdateRank(double a_now, const ViewConfig& a_cfg)
		{
			const size_t n = rank.size();
			std::vector<float> keys(n);
			for (size_t i = 0; i < n; ++i)
				keys[i] = RankKey(rank[i]);

			std::unordered_map<uint64_t, double> next;
			for (size_t i = 0; i < n; ++i) {
				for (size_t j = i + 1; j < n; ++j) {
					if (!Beats(keys[j], keys[i], a_cfg.swapThreshold))
						continue;
					const uint64_t key = PairKey(rank[i], rank[j]);
					auto it = challenge.find(key);
					next.emplace(key, it != challenge.end() ? it->second : a_now);
				}
			}
			challenge = std::move(next);

			const auto matured = [&](int a_upper, int a_lower) {
				auto it = challenge.find(PairKey(a_upper, a_lower));
				return it != challenge.end() && (a_now - it->second) >= a_cfg.swapHoldSeconds;
			};

			std::vector<int> result;
			result.reserve(n);
			for (size_t i = 0; i < n; ++i) {
				const int id = rank[i];
				size_t pos = result.size();
				while (pos > 0 && matured(result[pos - 1], id))
					--pos;
				result.insert(result.begin() + pos, id);
			}
			rank = std::move(result);
		}

		std::unordered_map<int, Entry> entries;
		std::vector<int> rank;  // every row, in hysteresis order (rows with no data at the end)
		std::unordered_map<uint64_t, double> challenge;
		double lastUpdate = 0.0;
		int nextSeq = 0;
	};
}
