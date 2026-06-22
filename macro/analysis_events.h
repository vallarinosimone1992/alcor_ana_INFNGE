#ifndef ANALYSIS_EVENTS_H
#define ANALYSIS_EVENTS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace analysis_events {

constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();

struct EventHitRef {
  std::size_t index = kInvalidIndex;
  int channel = -1;
  int tdc = -1;
  double time_ns = 0.0;
  double dt_ns = 0.0;
};

struct Event {
  int event_id = 0;
  int spill = 0;
  double reference_time_ns = 0.0;
  std::size_t reference_index = kInvalidIndex;
  int reference_channel = -1;
  int reference_tdc = -1;
  std::vector<EventHitRef> hits;
};

inline double Median(std::vector<double> values)
{
  if (values.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  if ((values.size() % 2) == 1) {
    return values[mid];
  }
  return 0.5 * (values[mid - 1] + values[mid]);
}

template <typename Hit>
std::vector<Event> BuildReferenceEvents(const std::vector<Hit> &hits,
                                        std::vector<std::size_t> reference_indices,
                                        std::vector<std::size_t> candidate_indices,
                                        double event_window_ns,
                                        bool signed_window,
                                        bool one_hit_per_channel = true)
{
  // Callers must pass hits from one run/spill. This helper sorts only by time
  // and does not protect against accidental cross-spill event construction.
  std::vector<Event> events;
  if (hits.empty() || reference_indices.empty() || candidate_indices.empty() || event_window_ns <= 0.0) {
    return events;
  }

  auto by_time = [&](std::size_t a, std::size_t b) {
    if (hits[a].time_ns != hits[b].time_ns) {
      return hits[a].time_ns < hits[b].time_ns;
    }
    return a < b;
  };
  std::sort(reference_indices.begin(), reference_indices.end(), by_time);
  std::sort(candidate_indices.begin(), candidate_indices.end(), by_time);

  int event_id = 0;
  for (std::size_t ref_index : reference_indices) {
    if (ref_index >= hits.size()) {
      continue;
    }
    const auto &ref = hits[ref_index];
    const double ref_time = ref.time_ns;
    const double min_time = signed_window ? ref_time - event_window_ns : ref_time;
    const double max_time = ref_time + event_window_ns;
    auto first = std::lower_bound(candidate_indices.begin(),
                                  candidate_indices.end(),
                                  min_time,
                                  [&](std::size_t idx, double time) { return hits[idx].time_ns < time; });

    Event event;
    event.event_id = event_id++;
    event.spill = ref.spill;
    event.reference_time_ns = ref_time;
    event.reference_index = ref_index;
    event.reference_channel = ref.channel;
    event.reference_tdc = ref.tdc;

    std::map<int, EventHitRef> best_by_channel;
    for (auto it = first; it != candidate_indices.end(); ++it) {
      const std::size_t idx = *it;
      if (idx >= hits.size() || idx == ref_index) {
        continue;
      }
      const auto &hit = hits[idx];
      if (hit.time_ns > max_time) {
        break;
      }
      const double dt = hit.time_ns - ref_time;
      if (!signed_window && dt < 0.0) {
        continue;
      }
      if (std::abs(dt) > event_window_ns) {
        continue;
      }
      EventHitRef ref_hit{idx, hit.channel, hit.tdc, hit.time_ns, dt};
      if (!one_hit_per_channel) {
        event.hits.push_back(ref_hit);
        continue;
      }
      auto best_it = best_by_channel.find(hit.channel);
      if (best_it == best_by_channel.end() ||
          std::abs(ref_hit.dt_ns) < std::abs(best_it->second.dt_ns)) {
        best_by_channel[hit.channel] = ref_hit;
      }
    }

    if (one_hit_per_channel) {
      for (const auto &kv : best_by_channel) {
        event.hits.push_back(kv.second);
      }
    }
    if (!event.hits.empty()) {
      events.push_back(std::move(event));
    }
  }
  return events;
}

template <typename Hit>
void AppendClusterEvent(const std::vector<Hit> &hits,
                        const std::vector<std::size_t> &cluster,
                        double event_window_ns,
                        int min_channels,
                        int &event_id,
                        std::vector<Event> &events)
{
  if (cluster.empty()) {
    return;
  }
  std::vector<double> times;
  times.reserve(cluster.size());
  for (std::size_t idx : cluster) {
    if (idx < hits.size()) {
      times.push_back(hits[idx].time_ns);
    }
  }
  const double preliminary_median = Median(times);
  if (!std::isfinite(preliminary_median)) {
    return;
  }

  std::map<int, std::size_t> best_by_channel;
  for (std::size_t idx : cluster) {
    if (idx >= hits.size()) {
      continue;
    }
    const auto &hit = hits[idx];
    auto best_it = best_by_channel.find(hit.channel);
    if (best_it == best_by_channel.end() ||
        std::abs(hit.time_ns - preliminary_median) <
            std::abs(hits[best_it->second].time_ns - preliminary_median)) {
      best_by_channel[hit.channel] = idx;
    }
  }
  if (static_cast<int>(best_by_channel.size()) < std::max(1, min_channels)) {
    return;
  }

  std::vector<double> unique_times;
  unique_times.reserve(best_by_channel.size());
  for (const auto &kv : best_by_channel) {
    unique_times.push_back(hits[kv.second].time_ns);
  }
  const double reference = Median(unique_times);
  if (!std::isfinite(reference)) {
    return;
  }

  Event event;
  event.event_id = event_id++;
  event.spill = hits[best_by_channel.begin()->second].spill;
  event.reference_time_ns = reference;
  event.reference_index = kInvalidIndex;
  event.reference_channel = -1;
  event.reference_tdc = hits[best_by_channel.begin()->second].tdc;
  for (const auto &kv : best_by_channel) {
    const auto &hit = hits[kv.second];
    const double dt = hit.time_ns - reference;
    if (std::abs(dt) > event_window_ns) {
      continue;
    }
    event.hits.push_back({kv.second, hit.channel, hit.tdc, hit.time_ns, dt});
  }
  if (static_cast<int>(event.hits.size()) >= std::max(1, min_channels)) {
    events.push_back(std::move(event));
  }
}

template <typename Hit>
std::vector<Event> BuildClusterEvents(const std::vector<Hit> &hits,
                                      std::vector<std::size_t> candidate_indices,
                                      double event_window_ns,
                                      int min_channels)
{
  // Event clusters are bounded relative to the first hit in the cluster, not by
  // a sliding nearest-neighbor chain. This matches the coincidence-window use.
  std::vector<Event> events;
  if (hits.empty() || candidate_indices.empty() || event_window_ns <= 0.0) {
    return events;
  }

  std::sort(candidate_indices.begin(), candidate_indices.end(), [&](std::size_t a, std::size_t b) {
    if (hits[a].time_ns != hits[b].time_ns) {
      return hits[a].time_ns < hits[b].time_ns;
    }
    return a < b;
  });

  int event_id = 0;
  std::vector<std::size_t> cluster;
  cluster.reserve(candidate_indices.size());
  double cluster_seed = hits[candidate_indices.front()].time_ns;
  for (std::size_t idx : candidate_indices) {
    if (!cluster.empty() && std::abs(hits[idx].time_ns - cluster_seed) > event_window_ns) {
      AppendClusterEvent(hits, cluster, event_window_ns, min_channels, event_id, events);
      cluster.clear();
      cluster_seed = hits[idx].time_ns;
    }
    cluster.push_back(idx);
  }
  AppendClusterEvent(hits, cluster, event_window_ns, min_channels, event_id, events);
  return events;
}

}  // namespace analysis_events

#endif
