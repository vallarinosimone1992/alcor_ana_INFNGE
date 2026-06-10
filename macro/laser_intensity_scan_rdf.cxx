#include <TCanvas.h>
#include <TChain.h>
#include <TColor.h>
#include <TF1.h>
#include <TFile.h>
#include <TGraph.h>
#include <TGraphErrors.h>
#include <TH1D.h>
#include <TH2D.h>
#include <THStack.h>
#include <TLegend.h>
#include <TLine.h>
#include <TList.h>
#include <TMultiGraph.h>
#include <TParameter.h>
#include <TPad.h>
#include <TProfile.h>
#include <TStyle.h>
#include <TSystem.h>

#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
constexpr int kNumAlcorChannels = 32;
constexpr double kTimewalkDtLimitNs = 20.0;
constexpr double kCorrectedTimewalkDtMinNs = -3.0;
constexpr double kCorrectedTimewalkDtMaxNs = 3.0;
constexpr double kCorrectedCoincidenceDtMinNs = -3.0;
constexpr double kCorrectedCoincidenceDtMaxNs = 3.0;

struct FitRange {
  bool enabled = false;
  double xmin = 0.0;
  double xmax = 0.0;
};

struct DtTotCut {
  bool enabled = false;
  double intercept = 0.0;
  double slope = 0.0;
  double tot_min = -std::numeric_limits<double>::infinity();
  double tot_max = std::numeric_limits<double>::infinity();

  double Boundary(double tot) const { return intercept + slope * tot; }

  bool Applies(double tot) const
  {
    return enabled && tot >= tot_min && tot <= tot_max;
  }

  bool Pass(double tot, double dt) const
  {
    if (!Applies(tot)) {
      return true;
    }
    return dt >= Boundary(tot);
  }
};

struct TotWindow {
  bool enabled = false;
  double min = 0.0;
  double max = 0.0;

  bool Pass(double tot) const
  {
    if (!enabled) {
      return true;
    }
    return std::isfinite(tot) && tot >= min && tot <= max;
  }
};

struct SpillRange {
  bool enabled = false;
  int min = 0;
  int max = 0;

  bool Pass(int spill) const
  {
    if (!enabled) {
      return true;
    }
    return spill >= min && spill <= max;
  }
};

enum class TimewalkFitModel {
  Pol1 = 0,
  LinExpPlateau = 1,
};

std::string TimewalkFitModelName(TimewalkFitModel model)
{
  switch (model) {
    case TimewalkFitModel::Pol1:
      return "pol1";
    case TimewalkFitModel::LinExpPlateau:
      return "lin-exp-plateau";
  }
  return "unknown";
}

TimewalkFitModel ParseTimewalkFitModel(const std::string &value)
{
  if (value == "pol1" || value == "linear") {
    return TimewalkFitModel::Pol1;
  }
  if (value == "lin-exp-plateau" || value == "lin_exp_plateau" || value == "piecewise") {
    return TimewalkFitModel::LinExpPlateau;
  }
  std::cerr << "Unknown timewalk fit model '" << value << "', using lin-exp-plateau" << std::endl;
  return TimewalkFitModel::LinExpPlateau;
}

bool WantsHelp(const char *arg)
{
  if (!arg) {
    return true;
  }
  std::string value(arg);
  return value == "-h" || value == "--help" || value == "help";
}

void PrintHelp()
{
  std::cout << "laser_intensity_scan_rdf usage:\n"
            << "  laser_intensity_scan_rdf(\"runlist.tsv\", \"fine.root\", \"channel.root\","
            << " \"out.pdf\", \"out.root\", \"out.txt\", 22, \"17,19\", 100, 30, 320, true,"
            << " true, 0, true, 50000, 1000000, 50000, false, \"17:7:18.5,19:3:15\","
            << " \"lin-exp-plateau\", \"\", \"\", \"\", \"\", -1, \"all\", 0, 0.01)\n\n"
            << "Runlist TSV columns: run_label, input_path, intensity, channels, thresholds, spill, vbias, note\n"
            << "input_path can be a decoded dir, run dir, or parent dir accepted by analysis_io::ResolveInputSpec.\n"
            << "Optional dt/ToT cuts use CH:DT0:SLOPE[:TOT_MIN:TOT_MAX] and keep dt >= DT0 + SLOPE*ToT.\n"
            << "Optional trigger ToT window uses MIN:MAX, e.g. 1:3.\n"
            << "Optional spill range uses MIN:MAX, inclusive.\n"
            << "Optional channel ToT windows use CH:MIN:MAX, e.g. 17:18:25.\n"
            << "Optional edge diagnostic spill uses -1 for the first selected spill.\n"
            << "Optional edge diagnostic channels use all, analysis, or a CSV list.\n"
            << "Optional edge diagnostic fraction is the initial spill fraction to plot, default 0.01.\n";
}

bool IsLeadingTdc(int tdc)
{
  return (tdc & 0x1) == 0;
}

bool IsTrailingTdc(int tdc)
{
  return (tdc & 0x1) == 1;
}

bool IsValidTdcId(int tdc)
{
  return tdc >= 0 && tdc <= 3;
}

int TdcPairIndex(int tdc)
{
  return tdc >> 1;
}

std::string SafeName(std::string value)
{
  for (char &ch : value) {
    const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    if (!keep) {
      ch = '_';
    }
  }
  return value;
}

std::vector<std::string> Split(const std::string &line, char sep)
{
  std::vector<std::string> out;
  std::string item;
  std::istringstream iss(line);
  while (std::getline(iss, item, sep)) {
    out.push_back(item);
  }
  return out;
}

std::vector<int> ParseChannelsCsv(const std::string &csv)
{
  std::vector<int> channels;
  for (const auto &token : Split(csv, ',')) {
    if (token.empty()) {
      continue;
    }
    channels.push_back(std::stoi(token));
  }
  std::sort(channels.begin(), channels.end());
  channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
  return channels;
}

std::vector<int> ParseEdgeChannelsCsv(const std::string &csv, const std::vector<int> &analysis_channels)
{
  if (csv.empty() || csv == "analysis" || csv == "selected") {
    return analysis_channels;
  }
  if (csv == "all" || csv == "*") {
    std::vector<int> channels;
    channels.reserve(kNumAlcorChannels);
    for (int ch = 0; ch < kNumAlcorChannels; ++ch) {
      channels.push_back(ch);
    }
    return channels;
  }
  return ParseChannelsCsv(csv);
}

std::map<int, FitRange> ParseFitRangesCsv(const std::string &csv)
{
  std::map<int, FitRange> ranges;
  for (const auto &token : Split(csv, ',')) {
    if (token.empty()) {
      continue;
    }
    auto fields = Split(token, ':');
    if (fields.size() != 3) {
      std::cerr << "Skipping invalid timewalk fit range: " << token << std::endl;
      continue;
    }
    try {
      const int channel = std::stoi(fields[0]);
      const double xmin = std::stod(fields[1]);
      const double xmax = std::stod(fields[2]);
      if (channel < 0 || xmax <= xmin) {
        std::cerr << "Skipping invalid timewalk fit range: " << token << std::endl;
        continue;
      }
      ranges[channel] = FitRange{true, xmin, xmax};
    } catch (const std::exception &) {
      std::cerr << "Skipping invalid timewalk fit range: " << token << std::endl;
    }
  }
  return ranges;
}

FitRange FitRangeForChannel(const std::map<int, FitRange> &ranges, int channel)
{
  auto it = ranges.find(channel);
  if (it == ranges.end()) {
    return FitRange{};
  }
  return it->second;
}

std::map<int, DtTotCut> ParseDtTotCutsCsv(const std::string &csv)
{
  std::map<int, DtTotCut> cuts;
  for (const auto &token : Split(csv, ',')) {
    if (token.empty()) {
      continue;
    }
    auto fields = Split(token, ':');
    if (fields.size() != 3 && fields.size() != 5) {
      std::cerr << "Skipping invalid dt/ToT cut: " << token << std::endl;
      continue;
    }
    try {
      const int channel = std::stoi(fields[0]);
      DtTotCut cut;
      cut.enabled = true;
      cut.intercept = std::stod(fields[1]);
      cut.slope = std::stod(fields[2]);
      if (fields.size() == 5) {
        cut.tot_min = std::stod(fields[3]);
        cut.tot_max = std::stod(fields[4]);
        if (cut.tot_max <= cut.tot_min) {
          std::cerr << "Skipping invalid dt/ToT cut: " << token << std::endl;
          continue;
        }
      }
      if (channel < 0 || !std::isfinite(cut.intercept) || !std::isfinite(cut.slope)) {
        std::cerr << "Skipping invalid dt/ToT cut: " << token << std::endl;
        continue;
      }
      cuts[channel] = cut;
    } catch (const std::exception &) {
      std::cerr << "Skipping invalid dt/ToT cut: " << token << std::endl;
    }
  }
  return cuts;
}

DtTotCut DtTotCutForChannel(const std::map<int, DtTotCut> &cuts, int channel)
{
  auto it = cuts.find(channel);
  if (it == cuts.end()) {
    return DtTotCut{};
  }
  return it->second;
}

TotWindow ParseTotWindow(const std::string &csv)
{
  TotWindow window;
  if (csv.empty()) {
    return window;
  }
  auto fields = Split(csv, ':');
  if (fields.size() != 2) {
    std::cerr << "Skipping invalid trigger ToT window: " << csv << std::endl;
    return window;
  }
  try {
    const double min = std::stod(fields[0]);
    const double max = std::stod(fields[1]);
    if (!std::isfinite(min) || !std::isfinite(max) || max <= min) {
      std::cerr << "Skipping invalid trigger ToT window: " << csv << std::endl;
      return window;
    }
    window.enabled = true;
    window.min = min;
    window.max = max;
  } catch (const std::exception &) {
    std::cerr << "Skipping invalid trigger ToT window: " << csv << std::endl;
  }
  return window;
}

std::map<int, TotWindow> ParseChannelTotWindowsCsv(const std::string &csv)
{
  std::map<int, TotWindow> windows;
  for (const auto &token : Split(csv, ',')) {
    if (token.empty()) {
      continue;
    }
    auto fields = Split(token, ':');
    if (fields.size() != 3) {
      std::cerr << "Skipping invalid channel ToT window: " << token << std::endl;
      continue;
    }
    try {
      const int channel = std::stoi(fields[0]);
      const double min = std::stod(fields[1]);
      const double max = std::stod(fields[2]);
      if (channel < 0 || !std::isfinite(min) || !std::isfinite(max) || max <= min) {
        std::cerr << "Skipping invalid channel ToT window: " << token << std::endl;
        continue;
      }
      windows[channel] = TotWindow{true, min, max};
    } catch (const std::exception &) {
      std::cerr << "Skipping invalid channel ToT window: " << token << std::endl;
    }
  }
  return windows;
}

TotWindow ChannelTotWindowForChannel(const std::map<int, TotWindow> &windows, int channel)
{
  auto it = windows.find(channel);
  if (it == windows.end()) {
    return TotWindow{};
  }
  return it->second;
}

SpillRange ParseSpillRange(const std::string &csv)
{
  SpillRange range;
  if (csv.empty()) {
    return range;
  }
  auto fields = Split(csv, ':');
  if (fields.size() != 2) {
    std::cerr << "Skipping invalid spill range: " << csv << std::endl;
    return range;
  }
  try {
    const int min = std::stoi(fields[0]);
    const int max = std::stoi(fields[1]);
    if (max < min) {
      std::cerr << "Skipping invalid spill range: " << csv << std::endl;
      return range;
    }
    range.enabled = true;
    range.min = min;
    range.max = max;
  } catch (const std::exception &) {
    std::cerr << "Skipping invalid spill range: " << csv << std::endl;
  }
  return range;
}

double ParseDoubleOrNan(const std::string &value)
{
  if (value.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  char *end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (!end || *end != '\0') {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return parsed;
}

struct RunConfig {
  std::string label;
  std::string input_path;
  double intensity = std::numeric_limits<double>::quiet_NaN();
  std::string channels;
  std::string thresholds;
  std::string spill;
  std::string vbias;
  std::string note;
};

std::vector<RunConfig> LoadRunList(const std::string &path)
{
  std::ifstream fin(path);
  std::vector<RunConfig> runs;
  if (!fin) {
    std::cerr << "Cannot open run list: " << path << std::endl;
    return runs;
  }

  std::string line;
  while (std::getline(fin, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto cols = Split(line, '\t');
    if (cols.size() < 3) {
      std::cerr << "Skipping invalid run-list row: " << line << std::endl;
      continue;
    }
    RunConfig run;
    run.label = cols[0];
    run.input_path = cols[1];
    run.intensity = ParseDoubleOrNan(cols[2]);
    if (cols.size() > 3) {
      run.channels = cols[3];
    }
    if (cols.size() > 4) {
      run.thresholds = cols[4];
    }
    if (cols.size() > 5) {
      run.spill = cols[5];
    }
    if (cols.size() > 6) {
      run.vbias = cols[6];
    }
    if (cols.size() > 7) {
      run.note = cols[7];
    }
    if (!std::isfinite(run.intensity)) {
      std::cerr << "Skipping row with invalid intensity: " << line << std::endl;
      continue;
    }
    runs.push_back(std::move(run));
  }
  return runs;
}

struct Hit {
  int channel = -1;
  int spill = 0;
  int fifo = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int fine = 0;
  Long64_t time_tick = 0;
  double time_ns_raw = 0.0;
  double time_ns = 0.0;
  double tot_ns = -1.0;
  bool leading = false;
};

struct EdgeHit {
  int channel = -1;
  int spill = 0;
  bool leading = false;
  double time_ns = 0.0;
};

struct StoredSensorHit {
  int channel = -1;
  int spill = 0;
  double time_ns = 0.0;
  double tot_ns = 0.0;
};

struct DurationKey {
  int spill = 0;
  int channel = 0;
  int pair = 0;

  bool operator<(const DurationKey &other) const
  {
    if (spill != other.spill) {
      return spill < other.spill;
    }
    if (channel != other.channel) {
      return channel < other.channel;
    }
    return pair < other.pair;
  }
};

void ComputeTot(std::vector<Hit> &hits, double max_duration_ns)
{
  std::map<DurationKey, std::vector<size_t>> groups;
  for (size_t i = 0; i < hits.size(); ++i) {
    if (!IsValidTdcId(hits[i].tdc)) {
      continue;
    }
    groups[{hits[i].spill, hits[i].channel, TdcPairIndex(hits[i].tdc)}].push_back(i);
  }

  for (auto &kv : groups) {
    auto &indices = kv.second;
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
      if (hits[a].time_tick != hits[b].time_tick) {
        return hits[a].time_tick < hits[b].time_tick;
      }
      return hits[a].fine < hits[b].fine;
    });

    size_t leading_index = std::numeric_limits<size_t>::max();
    int leading_tdc = -1;
    for (size_t idx : indices) {
      const auto &hit = hits[idx];
      if (IsLeadingTdc(hit.tdc)) {
        leading_index = idx;
        leading_tdc = hit.tdc;
        continue;
      }
      if (!IsTrailingTdc(hit.tdc)) {
        continue;
      }
      if (leading_index == std::numeric_limits<size_t>::max()) {
        continue;
      }
      if (hit.tdc != (leading_tdc ^ 0x1)) {
        continue;
      }
      const double dt = hit.time_ns_raw - hits[leading_index].time_ns_raw;
      const bool within_max = max_duration_ns <= 0.0 || dt <= max_duration_ns;
      if (dt > 0.0 && within_max) {
        hits[leading_index].tot_ns = dt;
      }
      leading_index = std::numeric_limits<size_t>::max();
      leading_tdc = -1;
    }
  }
}

struct Stats {
  long long n = 0;
  double mean = std::numeric_limits<double>::quiet_NaN();
  double rms = std::numeric_limits<double>::quiet_NaN();
  double err_mean = std::numeric_limits<double>::quiet_NaN();
  double err_rms = std::numeric_limits<double>::quiet_NaN();
};

Stats ComputeStats(const std::vector<double> &values)
{
  Stats stats;
  stats.n = static_cast<long long>(values.size());
  if (values.empty()) {
    return stats;
  }
  const double sum = std::accumulate(values.begin(), values.end(), 0.0);
  stats.mean = sum / static_cast<double>(values.size());
  double var = 0.0;
  for (double value : values) {
    const double d = value - stats.mean;
    var += d * d;
  }
  var /= static_cast<double>(values.size());
  stats.rms = std::sqrt(std::max(0.0, var));
  stats.err_mean = stats.rms / std::sqrt(static_cast<double>(values.size()));
  if (values.size() > 1) {
    stats.err_rms = stats.rms / std::sqrt(2.0 * static_cast<double>(values.size() - 1));
  } else {
    stats.err_rms = 0.0;
  }
  return stats;
}

struct ChannelSummary {
  Stats dt;
  Stats tot;
};

struct TimewalkCorrection {
  bool valid = false;
  TimewalkFitModel model = TimewalkFitModel::Pol1;
  double p0 = 0.0;
  double p1 = 0.0;
  double p2 = 0.0;
  double p3 = 1.0;
  double p4 = 0.0;
  FitRange fit_range;

  double CorrectionNs(double tot) const
  {
    if (!valid || !std::isfinite(tot)) {
      return 0.0;
    }
    double value = 0.0;
    if (model == TimewalkFitModel::LinExpPlateau) {
      if (tot <= p2 || p3 <= 0.0) {
        value = p0 + p1 * tot;
      } else {
        value = p4 + (p0 + p1 * p2 - p4) * std::exp(-(tot - p2) / p3);
      }
    } else {
      value = p0 + p1 * tot;
    }
    if (!std::isfinite(value) || value <= 0.0) {
      return 0.0;
    }
    return value;
  }
};

struct RunResult {
  RunConfig config;
  std::map<int, ChannelSummary> channels;
  std::vector<std::unique_ptr<TH1D>> histograms;
  std::vector<std::unique_ptr<TH2D>> histograms2d;
  std::vector<StoredSensorHit> trigger_hits;
  std::vector<StoredSensorHit> sensor_hits;
  long long hits_read = 0;
  long long leading_hits = 0;
  long long trigger22_raw_candidates = 0;
  long long trigger22_tot_window_candidates = 0;
  long long trigger22_clean_candidates = 0;
  Stats trigger22_clean_period;
};

TH1D *MakeHist(std::vector<std::unique_ptr<TH1D>> &owner,
               const std::string &name,
               const std::string &title,
               int bins,
               double xmin,
               double xmax)
{
  auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), bins, xmin, xmax);
  hist->SetDirectory(nullptr);
  TH1D *ptr = hist.get();
  owner.push_back(std::move(hist));
  return ptr;
}

TH2D *MakeHist2D(std::vector<std::unique_ptr<TH2D>> &owner,
                 const std::string &name,
                 const std::string &title,
                 int xbins,
                 double xmin,
                 double xmax,
                 int ybins,
                 double ymin,
                 double ymax)
{
  auto hist = std::make_unique<TH2D>(name.c_str(), title.c_str(), xbins, xmin, xmax, ybins, ymin, ymax);
  hist->SetDirectory(nullptr);
  TH2D *ptr = hist.get();
  owner.push_back(std::move(hist));
  return ptr;
}

double TimewalkDtMin(bool signed_dt, double match_window_ns)
{
  if (!signed_dt) {
    return 0.0;
  }
  return -std::min(std::abs(match_window_ns), kTimewalkDtLimitNs);
}

double TimewalkDtMax(double match_window_ns)
{
  return std::min(std::abs(match_window_ns), kTimewalkDtLimitNs);
}

std::unique_ptr<TProfile> MakeTimewalkProfile(const TH2D &hist)
{
  auto profile = std::unique_ptr<TProfile>(
      hist.ProfileX((std::string(hist.GetName()) + "_pfx").c_str(), 1, -1, ""));
  if (!profile) {
    return nullptr;
  }
  profile->SetDirectory(nullptr);
  profile->SetTitle((std::string(hist.GetTitle()) + " profile;ToT [ns];mean #Deltat [ns]").c_str());
  profile->SetMarkerStyle(20);
  profile->SetMarkerSize(0.75);
  profile->SetMarkerColor(kBlack);
  profile->SetLineColor(kBlack);
  profile->SetLineWidth(2);
  return profile;
}

std::unique_ptr<TProfile> MakeTotVsSpillProfile(const TH2D &hist)
{
  auto profile = std::unique_ptr<TProfile>(
      hist.ProfileX((std::string(hist.GetName()) + "_pfx").c_str(), 1, -1, ""));
  if (!profile) {
    return nullptr;
  }
  profile->SetDirectory(nullptr);
  profile->SetTitle((std::string(hist.GetTitle()) + " profile;spill;mean ToT [ns]").c_str());
  profile->SetMarkerStyle(20);
  profile->SetMarkerSize(0.75);
  profile->SetMarkerColor(kBlack);
  profile->SetLineColor(kBlack);
  profile->SetLineWidth(2);
  return profile;
}

std::unique_ptr<TProfile> MakeDtVsSpillProfile(const TH2D &hist)
{
  auto profile = std::unique_ptr<TProfile>(
      hist.ProfileX((std::string(hist.GetName()) + "_pfx").c_str(), 1, -1, ""));
  if (!profile) {
    return nullptr;
  }
  profile->SetDirectory(nullptr);
  profile->SetTitle((std::string(hist.GetTitle()) + " profile;spill;mean #Deltat [ns]").c_str());
  profile->SetMarkerStyle(20);
  profile->SetMarkerSize(0.75);
  profile->SetMarkerColor(kBlack);
  profile->SetLineColor(kBlack);
  profile->SetLineWidth(2);
  return profile;
}

void LabelChannelAxis(TH2D *hist, int channel_min, int channel_max)
{
  if (!hist) {
    return;
  }
  auto *axis = hist->GetYaxis();
  for (int ch = channel_min; ch <= channel_max; ++ch) {
    axis->SetBinLabel(ch - channel_min + 1, std::to_string(ch).c_str());
  }
}

int FullSpillTimeBins(double range_ms)
{
  if (range_ms <= 0.0 || !std::isfinite(range_ms)) {
    return 1;
  }
  return std::min(4000, std::max(100, static_cast<int>(std::ceil(range_ms * 10.0))));
}

int PhaseTimeBins(double period_ns)
{
  if (period_ns <= 0.0 || !std::isfinite(period_ns)) {
    return 1;
  }
  return std::min(2000, std::max(200, static_cast<int>(std::ceil(period_ns / 1000.0))));
}

void BuildSingleSpillEdgePlots(RunResult &result,
                               const std::vector<EdgeHit> &edge_hits,
                               const std::vector<int> &edge_channels,
                               int edge_spill,
                               int trigger_channel,
                               double trigger_period_ns,
                               double edge_phase_period_ns,
                               double edge_spill_fraction)
{
  if (edge_hits.empty() || edge_channels.empty()) {
    return;
  }

  double min_time_ns = std::numeric_limits<double>::infinity();
  double max_time_ns = -std::numeric_limits<double>::infinity();
  double trigger_reference_ns = std::numeric_limits<double>::quiet_NaN();
  for (const auto &hit : edge_hits) {
    min_time_ns = std::min(min_time_ns, hit.time_ns);
    max_time_ns = std::max(max_time_ns, hit.time_ns);
    if (!std::isfinite(trigger_reference_ns) && hit.channel == trigger_channel && hit.leading) {
      trigger_reference_ns = hit.time_ns;
    }
  }
  if (!std::isfinite(min_time_ns) || !std::isfinite(max_time_ns)) {
    return;
  }
  if (!std::isfinite(trigger_reference_ns)) {
    trigger_reference_ns = min_time_ns;
  }

  const double full_time_range_ns = std::max(1.0, max_time_ns - min_time_ns);
  const double spill_fraction =
      (edge_spill_fraction > 0.0 && edge_spill_fraction <= 1.0 && std::isfinite(edge_spill_fraction))
          ? edge_spill_fraction
          : 1.0;
  const double window_max_time_ns = min_time_ns + spill_fraction * full_time_range_ns;
  const int channel_min = edge_channels.front();
  const int channel_max = edge_channels.back();
  const int channel_bins = std::max(1, channel_max - channel_min + 1);
  const double time_range_ms = std::max(1e-6, (window_max_time_ns - min_time_ns) / 1.0e6);
  const int time_bins = FullSpillTimeBins(time_range_ms);
  const std::string safe_label = SafeName(result.config.label);

  auto make_edge_time_hist = [&](const std::string &edge_name, const std::string &edge_title) {
    std::ostringstream title;
    title << result.config.label << " spill " << edge_spill << " first " << 100.0 * spill_fraction << "% "
          << edge_title
          << ";time since first hit in spill [ms];channel;entries";
    auto *hist = MakeHist2D(result.histograms2d,
                            "h_edge_time_" + edge_name + "_" + safe_label + "_spill" +
                                std::to_string(edge_spill),
                            title.str(),
                            time_bins,
                            0.0,
                            time_range_ms,
                            channel_bins,
                            static_cast<double>(channel_min) - 0.5,
                            static_cast<double>(channel_max) + 0.5);
    LabelChannelAxis(hist, channel_min, channel_max);
    return hist;
  };

  TH2D *h_leading = make_edge_time_hist("leading", "leading-edge time map");
  TH2D *h_trailing = make_edge_time_hist("trailing", "trailing-edge time map");

  const double phase_period_ns = edge_phase_period_ns > 0.0 ? edge_phase_period_ns : trigger_period_ns;
  TH2D *h_phase_leading = nullptr;
  TH2D *h_phase_trailing = nullptr;
  if (phase_period_ns > 0.0) {
    const int phase_bins = PhaseTimeBins(phase_period_ns);
    const double phase_range_us = phase_period_ns / 1000.0;
    auto make_phase_hist = [&](const std::string &edge_name, const std::string &edge_title) {
      std::ostringstream title;
      title << result.config.label << " spill " << edge_spill << " first " << 100.0 * spill_fraction << "% "
            << edge_title << " folded with " << phase_period_ns << " ns;time modulo period from first trigger ch"
            << trigger_channel
            << " [us];channel;entries";
      auto *hist = MakeHist2D(result.histograms2d,
                              "h_edge_phase_" + edge_name + "_" + safe_label + "_spill" +
                                  std::to_string(edge_spill),
                              title.str(),
                              phase_bins,
                              0.0,
                              phase_range_us,
                              channel_bins,
                              static_cast<double>(channel_min) - 0.5,
                              static_cast<double>(channel_max) + 0.5);
      LabelChannelAxis(hist, channel_min, channel_max);
      return hist;
    };
    h_phase_leading = make_phase_hist("leading", "leading-edge phase map");
    h_phase_trailing = make_phase_hist("trailing", "trailing-edge phase map");
  }

  for (const auto &hit : edge_hits) {
    if (hit.time_ns > window_max_time_ns) {
      continue;
    }
    const double time_ms = (hit.time_ns - min_time_ns) / 1.0e6;
    (hit.leading ? h_leading : h_trailing)->Fill(time_ms, hit.channel);
    if (phase_period_ns > 0.0) {
      double phase_ns = std::fmod(hit.time_ns - trigger_reference_ns, phase_period_ns);
      if (phase_ns < 0.0) {
        phase_ns += phase_period_ns;
      }
      const double phase_us = phase_ns / 1000.0;
      (hit.leading ? h_phase_leading : h_phase_trailing)->Fill(phase_us, hit.channel);
    }
  }
}

struct LinExpPlateauSeed {
  double p0 = 0.0;
  double p1 = -1.0;
  double x0 = 0.0;
  double tau = 0.5;
  double plateau = 0.0;
};

LinExpPlateauSeed EstimateLinExpPlateauSeed(TProfile *profile, double xmin, double xmax)
{
  LinExpPlateauSeed seed;
  seed.x0 = xmin + 0.7 * (xmax - xmin);

  struct Point {
    double x = 0.0;
    double y = 0.0;
  };
  std::vector<Point> points;
  for (int bin = 1; bin <= profile->GetNbinsX(); ++bin) {
    if (profile->GetBinEntries(bin) <= 0.0) {
      continue;
    }
    const double x = profile->GetXaxis()->GetBinCenter(bin);
    const double y = profile->GetBinContent(bin);
    if (x < xmin || x > xmax || !std::isfinite(y)) {
      continue;
    }
    points.push_back({x, y});
  }
  if (points.size() < 2) {
    return seed;
  }

  const double linear_seed_xmax = xmin + 0.6 * (xmax - xmin);
  double sx = 0.0;
  double sy = 0.0;
  double sxx = 0.0;
  double sxy = 0.0;
  int nlinear = 0;
  for (const auto &point : points) {
    if (point.x > linear_seed_xmax) {
      continue;
    }
    sx += point.x;
    sy += point.y;
    sxx += point.x * point.x;
    sxy += point.x * point.y;
    ++nlinear;
  }
  if (nlinear >= 2) {
    const double denom = static_cast<double>(nlinear) * sxx - sx * sx;
    if (std::abs(denom) > 1e-12) {
      seed.p1 = (static_cast<double>(nlinear) * sxy - sx * sy) / denom;
      seed.p0 = (sy - seed.p1 * sx) / static_cast<double>(nlinear);
    }
  } else {
    seed.p1 = (points.back().y - points.front().y) / (points.back().x - points.front().x);
    seed.p0 = points.front().y - seed.p1 * points.front().x;
  }

  const double residual_threshold = 0.5;
  for (const auto &point : points) {
    if (point.x < linear_seed_xmax) {
      continue;
    }
    const double linear_y = seed.p0 + seed.p1 * point.x;
    if (linear_y - point.y > residual_threshold) {
      seed.x0 = point.x;
      break;
    }
  }

  const double plateau_xmin = xmin + 0.8 * (xmax - xmin);
  double plateau_sum = 0.0;
  int plateau_count = 0;
  for (const auto &point : points) {
    if (point.x >= plateau_xmin) {
      plateau_sum += point.y;
      ++plateau_count;
    }
  }
  if (plateau_count > 0) {
    seed.plateau = plateau_sum / static_cast<double>(plateau_count);
  } else {
    seed.plateau = points.back().y;
  }
  seed.tau = std::max(0.15, 0.25 * std::max(0.1, xmax - seed.x0));
  return seed;
}

std::unique_ptr<TF1> FitTimewalkProfile(TProfile *profile,
                                        const FitRange &fit_range = FitRange{},
                                        TimewalkFitModel fit_model = TimewalkFitModel::Pol1)
{
  if (!profile) {
    return nullptr;
  }
  int first_bin = -1;
  int last_bin = -1;
  int valid_bins = 0;
  for (int bin = 1; bin <= profile->GetNbinsX(); ++bin) {
    const double x = profile->GetXaxis()->GetBinCenter(bin);
    if (fit_range.enabled && (x < fit_range.xmin || x > fit_range.xmax)) {
      continue;
    }
    if (profile->GetBinEntries(bin) <= 0.0) {
      continue;
    }
    if (!std::isfinite(profile->GetBinContent(bin))) {
      continue;
    }
    if (first_bin < 0) {
      first_bin = bin;
    }
    last_bin = bin;
    ++valid_bins;
  }
  if (valid_bins < 2) {
    return nullptr;
  }

  const double xmin = fit_range.enabled ? fit_range.xmin : profile->GetXaxis()->GetBinLowEdge(first_bin);
  const double xmax = fit_range.enabled ? fit_range.xmax : profile->GetXaxis()->GetBinUpEdge(last_bin);
  std::unique_ptr<TF1> fit;
  std::string fit_options = "QNR";
  if (fit_model == TimewalkFitModel::LinExpPlateau) {
    fit = std::make_unique<TF1>((std::string(profile->GetName()) + "_lin_exp_plateau").c_str(),
                                "x<[2] ? [0]+[1]*x : [4]+([0]+[1]*[2]-[4])*exp(-(x-[2])/[3])",
                                xmin,
                                xmax);
    auto seed = EstimateLinExpPlateauSeed(profile, xmin, xmax);
    fit->SetParameters(seed.p0, seed.p1, seed.x0, seed.tau, seed.plateau);
    fit->SetParNames("p0", "p1", "x0", "tau", "plateau");
    fit->SetParLimits(2, xmin + 0.1, xmax - 0.1);
    fit->SetParLimits(3, 0.03, 10.0);
    fit->SetParLimits(4, -5.0, 5.0);
    // The profile has tiny statistical errors in highly populated bins; equal-bin weights better follow the shape.
    fit_options = "QNRW";
  } else {
    fit = std::make_unique<TF1>((std::string(profile->GetName()) + "_pol1").c_str(), "pol1", xmin, xmax);
  }
  fit->SetLineColor(kRed + 1);
  fit->SetLineWidth(2);
  fit->SetNpx(200);
  profile->Fit(fit.get(), fit_options.c_str());
  return fit;
}

std::unique_ptr<TGraph> MakeFitGraph(const TF1 &fit,
                                     const std::string &name,
                                     double xmin,
                                     double xmax,
                                     int color)
{
  constexpr int npoints = 200;
  auto graph = std::make_unique<TGraph>(npoints);
  graph->SetName(name.c_str());
  graph->SetLineColor(color);
  graph->SetLineWidth(2);
  for (int i = 0; i < npoints; ++i) {
    const double frac = npoints > 1 ? static_cast<double>(i) / static_cast<double>(npoints - 1) : 0.0;
    const double x = xmin + frac * (xmax - xmin);
    graph->SetPoint(i, x, fit.Eval(x));
  }
  return graph;
}

TH2D *FindTimewalkHist(const RunResult &result, int channel)
{
  const std::string name = "h_dt_vs_tot_" + SafeName(result.config.label) + "_ch" + std::to_string(channel);
  for (const auto &owned : result.histograms2d) {
    if (std::string(owned->GetName()) == name) {
      return owned.get();
    }
  }
  return nullptr;
}

TH2D *FindRunHist2D(const RunResult &result, const std::string &prefix, int channel)
{
  const std::string name = prefix + SafeName(result.config.label) + "_ch" + std::to_string(channel);
  for (const auto &owned : result.histograms2d) {
    if (std::string(owned->GetName()) == name) {
      return owned.get();
    }
  }
  return nullptr;
}

TH2D *FindRunHist2DByPrefix(const RunResult &result, const std::string &prefix)
{
  for (const auto &owned : result.histograms2d) {
    if (std::string(owned->GetName()).rfind(prefix, 0) == 0) {
      return owned.get();
    }
  }
  return nullptr;
}

std::unique_ptr<TH2D> MakeAccumulatedHist2D(const std::vector<RunResult> &results,
                                            int channel,
                                            const std::string &input_prefix,
                                            const std::string &output_name,
                                            const std::string &title)
{
  std::unique_ptr<TH2D> accumulated;
  for (const auto &result : results) {
    TH2D *hist = FindRunHist2D(result, input_prefix, channel);
    if (!hist || hist->GetEntries() <= 0.0) {
      continue;
    }
    if (!accumulated) {
      accumulated = std::unique_ptr<TH2D>(static_cast<TH2D *>(hist->Clone(output_name.c_str())));
      accumulated->SetDirectory(nullptr);
      accumulated->Reset("ICES");
      accumulated->SetTitle(title.c_str());
    }
    accumulated->Add(hist);
  }
  return accumulated;
}

std::unique_ptr<TH2D> MakeAccumulatedTimewalkHist(const std::vector<RunResult> &results, int channel)
{
  return MakeAccumulatedHist2D(results,
                               channel,
                               "h_dt_vs_tot_",
                               "h_dt_vs_tot_accum_ch" + std::to_string(channel),
                               "Accumulated #Deltat vs ToT ch" + std::to_string(channel) +
                                   ";ToT [ns];t_{ch} - t_{trigger} [ns];entries");
}

std::unique_ptr<TH2D> MakeAccumulatedRawTimewalkHist(const std::vector<RunResult> &results, int channel)
{
  return MakeAccumulatedHist2D(results,
                               channel,
                               "h_raw_dt_vs_tot_",
                               "h_raw_dt_vs_tot_accum_ch" + std::to_string(channel),
                               "Accumulated raw #Deltat vs ToT ch" + std::to_string(channel) +
                                   ";ToT [ns];t_{ch} - t_{trigger} [ns];entries");
}

std::unique_ptr<TH2D> MakeAccumulatedRejectedTimewalkHist(const std::vector<RunResult> &results, int channel)
{
  return MakeAccumulatedHist2D(results,
                               channel,
                               "h_rejected_dt_vs_tot_",
                               "h_rejected_dt_vs_tot_accum_ch" + std::to_string(channel),
                               "Accumulated rejected #Deltat vs ToT ch" + std::to_string(channel) +
                                   ";ToT [ns];t_{ch} - t_{trigger} [ns];entries");
}

std::map<int, TimewalkCorrection> BuildTimewalkCorrections(const std::vector<RunResult> &results,
                                                           const std::vector<int> &sensor_channels,
                                                           const std::map<int, FitRange> &fit_ranges,
                                                           TimewalkFitModel fit_model)
{
  std::map<int, TimewalkCorrection> corrections;
  for (int ch : sensor_channels) {
    TimewalkCorrection correction;
    correction.model = fit_model;
    correction.fit_range = FitRangeForChannel(fit_ranges, ch);
    auto accumulated = MakeAccumulatedTimewalkHist(results, ch);
    if (!accumulated || accumulated->GetEntries() <= 0.0) {
      corrections[ch] = correction;
      continue;
    }
    auto profile = MakeTimewalkProfile(*accumulated);
    auto fit = FitTimewalkProfile(profile.get(), correction.fit_range, fit_model);
    if (!fit) {
      corrections[ch] = correction;
      continue;
    }
    correction.valid = true;
    correction.p0 = fit->GetParameter(0);
    correction.p1 = fit->GetParameter(1);
    if (fit_model == TimewalkFitModel::LinExpPlateau && fit->GetNpar() >= 5) {
      correction.p2 = fit->GetParameter(2);
      correction.p3 = fit->GetParameter(3);
      correction.p4 = fit->GetParameter(4);
    }
    corrections[ch] = correction;
    std::cout << "Timewalk correction ch" << ch << " model=" << TimewalkFitModelName(fit_model) << ": ";
    if (fit_model == TimewalkFitModel::LinExpPlateau) {
      std::cout << "linear p0=" << correction.p0 << " p1=" << correction.p1 << ", x0=" << correction.p2
                << ", tau=" << correction.p3 << ", plateau=" << correction.p4;
    } else {
      std::cout << "dt = " << correction.p0 << " + " << correction.p1 << " * ToT";
    }
    if (correction.fit_range.enabled) {
      std::cout << " fitted in ToT [" << correction.fit_range.xmin << ", " << correction.fit_range.xmax << "] ns";
    }
    std::cout << "; applied correction is max(f(ToT), 0)" << std::endl;
  }
  return corrections;
}

double CorrectedSensorTime(const StoredSensorHit &hit, const std::map<int, TimewalkCorrection> &corrections)
{
  auto it = corrections.find(hit.channel);
  if (it == corrections.end()) {
    return hit.time_ns;
  }
  return hit.time_ns - it->second.CorrectionNs(hit.tot_ns);
}

bool FindMatchedReference(double sensor_time,
                          const std::vector<StoredSensorHit> &references,
                          double match_window_ns,
                          bool signed_dt,
                          size_t &reference_index,
                          double &dt)
{
  if (references.empty()) {
    return false;
  }
  auto upper = std::upper_bound(references.begin(),
                                references.end(),
                                sensor_time,
                                [](double value, const StoredSensorHit &ref) { return value < ref.time_ns; });

  if (signed_dt) {
    bool found = false;
    double best_abs_dt = std::numeric_limits<double>::max();
    if (upper != references.end()) {
      const double candidate_dt = sensor_time - upper->time_ns;
      best_abs_dt = std::abs(candidate_dt);
      dt = candidate_dt;
      reference_index = static_cast<size_t>(std::distance(references.begin(), upper));
      found = true;
    }
    if (upper != references.begin()) {
      auto previous = std::prev(upper);
      const double candidate_dt = sensor_time - previous->time_ns;
      const double abs_dt = std::abs(candidate_dt);
      if (!found || abs_dt < best_abs_dt) {
        best_abs_dt = abs_dt;
        dt = candidate_dt;
        reference_index = static_cast<size_t>(std::distance(references.begin(), previous));
        found = true;
      }
    }
    return found && best_abs_dt <= match_window_ns;
  }

  if (upper == references.begin()) {
    return false;
  }
  auto previous = std::prev(upper);
  dt = sensor_time - previous->time_ns;
  reference_index = static_cast<size_t>(std::distance(references.begin(), previous));
  return dt >= 0.0 && dt <= match_window_ns;
}

bool ComputeMatchedDt(double sensor_time,
                      const std::vector<StoredSensorHit> &references,
                      double match_window_ns,
                      bool signed_dt,
                      double &dt)
{
  size_t reference_index = 0;
  return FindMatchedReference(sensor_time, references, match_window_ns, signed_dt, reference_index, dt);
}

struct MatchedCorrectedHit {
  double time_ns = 0.0;
  double tot_ns = 0.0;
  double dt_to_trigger_ns = 0.0;
};

void BuildCorrectedTimewalkAndCoincidencePlots(std::vector<RunResult> &results,
                                               const std::vector<int> &sensor_channels,
                                               const std::map<int, TimewalkCorrection> &corrections,
                                               double match_window_ns,
                                               double max_duration_ns,
                                               bool signed_dt,
                                               std::vector<std::unique_ptr<TH2D>> &corrected_accumulated_histograms)
{
  if (sensor_channels.empty()) {
    return;
  }

  const double tot_max = std::max(1.0, max_duration_ns);
  std::map<int, TH2D *> h_dt_corr_vs_tot_accum;
  for (int ch : sensor_channels) {
    std::ostringstream title;
    title << "Accumulated corrected #Deltat vs ToT ch" << ch << " to clean trigger"
          << ";ToT [ns];t_{ch,corr} - t_{trigger} [ns];entries";
    h_dt_corr_vs_tot_accum[ch] = MakeHist2D(corrected_accumulated_histograms,
                                            "h_dt_corr_vs_tot_accum_ch" + std::to_string(ch),
                                            title.str(),
                                            200,
                                            0.0,
                                            tot_max,
                                            400,
                                            kCorrectedTimewalkDtMinNs,
                                            kCorrectedTimewalkDtMaxNs);
  }

  for (auto &result : results) {
    const std::string safe_label = SafeName(result.config.label);
    std::map<int, std::vector<StoredSensorHit>> triggers_by_spill;
    for (const auto &trigger : result.trigger_hits) {
      triggers_by_spill[trigger.spill].push_back(trigger);
    }
    for (auto &kv : triggers_by_spill) {
      std::sort(kv.second.begin(), kv.second.end(), [](const StoredSensorHit &a, const StoredSensorHit &b) {
        return a.time_ns < b.time_ns;
      });
    }

    std::map<int, std::map<int, std::vector<StoredSensorHit>>> sensors_by_channel_spill;
    for (const auto &hit : result.sensor_hits) {
      StoredSensorHit corrected = hit;
      corrected.time_ns = CorrectedSensorTime(hit, corrections);
      sensors_by_channel_spill[hit.channel][hit.spill].push_back(corrected);
    }
    for (auto &by_channel : sensors_by_channel_spill) {
      for (auto &by_spill : by_channel.second) {
        std::sort(by_spill.second.begin(), by_spill.second.end(), [](const StoredSensorHit &a, const StoredSensorHit &b) {
          return a.time_ns < b.time_ns;
        });
      }
    }

    std::map<int, std::map<size_t, std::map<int, std::vector<MatchedCorrectedHit>>>> matched_by_spill_trigger_channel;
    for (int ch : sensor_channels) {
      auto channel_it = sensors_by_channel_spill.find(ch);
      if (channel_it == sensors_by_channel_spill.end()) {
        continue;
      }
      for (const auto &spill_entry : channel_it->second) {
        auto trigger_it = triggers_by_spill.find(spill_entry.first);
        if (trigger_it == triggers_by_spill.end() || trigger_it->second.empty()) {
          continue;
        }
        const auto &triggers = trigger_it->second;
        for (const auto &hit : spill_entry.second) {
          size_t trigger_index = 0;
          double dt = std::numeric_limits<double>::quiet_NaN();
          if (!FindMatchedReference(hit.time_ns, triggers, match_window_ns, signed_dt, trigger_index, dt)) {
            continue;
          }
          matched_by_spill_trigger_channel[hit.spill][trigger_index][ch].push_back(
              {hit.time_ns, hit.tot_ns, dt});
          if (dt >= kCorrectedTimewalkDtMinNs && dt <= kCorrectedTimewalkDtMaxNs) {
            h_dt_corr_vs_tot_accum[ch]->Fill(hit.tot_ns, dt);
          }
        }
      }
    }

    if (sensor_channels.size() < 2) {
      continue;
    }
    const int ch_a = sensor_channels[0];
    const int ch_b = sensor_channels[1];
    std::ostringstream title;
    title << result.config.label << " I=" << result.config.intensity << " corrected coincidence ch" << ch_a
          << "-ch" << ch_b << ";t_{" << ch_a << ",corr} - t_{" << ch_b << ",corr} [ns];entries";
    TH1D *hist = MakeHist(result.histograms,
                          "h_dt_corr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" + safe_label,
                          title.str(),
                          240,
                          kCorrectedCoincidenceDtMinNs,
                          kCorrectedCoincidenceDtMaxNs);

    for (const auto &spill_entry : matched_by_spill_trigger_channel) {
      for (const auto &trigger_entry : spill_entry.second) {
        const auto &by_channel = trigger_entry.second;
        auto hits_a_it = by_channel.find(ch_a);
        auto hits_b_it = by_channel.find(ch_b);
        if (hits_a_it == by_channel.end() || hits_b_it == by_channel.end() || hits_a_it->second.empty() ||
            hits_b_it->second.empty()) {
          continue;
        }
        const auto best_hit = [](const std::vector<MatchedCorrectedHit> &hits) {
          return std::min_element(hits.begin(), hits.end(), [](const MatchedCorrectedHit &a,
                                                               const MatchedCorrectedHit &b) {
            return std::abs(a.dt_to_trigger_ns) < std::abs(b.dt_to_trigger_ns);
          });
        };
        const auto best_a = best_hit(hits_a_it->second);
        const auto best_b = best_hit(hits_b_it->second);
        if (best_a == hits_a_it->second.end() || best_b == hits_b_it->second.end()) {
          continue;
        }
        const double dt_ab = best_a->time_ns - best_b->time_ns;
        if (dt_ab >= kCorrectedCoincidenceDtMinNs && dt_ab <= kCorrectedCoincidenceDtMaxNs) {
          hist->Fill(dt_ab);
        }
      }
    }
  }
}

void CleanTriggerCandidates(std::vector<size_t> &indices,
                            const std::vector<Hit> &hits,
                            double trigger_deadtime_ns,
                            double trigger_period_ns,
                            double trigger_period_tolerance_ns)
{
  std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return hits[a].time_ns < hits[b].time_ns; });
  if (indices.size() < 2) {
    return;
  }

  std::vector<size_t> clustered;
  clustered.reserve(indices.size());
  if (trigger_deadtime_ns > 0.0) {
    double last_trigger_time = -std::numeric_limits<double>::max();
    for (size_t idx : indices) {
      const double time = hits[idx].time_ns;
      if (clustered.empty() || time - last_trigger_time >= trigger_deadtime_ns) {
        clustered.push_back(idx);
        last_trigger_time = time;
      }
    }
  } else {
    clustered = indices;
  }

  if (trigger_period_ns <= 0.0 || trigger_period_tolerance_ns <= 0.0 || clustered.size() < 2) {
    indices.swap(clustered);
    return;
  }

  std::vector<size_t> periodic;
  periodic.reserve(clustered.size());
  periodic.push_back(clustered.front());
  for (size_t idx : clustered) {
    if (idx == periodic.front()) {
      continue;
    }
    const double dt = hits[idx].time_ns - hits[periodic.back()].time_ns;
    if (dt <= 0.0) {
      continue;
    }
    const double periods = std::max(1.0, std::round(dt / trigger_period_ns));
    const double residual = std::abs(dt - periods * trigger_period_ns);
    if (residual <= trigger_period_tolerance_ns) {
      periodic.push_back(idx);
    }
  }

  indices.swap(periodic);
}

int ColorForIndex(size_t index)
{
  static const std::array<int, 8> colors = {kAzure + 2, kOrange + 7, kGreen + 2, kMagenta + 1,
                                            kRed + 1,   kCyan + 2,   kViolet + 1, kPink + 7};
  return colors[index % colors.size()];
}

void DrawDtTotCutLine(const DtTotCut &cut, TH2D *hist)
{
  if (!hist || !cut.enabled) {
    return;
  }
  double xmin = hist->GetXaxis()->GetXmin();
  double xmax = hist->GetXaxis()->GetXmax();
  if (std::isfinite(cut.tot_min)) {
    xmin = std::max(xmin, cut.tot_min);
  }
  if (std::isfinite(cut.tot_max)) {
    xmax = std::min(xmax, cut.tot_max);
  }
  if (xmax <= xmin) {
    return;
  }
  auto *line = new TLine(xmin, cut.Boundary(xmin), xmax, cut.Boundary(xmax));
  line->SetBit(kCanDelete);
  line->SetLineColor(kMagenta + 2);
  line->SetLineWidth(3);
  line->SetLineStyle(2);
  line->Draw("same");
}

RunResult AnalyzeRun(const RunConfig &run,
                     const std::vector<int> &sensor_channels,
                     int trigger_channel,
                     const analysis_time::FineCalib &fine_calib,
                     const analysis_time::ChannelCalib &chan_calib,
                     double match_window_ns,
                     double trigger_deadtime_ns,
                     double trigger_period_ns,
                     double trigger_period_tolerance_ns,
                     double max_duration_ns,
                     double clock_mhz,
                     bool use_fine,
                     int fine_cut,
                     bool require_valid_tot,
                     bool signed_dt,
                     const std::map<int, DtTotCut> &dt_tot_cuts,
                     const TotWindow &trigger_tot_window,
                     const SpillRange &spill_range,
                     const std::map<int, TotWindow> &channel_tot_windows,
                     int edge_spill,
                     const std::vector<int> &edge_channels,
                     double edge_phase_period_ns,
                     double edge_spill_fraction)
{
  RunResult result;
  result.config = run;

  std::unordered_set<int> selected_channels(sensor_channels.begin(), sensor_channels.end());
  selected_channels.insert(trigger_channel);
  std::unordered_set<int> edge_channel_set(edge_channels.begin(), edge_channels.end());
  int selected_edge_spill = edge_spill;

  const std::string safe_label = SafeName(run.label);
  const double timewalk_dt_min = TimewalkDtMin(signed_dt, match_window_ns);
  const double timewalk_dt_max = TimewalkDtMax(match_window_ns);
  std::map<int, TH1D *> h_dt;
  std::map<int, TH1D *> h_tot;
  std::map<int, TH2D *> h_dt_vs_tot;
  std::map<int, TH2D *> h_raw_dt_vs_tot;
  std::map<int, TH2D *> h_rejected_dt_vs_tot;
  std::map<int, TH2D *> h_tot_vs_spill;
  std::map<int, TH2D *> h_tot_vs_spill_selected;
  std::map<int, TH2D *> h_tot_vs_spill_rejected;
  std::map<int, TH2D *> h_dt_vs_spill;
  std::map<int, TH2D *> h_raw_dt_vs_spill;
  std::map<int, TH2D *> h_rejected_dt_vs_spill;
  for (int ch : sensor_channels) {
    std::ostringstream title;
    title << run.label << " I=" << run.intensity << " ch" << ch
          << (signed_dt ? " - nearest clean trigger ch" : " - previous clean trigger ch") << trigger_channel
          << ";t_{ch} - t_{trigger} [ns];entries";
    h_dt[ch] = MakeHist(result.histograms,
                        "h_dt_" + safe_label + "_ch" + std::to_string(ch),
                        title.str(),
                        400,
                        signed_dt ? -match_window_ns : 0.0,
                        match_window_ns);

    std::ostringstream corr_title;
    corr_title << run.label << " I=" << run.intensity << " ch" << ch << " #Deltat vs ToT to trigger ch"
               << trigger_channel << ";ToT [ns];t_{ch} - t_{trigger} [ns];entries";
    std::ostringstream raw_corr_title;
    raw_corr_title << run.label << " I=" << run.intensity << " ch" << ch << " raw #Deltat vs ToT to trigger ch"
                   << trigger_channel << ";ToT [ns];t_{ch} - t_{trigger} [ns];entries";
    std::ostringstream rejected_corr_title;
    rejected_corr_title << run.label << " I=" << run.intensity << " ch" << ch
                        << " rejected by #Deltat-ToT cut;ToT [ns];t_{ch} - t_{trigger} [ns];entries";
    h_raw_dt_vs_tot[ch] = MakeHist2D(result.histograms2d,
                                     "h_raw_dt_vs_tot_" + safe_label + "_ch" + std::to_string(ch),
                                     raw_corr_title.str(),
                                     200,
                                     0.0,
                                     std::max(1.0, max_duration_ns),
                                     400,
                                     timewalk_dt_min,
                                     timewalk_dt_max);
    h_dt_vs_tot[ch] = MakeHist2D(result.histograms2d,
                                 "h_dt_vs_tot_" + safe_label + "_ch" + std::to_string(ch),
                                 corr_title.str(),
                                 200,
                                 0.0,
                                 std::max(1.0, max_duration_ns),
                                 400,
                                 timewalk_dt_min,
                                 timewalk_dt_max);
    h_rejected_dt_vs_tot[ch] = MakeHist2D(result.histograms2d,
                                          "h_rejected_dt_vs_tot_" + safe_label + "_ch" + std::to_string(ch),
                                          rejected_corr_title.str(),
                                          200,
                                          0.0,
                                          std::max(1.0, max_duration_ns),
                                          400,
                                          timewalk_dt_min,
                                          timewalk_dt_max);
  }
  std::vector<int> tot_channels = sensor_channels;
  tot_channels.push_back(trigger_channel);
  std::sort(tot_channels.begin(), tot_channels.end());
  tot_channels.erase(std::unique(tot_channels.begin(), tot_channels.end()), tot_channels.end());
  for (int ch : tot_channels) {
    std::ostringstream title;
    title << run.label << " I=" << run.intensity << " ToT ch" << ch << ";ToT [ns];entries";
    h_tot[ch] = MakeHist(result.histograms,
                         "h_tot_" + safe_label + "_ch" + std::to_string(ch),
                         title.str(),
                         200,
                         0.0,
                         std::max(1.0, max_duration_ns));
  }

  auto input = analysis_io::ResolveInputSpec(run.input_path);
  if (input.files.empty()) {
    std::cerr << "No decoded ROOT input found for " << run.label << " at " << run.input_path << std::endl;
    return result;
  }

  TChain chain(input.tree_name.c_str());
  for (const auto &file : input.files) {
    chain.Add(file.c_str());
  }

  int device = 0;
  int fifo = 0;
  int type = 0;
  int counter = 0;
  int spill = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int rollover = 0;
  int coarse = 0;
  int fine = 0;

  const bool has_device = chain.GetBranch("device") != nullptr;
  const bool has_counter = chain.GetBranch("counter") != nullptr;
  const bool has_spill = chain.GetBranch("spill") != nullptr;
  if (has_device) {
    chain.SetBranchAddress("device", &device);
  }
  chain.SetBranchAddress("fifo", &fifo);
  chain.SetBranchAddress("type", &type);
  if (has_counter) {
    chain.SetBranchAddress("counter", &counter);
  }
  if (has_spill) {
    chain.SetBranchAddress("spill", &spill);
  }
  chain.SetBranchAddress("column", &column);
  chain.SetBranchAddress("pixel", &pixel);
  chain.SetBranchAddress("tdc", &tdc);
  chain.SetBranchAddress("rollover", &rollover);
  chain.SetBranchAddress("coarse", &coarse);
  chain.SetBranchAddress("fine", &fine);

  std::vector<Hit> hits;
  std::vector<EdgeHit> edge_hits;
  hits.reserve(static_cast<size_t>(std::min<Long64_t>(chain.GetEntries(), 1000000)));
  const double tick_ns = analysis_time::TickNs(clock_mhz);
  const Long64_t entries = chain.GetEntries();
  for (Long64_t i = 0; i < entries; ++i) {
    chain.GetEntry(i);
    if (type != 1) {
      continue;
    }
    const int channel = column * 4 + pixel;
    if (!IsValidTdcId(tdc)) {
      continue;
    }
    const int tdc_index = analysis_time::TdcIndex(fifo, column, pixel, tdc);
    if (!analysis_time::PassFineCut(fine_calib, fine, tdc_index, fine_cut)) {
      continue;
    }
    const int hit_spill = has_spill ? spill : 0;
    if (!spill_range.Pass(hit_spill)) {
      continue;
    }
    if (!edge_channel_set.empty()) {
      const bool is_edge_channel = edge_channel_set.find(channel) != edge_channel_set.end();
      if (selected_edge_spill < 0 && is_edge_channel) {
        selected_edge_spill = hit_spill;
      }
      if (is_edge_channel && hit_spill == selected_edge_spill) {
        edge_hits.push_back({channel,
                             hit_spill,
                             IsLeadingTdc(tdc),
                             analysis_time::TimeNsFromTick(
                                 fine_calib, analysis_time::TimeTick(rollover, coarse), fine, tdc_index, tick_ns, use_fine)});
      }
    }
    Hit hit;
    if (selected_channels.find(channel) == selected_channels.end()) {
      continue;
    }
    hit.channel = channel;
    hit.spill = hit_spill;
    hit.fifo = fifo;
    hit.column = column;
    hit.pixel = pixel;
    hit.tdc = tdc;
    hit.fine = fine;
    hit.time_tick = analysis_time::TimeTick(rollover, coarse);
    hit.time_ns_raw =
        analysis_time::TimeNsFromTick(fine_calib, hit.time_tick, fine, tdc_index, tick_ns, use_fine);
    hit.time_ns = hit.time_ns_raw;
    hit.leading = IsLeadingTdc(tdc);
    hits.push_back(hit);
  }

  result.hits_read = static_cast<long long>(hits.size());
  if (selected_edge_spill >= 0) {
    BuildSingleSpillEdgePlots(result,
                              edge_hits,
                              edge_channels,
                              selected_edge_spill,
                              trigger_channel,
                              trigger_period_ns,
                              edge_phase_period_ns,
                              edge_spill_fraction);
  }
  ComputeTot(hits, max_duration_ns);

  int min_spill = std::numeric_limits<int>::max();
  int max_spill = std::numeric_limits<int>::min();
  for (const auto &hit : hits) {
    if (!hit.leading || hit.tot_ns <= 0.0) {
      continue;
    }
    min_spill = std::min(min_spill, hit.spill);
    max_spill = std::max(max_spill, hit.spill);
  }
  if (min_spill == std::numeric_limits<int>::max()) {
    min_spill = 0;
    max_spill = 0;
  }
  const int spill_span = std::max(1, max_spill - min_spill + 1);
  const int spill_bins = std::min(2000, spill_span);
  const double spill_xmin = static_cast<double>(min_spill) - 0.5;
  const double spill_xmax = static_cast<double>(max_spill) + 0.5;
  for (int ch : tot_channels) {
    std::ostringstream title;
    title << run.label << " I=" << run.intensity << " ToT vs spill ch" << ch << ";spill;ToT [ns];entries";
    h_tot_vs_spill[ch] = MakeHist2D(result.histograms2d,
                                    "h_tot_vs_spill_" + safe_label + "_ch" + std::to_string(ch),
                                    title.str(),
                                    spill_bins,
                                    spill_xmin,
                                    spill_xmax,
                                    200,
                                    0.0,
                                    std::max(1.0, max_duration_ns));
  }
  for (int ch : sensor_channels) {
    std::ostringstream selected_title;
    selected_title << run.label << " I=" << run.intensity << " selected ToT vs spill ch" << ch
                   << ";spill;ToT [ns];entries";
    h_tot_vs_spill_selected[ch] = MakeHist2D(result.histograms2d,
                                            "h_tot_vs_spill_selected_" + safe_label + "_ch" + std::to_string(ch),
                                            selected_title.str(),
                                            spill_bins,
                                            spill_xmin,
                                            spill_xmax,
                                            200,
                                            0.0,
                                            std::max(1.0, max_duration_ns));

    std::ostringstream rejected_title;
    rejected_title << run.label << " I=" << run.intensity << " rejected ToT vs spill ch" << ch
                   << ";spill;ToT [ns];entries";
    h_tot_vs_spill_rejected[ch] = MakeHist2D(result.histograms2d,
                                            "h_tot_vs_spill_rejected_" + safe_label + "_ch" + std::to_string(ch),
                                            rejected_title.str(),
                                            spill_bins,
                                            spill_xmin,
                                            spill_xmax,
                                            200,
                                            0.0,
                                            std::max(1.0, max_duration_ns));

    std::ostringstream raw_dt_title;
    raw_dt_title << run.label << " I=" << run.intensity << " raw #Deltat vs spill ch" << ch
                 << ";spill;t_{ch} - t_{trigger} [ns];entries";
    h_raw_dt_vs_spill[ch] = MakeHist2D(result.histograms2d,
                                       "h_raw_dt_vs_spill_" + safe_label + "_ch" + std::to_string(ch),
                                       raw_dt_title.str(),
                                       spill_bins,
                                       spill_xmin,
                                       spill_xmax,
                                       400,
                                       timewalk_dt_min,
                                       timewalk_dt_max);

    std::ostringstream selected_dt_title;
    selected_dt_title << run.label << " I=" << run.intensity << " selected #Deltat vs spill ch" << ch
                      << ";spill;t_{ch} - t_{trigger} [ns];entries";
    h_dt_vs_spill[ch] = MakeHist2D(result.histograms2d,
                                   "h_dt_vs_spill_" + safe_label + "_ch" + std::to_string(ch),
                                   selected_dt_title.str(),
                                   spill_bins,
                                   spill_xmin,
                                   spill_xmax,
                                   400,
                                   timewalk_dt_min,
                                   timewalk_dt_max);

    std::ostringstream rejected_dt_title;
    rejected_dt_title << run.label << " I=" << run.intensity << " rejected #Deltat vs spill ch" << ch
                      << ";spill;t_{ch} - t_{trigger} [ns];entries";
    h_rejected_dt_vs_spill[ch] = MakeHist2D(result.histograms2d,
                                           "h_rejected_dt_vs_spill_" + safe_label + "_ch" + std::to_string(ch),
                                           rejected_dt_title.str(),
                                           spill_bins,
                                           spill_xmin,
                                           spill_xmax,
                                           400,
                                           timewalk_dt_min,
                                           timewalk_dt_max);
  }

  for (auto &hit : hits) {
    if (!hit.leading) {
      continue;
    }
    ++result.leading_hits;
    if (chan_calib.loaded && hit.tot_ns > 0.0) {
      hit.time_ns = hit.time_ns_raw - chan_calib.CorrectionNs(hit.channel, hit.tot_ns);
    }
  }

  std::map<int, std::vector<size_t>> trigger_by_spill;
  std::map<int, std::map<int, std::vector<size_t>>> sensor_by_channel_spill;
  std::map<int, std::vector<double>> tot_values;
  for (size_t i = 0; i < hits.size(); ++i) {
    const Hit &hit = hits[i];
    if (!hit.leading) {
      continue;
    }
    if (require_valid_tot && hit.tot_ns <= 0.0) {
      continue;
    }
    const bool is_trigger = hit.channel == trigger_channel;
    if (is_trigger) {
      ++result.trigger22_raw_candidates;
    }
    const TotWindow channel_tot_window = ChannelTotWindowForChannel(channel_tot_windows, hit.channel);
    if (!channel_tot_window.Pass(hit.tot_ns)) {
      continue;
    }
    if (is_trigger) {
      if (!trigger_tot_window.Pass(hit.tot_ns)) {
        continue;
      }
      ++result.trigger22_tot_window_candidates;
      trigger_by_spill[hit.spill].push_back(i);
      continue;
    }
    if (selected_channels.find(hit.channel) != selected_channels.end()) {
      sensor_by_channel_spill[hit.channel][hit.spill].push_back(i);
      if (hit.tot_ns > 0.0) {
        tot_values[hit.channel].push_back(hit.tot_ns);
        h_tot[hit.channel]->Fill(hit.tot_ns);
        if (h_tot_vs_spill.count(hit.channel) > 0) {
          h_tot_vs_spill[hit.channel]->Fill(hit.spill, hit.tot_ns);
        }
      }
    }
  }

  for (auto &kv : trigger_by_spill) {
    auto &indices = kv.second;
    // The cleanup is intentionally applied only to the laser trigger channel.
    // SiPM channels are not cleaned here; they are only matched to the cleaned trigger sequence below.
    CleanTriggerCandidates(indices, hits, trigger_deadtime_ns, trigger_period_ns, trigger_period_tolerance_ns);
    result.trigger22_clean_candidates += static_cast<long long>(indices.size());
  }

  std::vector<double> clean_trigger_spacings;
  for (const auto &kv : trigger_by_spill) {
    const auto &indices = kv.second;
    for (size_t i = 1; i < indices.size(); ++i) {
      const double spacing = hits[indices[i]].time_ns - hits[indices[i - 1]].time_ns;
      if (spacing > 0.0) {
        clean_trigger_spacings.push_back(spacing);
      }
    }
  }
  result.trigger22_clean_period = ComputeStats(clean_trigger_spacings);

  for (auto &by_channel : sensor_by_channel_spill) {
    for (auto &by_spill : by_channel.second) {
      auto &indices = by_spill.second;
      std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return hits[a].time_ns < hits[b].time_ns; });
    }
  }

  std::map<int, std::vector<double>> dt_values;
  std::vector<double> trigger_tot_values;
  for (const auto &kv : trigger_by_spill) {
    for (size_t idx : kv.second) {
      if (hits[idx].tot_ns > 0.0) {
        trigger_tot_values.push_back(hits[idx].tot_ns);
        h_tot[trigger_channel]->Fill(hits[idx].tot_ns);
        if (h_tot_vs_spill.count(trigger_channel) > 0) {
          h_tot_vs_spill[trigger_channel]->Fill(hits[idx].spill, hits[idx].tot_ns);
        }
        result.trigger_hits.push_back({trigger_channel, hits[idx].spill, hits[idx].time_ns, hits[idx].tot_ns});
      }
    }
  }
  tot_values[trigger_channel] = trigger_tot_values;

  for (int ch : sensor_channels) {
    auto channel_it = sensor_by_channel_spill.find(ch);
    if (channel_it == sensor_by_channel_spill.end()) {
      continue;
    }
    for (const auto &spill_entry : channel_it->second) {
      auto trigger_it = trigger_by_spill.find(spill_entry.first);
      if (trigger_it == trigger_by_spill.end() || trigger_it->second.empty()) {
        continue;
      }
      const auto &triggers = trigger_it->second;
      for (size_t sensor_idx : spill_entry.second) {
        const double sensor_time = hits[sensor_idx].time_ns;
        auto upper = std::upper_bound(triggers.begin(), triggers.end(), sensor_time, [&](double value, size_t idx) {
          return value < hits[idx].time_ns;
        });

        double dt = std::numeric_limits<double>::quiet_NaN();
        if (signed_dt) {
          bool found = false;
          double best_abs_dt = std::numeric_limits<double>::max();
          if (upper != triggers.end()) {
            const double candidate_dt = sensor_time - hits[*upper].time_ns;
            best_abs_dt = std::abs(candidate_dt);
            dt = candidate_dt;
            found = true;
          }
          if (upper != triggers.begin()) {
            const double candidate_dt = sensor_time - hits[*std::prev(upper)].time_ns;
            const double abs_dt = std::abs(candidate_dt);
            if (!found || abs_dt < best_abs_dt) {
              best_abs_dt = abs_dt;
              dt = candidate_dt;
              found = true;
            }
          }
          if (!found || best_abs_dt > match_window_ns) {
            continue;
          }
        } else {
          if (upper == triggers.begin()) {
            continue;
          }
          const size_t best_idx = *std::prev(upper);
          dt = sensor_time - hits[best_idx].time_ns;
          if (dt < 0.0 || dt > match_window_ns) {
            continue;
          }
        }

        const double sensor_tot = hits[sensor_idx].tot_ns;
        const bool has_valid_sensor_tot = sensor_tot > 0.0;
        const bool in_timewalk_range = dt >= timewalk_dt_min && dt <= timewalk_dt_max;
        if (has_valid_sensor_tot && h_raw_dt_vs_tot.count(ch) > 0 && in_timewalk_range) {
          h_raw_dt_vs_tot[ch]->Fill(sensor_tot, dt);
        }
        if (h_raw_dt_vs_spill.count(ch) > 0 && in_timewalk_range) {
          h_raw_dt_vs_spill[ch]->Fill(hits[sensor_idx].spill, dt);
        }

        const DtTotCut cut = DtTotCutForChannel(dt_tot_cuts, ch);
        const bool pass_dt_tot_cut = !has_valid_sensor_tot || cut.Pass(sensor_tot, dt);
        if (!pass_dt_tot_cut) {
          if (has_valid_sensor_tot && h_rejected_dt_vs_tot.count(ch) > 0 && in_timewalk_range) {
            h_rejected_dt_vs_tot[ch]->Fill(sensor_tot, dt);
          }
          if (h_rejected_dt_vs_spill.count(ch) > 0 && in_timewalk_range) {
            h_rejected_dt_vs_spill[ch]->Fill(hits[sensor_idx].spill, dt);
          }
          if (has_valid_sensor_tot && h_tot_vs_spill_rejected.count(ch) > 0) {
            h_tot_vs_spill_rejected[ch]->Fill(hits[sensor_idx].spill, sensor_tot);
          }
          continue;
        }

        dt_values[ch].push_back(dt);
        h_dt[ch]->Fill(dt);
        if (has_valid_sensor_tot && h_dt_vs_tot.count(ch) > 0 && in_timewalk_range) {
          h_dt_vs_tot[ch]->Fill(sensor_tot, dt);
        }
        if (h_dt_vs_spill.count(ch) > 0 && in_timewalk_range) {
          h_dt_vs_spill[ch]->Fill(hits[sensor_idx].spill, dt);
        }
        if (has_valid_sensor_tot && h_tot_vs_spill_selected.count(ch) > 0) {
          h_tot_vs_spill_selected[ch]->Fill(hits[sensor_idx].spill, sensor_tot);
        }
        if (has_valid_sensor_tot) {
          result.sensor_hits.push_back({ch, hits[sensor_idx].spill, hits[sensor_idx].time_ns, sensor_tot});
        }
      }
    }
  }

  for (int ch : sensor_channels) {
    result.channels[ch].dt = ComputeStats(dt_values[ch]);
    result.channels[ch].tot = ComputeStats(tot_values[ch]);
  }
  result.channels[trigger_channel].tot = ComputeStats(tot_values[trigger_channel]);
  return result;
}

std::unique_ptr<TGraphErrors> MakeGraph(const std::vector<RunResult> &results,
                                        int channel,
                                        bool use_dt,
                                        bool use_rms,
                                        const std::string &name,
                                        const std::string &title,
                                        int color)
{
  struct Point {
    double x = 0.0;
    double y = 0.0;
    double ey = 0.0;
  };
  std::vector<Point> points;
  for (const auto &result : results) {
    auto it = result.channels.find(channel);
    if (it == result.channels.end()) {
      continue;
    }
    const Stats &stats = use_dt ? it->second.dt : it->second.tot;
    if (stats.n <= 0) {
      continue;
    }
    Point p;
    p.x = result.config.intensity;
    p.y = use_rms ? stats.rms : stats.mean;
    p.ey = use_rms ? stats.err_rms : stats.err_mean;
    points.push_back(p);
  }
  std::sort(points.begin(), points.end(), [](const Point &a, const Point &b) { return a.x < b.x; });

  auto graph = std::make_unique<TGraphErrors>(static_cast<int>(points.size()));
  graph->SetName(name.c_str());
  graph->SetTitle(title.c_str());
  graph->SetLineColor(color);
  graph->SetMarkerColor(color);
  graph->SetMarkerStyle(20);
  graph->SetLineWidth(2);
  for (int i = 0; i < static_cast<int>(points.size()); ++i) {
    graph->SetPoint(i, points[static_cast<size_t>(i)].x, points[static_cast<size_t>(i)].y);
    graph->SetPointError(i, 0.0, points[static_cast<size_t>(i)].ey);
  }
  return graph;
}

void DrawGraphs(TCanvas &canvas,
                const std::vector<TGraphErrors *> &graphs,
                const std::vector<std::string> &labels,
                const std::string &title,
                const std::string &y_title,
                const std::string &out_pdf)
{
  canvas.Clear();
  bool have_points = false;
  double xmin = std::numeric_limits<double>::max();
  double xmax = -std::numeric_limits<double>::max();
  double ymin = std::numeric_limits<double>::max();
  double ymax = -std::numeric_limits<double>::max();
  for (auto *graph : graphs) {
    if (!graph) {
      continue;
    }
    for (int i = 0; i < graph->GetN(); ++i) {
      double x = 0.0;
      double y = 0.0;
      graph->GetPoint(i, x, y);
      if (!std::isfinite(x) || !std::isfinite(y)) {
        continue;
      }
      const double ey = graph->GetErrorY(i);
      xmin = std::min(xmin, x);
      xmax = std::max(xmax, x);
      ymin = std::min(ymin, y - ey);
      ymax = std::max(ymax, y + ey);
      have_points = true;
    }
  }
  if (!have_points) {
    xmin = 0.0;
    xmax = 1.0;
    ymin = 0.0;
    ymax = 1.0;
  }
  const double xpad = std::max(0.1, 0.08 * (xmax - xmin));
  const double y_range = ymax - ymin;
  const double ypad = std::max(0.1, 0.12 * (y_range > 0.0 ? y_range : 1.0));
  xmin -= xpad;
  xmax += xpad;
  ymin -= ypad;
  ymax += ypad;

  auto *multi = new TMultiGraph();
  multi->SetBit(kCanDelete);
  multi->SetTitle((title + ";Laser intensity;" + y_title).c_str());
  multi->SetMinimum(ymin);
  multi->SetMaximum(ymax);

  auto *legend = new TLegend(0.72, 0.72, 0.92, 0.90);
  legend->SetBit(kCanDelete);
  legend->SetBorderSize(0);
  legend->SetFillStyle(0);
  for (size_t i = 0; i < graphs.size(); ++i) {
    if (!graphs[i] || graphs[i]->GetN() <= 0) {
      continue;
    }
    auto *draw_graph = static_cast<TGraphErrors *>(graphs[i]->Clone());
    multi->Add(draw_graph, "LPE");
    legend->AddEntry(draw_graph, labels[i].c_str(), "lp");
  }
  multi->Draw("A");
  if (multi->GetXaxis()) {
    multi->GetXaxis()->SetLimits(xmin, xmax);
  }
  legend->Draw();
  canvas.Print(out_pdf.c_str());
}

void DrawRunHistograms(TCanvas &canvas,
                       const RunResult &result,
                       const std::vector<int> &sensor_channels,
                       int trigger_channel,
                       const std::map<int, FitRange> &fit_ranges,
                       TimewalkFitModel fit_model,
                       const std::string &out_pdf)
{
  canvas.Clear();
  canvas.SetRightMargin(0.05);
  auto *legend_dt = new TLegend(0.72, 0.72, 0.92, 0.90);
  legend_dt->SetBit(kCanDelete);
  legend_dt->SetBorderSize(0);
  legend_dt->SetFillStyle(0);
  auto *stack_dt = new THStack(("stack_dt_" + SafeName(result.config.label)).c_str(),
                               (result.config.label + " I=" + std::to_string(result.config.intensity) +
                                " time difference;t_{ch} - t_{trigger} [ns];entries")
                                   .c_str());
  stack_dt->SetBit(kCanDelete);
  double max_dt = 0.0;
  for (size_t i = 0; i < sensor_channels.size(); ++i) {
    const std::string name = "h_dt_" + SafeName(result.config.label) + "_ch" + std::to_string(sensor_channels[i]);
    TH1D *hist = nullptr;
    for (const auto &owned : result.histograms) {
      if (std::string(owned->GetName()) == name) {
        hist = owned.get();
        break;
      }
    }
    if (!hist) {
      continue;
    }
    hist->SetLineColor(ColorForIndex(i));
    hist->SetLineWidth(2);
    max_dt = std::max(max_dt, hist->GetMaximum());
    auto *draw_hist = static_cast<TH1D *>(hist->Clone());
    draw_hist->SetDirectory(nullptr);
    stack_dt->Add(draw_hist, "hist");
    legend_dt->AddEntry(draw_hist, ("ch " + std::to_string(sensor_channels[i])).c_str(), "l");
  }
  if (stack_dt->GetHists()) {
    stack_dt->GetHists()->SetOwner(kTRUE);
  }
  stack_dt->SetMaximum(max_dt * 1.2 + 1.0);
  stack_dt->Draw("nostack hist");
  legend_dt->Draw();
  canvas.Print(out_pdf.c_str());

  canvas.Clear();
  canvas.SetRightMargin(0.05);
  auto *legend_tot = new TLegend(0.72, 0.72, 0.92, 0.90);
  legend_tot->SetBit(kCanDelete);
  legend_tot->SetBorderSize(0);
  legend_tot->SetFillStyle(0);
  auto *stack_tot = new THStack(("stack_tot_" + SafeName(result.config.label)).c_str(),
                                (result.config.label + " I=" + std::to_string(result.config.intensity) +
                                 " ToT;ToT [ns];entries")
                                    .c_str());
  stack_tot->SetBit(kCanDelete);
  std::vector<int> tot_channels = sensor_channels;
  tot_channels.push_back(trigger_channel);
  std::sort(tot_channels.begin(), tot_channels.end());
  tot_channels.erase(std::unique(tot_channels.begin(), tot_channels.end()), tot_channels.end());
  double max_tot = 0.0;
  for (size_t i = 0; i < tot_channels.size(); ++i) {
    const std::string name = "h_tot_" + SafeName(result.config.label) + "_ch" + std::to_string(tot_channels[i]);
    TH1D *hist = nullptr;
    for (const auto &owned : result.histograms) {
      if (std::string(owned->GetName()) == name) {
        hist = owned.get();
        break;
      }
    }
    if (!hist) {
      continue;
    }
    hist->SetLineColor(ColorForIndex(i));
    hist->SetLineWidth(2);
    max_tot = std::max(max_tot, hist->GetMaximum());
    auto *draw_hist = static_cast<TH1D *>(hist->Clone());
    draw_hist->SetDirectory(nullptr);
    stack_tot->Add(draw_hist, "hist");
    legend_tot->AddEntry(draw_hist, ("ch " + std::to_string(tot_channels[i])).c_str(), "l");
  }
  if (stack_tot->GetHists()) {
    stack_tot->GetHists()->SetOwner(kTRUE);
  }
  stack_tot->SetMaximum(max_tot * 1.2 + 1.0);
  stack_tot->Draw("nostack hist");
  legend_tot->Draw();
  canvas.Print(out_pdf.c_str());

  auto draw_edge_pair = [&](const std::string &leading_prefix,
                            const std::string &trailing_prefix) {
    TH2D *leading = FindRunHist2DByPrefix(result, leading_prefix + SafeName(result.config.label) + "_spill");
    TH2D *trailing = FindRunHist2DByPrefix(result, trailing_prefix + SafeName(result.config.label) + "_spill");
    if ((!leading || leading->GetEntries() <= 0.0) && (!trailing || trailing->GetEntries() <= 0.0)) {
      return;
    }
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(2, 1, 0.001, 0.001);
    for (int i = 0; i < 2; ++i) {
      TH2D *hist = i == 0 ? leading : trailing;
      canvas.cd(i + 1);
      if (auto *pad = gPad) {
        pad->SetRightMargin(0.14);
      }
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      auto *draw_hist = static_cast<TH2D *>(hist->Clone());
      draw_hist->SetDirectory(nullptr);
      draw_hist->SetBit(kCanDelete);
      draw_hist->SetStats(false);
      draw_hist->Draw("colz");
    }
    canvas.Print(out_pdf.c_str());
  };

  draw_edge_pair("h_edge_time_leading_", "h_edge_time_trailing_");
  draw_edge_pair("h_edge_phase_leading_", "h_edge_phase_trailing_");

  canvas.Clear();
  canvas.SetRightMargin(0.05);
  canvas.Divide(static_cast<int>(tot_channels.size()), 1, 0.001, 0.001);
  bool has_tot_vs_spill = false;
  std::vector<std::unique_ptr<TProfile>> tot_vs_spill_profiles;
  tot_vs_spill_profiles.reserve(tot_channels.size());
  for (size_t i = 0; i < tot_channels.size(); ++i) {
    const int ch = tot_channels[i];
    const std::string name = "h_tot_vs_spill_" + SafeName(result.config.label) + "_ch" + std::to_string(ch);
    TH2D *hist = nullptr;
    for (const auto &owned : result.histograms2d) {
      if (std::string(owned->GetName()) == name) {
        hist = owned.get();
        break;
      }
    }
    canvas.cd(static_cast<int>(i + 1));
    if (auto *pad = gPad) {
      pad->SetRightMargin(0.14);
    }
    if (!hist || hist->GetEntries() <= 0.0) {
      continue;
    }
    has_tot_vs_spill = true;
    hist->Draw("colz");
    auto profile = MakeTotVsSpillProfile(*hist);
    if (profile) {
      profile->Draw("E1 same");
      tot_vs_spill_profiles.push_back(std::move(profile));
    }
  }
  if (has_tot_vs_spill) {
    canvas.Print(out_pdf.c_str());
  }

  auto draw_sensor_tot_vs_spill_group = [&](const std::string &prefix, const std::string &label) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
    bool has_group = false;
    std::vector<std::unique_ptr<TProfile>> profiles;
    profiles.reserve(sensor_channels.size());
    for (size_t i = 0; i < sensor_channels.size(); ++i) {
      const int ch = sensor_channels[i];
      const std::string name = prefix + SafeName(result.config.label) + "_ch" + std::to_string(ch);
      TH2D *hist = nullptr;
      for (const auto &owned : result.histograms2d) {
        if (std::string(owned->GetName()) == name) {
          hist = owned.get();
          break;
        }
      }
      canvas.cd(static_cast<int>(i + 1));
      if (auto *pad = gPad) {
        pad->SetRightMargin(0.14);
      }
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      has_group = true;
      hist->SetTitle((result.config.label + " I=" + std::to_string(result.config.intensity) + " " + label +
                      " ToT vs spill ch" + std::to_string(ch) + ";spill;ToT [ns];entries")
                         .c_str());
      hist->Draw("colz");
      auto profile = MakeTotVsSpillProfile(*hist);
      if (profile) {
        profile->Draw("E1 same");
        profiles.push_back(std::move(profile));
      }
    }
    if (has_group) {
      canvas.Print(out_pdf.c_str());
    }
  };

  draw_sensor_tot_vs_spill_group("h_tot_vs_spill_selected_", "selected");
  draw_sensor_tot_vs_spill_group("h_tot_vs_spill_rejected_", "rejected");

  auto draw_sensor_dt_vs_spill_group = [&](const std::string &prefix, const std::string &label) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
    bool has_group = false;
    std::vector<std::unique_ptr<TProfile>> profiles;
    profiles.reserve(sensor_channels.size());
    for (size_t i = 0; i < sensor_channels.size(); ++i) {
      const int ch = sensor_channels[i];
      const std::string name = prefix + SafeName(result.config.label) + "_ch" + std::to_string(ch);
      TH2D *hist = nullptr;
      for (const auto &owned : result.histograms2d) {
        if (std::string(owned->GetName()) == name) {
          hist = owned.get();
          break;
        }
      }
      canvas.cd(static_cast<int>(i + 1));
      if (auto *pad = gPad) {
        pad->SetRightMargin(0.14);
      }
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      has_group = true;
      hist->SetTitle((result.config.label + " I=" + std::to_string(result.config.intensity) + " " + label +
                      " #Deltat vs spill ch" + std::to_string(ch) +
                      ";spill;t_{ch} - t_{trigger} [ns];entries")
                         .c_str());
      hist->Draw("colz");
      auto profile = MakeDtVsSpillProfile(*hist);
      if (profile) {
        profile->Draw("E1 same");
        profiles.push_back(std::move(profile));
      }
    }
    if (has_group) {
      canvas.Print(out_pdf.c_str());
    }
  };

  draw_sensor_dt_vs_spill_group("h_raw_dt_vs_spill_", "raw");
  draw_sensor_dt_vs_spill_group("h_dt_vs_spill_", "selected");
  draw_sensor_dt_vs_spill_group("h_rejected_dt_vs_spill_", "rejected");
  canvas.Clear();

  if (sensor_channels.size() >= 2) {
    const int ch_a = sensor_channels[0];
    const int ch_b = sensor_channels[1];
    const std::string name = "h_dt_corr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" +
                             SafeName(result.config.label);
    TH1D *hist = nullptr;
    for (const auto &owned : result.histograms) {
      if (std::string(owned->GetName()) == name) {
        hist = owned.get();
        break;
      }
    }
    if (hist) {
      canvas.Clear();
      canvas.SetRightMargin(0.05);
      auto *stack = new THStack(("stack_" + name).c_str(), hist->GetTitle());
      stack->SetBit(kCanDelete);
      auto *draw_hist = static_cast<TH1D *>(hist->Clone());
      draw_hist->SetDirectory(nullptr);
      draw_hist->SetLineColor(kBlack);
      draw_hist->SetLineWidth(2);
      stack->Add(draw_hist, "hist");
      if (stack->GetHists()) {
        stack->GetHists()->SetOwner(kTRUE);
      }
      stack->Draw("hist");
      canvas.Print(out_pdf.c_str());
    }
  }
  canvas.SetRightMargin(0.05);
}

void DrawTimewalkFitSummaries(TCanvas &canvas,
                              const std::vector<RunResult> &results,
                              const std::vector<int> &sensor_channels,
                              const std::map<int, FitRange> &fit_ranges,
                              TimewalkFitModel fit_model,
                              double max_duration_ns,
                              double match_window_ns,
                              bool signed_dt,
                              const std::string &out_pdf)
{
  const double ymin = TimewalkDtMin(signed_dt, match_window_ns);
  const double ymax = TimewalkDtMax(match_window_ns);
  const double xmax = std::max(1.0, max_duration_ns);

  for (int ch : sensor_channels) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    auto *multi = new TMultiGraph();
    multi->SetBit(kCanDelete);
    multi->SetTitle(("Timewalk correction fits ch" + std::to_string(ch) + ";ToT [ns];#Deltat fit [ns]").c_str());
    multi->SetMinimum(ymin);
    multi->SetMaximum(ymax);

    auto *legend = new TLegend(0.62, 0.58, 0.92, 0.90);
    legend->SetBit(kCanDelete);
    legend->SetBorderSize(0);
    legend->SetFillStyle(0);

    int nfits = 0;
    for (size_t i = 0; i < results.size(); ++i) {
      const auto &result = results[i];
      TH2D *hist = FindTimewalkHist(result, ch);
      if (!hist) {
        continue;
      }
      auto profile = MakeTimewalkProfile(*hist);
      auto fit = FitTimewalkProfile(profile.get(), FitRangeForChannel(fit_ranges, ch), fit_model);
      if (!fit) {
        continue;
      }

      const int color = ColorForIndex(i);
      auto graph = MakeFitGraph(*fit, "g_timewalk_fit_" + SafeName(result.config.label) + "_ch" + std::to_string(ch), 0.0, xmax, color);
      std::ostringstream label;
      label << result.config.label << " I=" << result.config.intensity;
      legend->AddEntry(graph.get(), label.str().c_str(), "l");
      multi->Add(graph.release(), "L");
      ++nfits;
    }

    auto accumulated = MakeAccumulatedTimewalkHist(results, ch);
    if (accumulated && accumulated->GetEntries() > 0.0) {
      auto profile = MakeTimewalkProfile(*accumulated);
      auto fit = FitTimewalkProfile(profile.get(), FitRangeForChannel(fit_ranges, ch), fit_model);
      if (fit) {
        auto graph = MakeFitGraph(
            *fit, "g_timewalk_fit_accum_ch" + std::to_string(ch), 0.0, xmax, kBlack);
        graph->SetLineWidth(4);
        legend->AddEntry(graph.get(), "accumulated", "l");
        multi->Add(graph.release(), "L");
        ++nfits;
      }
    }

    if (nfits <= 0) {
      delete multi;
      delete legend;
      continue;
    }
    if (multi->GetListOfGraphs()) {
      multi->GetListOfGraphs()->SetOwner(kTRUE);
    }
    multi->Draw("A");
    if (multi->GetXaxis()) {
      multi->GetXaxis()->SetLimits(0.0, xmax);
    }
    legend->Draw();
    canvas.Print(out_pdf.c_str());
  }
}

void DrawAccumulatedTimewalkFits(TCanvas &canvas,
                                 const std::vector<RunResult> &results,
                                 const std::vector<int> &sensor_channels,
                                 const std::map<int, FitRange> &fit_ranges,
                                 TimewalkFitModel fit_model,
                                 const std::map<int, DtTotCut> &dt_tot_cuts,
                                 const std::string &out_pdf)
{
  canvas.SetRightMargin(0.14);
  for (int ch : sensor_channels) {
    const DtTotCut cut = DtTotCutForChannel(dt_tot_cuts, ch);
    auto raw_accumulated = MakeAccumulatedRawTimewalkHist(results, ch);
    if (cut.enabled && raw_accumulated && raw_accumulated->GetEntries() > 0.0) {
      canvas.Clear();
      auto *draw_raw = static_cast<TH2D *>(raw_accumulated->Clone());
      draw_raw->SetDirectory(nullptr);
      draw_raw->SetBit(kCanDelete);
      draw_raw->Draw("colz");
      DrawDtTotCutLine(cut, draw_raw);
      canvas.Print(out_pdf.c_str());
    }

    auto accumulated = MakeAccumulatedTimewalkHist(results, ch);
    if (!accumulated || accumulated->GetEntries() <= 0.0) {
      auto rejected = MakeAccumulatedRejectedTimewalkHist(results, ch);
      if (cut.enabled && rejected && rejected->GetEntries() > 0.0) {
        canvas.Clear();
        auto *draw_rejected = static_cast<TH2D *>(rejected->Clone());
        draw_rejected->SetDirectory(nullptr);
        draw_rejected->SetBit(kCanDelete);
        draw_rejected->Draw("colz");
        DrawDtTotCutLine(cut, draw_rejected);
        canvas.Print(out_pdf.c_str());
      }
      continue;
    }

    canvas.Clear();
    auto *draw_hist = static_cast<TH2D *>(accumulated->Clone());
    draw_hist->SetDirectory(nullptr);
    draw_hist->SetBit(kCanDelete);
    auto profile = MakeTimewalkProfile(*draw_hist);
    auto fit = FitTimewalkProfile(profile.get(), FitRangeForChannel(fit_ranges, ch), fit_model);
    draw_hist->Draw("colz");
    if (profile) {
      profile->Draw("E1 same");
    }
    if (fit) {
      fit->Draw("same");
    }
    DrawDtTotCutLine(cut, draw_hist);
    canvas.Print(out_pdf.c_str());

    if (profile) {
      canvas.Clear();
      canvas.SetRightMargin(0.05);
      profile->SetMinimum(draw_hist->GetYaxis()->GetXmin());
      profile->SetMaximum(draw_hist->GetYaxis()->GetXmax());
      profile->Draw("E1");
      if (fit) {
        fit->Draw("same");
      }
      canvas.Print(out_pdf.c_str());
      canvas.SetRightMargin(0.14);
    }

    auto rejected = MakeAccumulatedRejectedTimewalkHist(results, ch);
    if (cut.enabled && rejected && rejected->GetEntries() > 0.0) {
      canvas.Clear();
      auto *draw_rejected = static_cast<TH2D *>(rejected->Clone());
      draw_rejected->SetDirectory(nullptr);
      draw_rejected->SetBit(kCanDelete);
      draw_rejected->Draw("colz");
      DrawDtTotCutLine(cut, draw_rejected);
      canvas.Print(out_pdf.c_str());
    }
  }
  canvas.SetRightMargin(0.05);
}

void DrawCorrectedAccumulatedTimewalk(TCanvas &canvas,
                                      const std::vector<std::unique_ptr<TH2D>> &corrected_accumulated_histograms,
                                      const std::string &out_pdf)
{
  canvas.SetRightMargin(0.14);
  for (const auto &hist : corrected_accumulated_histograms) {
    if (!hist || hist->GetEntries() <= 0.0) {
      continue;
    }
    canvas.Clear();
    auto *draw_hist = static_cast<TH2D *>(hist->Clone());
    draw_hist->SetDirectory(nullptr);
    draw_hist->SetBit(kCanDelete);
    draw_hist->Draw("colz");
    canvas.Print(out_pdf.c_str());
  }
  canvas.SetRightMargin(0.05);
}

void WriteTextSummary(const std::string &path,
                      const std::vector<RunResult> &results,
                      const std::vector<int> &sensor_channels,
	                      int trigger_channel,
	                      const std::string &runlist_path,
	                      const std::string &fine_calib_path,
	                      const std::string &chan_calib_path,
	                      const std::map<int, TimewalkCorrection> &timewalk_corrections,
	                      const std::map<int, DtTotCut> &dt_tot_cuts,
	                      const TotWindow &trigger_tot_window,
	                      const SpillRange &spill_range,
	                      const std::map<int, TotWindow> &channel_tot_windows,
	                      int edge_spill,
	                      const std::string &edge_channels_csv,
	                      double edge_phase_period_ns,
	                      double edge_spill_fraction,
	                      bool signed_dt)
{
  std::ofstream out(path);
  if (!out) {
    std::cerr << "Cannot write text summary: " << path << std::endl;
    return;
  }
  out << "# runlist: " << runlist_path << "\n";
  out << "# fine_calib: " << fine_calib_path << "\n";
	  out << "# channel_calib: " << chan_calib_path << "\n";
	  out << "# dt is t_channel - t_trigger for channels matched to trigger ch" << trigger_channel << "\n";
	  out << "# dt trigger matching: " << (signed_dt ? "nearest signed trigger" : "previous trigger, dt >= 0") << "\n";
	  out << "# ToT statistics use valid leading/trailing hits after optional spill and channel-ToT selections, before dt matching\n";
	  out << "# optional dt/ToT cut keeps events with dt >= DT0 + SLOPE*ToT; rejected events are written as h_rejected_*\n";
	  if (trigger_tot_window.enabled) {
	    out << "# trigger ToT window: [" << trigger_tot_window.min << "," << trigger_tot_window.max
	        << "] ns, applied before trigger cleanup and sensor matching\n";
	  }
	  if (spill_range.enabled) {
	    out << "# spill range: [" << spill_range.min << "," << spill_range.max
	        << "], applied while reading hits\n";
	  }
	  for (const auto &kv : channel_tot_windows) {
	    if (!kv.second.enabled) {
	      continue;
	    }
	    out << "# channel_tot_window_ch" << kv.first << ": [" << kv.second.min << "," << kv.second.max
	        << "] ns, applied before trigger/sensor grouping\n";
	  }
	  out << "# h_dt_vs_tot histograms correlate matched dt with ToT for each sensor channel, using |dt|/dt < "
	      << kTimewalkDtLimitNs << " ns range\n";
	  out << "# h_dt_vs_tot ProfileX objects are fitted with the configured timewalk model and written as *_pfx plus TF1\n";
	  out << "# h_dt_vs_tot_accum_ch* objects sum all intensities per channel and are fitted in the same way\n";
	  out << "# h_tot_vs_spill_* histograms show ToT versus spill for each channel, with *_pfx mean-ToT profiles\n";
	  out << "# h_*dt_vs_spill_* histograms show matched dt versus spill for raw/selected/rejected sensor events, with *_pfx mean-dt profiles\n";
	  out << "# h_edge_time_* histograms show leading/trailing edge times within one spill for channels "
	      << edge_channels_csv << "; requested spill=" << edge_spill
	      << " (-1 means first selected spill), initial fraction=" << edge_spill_fraction << "\n";
	  out << "# h_edge_phase_* histograms fold the same single-spill edges modulo "
	      << edge_phase_period_ns << " ns\n";
	  out << "# h_dt_corr_vs_tot_accum_ch* histograms use all corrected events accumulated over all intensities\n";
	  out << "# h_dt_corr_ch*_ch* histograms are corrected channel-channel coincidences in ["
	      << kCorrectedCoincidenceDtMinNs << ", " << kCorrectedCoincidenceDtMaxNs
	      << "] ns, matched through the same clean trigger\n";
	  out << "# trigger ToT statistics use the cleaned channel-" << trigger_channel << " trigger candidates\n";
	  out << "# trigger22 cleanup/dead-time is applied only to channel " << trigger_channel << "\n";
  out << "# trigger22 period cleanup keeps only candidates compatible with the expected trigger period\n";
  out << "# trigger22_clean_period is computed after channel-" << trigger_channel << " dead-time cleaning\n";
  for (int ch : sensor_channels) {
    auto cut_it = dt_tot_cuts.find(ch);
    if (cut_it != dt_tot_cuts.end() && cut_it->second.enabled) {
      const auto &cut = cut_it->second;
      out << "# dt_tot_cut_ch" << ch << ": keep dt >= " << cut.intercept << " + " << cut.slope << "*ToT";
      if (std::isfinite(cut.tot_min) || std::isfinite(cut.tot_max)) {
        out << " for ToT in [" << cut.tot_min << "," << cut.tot_max << "] ns";
      }
      out << "\n";
    }
    auto correction_it = timewalk_corrections.find(ch);
    if (correction_it == timewalk_corrections.end() || !correction_it->second.valid) {
      out << "# timewalk_correction_ch" << ch << ": not available\n";
      continue;
    }
    const auto &correction = correction_it->second;
    out << "# timewalk_correction_ch" << ch << ": model=" << TimewalkFitModelName(correction.model);
    if (correction.model == TimewalkFitModel::LinExpPlateau) {
      out << " correction_ns=max(linear/exponential-plateau, 0)"
          << " p0=" << correction.p0 << " p1=" << correction.p1 << " x0=" << correction.p2
          << " tau=" << correction.p3 << " plateau=" << correction.p4;
    } else {
      out << " correction_ns=max(" << correction.p0 << " + " << correction.p1 << "*ToT, 0)";
    }
    if (correction.fit_range.enabled) {
      out << " fit_range_ns=[" << correction.fit_range.xmin << "," << correction.fit_range.xmax << "]";
    }
    out << "\n";
  }
  out << "run\tintensity\tchannel\thits_read\tleading_hits\ttrigger22_raw_candidates"
      << "\ttrigger22_tot_window_candidates\ttrigger22_clean_candidates"
      << "\ttrigger22_clean_period_mean_ns\ttrigger22_clean_period_rms_ns"
      << "\tdt_entries\tdt_mean_ns\tdt_rms_ns\tdt_err_mean_ns"
      << "\ttot_entries\ttot_mean_ns\ttot_rms_ns\ttot_err_mean_ns\n";

  std::vector<int> channels = sensor_channels;
  channels.push_back(trigger_channel);
  std::sort(channels.begin(), channels.end());
  channels.erase(std::unique(channels.begin(), channels.end()), channels.end());

  out << std::setprecision(10);
  for (const auto &result : results) {
    for (int ch : channels) {
      auto it = result.channels.find(ch);
      const Stats empty;
      const Stats &dt = (it != result.channels.end()) ? it->second.dt : empty;
      const Stats &tot = (it != result.channels.end()) ? it->second.tot : empty;
      out << result.config.label << '\t' << result.config.intensity << '\t' << ch << '\t' << result.hits_read << '\t'
          << result.leading_hits << '\t' << result.trigger22_raw_candidates << '\t' << result.trigger22_tot_window_candidates
          << '\t' << result.trigger22_clean_candidates << '\t' << result.trigger22_clean_period.mean << '\t' << result.trigger22_clean_period.rms << '\t' << dt.n << '\t' << dt.mean << '\t'
          << dt.rms << '\t' << dt.err_mean << '\t' << tot.n << '\t' << tot.mean << '\t' << tot.rms << '\t'
          << tot.err_mean << '\n';
    }
  }
}
}  // namespace

void laser_intensity_scan_rdf(const char *runlist_path = "help",
                              const char *fine_calib_path = "",
                              const char *chan_calib_path = "",
                              const char *out_pdf = "laser_intensity_scan.pdf",
                              const char *out_root = "laser_intensity_scan.root",
                              const char *out_txt = "laser_intensity_scan.txt",
                              int trigger_channel = 22,
                              const char *sensor_channels_csv = "17,19",
                              double match_window_ns = 100.0,
                              double max_duration_ns = 30.0,
                              double clock_mhz = 320.0,
                              bool use_fine = true,
                              bool use_lut = true,
                              int fine_cut = 0,
                              bool require_valid_tot = true,
                              double trigger_deadtime_ns = 50000.0,
                              double trigger_period_ns = 1000000.0,
                              double trigger_period_tolerance_ns = 50000.0,
                              bool signed_dt = false,
                              const char *timewalk_fit_ranges_csv = "17:7:18.5,19:3:15",
                              const char *timewalk_fit_model_name = "lin-exp-plateau",
                              const char *dt_tot_cuts_csv = "",
                              const char *trigger_tot_window_csv = "",
                              const char *spill_range_csv = "",
                              const char *channel_tot_windows_csv = "",
                              int edge_spill = -1,
                              const char *edge_channels_csv = "all",
                              double edge_phase_period_ns = 0.0,
                              double edge_spill_fraction = 0.01)
{
  if (WantsHelp(runlist_path)) {
    PrintHelp();
    return;
  }
  if (max_duration_ns <= 0.0) {
    max_duration_ns = 30.0;
  }
  if (match_window_ns <= 0.0) {
    match_window_ns = 100.0;
  }
  if (trigger_period_ns < 0.0) {
    trigger_period_ns = 0.0;
  }
  if (trigger_period_tolerance_ns < 0.0) {
    trigger_period_tolerance_ns = 0.0;
  }

  auto runs = LoadRunList(runlist_path);
  if (runs.empty()) {
    std::cerr << "No runs loaded from " << runlist_path << std::endl;
    return;
  }
  auto sensor_channels = ParseChannelsCsv(sensor_channels_csv ? sensor_channels_csv : "");
  if (sensor_channels.empty()) {
    std::cerr << "No sensor channels configured." << std::endl;
    return;
  }
  std::vector<int> analysis_channels = sensor_channels;
  analysis_channels.push_back(trigger_channel);
  std::sort(analysis_channels.begin(), analysis_channels.end());
  analysis_channels.erase(std::unique(analysis_channels.begin(), analysis_channels.end()), analysis_channels.end());
  auto edge_channels = ParseEdgeChannelsCsv(edge_channels_csv ? edge_channels_csv : "all", analysis_channels);
  auto timewalk_fit_ranges = ParseFitRangesCsv(timewalk_fit_ranges_csv ? timewalk_fit_ranges_csv : "");
  auto dt_tot_cuts = ParseDtTotCutsCsv(dt_tot_cuts_csv ? dt_tot_cuts_csv : "");
  auto trigger_tot_window = ParseTotWindow(trigger_tot_window_csv ? trigger_tot_window_csv : "");
  auto spill_range = ParseSpillRange(spill_range_csv ? spill_range_csv : "");
  auto channel_tot_windows = ParseChannelTotWindowsCsv(channel_tot_windows_csv ? channel_tot_windows_csv : "");
  const auto timewalk_fit_model =
      ParseTimewalkFitModel(timewalk_fit_model_name ? timewalk_fit_model_name : "lin-exp-plateau");

  analysis_time::FineCalib fine_calib;
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    if (!fine_calib.LoadFromFile(fine_calib_path)) {
      std::cerr << "Warning: fine calibration not loaded: " << fine_calib_path << std::endl;
    }
  }
  fine_calib.use_lut = use_lut;
  analysis_time::ChannelCalib chan_calib;
  if (chan_calib_path && chan_calib_path[0] != '\0') {
    if (!chan_calib.LoadFromFile(chan_calib_path)) {
      std::cerr << "Warning: channel calibration not loaded: " << chan_calib_path << std::endl;
    }
    if (chan_calib.meta_loaded && chan_calib.meta_maxdur > 0.0 &&
        std::abs(max_duration_ns - chan_calib.meta_maxdur) > 1e-6) {
      std::cout << "Warning: max_duration_ns=" << max_duration_ns << " differs from channel calibration "
                << chan_calib.meta_maxdur << " ns; using calibration value." << std::endl;
      max_duration_ns = chan_calib.meta_maxdur;
    }
  }

  std::cout << "Run list: " << runlist_path << std::endl;
  std::cout << "Runs: " << runs.size() << std::endl;
  std::cout << "Trigger channel: " << trigger_channel << std::endl;
  std::cout << "Sensor channels: " << sensor_channels_csv << std::endl;
  std::cout << "Match window (ns): " << match_window_ns << std::endl;
  std::cout << "Trigger dead-time cleaning (ns): " << trigger_deadtime_ns << std::endl;
  std::cout << "Trigger expected period (ns): " << trigger_period_ns << std::endl;
  std::cout << "Trigger period tolerance (ns): " << trigger_period_tolerance_ns << std::endl;
  std::cout << "Signed nearest-trigger dt: " << signed_dt << std::endl;
  std::cout << "Timewalk fit ranges: " << (timewalk_fit_ranges_csv ? timewalk_fit_ranges_csv : "") << std::endl;
  std::cout << "Timewalk fit model: " << TimewalkFitModelName(timewalk_fit_model) << std::endl;
  std::cout << "dt/ToT selection cuts: " << (dt_tot_cuts_csv ? dt_tot_cuts_csv : "") << std::endl;
  std::cout << "Trigger ToT window: " << (trigger_tot_window_csv ? trigger_tot_window_csv : "") << std::endl;
  std::cout << "Spill range: " << (spill_range_csv ? spill_range_csv : "") << std::endl;
  std::cout << "Channel ToT windows: " << (channel_tot_windows_csv ? channel_tot_windows_csv : "") << std::endl;
  std::cout << "Single-spill edge diagnostic spill: " << edge_spill << std::endl;
  std::cout << "Single-spill edge diagnostic channels: " << (edge_channels_csv ? edge_channels_csv : "all")
            << std::endl;
  std::cout << "Single-spill edge diagnostic phase period (ns): "
            << (edge_phase_period_ns > 0.0 ? edge_phase_period_ns : trigger_period_ns) << std::endl;
  std::cout << "Single-spill edge diagnostic initial fraction: " << edge_spill_fraction << std::endl;
  std::cout << "Max ToT duration (ns): " << max_duration_ns << std::endl;
  std::cout << "Clock (MHz): " << clock_mhz << std::endl;
  std::cout << "Use fine: " << use_fine << ", use LUT: " << use_lut << ", require valid ToT: " << require_valid_tot
            << std::endl;

  std::vector<RunResult> results;
  results.reserve(runs.size());
  for (const auto &run : runs) {
    std::cout << "== Analyze " << run.label << " intensity=" << run.intensity << std::endl;
    results.push_back(AnalyzeRun(run,
                                 sensor_channels,
                                 trigger_channel,
                                 fine_calib,
                                 chan_calib,
                                 match_window_ns,
                                 trigger_deadtime_ns,
                                 trigger_period_ns,
                                 trigger_period_tolerance_ns,
                                 max_duration_ns,
                                 clock_mhz,
                                 use_fine,
                                 fine_cut,
                                 require_valid_tot,
                                 signed_dt,
                                 dt_tot_cuts,
                                 trigger_tot_window,
                                 spill_range,
                                 channel_tot_windows,
                                 edge_spill,
                                 edge_channels,
                                 edge_phase_period_ns,
                                 edge_spill_fraction));
  }

  auto timewalk_corrections =
      BuildTimewalkCorrections(results, sensor_channels, timewalk_fit_ranges, timewalk_fit_model);
  std::vector<std::unique_ptr<TH2D>> corrected_accumulated_timewalk_histograms;
  BuildCorrectedTimewalkAndCoincidencePlots(results,
                                            sensor_channels,
                                            timewalk_corrections,
                                            match_window_ns,
                                            max_duration_ns,
                                            signed_dt,
                                            corrected_accumulated_timewalk_histograms);

  std::vector<std::unique_ptr<TGraphErrors>> graphs;
  std::vector<TGraphErrors *> dt_mean_graphs;
  std::vector<TGraphErrors *> dt_rms_graphs;
  std::vector<TGraphErrors *> tot_mean_graphs;
  std::vector<TGraphErrors *> tot_rms_graphs;
  std::vector<std::string> dt_labels;
  std::vector<std::string> tot_labels;

  for (size_t i = 0; i < sensor_channels.size(); ++i) {
    const int ch = sensor_channels[i];
    const int color = ColorForIndex(i);
    auto g_dt_mean = MakeGraph(results,
                               ch,
                               true,
                               false,
                               "g_dt_mean_ch" + std::to_string(ch),
                               "mean #Deltat ch" + std::to_string(ch),
                               color);
    dt_mean_graphs.push_back(g_dt_mean.get());
    graphs.push_back(std::move(g_dt_mean));

    auto g_dt_rms = MakeGraph(results,
                              ch,
                              true,
                              true,
                              "g_dt_rms_ch" + std::to_string(ch),
                              "RMS #Deltat ch" + std::to_string(ch),
                              color);
    dt_rms_graphs.push_back(g_dt_rms.get());
    graphs.push_back(std::move(g_dt_rms));
    dt_labels.push_back("ch " + std::to_string(ch));
  }

  std::vector<int> tot_channels = sensor_channels;
  tot_channels.push_back(trigger_channel);
  std::sort(tot_channels.begin(), tot_channels.end());
  tot_channels.erase(std::unique(tot_channels.begin(), tot_channels.end()), tot_channels.end());
  for (size_t i = 0; i < tot_channels.size(); ++i) {
    const int ch = tot_channels[i];
    const int color = ColorForIndex(i);
    auto g_tot_mean = MakeGraph(results,
                                ch,
                                false,
                                false,
                                "g_tot_mean_ch" + std::to_string(ch),
                                "mean ToT ch" + std::to_string(ch),
                                color);
    tot_mean_graphs.push_back(g_tot_mean.get());
    graphs.push_back(std::move(g_tot_mean));

    auto g_tot_rms = MakeGraph(results,
                               ch,
                               false,
                               true,
                               "g_tot_rms_ch" + std::to_string(ch),
                               "RMS ToT ch" + std::to_string(ch),
                               color);
    tot_rms_graphs.push_back(g_tot_rms.get());
    graphs.push_back(std::move(g_tot_rms));
    tot_labels.push_back("ch " + std::to_string(ch));
  }

  if (out_txt && out_txt[0] != '\0') {
	    WriteTextSummary(
	        out_txt,
	        results,
	        sensor_channels,
	        trigger_channel,
	        runlist_path,
	        fine_calib_path,
	        chan_calib_path,
	        timewalk_corrections,
	        dt_tot_cuts,
	        trigger_tot_window,
	        spill_range,
	        channel_tot_windows,
	        edge_spill,
	        edge_channels_csv ? edge_channels_csv : "all",
	        edge_phase_period_ns > 0.0 ? edge_phase_period_ns : trigger_period_ns,
	        edge_spill_fraction,
	        signed_dt);
  }

  if (out_root && out_root[0] != '\0') {
    TFile fout(out_root, "RECREATE");
    TParameter<int>("trigger_channel", trigger_channel).Write();
    TParameter<double>("match_window_ns", match_window_ns).Write();
    TParameter<double>("trigger_deadtime_ns", trigger_deadtime_ns).Write();
    TParameter<double>("trigger_period_ns", trigger_period_ns).Write();
    TParameter<double>("trigger_period_tolerance_ns", trigger_period_tolerance_ns).Write();
    TParameter<int>("signed_dt", signed_dt ? 1 : 0).Write();
    TParameter<double>("max_duration_ns", max_duration_ns).Write();
    TParameter<double>("clock_mhz", clock_mhz).Write();
    TParameter<int>("trigger_tot_window_enabled", trigger_tot_window.enabled ? 1 : 0).Write();
    TParameter<double>("trigger_tot_window_min", trigger_tot_window.min).Write();
    TParameter<double>("trigger_tot_window_max", trigger_tot_window.max).Write();
    TParameter<int>("spill_range_enabled", spill_range.enabled ? 1 : 0).Write();
    TParameter<int>("spill_range_min", spill_range.min).Write();
    TParameter<int>("spill_range_max", spill_range.max).Write();
    TParameter<int>("edge_spill_requested", edge_spill).Write();
    TParameter<double>("edge_phase_period_ns", edge_phase_period_ns > 0.0 ? edge_phase_period_ns : trigger_period_ns)
        .Write();
    TParameter<double>("edge_spill_fraction", edge_spill_fraction).Write();
    for (const auto &kv : channel_tot_windows) {
      const int ch = kv.first;
      const auto &window = kv.second;
      TParameter<int>(("channel_tot_window_enabled_ch" + std::to_string(ch)).c_str(), window.enabled ? 1 : 0)
          .Write();
      TParameter<double>(("channel_tot_window_min_ch" + std::to_string(ch)).c_str(), window.min).Write();
      TParameter<double>(("channel_tot_window_max_ch" + std::to_string(ch)).c_str(), window.max).Write();
    }
    for (const auto &kv : timewalk_corrections) {
	      const int ch = kv.first;
	      const auto &correction = kv.second;
	      TParameter<int>(("timewalk_corr_valid_ch" + std::to_string(ch)).c_str(), correction.valid ? 1 : 0).Write();
	      TParameter<int>(("timewalk_corr_model_ch" + std::to_string(ch)).c_str(),
	                      static_cast<int>(correction.model))
	          .Write();
	      TParameter<double>(("timewalk_corr_p0_ch" + std::to_string(ch)).c_str(), correction.p0).Write();
	      TParameter<double>(("timewalk_corr_p1_ch" + std::to_string(ch)).c_str(), correction.p1).Write();
	      TParameter<double>(("timewalk_corr_p2_ch" + std::to_string(ch)).c_str(), correction.p2).Write();
	      TParameter<double>(("timewalk_corr_p3_ch" + std::to_string(ch)).c_str(), correction.p3).Write();
	      TParameter<double>(("timewalk_corr_p4_ch" + std::to_string(ch)).c_str(), correction.p4).Write();
      if (correction.fit_range.enabled) {
        TParameter<double>(("timewalk_corr_fit_xmin_ch" + std::to_string(ch)).c_str(), correction.fit_range.xmin)
            .Write();
        TParameter<double>(("timewalk_corr_fit_xmax_ch" + std::to_string(ch)).c_str(), correction.fit_range.xmax)
            .Write();
      }
    }
    for (const auto &kv : dt_tot_cuts) {
      const int ch = kv.first;
      const auto &cut = kv.second;
      TParameter<int>(("dt_tot_cut_enabled_ch" + std::to_string(ch)).c_str(), cut.enabled ? 1 : 0).Write();
      TParameter<double>(("dt_tot_cut_intercept_ch" + std::to_string(ch)).c_str(), cut.intercept).Write();
      TParameter<double>(("dt_tot_cut_slope_ch" + std::to_string(ch)).c_str(), cut.slope).Write();
      TParameter<double>(("dt_tot_cut_tot_min_ch" + std::to_string(ch)).c_str(), cut.tot_min).Write();
      TParameter<double>(("dt_tot_cut_tot_max_ch" + std::to_string(ch)).c_str(), cut.tot_max).Write();
    }
    for (auto &graph : graphs) {
      graph->Write();
    }
    for (const auto &result : results) {
      for (const auto &hist : result.histograms) {
        hist->Write();
      }
      for (const auto &hist : result.histograms2d) {
        hist->Write();
        const std::string hist_name = hist->GetName();
        if (hist_name.rfind("h_dt_vs_tot_", 0) == 0) {
	          const int ch = std::atoi(hist_name.substr(hist_name.rfind("_ch") + 3).c_str());
	          auto profile = MakeTimewalkProfile(*hist);
	          auto fit =
	              FitTimewalkProfile(profile.get(), FitRangeForChannel(timewalk_fit_ranges, ch), timewalk_fit_model);
          if (profile) {
            profile->Write();
          }
          if (fit) {
            fit->Write();
          }
        } else if (hist_name.rfind("h_tot_vs_spill_", 0) == 0) {
          auto profile = MakeTotVsSpillProfile(*hist);
          if (profile) {
            profile->Write();
          }
        } else if (hist_name.rfind("h_raw_dt_vs_spill_", 0) == 0 ||
                   hist_name.rfind("h_dt_vs_spill_", 0) == 0 ||
                   hist_name.rfind("h_rejected_dt_vs_spill_", 0) == 0) {
          auto profile = MakeDtVsSpillProfile(*hist);
          if (profile) {
            profile->Write();
          }
        }
      }
    }
    for (int ch : sensor_channels) {
      auto raw_accumulated = MakeAccumulatedRawTimewalkHist(results, ch);
      if (raw_accumulated && raw_accumulated->GetEntries() > 0.0) {
        raw_accumulated->Write();
      }
      auto rejected = MakeAccumulatedRejectedTimewalkHist(results, ch);
      if (rejected && rejected->GetEntries() > 0.0) {
        rejected->Write();
      }

      auto accumulated = MakeAccumulatedTimewalkHist(results, ch);
      if (!accumulated || accumulated->GetEntries() <= 0.0) {
        continue;
      }
	      accumulated->Write();
	      auto profile = MakeTimewalkProfile(*accumulated);
	      auto fit =
	          FitTimewalkProfile(profile.get(), FitRangeForChannel(timewalk_fit_ranges, ch), timewalk_fit_model);
      if (profile) {
        profile->Write();
      }
      if (fit) {
        fit->Write();
      }
    }
    for (const auto &hist : corrected_accumulated_timewalk_histograms) {
      if (hist) {
        hist->Write();
      }
    }
    fout.Close();
    std::cout << "Wrote ROOT output: " << out_root << std::endl;
  }

  if (out_pdf && out_pdf[0] != '\0') {
    gStyle->SetOptStat(1110);
    gStyle->SetEndErrorSize(4);
    TCanvas canvas("c_laser_intensity_scan", "laser intensity scan", 1600, 900);
    canvas.Print((std::string(out_pdf) + "[").c_str());
    DrawGraphs(canvas,
               dt_mean_graphs,
               dt_labels,
               "Mean time difference to laser trigger",
               "mean(t_{ch} - t_{trigger}) [ns]",
               out_pdf);
    DrawGraphs(canvas,
               dt_rms_graphs,
               dt_labels,
               "Time-difference RMS to laser trigger",
               "RMS(t_{ch} - t_{trigger}) [ns]",
               out_pdf);
	    DrawGraphs(canvas, tot_mean_graphs, tot_labels, "Mean ToT", "mean ToT [ns]", out_pdf);
	    DrawGraphs(canvas, tot_rms_graphs, tot_labels, "ToT RMS", "RMS ToT [ns]", out_pdf);
	    for (const auto &result : results) {
	      DrawRunHistograms(
	          canvas, result, sensor_channels, trigger_channel, timewalk_fit_ranges, timewalk_fit_model, out_pdf);
	    }
	    DrawAccumulatedTimewalkFits(
	        canvas, results, sensor_channels, timewalk_fit_ranges, timewalk_fit_model, dt_tot_cuts, out_pdf);
	    DrawCorrectedAccumulatedTimewalk(canvas, corrected_accumulated_timewalk_histograms, out_pdf);
	    DrawTimewalkFitSummaries(
	        canvas,
	        results,
	        sensor_channels,
	        timewalk_fit_ranges,
	        timewalk_fit_model,
	        max_duration_ns,
	        match_window_ns,
	        signed_dt,
	        out_pdf);
    canvas.Print((std::string(out_pdf) + "]").c_str());
    std::cout << "Wrote PDF output: " << out_pdf << std::endl;
  }
}
