#include <TCanvas.h>
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
#include <TLatex.h>
#include <TMultiGraph.h>
#include <TParameter.h>
#include <TPad.h>
#include <TProfile.h>
#include <TStyle.h>
#include <TSystem.h>
#include <TTree.h>

#include "analysis_events.h"
#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
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
constexpr double kCorrectedCoincidenceDtMinNs = -3.0;
constexpr double kCorrectedCoincidenceDtMaxNs = 3.0;

struct FitRange {
  bool enabled = false;
  double xmin = 0.0;
  double xmax = 0.0;
};

enum class DtTotCutDirection {
  KeepBelow = 0,
  KeepAbove = 1,
};

std::string DtTotCutDirectionName(DtTotCutDirection direction)
{
  return direction == DtTotCutDirection::KeepAbove ? "above" : "below";
}

struct DtTotCut {
  bool enabled = false;
  double intercept = 0.0;
  double slope = 0.0;
  double tot_min = -std::numeric_limits<double>::infinity();
  double tot_max = std::numeric_limits<double>::infinity();
  DtTotCutDirection direction = DtTotCutDirection::KeepBelow;

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
    if (direction == DtTotCutDirection::KeepAbove) {
      return dt >= Boundary(tot);
    }
    return dt <= Boundary(tot);
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
  Pol1Plateau = 2,
};

enum class TimeReferenceMode {
  EventMedian = 0,
  Trigger = 1,
};

enum class RunPlotGroup {
  Results = 0,
  Diagnostics = 1,
};

std::string TimeReferenceModeName(TimeReferenceMode mode)
{
  switch (mode) {
    case TimeReferenceMode::EventMedian:
      return "event-median";
    case TimeReferenceMode::Trigger:
      return "trigger";
  }
  return "unknown";
}

TimeReferenceMode ParseTimeReferenceMode(const std::string &value)
{
  if (value.empty() || value == "trigger" || value == "ch22" || value == "reference-channel" ||
      value == "reference_channel") {
    return TimeReferenceMode::Trigger;
  }
  if (value == "event-median" || value == "event_median" || value == "median" || value == "event") {
    return TimeReferenceMode::EventMedian;
  }
  std::cerr << "Unknown reference mode '" << value << "', using trigger" << std::endl;
  return TimeReferenceMode::Trigger;
}

std::string DtExpressionTitle(TimeReferenceMode mode, int trigger_channel)
{
  if (mode == TimeReferenceMode::EventMedian) {
    return "t_{ch} - t_{event median}";
  }
  return "t_{ch} - t_{trigger ch" + std::to_string(trigger_channel) + "}";
}

std::string DtAxisTitle(TimeReferenceMode mode, int trigger_channel)
{
  return DtExpressionTitle(mode, trigger_channel) + " [ns]";
}

std::string TimewalkFitModelName(TimewalkFitModel model)
{
  switch (model) {
    case TimewalkFitModel::Pol1:
      return "pol1";
    case TimewalkFitModel::LinExpPlateau:
      return "lin-exp-plateau";
    case TimewalkFitModel::Pol1Plateau:
      return "pol1-plateau";
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
  if (value == "pol1-plateau" || value == "pol1_plateau" || value == "linear-plateau" ||
      value == "linear_plateau" || value == "piecewise-linear" || value == "piecewise_linear") {
    return TimewalkFitModel::Pol1Plateau;
  }
  std::cerr << "Unknown timewalk fit model '" << value << "', using pol1-plateau" << std::endl;
  return TimewalkFitModel::Pol1Plateau;
}

// ALCOR channels can run in ToT mode (TDC-even/TDC-odd = rising/falling edge of
// one threshold) or Slew-Rate (SR) mode (TDC-even/TDC-odd = rising edge of the
// first/second threshold). Both couple TDC0/TDC1 and TDC2/TDC3 identically, so
// the leading/trailing pairing logic is unaffected, but the resulting dt is a
// rise-time/slew proxy rather than a pulse-width ToT and typically shrinks
// (rather than grows) with pulse amplitude. This only changes how the
// quantity is labeled in plots/summaries below.
enum class TdcOperatingMode {
  Tot,
  SlewRate,
};

std::string TdcOperatingModeName(TdcOperatingMode mode)
{
  return mode == TdcOperatingMode::SlewRate ? "slew-rate" : "tot";
}

TdcOperatingMode ParseTdcOperatingMode(const std::string &value)
{
  if (value == "slew-rate" || value == "slew_rate" || value == "slew" || value == "sr") {
    return TdcOperatingMode::SlewRate;
  }
  if (!value.empty() && value != "tot") {
    std::cerr << "Unknown TDC operating mode '" << value << "', using tot" << std::endl;
  }
  return TdcOperatingMode::Tot;
}

std::string g_tot_quantity_label = "ToT";
std::string g_tot_axis_label = "ToT [ns]";
TdcOperatingMode g_tdc_operating_mode = TdcOperatingMode::Tot;

void ConfigureTotLabels(TdcOperatingMode mode)
{
  g_tdc_operating_mode = mode;
  if (mode == TdcOperatingMode::SlewRate) {
    g_tot_quantity_label = "slew dt";
    g_tot_axis_label = "slew #Deltat [ns]";
  } else {
    g_tot_quantity_label = "ToT";
    g_tot_axis_label = "ToT [ns]";
  }
}

// Slew-rate dt is a rise-time proxy that is typically much smaller than the
// booked [0, max_duration_ns] axis range (which is tuned for ToT pulse widths),
// so the populated region can be a tiny sliver of the plotted range. Zoom the
// given axis to the histogram's actual non-empty range (with padding) so the
// plotted data is visible; a no-op in ToT mode where the booked range already
// roughly matches the data.
void AutoZoomTotAxis(TH1 *hist, TAxis *axis, int axis_num)
{
  if (g_tdc_operating_mode != TdcOperatingMode::SlewRate) {
    return;
  }
  if (!hist || !axis || hist->GetEntries() <= 0.0) {
    return;
  }
  const int first = hist->FindFirstBinAbove(0.0, axis_num);
  const int last = hist->FindLastBinAbove(0.0, axis_num);
  if (first < 1 || last < first) {
    return;
  }
  double lo = axis->GetBinLowEdge(first);
  double hi = axis->GetBinUpEdge(last);
  const double pad = std::max(0.08 * (hi - lo), 0.05);
  lo = std::max(axis->GetXmin(), lo - pad);
  hi = std::min(axis->GetXmax(), hi + pad);
  if (hi > lo) {
    axis->SetRangeUser(lo, hi);
  }
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
            << " true, 0, true, 0, 1000000, 50000, false, \"17:0:30,19:0:30\","
            << " \"pol1-plateau\", \"\", \"\", \"\", \"\", -1, \"all\", 0, 0.01,"
            << " \"17:0,19:0,22:0\", \"trigger\", 3, 0.0, \"below\", \"tot\", 0.0)\n\n"
            << "Default reference mode uses trigger_channel, normally ch22, as laser reference.\n"
            << "Event-median reference can be selected explicitly with reference_mode=\"event-median\".\n"
            << "event_window_ns=0 uses match_window_ns for event building.\n"
            << "Timewalk corrections are fitted from t_sensor - t_reference versus sensor ToT.\n"
            << "opmode_name selects the ALCOR pixel TDC mode: \"tot\" (default, pulse width) or\n"
            << "\"slew-rate\" (TDC-even/TDC-odd dt is the inter-threshold rise time, not pulse width).\n"
            << "This only changes plot/summary labeling (\"ToT\" -> \"slew dt\"); leading/trailing TDC\n"
            << "pairing is identical in both modes. Pass fit ranges/ToT windows explicitly for\n"
            << "slew-rate data since defaults below are tuned for ToT-mode amplitude scaling.\n"
            << "Runlist TSV columns: run_label, input_path, intensity, channels, thresholds, spill, vbias, note\n"
            << "input_path can be a decoded dir, run dir, or parent dir accepted by analysis_io::ResolveInputSpec.\n"
            << "Optional dt/ToT cuts use CH:DT0:SLOPE[:TOT_MIN:TOT_MAX]; direction below keeps dt <= line.\n"
            << "Optional trigger ToT window uses MIN:MAX, e.g. 1:3.\n"
            << "Optional spill range uses MIN:MAX, inclusive.\n"
            << "Optional channel ToT windows use CH:MIN:MAX, e.g. 17:18:25.\n"
            << "Optional leading TDC selection uses TDC or CH:TDC CSV, e.g. 0 or 17:0,19:2,22:0.\n"
            << "  Only leading TDC 0 or 2 is accepted; the trailing partner is kept for ToT.\n"
            << "Trigger veto/dead-time is applied after an accepted trigger; default is 0 ns.\n"
            << "sensor_duration_ns sets the x-axis maximum of sensor dt-vs-ToT histograms independently\n"
            << "  of max_duration_ns (which must stay large enough to cover the trigger channel ToT).\n"
            << "  Default 0 uses max_duration_ns. Useful in slew-rate mode where sensor slew-dt << ToT.\n"
            << "Timewalk fit models: pol1, pol1-plateau, lin-exp-plateau.\n"
            << "Optional edge diagnostic spill uses -1 for the first selected spill.\n"
            << "Optional edge diagnostic channels use all, analysis, or a CSV list.\n"
            << "Optional edge diagnostic fraction is the initial spill fraction to plot, default 0.01.\n"
            << "The PDF also includes per-channel leading-hit inter-arrival and ToT-vs-previous-hit diagnostics.\n";
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

std::string TrimCopy(const std::string &value)
{
  size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  size_t last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    --last;
  }
  return value.substr(first, last - first);
}

struct TdcSelection {
  bool default_enabled = false;
  int default_leading_tdc = -1;
  std::map<int, int> leading_tdc_by_channel;

  bool Empty() const
  {
    return !default_enabled && leading_tdc_by_channel.empty();
  }

  int LeadingTdcForChannel(int channel) const
  {
    auto it = leading_tdc_by_channel.find(channel);
    if (it != leading_tdc_by_channel.end()) {
      return it->second;
    }
    return default_enabled ? default_leading_tdc : -1;
  }

  bool KeepLeadingHit(int channel, int tdc) const
  {
    const int selected = LeadingTdcForChannel(channel);
    return selected < 0 || tdc == selected;
  }

  bool KeepRawHitForTot(int channel, int tdc) const
  {
    const int selected = LeadingTdcForChannel(channel);
    if (selected < 0) {
      return true;
    }
    return tdc == selected || tdc == (selected ^ 0x1);
  }

  std::string Description(const std::vector<int> &channels) const
  {
    if (Empty()) {
      return "";
    }
    std::ostringstream out;
    bool first = true;
    for (int ch : channels) {
      const int tdc = LeadingTdcForChannel(ch);
      if (tdc < 0) {
        continue;
      }
      if (!first) {
        out << ",";
      }
      out << ch << ":" << tdc;
      first = false;
    }
    return out.str();
  }
};

bool ParseLeadingTdcValue(const std::string &token, int &tdc)
{
  try {
    tdc = std::stoi(TrimCopy(token));
  } catch (const std::exception &) {
    return false;
  }
  return tdc == 0 || tdc == 2;
}

TdcSelection ParseTdcSelectionCsv(const std::string &csv)
{
  TdcSelection selection;
  if (csv.empty()) {
    return selection;
  }

  for (const auto &raw_token : Split(csv, ',')) {
    const std::string token = TrimCopy(raw_token);
    if (token.empty()) {
      continue;
    }
    auto fields = Split(token, ':');
    if (fields.size() == 1) {
      int tdc = -1;
      if (!ParseLeadingTdcValue(fields[0], tdc)) {
        std::cerr << "Skipping invalid TDC selection: " << token
                  << " (use leading TDC 0 or 2)" << std::endl;
        continue;
      }
      selection.default_enabled = true;
      selection.default_leading_tdc = tdc;
      continue;
    }
    if (fields.size() != 2) {
      std::cerr << "Skipping invalid TDC selection: " << token << std::endl;
      continue;
    }

    int tdc = -1;
    if (!ParseLeadingTdcValue(fields[1], tdc)) {
      std::cerr << "Skipping invalid TDC selection: " << token
                << " (use leading TDC 0 or 2; trailing partner is kept for ToT)" << std::endl;
      continue;
    }

    const std::string channel_token = TrimCopy(fields[0]);
    if (channel_token == "*" || channel_token == "all" || channel_token == "default") {
      selection.default_enabled = true;
      selection.default_leading_tdc = tdc;
      continue;
    }

    try {
      const int channel = std::stoi(channel_token);
      if (channel < 0 || channel >= kNumAlcorChannels) {
        std::cerr << "Skipping invalid TDC selection channel: " << token << std::endl;
        continue;
      }
      selection.leading_tdc_by_channel[channel] = tdc;
    } catch (const std::exception &) {
      std::cerr << "Skipping invalid TDC selection channel: " << token << std::endl;
    }
  }

  return selection;
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

std::string ChannelsLabel(const std::vector<int> &channels)
{
  std::ostringstream out;
  for (size_t i = 0; i < channels.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << channels[i];
  }
  return out.str();
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

DtTotCutDirection ParseDtTotCutDirection(const std::string &value)
{
  if (value.empty() || value == "below" || value == "keep-below" || value == "keep_below" ||
      value == "upper" || value == "max" || value == "<=" || value == "lt") {
    return DtTotCutDirection::KeepBelow;
  }
  if (value == "above" || value == "keep-above" || value == "keep_above" ||
      value == "lower" || value == "min" || value == ">=" || value == "gt") {
    return DtTotCutDirection::KeepAbove;
  }
  std::cerr << "Unknown dt/ToT cut direction '" << value << "', using below" << std::endl;
  return DtTotCutDirection::KeepBelow;
}

std::map<int, DtTotCut> ParseDtTotCutsCsv(const std::string &csv, DtTotCutDirection direction)
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
      cut.direction = direction;
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

double ParseIntensityOrNan(const std::string &value, bool &used_average)
{
  used_average = false;
  const double scalar = ParseDoubleOrNan(value);
  if (std::isfinite(scalar)) {
    return scalar;
  }

  std::vector<double> numbers;
  const char *cursor = value.c_str();
  while (*cursor != '\0') {
    if (!std::isdigit(static_cast<unsigned char>(*cursor)) && *cursor != '.') {
      ++cursor;
      continue;
    }
    char *end = nullptr;
    const double parsed = std::strtod(cursor, &end);
    if (end && end != cursor && std::isfinite(parsed)) {
      numbers.push_back(parsed);
      cursor = end;
      continue;
    }
    ++cursor;
  }

  if (numbers.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (numbers.size() == 1) {
    return numbers.front();
  }
  used_average = true;
  return std::accumulate(numbers.begin(), numbers.end(), 0.0) / static_cast<double>(numbers.size());
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
    bool averaged_intensity = false;
    run.intensity = ParseIntensityOrNan(cols[2], averaged_intensity);
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
    if (averaged_intensity) {
      std::cerr << "Using average numeric intensity " << run.intensity
                << " for non-scalar intensity field '" << cols[2] << "' in run " << run.label << std::endl;
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

struct TreeCursor {
  std::string path;
  std::unique_ptr<TFile> file;
  TTree *tree = nullptr;
  Long64_t entry = 0;
  Long64_t entries = 0;
  int current_spill = 0;
  bool has_channel = false;
  bool has_time_tick = false;
  bool has_spill = false;
  bool has_pending = false;
  bool eof = false;
  Hit pending;

  int fifo = 0;
  int type = 0;
  int spill = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int rollover = 0;
  int coarse = 0;
  int fine = 0;
  int channel = 0;
  Long64_t time_tick = 0;
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

bool HasTreeBranch(TTree *tree, const char *name)
{
  return tree && tree->GetBranch(name) != nullptr;
}

void BindTreeCursorBranches(TreeCursor &cursor)
{
  if (!cursor.tree) {
    return;
  }
  cursor.tree->SetBranchAddress("type", &cursor.type);
  cursor.tree->SetBranchAddress("fifo", &cursor.fifo);
  cursor.tree->SetBranchAddress("column", &cursor.column);
  cursor.tree->SetBranchAddress("pixel", &cursor.pixel);
  cursor.tree->SetBranchAddress("tdc", &cursor.tdc);
  cursor.tree->SetBranchAddress("rollover", &cursor.rollover);
  cursor.tree->SetBranchAddress("coarse", &cursor.coarse);
  cursor.tree->SetBranchAddress("fine", &cursor.fine);
  if (cursor.has_spill) {
    cursor.tree->SetBranchAddress("spill", &cursor.spill);
  }
  if (cursor.has_channel) {
    cursor.tree->SetBranchAddress("channel", &cursor.channel);
  }
  if (cursor.has_time_tick) {
    cursor.tree->SetBranchAddress("time_tick", &cursor.time_tick);
  }
}

bool OpenTreeCursor(const std::string &path, const std::string &tree_name, TreeCursor &cursor)
{
  cursor = TreeCursor{};
  cursor.path = path;
  cursor.file.reset(TFile::Open(path.c_str(), "READ"));
  if (!cursor.file || cursor.file->IsZombie()) {
    std::cerr << "Failed to open decoded ROOT file: " << path << std::endl;
    return false;
  }
  cursor.tree = dynamic_cast<TTree *>(cursor.file->Get(tree_name.c_str()));
  if (!cursor.tree) {
    std::cerr << "Missing tree '" << tree_name << "' in " << path << std::endl;
    return false;
  }

  std::vector<std::string> missing;
  for (const auto &name : {"type", "fifo", "column", "pixel", "tdc", "rollover", "coarse", "fine"}) {
    if (!HasTreeBranch(cursor.tree, name)) {
      missing.emplace_back(name);
    }
  }
  if (!missing.empty()) {
    std::cerr << "Missing required branches in " << path << ": ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i) {
        std::cerr << ", ";
      }
      std::cerr << missing[i];
    }
    std::cerr << std::endl;
    return false;
  }

  cursor.has_channel = HasTreeBranch(cursor.tree, "channel");
  cursor.has_time_tick = HasTreeBranch(cursor.tree, "time_tick");
  cursor.has_spill = HasTreeBranch(cursor.tree, "spill");

  cursor.tree->SetBranchStatus("*", 0);
  auto enable = [&cursor](const char *name) {
    if (HasTreeBranch(cursor.tree, name)) {
      cursor.tree->SetBranchStatus(name, 1);
    }
  };
  for (const auto &name : {"type", "fifo", "column", "pixel", "tdc", "rollover", "coarse", "fine",
                           "spill", "channel", "time_tick"}) {
    enable(name);
  }

  BindTreeCursorBranches(cursor);

  cursor.entries = cursor.tree->GetEntries();
  return true;
}

bool AdvanceCursor(TreeCursor &cursor,
                   const std::unordered_set<int> &selected_channels,
                   const std::unordered_set<int> &edge_channels,
                   const TdcSelection &tdc_selection,
                   const analysis_time::FineCalib &fine_calib,
                   const analysis_time::ChannelTdcOffsetCalib &tdc_offset_calib,
                   double tick_ns,
                   bool use_fine,
                   int fine_cut,
                   const SpillRange &spill_range)
{
  cursor.has_pending = false;
  while (cursor.entry < cursor.entries) {
    cursor.tree->GetEntry(cursor.entry++);
    if (!cursor.has_spill && cursor.type == 15) {
      ++cursor.current_spill;
      continue;
    }
    if (cursor.type != 1 || !IsValidTdcId(cursor.tdc)) {
      continue;
    }

    const int channel = cursor.has_channel ? cursor.channel : cursor.column * 4 + cursor.pixel;
    const bool selected = selected_channels.find(channel) != selected_channels.end();
    const bool edge_selected = edge_channels.find(channel) != edge_channels.end();
    if (selected && !tdc_selection.KeepRawHitForTot(channel, cursor.tdc)) {
      continue;
    }
    if (!selected && !edge_selected) {
      continue;
    }
    const int hit_spill = cursor.has_spill ? cursor.spill : cursor.current_spill;
    if (!spill_range.Pass(hit_spill)) {
      continue;
    }

    const int tdc_index = analysis_time::TdcIndex(cursor.fifo, cursor.column, cursor.pixel, cursor.tdc);
    if (!analysis_time::PassFineCut(fine_calib, cursor.fine, tdc_index, fine_cut)) {
      continue;
    }

    Hit hit;
    hit.channel = channel;
    hit.spill = hit_spill;
    hit.fifo = cursor.fifo;
    hit.column = cursor.column;
    hit.pixel = cursor.pixel;
    hit.tdc = cursor.tdc;
    hit.fine = cursor.fine;
    hit.time_tick = cursor.has_time_tick ? cursor.time_tick : analysis_time::TimeTick(cursor.rollover, cursor.coarse);
    hit.time_ns_raw = analysis_time::TimeNsFromTick(fine_calib, hit.time_tick, cursor.fine, tdc_index, tick_ns, use_fine);
    hit.time_ns = hit.time_ns_raw - tdc_offset_calib.CorrectionNs(channel, cursor.tdc);
    hit.leading = IsLeadingTdc(cursor.tdc);
    cursor.pending = hit;
    cursor.has_pending = true;
    return true;
  }

  cursor.eof = true;
  return false;
}

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

struct OnlineStats {
  long long n = 0;
  double mean = 0.0;
  double m2 = 0.0;

  void Add(double value)
  {
    if (!std::isfinite(value)) {
      return;
    }
    ++n;
    const double delta = value - mean;
    mean += delta / static_cast<double>(n);
    const double delta2 = value - mean;
    m2 += delta * delta2;
  }

  Stats Get() const
  {
    Stats stats;
    stats.n = n;
    if (n <= 0) {
      return stats;
    }
    stats.mean = mean;
    const double var = m2 / static_cast<double>(n);
    stats.rms = std::sqrt(std::max(0.0, var));
    stats.err_mean = stats.rms / std::sqrt(static_cast<double>(n));
    stats.err_rms = n > 1 ? stats.rms / std::sqrt(2.0 * static_cast<double>(n - 1)) : 0.0;
    return stats;
  }
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

struct SpillBounds {
  bool valid = false;
  int min = 0;
  int max = 0;

  void Add(int spill)
  {
    if (!valid) {
      valid = true;
      min = spill;
      max = spill;
      return;
    }
    min = std::min(min, spill);
    max = std::max(max, spill);
  }
};

SpillBounds FindSelectedSpillBounds(const analysis_io::InputSpec &input,
                                    const std::unordered_set<int> &selected_channels,
                                    const SpillRange &spill_range)
{
  SpillBounds bounds;
  for (const auto &path : input.files) {
    std::unique_ptr<TFile> file(TFile::Open(path.c_str(), "READ"));
    if (!file || file->IsZombie()) {
      std::cerr << "Failed to open decoded ROOT file for spill scan: " << path << std::endl;
      continue;
    }
    TTree *tree = dynamic_cast<TTree *>(file->Get(input.tree_name.c_str()));
    if (!tree) {
      std::cerr << "Missing tree '" << input.tree_name << "' in " << path << std::endl;
      continue;
    }
    if (!HasTreeBranch(tree, "type") || !HasTreeBranch(tree, "column") || !HasTreeBranch(tree, "pixel")) {
      std::cerr << "Skipping spill scan for " << path << " (missing type/column/pixel)" << std::endl;
      continue;
    }

    int type = 0;
    int spill = 0;
    int column = 0;
    int pixel = 0;
    int channel = 0;
    int current_spill = 0;
    const bool has_spill = HasTreeBranch(tree, "spill");
    const bool has_channel = HasTreeBranch(tree, "channel");

    tree->SetBranchStatus("*", 0);
    tree->SetBranchStatus("type", 1);
    tree->SetBranchStatus("column", 1);
    tree->SetBranchStatus("pixel", 1);
    tree->SetBranchAddress("type", &type);
    tree->SetBranchAddress("column", &column);
    tree->SetBranchAddress("pixel", &pixel);
    if (has_spill) {
      tree->SetBranchStatus("spill", 1);
      tree->SetBranchAddress("spill", &spill);
    }
    if (has_channel) {
      tree->SetBranchStatus("channel", 1);
      tree->SetBranchAddress("channel", &channel);
    }

    const Long64_t entries = tree->GetEntries();
    for (Long64_t i = 0; i < entries; ++i) {
      tree->GetEntry(i);
      if (!has_spill && type == 15) {
        ++current_spill;
        continue;
      }
      if (type != 1) {
        continue;
      }
      const int ch = has_channel ? channel : column * 4 + pixel;
      if (selected_channels.find(ch) == selected_channels.end()) {
        continue;
      }
      const int hit_spill = has_spill ? spill : current_spill;
      if (spill_range.Pass(hit_spill)) {
        bounds.Add(hit_spill);
      }
    }
  }
  return bounds;
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
  double baseline = 0.0;
  FitRange fit_range;

  double EvalNs(double tot) const
  {
    if (!std::isfinite(tot)) {
      return 0.0;
    }
    if (model == TimewalkFitModel::LinExpPlateau) {
      if (tot <= p2 || p3 <= 0.0) {
        return p0 + p1 * tot;
      }
      return p4 + (p0 + p1 * p2 - p4) * std::exp(-(tot - p2) / p3);
    }
    if (model == TimewalkFitModel::Pol1Plateau) {
      return p0 + p1 * std::min(tot, p2);
    }
    return p0 + p1 * tot;
  }

  double CorrectionNs(double tot) const
  {
    if (!valid || !std::isfinite(tot)) {
      return 0.0;
    }
    const double value = EvalNs(tot);
    if (!std::isfinite(value)) {
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

std::vector<double> MakeLogBins(int bins, double xmin, double xmax)
{
  bins = std::max(1, bins);
  xmin = std::max(xmin, 1e-6);
  xmax = std::max(xmax, xmin * 10.0);
  std::vector<double> edges(static_cast<size_t>(bins) + 1);
  const double log_min = std::log10(xmin);
  const double log_max = std::log10(xmax);
  for (int i = 0; i <= bins; ++i) {
    const double fraction = static_cast<double>(i) / static_cast<double>(bins);
    edges[static_cast<size_t>(i)] = std::pow(10.0, log_min + fraction * (log_max - log_min));
  }
  return edges;
}

TH1D *MakeLogHist(std::vector<std::unique_ptr<TH1D>> &owner,
                  const std::string &name,
                  const std::string &title,
                  int bins,
                  double xmin,
                  double xmax)
{
  const auto edges = MakeLogBins(bins, xmin, xmax);
  auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), bins, edges.data());
  hist->SetDirectory(nullptr);
  TH1D *ptr = hist.get();
  owner.push_back(std::move(hist));
  return ptr;
}

TH2D *MakeLogXHist2D(std::vector<std::unique_ptr<TH2D>> &owner,
                     const std::string &name,
                     const std::string &title,
                     int xbins,
                     double xmin,
                     double xmax,
                     int ybins,
                     double ymin,
                     double ymax)
{
  const auto xedges = MakeLogBins(xbins, xmin, xmax);
  auto hist = std::make_unique<TH2D>(name.c_str(), title.c_str(), xbins, xedges.data(), ybins, ymin, ymax);
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
  profile->SetTitle((std::string(hist.GetTitle()) + " profile;" + g_tot_axis_label + ";mean #Deltat [ns]").c_str());
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
  profile->SetTitle((std::string(hist.GetTitle()) + " profile;spill;mean " + g_tot_axis_label).c_str());
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
  if (fit_model == TimewalkFitModel::Pol1Plateau) {
    fit = std::make_unique<TF1>((std::string(profile->GetName()) + "_pol1_plateau").c_str(),
                                "x<[2] ? [0]+[1]*x : [0]+[1]*[2]",
                                xmin,
                                xmax);
    auto seed = EstimateLinExpPlateauSeed(profile, xmin, xmax);
    fit->SetParameters(seed.p0, seed.p1, seed.x0);
    fit->SetParNames("p0", "p1", "x0");
    fit->SetParLimits(2, xmin + 0.1, xmax - 0.1);
    // The profile has tiny statistical errors in highly populated bins; equal-bin weights better follow the shape.
    fit_options = "QNRW";
  } else if (fit_model == TimewalkFitModel::LinExpPlateau) {
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

TH1D *FindRunHist1D(const RunResult &result, const std::string &prefix, int channel)
{
  const std::string name = prefix + SafeName(result.config.label) + "_ch" + std::to_string(channel);
  for (const auto &owned : result.histograms) {
    if (std::string(owned->GetName()) == name) {
      return owned.get();
    }
  }
  return nullptr;
}

TH1D *FindRunHist1DByName(const RunResult &result, const std::string &name)
{
  for (const auto &owned : result.histograms) {
    if (owned && std::string(owned->GetName()) == name) {
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
	                               "Accumulated #Deltat vs " + g_tot_quantity_label + " ch" + std::to_string(channel) +
	                                   ";" + g_tot_axis_label + ";#Deltat [ns];entries");
}

std::unique_ptr<TH2D> MakeAccumulatedRawTimewalkHist(const std::vector<RunResult> &results, int channel)
{
  return MakeAccumulatedHist2D(results,
                               channel,
                               "h_raw_dt_vs_tot_",
                               "h_raw_dt_vs_tot_accum_ch" + std::to_string(channel),
                               "Accumulated raw #Deltat vs " + g_tot_quantity_label + " ch" + std::to_string(channel) +
                                   ";" + g_tot_axis_label + ";#Deltat [ns];entries");
}

std::unique_ptr<TH2D> MakeAccumulatedRejectedTimewalkHist(const std::vector<RunResult> &results, int channel)
{
  return MakeAccumulatedHist2D(results,
                               channel,
                               "h_rejected_dt_vs_tot_",
                               "h_rejected_dt_vs_tot_accum_ch" + std::to_string(channel),
                               "Accumulated rejected #Deltat vs " + g_tot_quantity_label + " ch" + std::to_string(channel) +
                                   ";" + g_tot_axis_label + ";#Deltat [ns];entries");
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
    if (fit_model == TimewalkFitModel::Pol1Plateau && fit->GetNpar() >= 3) {
      correction.p2 = fit->GetParameter(2);
      correction.p3 = 0.0;
      correction.p4 = correction.EvalNs(correction.p2);
      correction.baseline = correction.p4;
    } else if (fit_model == TimewalkFitModel::LinExpPlateau && fit->GetNpar() >= 5) {
      correction.p2 = fit->GetParameter(2);
      correction.p3 = fit->GetParameter(3);
      correction.p4 = fit->GetParameter(4);
      correction.baseline = correction.p4;
    } else {
      const double baseline_tot = correction.fit_range.enabled ? correction.fit_range.xmax : accumulated->GetXaxis()->GetXmax();
      correction.baseline = correction.EvalNs(baseline_tot);
    }
    corrections[ch] = correction;
    std::cout << "Timewalk correction ch" << ch << " model=" << TimewalkFitModelName(fit_model) << ": ";
    if (fit_model == TimewalkFitModel::Pol1Plateau) {
      std::cout << "linear p0=" << correction.p0 << " p1=" << correction.p1 << ", x0=" << correction.p2
                << ", plateau=" << correction.p4;
    } else if (fit_model == TimewalkFitModel::LinExpPlateau) {
      std::cout << "linear p0=" << correction.p0 << " p1=" << correction.p1 << ", x0=" << correction.p2
                << ", tau=" << correction.p3 << ", plateau=" << correction.p4;
    } else {
      std::cout << "dt = " << correction.p0 << " + " << correction.p1 << " * " << g_tot_quantity_label;
    }
    if (correction.fit_range.enabled) {
      std::cout << " fitted in " << g_tot_quantity_label << " [" << correction.fit_range.xmin << ", "
                << correction.fit_range.xmax << "] ns";
    }
    std::cout << "; applied correction is f(" << g_tot_quantity_label << "); baseline/plateau=" << correction.baseline
              << " ns" << std::endl;
  }
  return corrections;
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

struct MatchedCorrectedHit {
  double time_ns = 0.0;
  double time_before_timewalk_ns = 0.0;
  double tot_ns = 0.0;
  double dt_to_trigger_ns = 0.0;
};

struct EventMedianCandidate {
  size_t index = 0;
  int channel = -1;
  int tdc = -1;
  double time_ns = 0.0;
};

struct EventMedianMatch {
  size_t index = 0;
  double dt_ns = 0.0;
  int event_id = 0;
};

void CleanTriggerCandidates(std::vector<size_t> &indices,
                            const std::vector<Hit> &hits,
                            double trigger_deadtime_ns,
                            double trigger_period_ns,
                            double trigger_period_tolerance_ns,
                            std::vector<size_t> *after_deadtime_indices = nullptr);

std::vector<EventMedianMatch> BuildEventMedianMatches(const std::vector<Hit> &hits,
                                                      const std::unordered_set<int> &selected_channels,
                                                      const TdcSelection &tdc_selection,
                                                      bool require_valid_tot,
                                                      const std::map<int, TotWindow> &channel_tot_windows,
                                                      int min_channels,
                                                      double event_window_ns);

void BuildCorrectedTimewalkAndCoincidencePlots(std::vector<RunResult> &results,
                                               const std::vector<int> &sensor_channels,
                                               int trigger_channel,
                                               const std::map<int, TimewalkCorrection> &corrections,
                                               double match_window_ns,
                                               double trigger_deadtime_ns,
                                               double trigger_period_ns,
                                               double trigger_period_tolerance_ns,
                                               double max_duration_ns,
                                               double sensor_duration_ns,
                                               double clock_mhz,
                                               const analysis_time::FineCalib &fine_calib,
                                               const analysis_time::ChannelTdcOffsetCalib &tdc_offset_calib,
                                               const analysis_time::ChannelCalib &chan_calib,
                                               TimeReferenceMode reference_mode,
                                               int event_reference_min_channels,
                                               bool use_fine,
                                               int fine_cut,
                                               bool require_valid_tot,
                                               bool signed_dt,
                                               const std::map<int, DtTotCut> &dt_tot_cuts,
                                               const TotWindow &trigger_tot_window,
                                               const SpillRange &spill_range,
                                               const std::map<int, TotWindow> &channel_tot_windows,
                                               const TdcSelection &tdc_selection,
                                               double event_window_ns,
                                               std::vector<std::unique_ptr<TH2D>> &corrected_accumulated_histograms)
{
  if (sensor_channels.empty()) {
    return;
  }

	  const double tot_max = std::max(1.0, sensor_duration_ns);
	  const double corrected_dt_min = TimewalkDtMin(signed_dt, match_window_ns);
	  const double corrected_dt_max = TimewalkDtMax(match_window_ns);
	  std::map<int, TH2D *> h_dt_corr_vs_tot_accum;
	  for (int ch : sensor_channels) {
    const std::string reference_label =
        reference_mode == TimeReferenceMode::EventMedian ? "event median" : "clean trigger";
    std::ostringstream title;
    title << "Accumulated corrected #Deltat vs " << g_tot_quantity_label << " ch" << ch << " to " << reference_label
          << ";" << g_tot_axis_label << ";#Deltat corrected [ns];entries";
    h_dt_corr_vs_tot_accum[ch] = MakeHist2D(corrected_accumulated_histograms,
                                            "h_dt_corr_vs_tot_accum_ch" + std::to_string(ch),
                                            title.str(),
                                            200,
	                                            0.0,
	                                            tot_max,
	                                            400,
	                                            corrected_dt_min,
		                                            corrected_dt_max);
  }

	  for (auto &result : results) {
	    const std::string safe_label = SafeName(result.config.label);
	    TH1D *uncorrected_coincidence_hist = nullptr;
	    TH1D *corrected_coincidence_hist = nullptr;
	    int ch_a = -1;
	    int ch_b = -1;
	    if (sensor_channels.size() >= 2) {
	      ch_a = sensor_channels[0];
	      ch_b = sensor_channels[1];
	      std::ostringstream raw_title;
	      raw_title << result.config.label << " I=" << result.config.intensity << " coincidence before timewalk ch"
	                << ch_a << "-ch" << ch_b << ";t_{" << ch_a << "} - t_{" << ch_b
	                << "} [ns];entries";
	      uncorrected_coincidence_hist =
	          MakeHist(result.histograms,
	                   "h_dt_uncorr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" + safe_label,
	                   raw_title.str(),
	                   240,
	                   kCorrectedCoincidenceDtMinNs,
	                   kCorrectedCoincidenceDtMaxNs);
	      std::ostringstream title;
	      title << result.config.label << " I=" << result.config.intensity << " corrected coincidence ch" << ch_a
	            << "-ch" << ch_b << ";t_{" << ch_a << ",corr} - t_{" << ch_b << ",corr} [ns];entries";
	      corrected_coincidence_hist =
          MakeHist(result.histograms,
                   "h_dt_corr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" + safe_label,
                   title.str(),
                   240,
                   kCorrectedCoincidenceDtMinNs,
                   kCorrectedCoincidenceDtMaxNs);
    }

    auto input = analysis_io::ResolveInputSpec(result.config.input_path);
    if (input.files.empty()) {
      continue;
    }

    std::unordered_set<int> selected_channels(sensor_channels.begin(), sensor_channels.end());
    if (reference_mode == TimeReferenceMode::Trigger) {
      selected_channels.insert(trigger_channel);
    }
    const std::unordered_set<int> no_edge_channels;
    const double tick_ns = analysis_time::TickNs(clock_mhz);

    auto process_spill = [&](std::vector<Hit> &hits) {
      if (hits.empty()) {
        return;
      }
      ComputeTot(hits, max_duration_ns);

	      for (auto &hit : hits) {
	        if (selected_channels.find(hit.channel) == selected_channels.end() || !hit.leading ||
	            !tdc_selection.KeepLeadingHit(hit.channel, hit.tdc)) {
	          continue;
	        }
	        if (chan_calib.loaded && hit.tot_ns > 0.0) {
	          hit.time_ns -= chan_calib.CorrectionNs(hit.channel, hit.tot_ns);
	        }
	      }
	      std::vector<double> time_before_timewalk(hits.size(), std::numeric_limits<double>::quiet_NaN());
	      for (size_t i = 0; i < hits.size(); ++i) {
	        time_before_timewalk[i] = hits[i].time_ns;
	      }

	      if (reference_mode == TimeReferenceMode::EventMedian) {
	        for (size_t i = 0; i < hits.size(); ++i) {
	          auto &hit = hits[i];
	          if (selected_channels.find(hit.channel) == selected_channels.end() || !hit.leading ||
	              !tdc_selection.KeepLeadingHit(hit.channel, hit.tdc) || hit.tot_ns <= 0.0) {
	            continue;
	          }
	          auto correction_it = corrections.find(hit.channel);
	          if (correction_it != corrections.end()) {
	            hit.time_ns -= correction_it->second.CorrectionNs(hit.tot_ns);
	          }
	        }

        auto matches = BuildEventMedianMatches(hits,
                                               selected_channels,
                                               tdc_selection,
                                               require_valid_tot,
                                               channel_tot_windows,
                                               event_reference_min_channels,
                                               event_window_ns);
        std::map<int, std::map<int, std::vector<MatchedCorrectedHit>>> matched_by_event_channel;
        for (const auto &match : matches) {
          const auto &hit = hits[match.index];
          if (hit.tot_ns <= 0.0) {
            continue;
          }
          const TotWindow channel_tot_window = ChannelTotWindowForChannel(channel_tot_windows, hit.channel);
          if (!channel_tot_window.Pass(hit.tot_ns)) {
            continue;
          }
	          if (match.dt_ns >= corrected_dt_min && match.dt_ns <= corrected_dt_max &&
	              h_dt_corr_vs_tot_accum.count(hit.channel) > 0) {
	            h_dt_corr_vs_tot_accum[hit.channel]->Fill(hit.tot_ns, match.dt_ns);
	          }
	          matched_by_event_channel[match.event_id][hit.channel].push_back(
	              {hit.time_ns, time_before_timewalk[match.index], hit.tot_ns, match.dt_ns});
	        }

	        if ((!corrected_coincidence_hist && !uncorrected_coincidence_hist) || ch_a < 0 || ch_b < 0) {
	          return;
	        }
	        for (const auto &event_entry : matched_by_event_channel) {
          const auto &by_channel = event_entry.second;
          auto hits_a_it = by_channel.find(ch_a);
          auto hits_b_it = by_channel.find(ch_b);
          if (hits_a_it == by_channel.end() || hits_b_it == by_channel.end() || hits_a_it->second.empty() ||
              hits_b_it->second.empty()) {
            continue;
          }
          const auto best_hit = [](const std::vector<MatchedCorrectedHit> &hit_list) {
            return std::min_element(hit_list.begin(), hit_list.end(), [](const MatchedCorrectedHit &a,
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
	          if (corrected_coincidence_hist && dt_ab >= kCorrectedCoincidenceDtMinNs &&
	              dt_ab <= kCorrectedCoincidenceDtMaxNs) {
	            corrected_coincidence_hist->Fill(dt_ab);
	          }
	          const double raw_dt_ab = best_a->time_before_timewalk_ns - best_b->time_before_timewalk_ns;
	          if (uncorrected_coincidence_hist && raw_dt_ab >= kCorrectedCoincidenceDtMinNs &&
	              raw_dt_ab <= kCorrectedCoincidenceDtMaxNs) {
	            uncorrected_coincidence_hist->Fill(raw_dt_ab);
	          }
	        }
	        return;
	      }

      std::vector<size_t> trigger_indices;
      std::vector<size_t> sensor_indices;
      for (size_t i = 0; i < hits.size(); ++i) {
        const Hit &hit = hits[i];
        if (selected_channels.find(hit.channel) == selected_channels.end() || !hit.leading ||
            !tdc_selection.KeepLeadingHit(hit.channel, hit.tdc)) {
          continue;
        }
        if (require_valid_tot && hit.tot_ns <= 0.0) {
          continue;
        }
        const TotWindow channel_tot_window = ChannelTotWindowForChannel(channel_tot_windows, hit.channel);
        if (!channel_tot_window.Pass(hit.tot_ns)) {
          continue;
        }
        if (hit.channel == trigger_channel) {
          if (trigger_tot_window.Pass(hit.tot_ns)) {
            trigger_indices.push_back(i);
          }
        } else {
          sensor_indices.push_back(i);
        }
      }

      CleanTriggerCandidates(trigger_indices, hits, trigger_deadtime_ns, trigger_period_ns, trigger_period_tolerance_ns);

      std::vector<StoredSensorHit> triggers;
      triggers.reserve(trigger_indices.size());
      for (size_t idx : trigger_indices) {
        if (hits[idx].tot_ns > 0.0) {
          triggers.push_back({trigger_channel, hits[idx].spill, hits[idx].time_ns, hits[idx].tot_ns});
        }
      }
      if (triggers.empty()) {
        return;
      }

      std::map<size_t, std::map<int, std::vector<MatchedCorrectedHit>>> matched_by_trigger_channel;
      auto events = analysis_events::BuildReferenceEvents(hits, trigger_indices, sensor_indices, event_window_ns, signed_dt);
      for (const auto &event : events) {
        if (event.reference_index >= hits.size()) {
          continue;
        }
        size_t trigger_index = 0;
        const auto trigger_it = std::find(trigger_indices.begin(), trigger_indices.end(), event.reference_index);
        if (trigger_it != trigger_indices.end()) {
          trigger_index = static_cast<size_t>(std::distance(trigger_indices.begin(), trigger_it));
        }
        for (const auto &event_hit : event.hits) {
          if (event_hit.index >= hits.size()) {
            continue;
          }
          const int ch = event_hit.channel;
          if (std::find(sensor_channels.begin(), sensor_channels.end(), ch) == sensor_channels.end()) {
            continue;
          }
          const double raw_dt = event_hit.dt_ns;
          if ((!signed_dt && (raw_dt < 0.0 || raw_dt > match_window_ns)) ||
              (signed_dt && std::abs(raw_dt) > match_window_ns)) {
            continue;
          }
          const size_t sensor_idx = event_hit.index;
          const double sensor_tot = hits[sensor_idx].tot_ns;
          if (sensor_tot <= 0.0) {
            continue;
          }
          const DtTotCut cut = DtTotCutForChannel(dt_tot_cuts, ch);
          if (!cut.Pass(sensor_tot, raw_dt)) {
            continue;
          }

          auto correction_it = corrections.find(ch);
          const double correction_ns =
              correction_it != corrections.end() ? correction_it->second.CorrectionNs(sensor_tot) : 0.0;
	          StoredSensorHit corrected{ch, hits[sensor_idx].spill, hits[sensor_idx].time_ns - correction_ns, sensor_tot};
	          double dt = std::numeric_limits<double>::quiet_NaN();
	          if (!FindMatchedReference(corrected.time_ns, triggers, match_window_ns, signed_dt, trigger_index, dt)) {
	            continue;
	          }
	          matched_by_trigger_channel[trigger_index][ch].push_back(
	              {corrected.time_ns, time_before_timewalk[sensor_idx], corrected.tot_ns, dt});
	          if (dt >= corrected_dt_min && dt <= corrected_dt_max) {
	            h_dt_corr_vs_tot_accum[ch]->Fill(corrected.tot_ns, dt);
	          }
	        }
	      }

	      if ((!corrected_coincidence_hist && !uncorrected_coincidence_hist) || ch_a < 0 || ch_b < 0) {
	        return;
	      }
      for (const auto &trigger_entry : matched_by_trigger_channel) {
        const auto &by_channel = trigger_entry.second;
        auto hits_a_it = by_channel.find(ch_a);
        auto hits_b_it = by_channel.find(ch_b);
        if (hits_a_it == by_channel.end() || hits_b_it == by_channel.end() || hits_a_it->second.empty() ||
            hits_b_it->second.empty()) {
          continue;
        }
        const auto best_hit = [](const std::vector<MatchedCorrectedHit> &hit_list) {
          return std::min_element(hit_list.begin(), hit_list.end(), [](const MatchedCorrectedHit &a,
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
	        if (corrected_coincidence_hist && dt_ab >= kCorrectedCoincidenceDtMinNs &&
	            dt_ab <= kCorrectedCoincidenceDtMaxNs) {
	          corrected_coincidence_hist->Fill(dt_ab);
	        }
	        const double raw_dt_ab = best_a->time_before_timewalk_ns - best_b->time_before_timewalk_ns;
	        if (uncorrected_coincidence_hist && raw_dt_ab >= kCorrectedCoincidenceDtMinNs &&
	            raw_dt_ab <= kCorrectedCoincidenceDtMaxNs) {
	          uncorrected_coincidence_hist->Fill(raw_dt_ab);
	        }
	      }
	    };

    std::vector<TreeCursor> cursors;
    cursors.reserve(input.files.size());
    for (const auto &file : input.files) {
      TreeCursor cursor;
      if (!OpenTreeCursor(file, input.tree_name, cursor)) {
        continue;
      }
      AdvanceCursor(cursor,
                    selected_channels,
                    no_edge_channels,
                    tdc_selection,
                    fine_calib,
                    tdc_offset_calib,
                    tick_ns,
                    use_fine,
                    fine_cut,
                    spill_range);
      cursors.push_back(std::move(cursor));
      // ROOT stores branch addresses as raw pointers; moving the cursor changes those addresses.
      BindTreeCursorBranches(cursors.back());
    }

    while (true) {
      bool found = false;
      int min_pending_spill = 0;
      for (const auto &cursor : cursors) {
        if (!cursor.has_pending) {
          continue;
        }
        if (!found || cursor.pending.spill < min_pending_spill) {
          min_pending_spill = cursor.pending.spill;
          found = true;
        }
      }
      if (!found) {
        break;
      }

      std::vector<Hit> spill_hits;
      for (auto &cursor : cursors) {
        while (cursor.has_pending && cursor.pending.spill == min_pending_spill) {
          spill_hits.push_back(cursor.pending);
          AdvanceCursor(cursor,
                        selected_channels,
                        no_edge_channels,
                        tdc_selection,
                        fine_calib,
                        tdc_offset_calib,
                        tick_ns,
                        use_fine,
                        fine_cut,
                        spill_range);
        }
      }
      process_spill(spill_hits);
    }
  }
}

void CleanTriggerCandidates(std::vector<size_t> &indices,
                            const std::vector<Hit> &hits,
                            double trigger_deadtime_ns,
                            double trigger_period_ns,
                            double trigger_period_tolerance_ns,
                            std::vector<size_t> *after_deadtime_indices)
{
  std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return hits[a].time_ns < hits[b].time_ns; });
  if (indices.size() < 2) {
    if (after_deadtime_indices) {
      *after_deadtime_indices = indices;
    }
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
  if (after_deadtime_indices) {
    *after_deadtime_indices = clustered;
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

void DrawSectionPage(TCanvas &canvas,
                     const std::string &title,
                     const std::string &subtitle,
                     const std::string &out_pdf)
{
  canvas.Clear();
  canvas.SetRightMargin(0.05);
  canvas.SetLeftMargin(0.05);
  auto *pad = static_cast<TPad *>(canvas.cd());
  if (pad) {
    pad->SetLogx(false);
    pad->SetLogy(false);
    pad->SetLogz(false);
  }
  TLatex text;
  text.SetNDC(true);
  text.SetTextAlign(12);
  text.SetTextFont(42);
  text.SetTextSize(0.055);
  text.DrawLatex(0.08, 0.62, title.c_str());
  text.SetTextSize(0.030);
  text.DrawLatex(0.08, 0.52, subtitle.c_str());
  canvas.Print(out_pdf.c_str());
}

double MedianOf(std::vector<double> values)
{
  if (values.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::sort(values.begin(), values.end());
  const size_t mid = values.size() / 2;
  if ((values.size() % 2) == 1) {
    return values[mid];
  }
  return 0.5 * (values[mid - 1] + values[mid]);
}

void ProcessEventMedianCluster(const std::vector<EventMedianCandidate> &cluster,
                               int min_channels,
                               int event_id,
                               std::vector<EventMedianMatch> &matches)
{
  if (cluster.empty()) {
    return;
  }
  std::vector<double> all_times;
  all_times.reserve(cluster.size());
  for (const auto &entry : cluster) {
    all_times.push_back(entry.time_ns);
  }
  const double preliminary_median = MedianOf(all_times);
  if (!std::isfinite(preliminary_median)) {
    return;
  }

  std::array<bool, kNumAlcorChannels> seen{};
  std::array<EventMedianCandidate, kNumAlcorChannels> best_by_channel{};
  for (const auto &entry : cluster) {
    if (entry.channel < 0 || entry.channel >= kNumAlcorChannels) {
      continue;
    }
    if (!seen[entry.channel] ||
        std::abs(entry.time_ns - preliminary_median) <
            std::abs(best_by_channel[entry.channel].time_ns - preliminary_median)) {
      seen[entry.channel] = true;
      best_by_channel[entry.channel] = entry;
    }
  }

  std::vector<EventMedianCandidate> unique_entries;
  std::vector<double> unique_times;
  unique_entries.reserve(cluster.size());
  unique_times.reserve(cluster.size());
  for (int ch = 0; ch < kNumAlcorChannels; ++ch) {
    if (!seen[ch]) {
      continue;
    }
    unique_entries.push_back(best_by_channel[ch]);
    unique_times.push_back(best_by_channel[ch].time_ns);
  }
  if (static_cast<int>(unique_entries.size()) < std::max(1, min_channels)) {
    return;
  }

  for (const auto &entry : unique_entries) {
    std::vector<double> reference_times;
    reference_times.reserve(unique_entries.size());
    for (const auto &other : unique_entries) {
      if (other.channel == entry.channel) {
        continue;
      }
      reference_times.push_back(other.time_ns);
    }
    const double reference = reference_times.empty() ? MedianOf(unique_times) : MedianOf(reference_times);
    if (std::isfinite(reference)) {
      matches.push_back({entry.index, entry.time_ns - reference, event_id});
    }
  }
}

std::vector<EventMedianMatch> BuildEventMedianMatches(const std::vector<Hit> &hits,
                                                      const std::unordered_set<int> &selected_channels,
                                                      const TdcSelection &tdc_selection,
                                                      bool require_valid_tot,
                                                      const std::map<int, TotWindow> &channel_tot_windows,
                                                      int min_channels,
                                                      double event_window_ns)
{
  (void)require_valid_tot;
  (void)channel_tot_windows;
  std::array<std::vector<size_t>, analysis_time::kTdcPerPixel> by_tdc;
  for (size_t i = 0; i < hits.size(); ++i) {
    const auto &hit = hits[i];
    if (selected_channels.find(hit.channel) == selected_channels.end() || !hit.leading ||
        !tdc_selection.KeepLeadingHit(hit.channel, hit.tdc)) {
      continue;
    }
    if (hit.tdc < 0 || hit.tdc >= analysis_time::kTdcPerPixel) {
      continue;
    }
    by_tdc[hit.tdc].push_back(i);
  }

  std::vector<EventMedianMatch> matches;
  int event_id = 0;
  for (const auto &indices : by_tdc) {
    auto events = analysis_events::BuildClusterEvents(hits, indices, event_window_ns, min_channels);
    for (const auto &event : events) {
      const int combined_event_id = event_id++;
      for (const auto &event_hit : event.hits) {
        matches.push_back({event_hit.index, event_hit.dt_ns, combined_event_id});
      }
    }
  }
  return matches;
}

RunResult AnalyzeRun(const RunConfig &run,
                     const std::vector<int> &sensor_channels,
                     int trigger_channel,
                     const analysis_time::FineCalib &fine_calib,
                     const analysis_time::ChannelTdcOffsetCalib &tdc_offset_calib,
                     const analysis_time::ChannelCalib &chan_calib,
                     TimeReferenceMode reference_mode,
                     int event_reference_min_channels,
                     double match_window_ns,
                     double event_window_ns,
                     double trigger_deadtime_ns,
                     double trigger_period_ns,
                     double trigger_period_tolerance_ns,
                     double max_duration_ns,
                     double sensor_duration_ns,
                     double clock_mhz,
                     bool use_fine,
                     int fine_cut,
                     bool require_valid_tot,
                     bool signed_dt,
                     const std::map<int, DtTotCut> &dt_tot_cuts,
                     const TotWindow &trigger_tot_window,
                     const SpillRange &spill_range,
                     const std::map<int, TotWindow> &channel_tot_windows,
                     const TdcSelection &tdc_selection,
                     int edge_spill,
                     const std::vector<int> &edge_channels,
                     double edge_phase_period_ns,
                     double edge_spill_fraction)
{
  RunResult result;
  result.config = run;

  std::unordered_set<int> selected_channels(sensor_channels.begin(), sensor_channels.end());
  if (reference_mode == TimeReferenceMode::Trigger) {
    selected_channels.insert(trigger_channel);
  }
  std::unordered_set<int> edge_channel_set(edge_channels.begin(), edge_channels.end());
  int selected_edge_spill = edge_spill;

  const std::string safe_label = SafeName(run.label);
  const double timewalk_dt_min = TimewalkDtMin(signed_dt, match_window_ns);
  const double timewalk_dt_max = TimewalkDtMax(match_window_ns);
  std::map<int, TH1D *> h_dt;
  std::map<int, TH1D *> h_tot;
  std::map<int, TH1D *> h_tot_no_selection;
  std::map<int, TH1D *> h_tot_full_selection;
  std::map<int, TH2D *> h_dt_vs_tot;
  std::map<int, TH2D *> h_raw_dt_vs_tot;
  std::map<int, TH2D *> h_rejected_dt_vs_tot;
  std::map<int, TH2D *> h_tot_vs_spill;
  std::map<int, TH2D *> h_tot_vs_spill_selected;
  std::map<int, TH2D *> h_tot_vs_spill_rejected;
  std::map<int, TH2D *> h_dt_vs_spill;
  std::map<int, TH2D *> h_raw_dt_vs_spill;
  std::map<int, TH2D *> h_rejected_dt_vs_spill;
  std::map<int, TH1D *> h_interhit_leading;
  std::map<int, TH1D *> h_interhit_full_selection;
  std::map<int, TH2D *> h_duration_vs_prev_interhit;
  const std::string dt_axis_title = DtAxisTitle(reference_mode, trigger_channel);
  for (int ch : sensor_channels) {
    const std::string reference_label =
        reference_mode == TimeReferenceMode::EventMedian ? "event median" : "clean trigger ch" + std::to_string(trigger_channel);
    std::ostringstream title;
    title << "[FULL SELECTION] " << run.label << " I=" << run.intensity << " ch" << ch
          << (reference_mode == TimeReferenceMode::EventMedian
	                  ? " - event median"
	                  : (signed_dt ? " - nearest clean trigger ch" : " - previous clean trigger ch") +
	                        std::to_string(trigger_channel))
	          << ";" << dt_axis_title << ";entries";
    h_dt[ch] = MakeHist(result.histograms,
                        "h_dt_" + safe_label + "_ch" + std::to_string(ch),
                        title.str(),
                        400,
                        signed_dt ? -match_window_ns : 0.0,
                        match_window_ns);

    std::ostringstream corr_title;
    corr_title << "[FULL SELECTION] " << run.label << " I=" << run.intensity << " ch" << ch
               << " #Deltat vs " << g_tot_quantity_label << " to "
               << reference_label << ";" << g_tot_axis_label << ";#Deltat [ns];entries";
    std::ostringstream raw_corr_title;
    raw_corr_title << "[CUT DIAGNOSTIC: before dt/ToT cut] " << run.label << " I=" << run.intensity
                   << " ch" << ch << " #Deltat vs " << g_tot_quantity_label << " to "
                   << reference_label << ";" << g_tot_axis_label << ";#Deltat [ns];entries";
    std::ostringstream rejected_corr_title;
    rejected_corr_title << "[CUT DIAGNOSTIC: rejected by dt/ToT cut] " << run.label << " I="
                        << run.intensity << " ch" << ch
                        << " rejected by #Deltat-" << g_tot_quantity_label << " cut;" << g_tot_axis_label << ";"
                        << dt_axis_title << ";entries";
    const double sensor_tot_xmax = std::max(1.0, sensor_duration_ns);
    h_raw_dt_vs_tot[ch] = MakeHist2D(result.histograms2d,
                                     "h_raw_dt_vs_tot_" + safe_label + "_ch" + std::to_string(ch),
                                     raw_corr_title.str(),
                                     200,
                                     0.0,
                                     sensor_tot_xmax,
                                     400,
                                     timewalk_dt_min,
                                     timewalk_dt_max);
    h_dt_vs_tot[ch] = MakeHist2D(result.histograms2d,
                                 "h_dt_vs_tot_" + safe_label + "_ch" + std::to_string(ch),
                                 corr_title.str(),
                                 200,
                                 0.0,
                                 sensor_tot_xmax,
                                 400,
                                 timewalk_dt_min,
                                 timewalk_dt_max);
    h_rejected_dt_vs_tot[ch] = MakeHist2D(result.histograms2d,
                                          "h_rejected_dt_vs_tot_" + safe_label + "_ch" + std::to_string(ch),
                                          rejected_corr_title.str(),
                                          200,
                                          0.0,
                                          sensor_tot_xmax,
                                          400,
                                          timewalk_dt_min,
                                          timewalk_dt_max);
  }
  std::vector<int> tot_channels = sensor_channels;
  if (reference_mode == TimeReferenceMode::Trigger) {
    tot_channels.push_back(trigger_channel);
  }
  std::sort(tot_channels.begin(), tot_channels.end());
  tot_channels.erase(std::unique(tot_channels.begin(), tot_channels.end()), tot_channels.end());
  for (int ch : tot_channels) {
    std::ostringstream title;
    title << run.label << " I=" << run.intensity << " " << g_tot_quantity_label << " ch" << ch << ";"
          << g_tot_axis_label << ";entries";
    h_tot[ch] = MakeHist(result.histograms,
                         "h_tot_" + safe_label + "_ch" + std::to_string(ch),
                         title.str(),
                         200,
                         0.0,
                         std::max(1.0, max_duration_ns));
  }

  std::vector<int> timing_diagnostic_channels = edge_channels;
  for (int ch : tot_channels) {
    timing_diagnostic_channels.push_back(ch);
  }
  std::sort(timing_diagnostic_channels.begin(), timing_diagnostic_channels.end());
  timing_diagnostic_channels.erase(std::unique(timing_diagnostic_channels.begin(), timing_diagnostic_channels.end()),
                                   timing_diagnostic_channels.end());
  const double interhit_min_ns = 0.1;
  const double interhit_max_ns =
      std::max(1.0e9, trigger_period_ns > 0.0 ? 10.0 * trigger_period_ns : 0.0);
  constexpr int interhit_bins = 160;
  constexpr int duration_bins = 120;
  for (int ch : timing_diagnostic_channels) {
    std::ostringstream tot_no_selection_title;
    tot_no_selection_title << "[NO ANALYSIS CUTS] " << run.label << " I=" << run.intensity << " "
                           << g_tot_quantity_label << " ch" << ch << ";" << g_tot_axis_label << ";entries";
    h_tot_no_selection[ch] = MakeHist(result.histograms,
                                      "h_tot_no_selection_" + safe_label + "_ch" + std::to_string(ch),
                                      tot_no_selection_title.str(),
                                      200,
                                      0.0,
                                      std::max(1.0, max_duration_ns));

    std::ostringstream interhit_title;
    interhit_title << "[NO ANALYSIS CUTS] " << run.label << " I=" << run.intensity
                   << " consecutive leading hits ch" << ch
                   << ";t_{i} - t_{i-1} [ns];entries";
    h_interhit_leading[ch] = MakeLogHist(result.histograms,
                                         "h_interhit_leading_" + safe_label + "_ch" + std::to_string(ch),
                                         interhit_title.str(),
                                         interhit_bins,
                                         interhit_min_ns,
                                         interhit_max_ns);

    std::ostringstream duration_title;
    duration_title << "[NO ANALYSIS CUTS] " << run.label << " I=" << run.intensity
                   << " hit duration vs previous leading hit ch" << ch
                   << ";t_{i} - t_{i-1} [ns];" << g_tot_axis_label << ";entries";
    h_duration_vs_prev_interhit[ch] = MakeLogXHist2D(result.histograms2d,
                                                    "h_duration_vs_prev_interhit_" + safe_label + "_ch" +
                                                        std::to_string(ch),
                                                    duration_title.str(),
                                                    interhit_bins,
                                                    interhit_min_ns,
                                                    interhit_max_ns,
                                                    duration_bins,
                                                    0.0,
                                                    std::max(1.0, max_duration_ns));
  }
  for (int ch : tot_channels) {
    std::ostringstream tot_full_selection_title;
    tot_full_selection_title << "[FULL SELECTION] " << run.label << " I=" << run.intensity << " "
                             << g_tot_quantity_label << " ch" << ch << ";" << g_tot_axis_label << ";entries";
    h_tot_full_selection[ch] = MakeHist(result.histograms,
                                        "h_tot_full_selection_" + safe_label + "_ch" + std::to_string(ch),
                                        tot_full_selection_title.str(),
                                        200,
                                        0.0,
                                        std::max(1.0, max_duration_ns));
    std::ostringstream interhit_full_title;
    interhit_full_title << "[FULL SELECTION] " << run.label << " I=" << run.intensity
                        << " consecutive accepted leading hits ch" << ch
                        << ";t_{i} - t_{i-1} [ns];entries";
    h_interhit_full_selection[ch] = MakeLogHist(result.histograms,
                                                "h_interhit_full_selection_" + safe_label + "_ch" +
                                                    std::to_string(ch),
                                                interhit_full_title.str(),
                                                interhit_bins,
                                                interhit_min_ns,
                                                interhit_max_ns);
  }
  TH1D *h_trigger_candidate_interhit = MakeLogHist(result.histograms,
                                                   "h_trigger_candidate_interhit_" + safe_label + "_ch" +
                                                       std::to_string(trigger_channel),
                                                   "[CUT DIAGNOSTIC] " + run.label + " trigger candidates ch" +
                                                       std::to_string(trigger_channel) +
                                                       ";t_{i} - t_{i-1} [ns];entries",
                                                   interhit_bins,
                                                   interhit_min_ns,
                                                   interhit_max_ns);
  TH1D *h_trigger_after_veto_interhit = MakeLogHist(result.histograms,
                                                    "h_trigger_after_veto_interhit_" + safe_label + "_ch" +
                                                        std::to_string(trigger_channel),
                                                    "[CUT DIAGNOSTIC] " + run.label + " trigger after veto ch" +
                                                        std::to_string(trigger_channel) +
                                                        ";t_{i} - t_{i-1} [ns];entries",
                                                    interhit_bins,
                                                    interhit_min_ns,
                                                    interhit_max_ns);
  TH1D *h_trigger_clean_interhit = MakeLogHist(result.histograms,
                                               "h_trigger_clean_interhit_" + safe_label + "_ch" +
                                                   std::to_string(trigger_channel),
                                               "[CUT DIAGNOSTIC] " + run.label + " clean trigger ch" +
                                                   std::to_string(trigger_channel) +
                                                   ";t_{i} - t_{i-1} [ns];entries",
                                               interhit_bins,
                                               interhit_min_ns,
                                               interhit_max_ns);

  auto input = analysis_io::ResolveInputSpec(run.input_path);
  if (input.files.empty()) {
    std::cerr << "No decoded ROOT input found for " << run.label << " at " << run.input_path << std::endl;
    return result;
  }

  const SpillBounds spill_bounds = FindSelectedSpillBounds(input, selected_channels, spill_range);
  const int min_spill = spill_bounds.valid ? spill_bounds.min : 0;
  const int max_spill = spill_bounds.valid ? spill_bounds.max : 0;
  const int spill_span = std::max(1, max_spill - min_spill + 1);
  const int spill_bins = std::min(2000, spill_span);
  const double spill_xmin = static_cast<double>(min_spill) - 0.5;
  const double spill_xmax = static_cast<double>(max_spill) + 0.5;
  for (int ch : tot_channels) {
    std::ostringstream title;
    title << "[CUT DIAGNOSTIC: before final matching] " << run.label << " I=" << run.intensity
          << " " << g_tot_quantity_label << " vs spill ch" << ch << ";spill;" << g_tot_axis_label << ";entries";
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
    selected_title << "[FULL SELECTION] " << run.label << " I=" << run.intensity
                   << " selected " << g_tot_quantity_label << " vs spill ch" << ch
                   << ";spill;" << g_tot_axis_label << ";entries";
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
    rejected_title << "[CUT DIAGNOSTIC: rejected by dt/ToT cut] " << run.label << " I=" << run.intensity
                   << " rejected " << g_tot_quantity_label << " vs spill ch" << ch
                   << ";spill;" << g_tot_axis_label << ";entries";
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
    raw_dt_title << "[CUT DIAGNOSTIC: before dt/ToT cut] " << run.label << " I=" << run.intensity
                 << " raw #Deltat vs spill ch" << ch
                 << ";spill;" << dt_axis_title << ";entries";
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
    selected_dt_title << "[FULL SELECTION] " << run.label << " I=" << run.intensity
                      << " selected #Deltat vs spill ch" << ch
                      << ";spill;" << dt_axis_title << ";entries";
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
    rejected_dt_title << "[CUT DIAGNOSTIC: rejected by dt/ToT cut] " << run.label << " I=" << run.intensity
                      << " rejected #Deltat vs spill ch" << ch
                      << ";spill;" << dt_axis_title << ";entries";
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

  std::map<int, OnlineStats> dt_stats;
  std::map<int, OnlineStats> tot_stats;
  OnlineStats clean_trigger_period_stats;
  std::vector<EdgeHit> edge_hits;
  const double tick_ns = analysis_time::TickNs(clock_mhz);

  auto process_spill = [&](std::vector<Hit> &hits) {
    if (hits.empty()) {
      return;
    }

    ComputeTot(hits, max_duration_ns);
    std::map<int, std::vector<double>> full_selection_times_by_channel;
    auto fill_full_selection_interhits = [&]() {
      for (auto &kv : full_selection_times_by_channel) {
        TH1D *hist = h_interhit_full_selection.count(kv.first) ? h_interhit_full_selection[kv.first] : nullptr;
        if (!hist) {
          continue;
        }
        auto &times = kv.second;
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        for (size_t i = 1; i < times.size(); ++i) {
          const double dt_prev_ns = times[i] - times[i - 1];
          if (dt_prev_ns > 0.0 && std::isfinite(dt_prev_ns)) {
            hist->Fill(dt_prev_ns);
          }
        }
      }
    };

    if (!edge_channel_set.empty()) {
      for (const auto &hit : hits) {
        const bool is_edge_channel = edge_channel_set.find(hit.channel) != edge_channel_set.end();
        if (!is_edge_channel) {
          continue;
        }
        if (selected_edge_spill < 0) {
          selected_edge_spill = hit.spill;
        }
        if (hit.spill == selected_edge_spill) {
          edge_hits.push_back({hit.channel, hit.spill, hit.leading, hit.time_ns});
        }
      }
    }

    std::map<int, std::vector<const Hit *>> leading_by_channel;
    for (const auto &hit : hits) {
      if (!hit.leading || !tdc_selection.KeepLeadingHit(hit.channel, hit.tdc)) {
        continue;
      }
      if (hit.tot_ns > 0.0 && h_tot_no_selection.count(hit.channel) > 0) {
        h_tot_no_selection[hit.channel]->Fill(hit.tot_ns);
      }
      if (h_interhit_leading.count(hit.channel) == 0 && h_duration_vs_prev_interhit.count(hit.channel) == 0) {
        continue;
      }
      leading_by_channel[hit.channel].push_back(&hit);
    }
    for (auto &kv : leading_by_channel) {
      auto &channel_hits = kv.second;
      std::sort(channel_hits.begin(), channel_hits.end(), [](const Hit *a, const Hit *b) {
        return a->time_ns < b->time_ns;
      });
      TH1D *interhit_hist = h_interhit_leading.count(kv.first) ? h_interhit_leading[kv.first] : nullptr;
      TH2D *duration_hist =
          h_duration_vs_prev_interhit.count(kv.first) ? h_duration_vs_prev_interhit[kv.first] : nullptr;
      for (size_t i = 1; i < channel_hits.size(); ++i) {
        const Hit *previous = channel_hits[i - 1];
        const Hit *current = channel_hits[i];
        const double dt_prev_ns = current->time_ns - previous->time_ns;
        if (dt_prev_ns <= 0.0 || !std::isfinite(dt_prev_ns)) {
          continue;
        }
        if (interhit_hist) {
          interhit_hist->Fill(dt_prev_ns);
        }
        if (duration_hist && current->tot_ns > 0.0) {
          duration_hist->Fill(dt_prev_ns, current->tot_ns);
        }
      }
    }

    for (auto &hit : hits) {
      if (selected_channels.find(hit.channel) == selected_channels.end()) {
        continue;
      }
      ++result.hits_read;
      if (!hit.leading) {
        continue;
      }
      if (!tdc_selection.KeepLeadingHit(hit.channel, hit.tdc)) {
        continue;
      }
      ++result.leading_hits;
      if (chan_calib.loaded && hit.tot_ns > 0.0) {
        hit.time_ns -= chan_calib.CorrectionNs(hit.channel, hit.tot_ns);
      }
    }

    if (reference_mode == TimeReferenceMode::EventMedian) {
      for (const auto &hit : hits) {
        if (selected_channels.find(hit.channel) == selected_channels.end() || !hit.leading ||
            !tdc_selection.KeepLeadingHit(hit.channel, hit.tdc)) {
          continue;
        }
        if (hit.tot_ns > 0.0) {
          tot_stats[hit.channel].Add(hit.tot_ns);
          h_tot[hit.channel]->Fill(hit.tot_ns);
          if (h_tot_vs_spill.count(hit.channel) > 0) {
            h_tot_vs_spill[hit.channel]->Fill(hit.spill, hit.tot_ns);
          }
        }
      }

      auto matches = BuildEventMedianMatches(hits,
                                             selected_channels,
                                             tdc_selection,
                                             require_valid_tot,
                                             channel_tot_windows,
                                             event_reference_min_channels,
                                             event_window_ns);
      for (const auto &match : matches) {
        const auto &hit = hits[match.index];
        const int ch = hit.channel;
        const double sensor_tot = hit.tot_ns;
        const bool has_valid_sensor_tot = sensor_tot > 0.0;
        if (require_valid_tot && !has_valid_sensor_tot) {
          continue;
        }
        const TotWindow channel_tot_window = ChannelTotWindowForChannel(channel_tot_windows, ch);
        if (!channel_tot_window.Pass(sensor_tot)) {
          continue;
        }
        const double dt = match.dt_ns;
        const bool in_timewalk_range = dt >= timewalk_dt_min && dt <= timewalk_dt_max;
        if (has_valid_sensor_tot && h_raw_dt_vs_tot.count(ch) > 0 && in_timewalk_range) {
          h_raw_dt_vs_tot[ch]->Fill(sensor_tot, dt);
        }
        if (h_raw_dt_vs_spill.count(ch) > 0 && in_timewalk_range) {
          h_raw_dt_vs_spill[ch]->Fill(hit.spill, dt);
        }

        const DtTotCut cut = DtTotCutForChannel(dt_tot_cuts, ch);
        const bool pass_dt_tot_cut = !has_valid_sensor_tot || cut.Pass(sensor_tot, dt);
        if (!pass_dt_tot_cut) {
          if (has_valid_sensor_tot && h_rejected_dt_vs_tot.count(ch) > 0 && in_timewalk_range) {
            h_rejected_dt_vs_tot[ch]->Fill(sensor_tot, dt);
          }
          if (h_rejected_dt_vs_spill.count(ch) > 0 && in_timewalk_range) {
            h_rejected_dt_vs_spill[ch]->Fill(hit.spill, dt);
          }
          if (has_valid_sensor_tot && h_tot_vs_spill_rejected.count(ch) > 0) {
            h_tot_vs_spill_rejected[ch]->Fill(hit.spill, sensor_tot);
          }
          continue;
        }

	        dt_stats[ch].Add(dt);
	        h_dt[ch]->Fill(dt);
	        full_selection_times_by_channel[ch].push_back(hit.time_ns);
	        if (has_valid_sensor_tot && h_tot_full_selection.count(ch) > 0) {
	          h_tot_full_selection[ch]->Fill(sensor_tot);
	        }
        if (has_valid_sensor_tot && h_dt_vs_tot.count(ch) > 0 && in_timewalk_range) {
          h_dt_vs_tot[ch]->Fill(sensor_tot, dt);
        }
        if (h_dt_vs_spill.count(ch) > 0 && in_timewalk_range) {
          h_dt_vs_spill[ch]->Fill(hit.spill, dt);
        }
	        if (has_valid_sensor_tot && h_tot_vs_spill_selected.count(ch) > 0) {
	          h_tot_vs_spill_selected[ch]->Fill(hit.spill, sensor_tot);
	        }
	      }
      fill_full_selection_interhits();
      return;
    }

    std::vector<size_t> trigger_indices;
    std::vector<size_t> sensor_indices;
    for (size_t i = 0; i < hits.size(); ++i) {
      const Hit &hit = hits[i];
      if (selected_channels.find(hit.channel) == selected_channels.end() || !hit.leading ||
          !tdc_selection.KeepLeadingHit(hit.channel, hit.tdc)) {
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
        trigger_indices.push_back(i);
        continue;
      }

      sensor_indices.push_back(i);
      if (hit.tot_ns > 0.0) {
        tot_stats[hit.channel].Add(hit.tot_ns);
        h_tot[hit.channel]->Fill(hit.tot_ns);
        if (h_tot_vs_spill.count(hit.channel) > 0) {
          h_tot_vs_spill[hit.channel]->Fill(hit.spill, hit.tot_ns);
        }
      }
    }

    // The cleanup is intentionally applied only to the laser trigger channel.
    // SiPM channels are not cleaned here; they are only matched to the cleaned trigger sequence below.
    auto fill_trigger_interhit = [&](TH1D *hist, const std::vector<size_t> &indices) {
      if (!hist || indices.size() < 2) {
        return;
      }
      std::vector<size_t> sorted_indices = indices;
      std::sort(sorted_indices.begin(), sorted_indices.end(), [&](size_t a, size_t b) {
        return hits[a].time_ns < hits[b].time_ns;
      });
      for (size_t i = 1; i < sorted_indices.size(); ++i) {
        const double dt_prev_ns = hits[sorted_indices[i]].time_ns - hits[sorted_indices[i - 1]].time_ns;
        if (dt_prev_ns > 0.0 && std::isfinite(dt_prev_ns)) {
          hist->Fill(dt_prev_ns);
        }
      }
    };
    fill_trigger_interhit(h_trigger_candidate_interhit, trigger_indices);
    std::vector<size_t> trigger_after_veto_indices;
    CleanTriggerCandidates(trigger_indices,
                           hits,
                           trigger_deadtime_ns,
                           trigger_period_ns,
                           trigger_period_tolerance_ns,
                           &trigger_after_veto_indices);
    fill_trigger_interhit(h_trigger_after_veto_interhit, trigger_after_veto_indices);
    fill_trigger_interhit(h_trigger_clean_interhit, trigger_indices);
    result.trigger22_clean_candidates += static_cast<long long>(trigger_indices.size());

    for (size_t i = 1; i < trigger_indices.size(); ++i) {
      const double spacing = hits[trigger_indices[i]].time_ns - hits[trigger_indices[i - 1]].time_ns;
      clean_trigger_period_stats.Add(spacing);
    }

    for (size_t idx : trigger_indices) {
      full_selection_times_by_channel[trigger_channel].push_back(hits[idx].time_ns);
      if (hits[idx].tot_ns > 0.0) {
        tot_stats[trigger_channel].Add(hits[idx].tot_ns);
        h_tot[trigger_channel]->Fill(hits[idx].tot_ns);
        if (h_tot_full_selection.count(trigger_channel) > 0) {
          h_tot_full_selection[trigger_channel]->Fill(hits[idx].tot_ns);
        }
        if (h_tot_vs_spill.count(trigger_channel) > 0) {
          h_tot_vs_spill[trigger_channel]->Fill(hits[idx].spill, hits[idx].tot_ns);
        }
      }
    }

    auto events = analysis_events::BuildReferenceEvents(hits, trigger_indices, sensor_indices, event_window_ns, signed_dt);
    for (const auto &event : events) {
      if (event.reference_index >= hits.size()) {
        continue;
      }
      for (const auto &event_hit : event.hits) {
        if (event_hit.index >= hits.size()) {
          continue;
        }
        const int ch = event_hit.channel;
        if (std::find(sensor_channels.begin(), sensor_channels.end(), ch) == sensor_channels.end()) {
          continue;
        }
        const double dt = event_hit.dt_ns;
        if ((!signed_dt && (dt < 0.0 || dt > match_window_ns)) ||
            (signed_dt && std::abs(dt) > match_window_ns)) {
          continue;
        }
        const size_t sensor_idx = event_hit.index;
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

        dt_stats[ch].Add(dt);
        h_dt[ch]->Fill(dt);
        full_selection_times_by_channel[ch].push_back(hits[sensor_idx].time_ns);
        if (has_valid_sensor_tot && h_tot_full_selection.count(ch) > 0) {
          h_tot_full_selection[ch]->Fill(sensor_tot);
        }
        if (has_valid_sensor_tot && h_dt_vs_tot.count(ch) > 0 && in_timewalk_range) {
          h_dt_vs_tot[ch]->Fill(sensor_tot, dt);
        }
        if (h_dt_vs_spill.count(ch) > 0 && in_timewalk_range) {
          h_dt_vs_spill[ch]->Fill(hits[sensor_idx].spill, dt);
        }
        if (has_valid_sensor_tot && h_tot_vs_spill_selected.count(ch) > 0) {
          h_tot_vs_spill_selected[ch]->Fill(hits[sensor_idx].spill, sensor_tot);
        }
      }
    }
    fill_full_selection_interhits();
  };

  std::vector<TreeCursor> cursors;
  cursors.reserve(input.files.size());
  for (const auto &file : input.files) {
    TreeCursor cursor;
    if (!OpenTreeCursor(file, input.tree_name, cursor)) {
      continue;
    }
    AdvanceCursor(cursor,
                  selected_channels,
                  edge_channel_set,
                  tdc_selection,
                  fine_calib,
                  tdc_offset_calib,
                  tick_ns,
                  use_fine,
                  fine_cut,
                  spill_range);
    cursors.push_back(std::move(cursor));
    // ROOT stores branch addresses as raw pointers; moving the cursor changes those addresses.
    BindTreeCursorBranches(cursors.back());
  }

  while (true) {
    bool found = false;
    int min_pending_spill = 0;
    for (const auto &cursor : cursors) {
      if (!cursor.has_pending) {
        continue;
      }
      if (!found || cursor.pending.spill < min_pending_spill) {
        min_pending_spill = cursor.pending.spill;
        found = true;
      }
    }
    if (!found) {
      break;
    }

    std::vector<Hit> spill_hits;
    for (auto &cursor : cursors) {
      while (cursor.has_pending && cursor.pending.spill == min_pending_spill) {
        spill_hits.push_back(cursor.pending);
        AdvanceCursor(cursor,
                      selected_channels,
                      edge_channel_set,
                      tdc_selection,
                      fine_calib,
                      tdc_offset_calib,
                      tick_ns,
                      use_fine,
                      fine_cut,
                      spill_range);
      }
    }
    process_spill(spill_hits);
  }

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

  result.trigger22_clean_period = clean_trigger_period_stats.Get();
  for (int ch : sensor_channels) {
    result.channels[ch].dt = dt_stats[ch].Get();
    result.channels[ch].tot = tot_stats[ch].Get();
  }
  result.channels[trigger_channel].tot = tot_stats[trigger_channel].Get();
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
	                       const std::vector<int> &edge_channels,
		                       TimeReferenceMode reference_mode,
		                       const std::map<int, FitRange> &fit_ranges,
		                       const std::map<int, DtTotCut> &dt_tot_cuts,
		                       TimewalkFitModel fit_model,
                         RunPlotGroup plot_group,
		                       const std::string &out_pdf)
{
  const bool draw_results = plot_group == RunPlotGroup::Results;
  const bool draw_diagnostics = plot_group == RunPlotGroup::Diagnostics;
  const std::string dt_axis_title = DtAxisTitle(reference_mode, trigger_channel);
  auto set_linear_2d_pad = []() {
    if (auto *pad = gPad) {
      pad->SetLogx(false);
      pad->SetLogy(false);
      pad->SetLogz(false);
      pad->SetRightMargin(0.14);
    }
  };
  auto set_linear_profile_pad = []() {
    if (auto *pad = gPad) {
      pad->SetLogx(false);
      pad->SetLogy(false);
      pad->SetLogz(false);
      pad->SetRightMargin(0.05);
    }
  };
  auto set_profile_y_range = [](TProfile *profile, const TH2D &hist) {
    if (!profile || !hist.GetYaxis()) {
      return;
    }
    profile->SetMinimum(hist.GetYaxis()->GetXmin());
    profile->SetMaximum(hist.GetYaxis()->GetXmax());
  };
  // Like set_profile_y_range, but for profiles whose Y values are a ToT/slew-dt
  // quantity: in slew-rate mode the booked axis range is tuned for ToT pulse
  // widths and is typically far wider than the actual rise-time-scale data, so
  // zoom to the profile's populated content range instead of the full axis.
  auto set_tot_profile_y_range = [](TProfile *profile, const TH2D &hist) {
    if (!profile || !hist.GetYaxis()) {
      return;
    }
    if (g_tdc_operating_mode != TdcOperatingMode::SlewRate) {
      profile->SetMinimum(hist.GetYaxis()->GetXmin());
      profile->SetMaximum(hist.GetYaxis()->GetXmax());
      return;
    }
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (int b = 1; b <= profile->GetNbinsX(); ++b) {
      if (profile->GetBinEntries(b) <= 0.0) {
        continue;
      }
      const double v = profile->GetBinContent(b);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) {
      profile->SetMinimum(hist.GetYaxis()->GetXmin());
      profile->SetMaximum(hist.GetYaxis()->GetXmax());
      return;
    }
    const double pad = std::max(0.15 * (hi - lo), 0.05);
    profile->SetMinimum(std::max(hist.GetYaxis()->GetXmin(), lo - pad));
    profile->SetMaximum(std::min(hist.GetYaxis()->GetXmax(), hi + pad));
  };
  if (draw_results) {
    DrawSectionPage(
        canvas,
        "FULL SELECTION",
        "Plots used as timewalk inputs after the configured trigger/reference, ToT, TDC, spill, and dt/ToT selections.",
        out_pdf);

    canvas.Clear();
    canvas.SetRightMargin(0.05);
    auto *legend_dt = new TLegend(0.72, 0.72, 0.92, 0.90);
    legend_dt->SetBit(kCanDelete);
    legend_dt->SetBorderSize(0);
    legend_dt->SetFillStyle(0);
    auto *stack_dt = new THStack(("stack_dt_" + SafeName(result.config.label)).c_str(),
                                 ("[FULL SELECTION] " + result.config.label + " I=" +
                                  std::to_string(result.config.intensity) + " time difference;" + dt_axis_title +
                                  ";entries")
                                     .c_str());
    stack_dt->SetBit(kCanDelete);
    double max_dt = 0.0;
    for (size_t i = 0; i < sensor_channels.size(); ++i) {
      const std::string name =
          "h_dt_" + SafeName(result.config.label) + "_ch" + std::to_string(sensor_channels[i]);
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
  }

  std::vector<int> tot_channels = sensor_channels;
  tot_channels.push_back(trigger_channel);
  std::sort(tot_channels.begin(), tot_channels.end());
  tot_channels.erase(std::unique(tot_channels.begin(), tot_channels.end()), tot_channels.end());

  if (draw_results) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    auto *legend_tot = new TLegend(0.72, 0.72, 0.92, 0.90);
    legend_tot->SetBit(kCanDelete);
    legend_tot->SetBorderSize(0);
    legend_tot->SetFillStyle(0);
    auto *stack_tot = new THStack(("stack_tot_" + SafeName(result.config.label)).c_str(),
                                  ("[FULL SELECTION] " + result.config.label + " I=" +
                                   std::to_string(result.config.intensity) + " " + g_tot_quantity_label + ";" +
                                   g_tot_axis_label + ";entries")
                                      .c_str());
    stack_tot->SetBit(kCanDelete);
    double max_tot = 0.0;
    for (size_t i = 0; i < tot_channels.size(); ++i) {
      const std::string name =
          "h_tot_full_selection_" + SafeName(result.config.label) + "_ch" + std::to_string(tot_channels[i]);
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
  }

  auto draw_timewalk_2d_group = [&](const std::string &prefix,
                                    const std::string &label,
                                    bool draw_profile_and_fit,
                                    bool draw_cut_line) {
	    canvas.Clear();
	    canvas.SetRightMargin(0.05);
	    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
	    bool has_group = false;
	    for (size_t i = 0; i < sensor_channels.size(); ++i) {
	      const int ch = sensor_channels[i];
	      TH2D *hist = FindRunHist2D(result, prefix, ch);
	      canvas.cd(static_cast<int>(i + 1));
      set_linear_2d_pad();
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      has_group = true;
      auto *draw_hist = static_cast<TH2D *>(hist->Clone());
      draw_hist->SetDirectory(nullptr);
      draw_hist->SetBit(kCanDelete);
      draw_hist->SetStats(false);
      draw_hist->SetTitle((label + " " + result.config.label + " I=" + std::to_string(result.config.intensity) +
                           " ch" + std::to_string(ch) + ";" + g_tot_axis_label + ";" + dt_axis_title + ";entries")
                              .c_str());
      AutoZoomTotAxis(draw_hist, draw_hist->GetXaxis(), 1);
      draw_hist->Draw("colz");
	      if (draw_cut_line) {
	        DrawDtTotCutLine(DtTotCutForChannel(dt_tot_cuts, ch), draw_hist);
	      }
	    }
	    if (has_group) {
	      canvas.Print(out_pdf.c_str());
	    }
	    if (!draw_profile_and_fit || !has_group) {
	      return;
	    }

	    canvas.Clear();
	    canvas.SetRightMargin(0.05);
	    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
	    bool has_profile_group = false;
	    std::vector<std::unique_ptr<TProfile>> profiles;
	    std::vector<std::unique_ptr<TF1>> fits;
	    profiles.reserve(sensor_channels.size());
	    fits.reserve(sensor_channels.size());
	    for (size_t i = 0; i < sensor_channels.size(); ++i) {
	      const int ch = sensor_channels[i];
	      TH2D *hist = FindRunHist2D(result, prefix, ch);
	      canvas.cd(static_cast<int>(i + 1));
	      set_linear_profile_pad();
	      if (!hist || hist->GetEntries() <= 0.0) {
	        continue;
	      }
	      auto profile = MakeTimewalkProfile(*hist);
	      if (!profile) {
	        continue;
	      }
	      has_profile_group = true;
	      profile->SetTitle(("[PROFILE ONLY] " + label + " " + result.config.label +
	                         " I=" + std::to_string(result.config.intensity) + " ch" + std::to_string(ch) +
	                         ";" + g_tot_axis_label + ";mean " + dt_axis_title)
	                            .c_str());
	      profile->SetMarkerColor(kBlack);
	      profile->SetLineColor(kBlack);
	      set_profile_y_range(profile.get(), *hist);
	      auto fit = FitTimewalkProfile(profile.get(), FitRangeForChannel(fit_ranges, ch), fit_model);
	      AutoZoomTotAxis(profile.get(), profile->GetXaxis(), 1);
	      profile->Draw("E1");
	      if (fit) {
	        fit->SetLineColor(kRed + 1);
	        fit->SetLineWidth(3);
	        fit->Draw("same");
	        fits.push_back(std::move(fit));
	      }
	      profiles.push_back(std::move(profile));
	    }
	    if (has_profile_group) {
	      canvas.Print(out_pdf.c_str());
	    }
	  };

  if (draw_diagnostics) {
    DrawSectionPage(
        canvas,
        "CUT DIAGNOSTICS",
        "Intermediate views that isolate one selection step, such as trigger veto/period cleanup or dt/ToT rejection.",
        out_pdf);
    draw_timewalk_2d_group("h_raw_dt_vs_tot_", "[CUT DIAGNOSTIC: before dt/ToT cut]", false, true);
  }
  if (draw_results) {
    draw_timewalk_2d_group("h_dt_vs_tot_", "[FULL SELECTION] #Deltat vs " + g_tot_quantity_label, true, true);
  }
  if (draw_diagnostics) {
    draw_timewalk_2d_group("h_rejected_dt_vs_tot_", "[CUT DIAGNOSTIC: rejected by dt/ToT cut]", false, true);
  }

  if (draw_diagnostics) {
    DrawSectionPage(
        canvas,
        "NO ANALYSIS CUTS",
        "Timing and ToT diagnostics before trigger veto, trigger-period cleanup, matching windows, ToT windows, and dt/ToT cuts.",
        out_pdf);
  }

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

  if (draw_diagnostics) {
    draw_edge_pair("h_edge_time_leading_", "h_edge_time_trailing_");
    draw_edge_pair("h_edge_phase_leading_", "h_edge_phase_trailing_");
  }

  std::vector<int> timing_diagnostic_channels = edge_channels;
  for (int ch : tot_channels) {
    timing_diagnostic_channels.push_back(ch);
  }
  std::sort(timing_diagnostic_channels.begin(), timing_diagnostic_channels.end());
  timing_diagnostic_channels.erase(std::unique(timing_diagnostic_channels.begin(), timing_diagnostic_channels.end()),
                                   timing_diagnostic_channels.end());

  auto draw_no_selection_tot_pages = [&]() {
    std::vector<int> channels_with_entries;
    channels_with_entries.reserve(timing_diagnostic_channels.size());
    for (int ch : timing_diagnostic_channels) {
      TH1D *hist = FindRunHist1D(result, "h_tot_no_selection_", ch);
      if (hist && hist->GetEntries() > 0.0) {
        channels_with_entries.push_back(ch);
      }
    }
    constexpr size_t pads_per_page = 8;
    for (size_t start = 0; start < channels_with_entries.size(); start += pads_per_page) {
      canvas.Clear();
      canvas.SetRightMargin(0.05);
      canvas.Divide(4, 2, 0.001, 0.001);
      bool has_page = false;
      const size_t stop = std::min(start + pads_per_page, channels_with_entries.size());
      for (size_t i = start; i < stop; ++i) {
        const int ch = channels_with_entries[i];
        TH1D *hist = FindRunHist1D(result, "h_tot_no_selection_", ch);
        canvas.cd(static_cast<int>(i - start + 1));
        if (auto *pad = gPad) {
          pad->SetLogx(false);
          pad->SetLogy(true);
          pad->SetRightMargin(0.08);
        }
        if (!hist || hist->GetEntries() <= 0.0) {
          continue;
        }
        has_page = true;
        hist->SetLineColor(ch == trigger_channel ? kRed + 1 : kBlue + 1);
        hist->SetLineWidth(ch == trigger_channel ? 3 : 2);
        hist->SetTitle(("[NO ANALYSIS CUTS] " + result.config.label + " " + g_tot_quantity_label + " ch" +
                        std::to_string(ch) + ";" + g_tot_axis_label + ";entries")
                           .c_str());
        AutoZoomTotAxis(hist, hist->GetXaxis(), 1);
        hist->Draw("hist");
      }
      if (has_page) {
        canvas.Print(out_pdf.c_str());
      }
    }
  };

  auto draw_interhit_overlay_pages = [&]() {
    std::vector<int> channels_with_entries;
    channels_with_entries.reserve(timing_diagnostic_channels.size());
    for (int ch : timing_diagnostic_channels) {
      TH1D *before = FindRunHist1D(result, "h_interhit_leading_", ch);
      TH1D *after = FindRunHist1D(result, "h_interhit_full_selection_", ch);
      if ((before && before->GetEntries() > 0.0) || (after && after->GetEntries() > 0.0)) {
        channels_with_entries.push_back(ch);
      }
    }
    constexpr size_t pads_per_page = 8;
    for (size_t start = 0; start < channels_with_entries.size(); start += pads_per_page) {
      canvas.Clear();
      canvas.SetRightMargin(0.05);
      canvas.Divide(4, 2, 0.001, 0.001);
      bool has_page = false;
      const size_t stop = std::min(start + pads_per_page, channels_with_entries.size());
      for (size_t i = start; i < stop; ++i) {
        const int ch = channels_with_entries[i];
        TH1D *before = FindRunHist1D(result, "h_interhit_leading_", ch);
        TH1D *after = FindRunHist1D(result, "h_interhit_full_selection_", ch);
        canvas.cd(static_cast<int>(i - start + 1));
        if (auto *pad = gPad) {
          pad->SetLogx(true);
          pad->SetLogy(true);
          pad->SetRightMargin(0.08);
        }
        if ((!before || before->GetEntries() <= 0.0) && (!after || after->GetEntries() <= 0.0)) {
          continue;
        }
        has_page = true;
        const double before_max = before ? before->GetMaximum() : 0.0;
        const double after_max = after ? after->GetMaximum() : 0.0;
        const double ymax = 1.25 * std::max(before_max, after_max) + 1.0;
        bool drawn = false;
        if (before && before->GetEntries() > 0.0) {
          before->SetLineColor(kGray + 2);
          before->SetLineWidth(2);
          before->SetMaximum(ymax);
          before->SetTitle((result.config.label + " consecutive leading hits ch" + std::to_string(ch) +
                            ";t_{i} - t_{i-1} [ns];entries")
                               .c_str());
          before->Draw("hist");
          drawn = true;
        }
        if (after && after->GetEntries() > 0.0) {
          after->SetLineColor(ch == trigger_channel ? kRed + 1 : kBlue + 1);
          after->SetLineWidth(ch == trigger_channel ? 3 : 2);
          after->SetMaximum(ymax);
          if (!drawn) {
            after->SetTitle((result.config.label + " consecutive leading hits ch" + std::to_string(ch) +
                             ";t_{i} - t_{i-1} [ns];entries")
                                .c_str());
            after->Draw("hist");
          } else {
            after->Draw("hist same");
          }
        }
        auto *legend = new TLegend(0.45, 0.72, 0.88, 0.88);
        legend->SetBit(kCanDelete);
        legend->SetBorderSize(0);
        legend->SetFillStyle(0);
        if (before && before->GetEntries() > 0.0) {
          legend->AddEntry(before, "no analysis cuts", "l");
        }
        if (after && after->GetEntries() > 0.0) {
          legend->AddEntry(after, "full selection", "l");
        }
        legend->Draw();
      }
      if (has_page) {
        canvas.Print(out_pdf.c_str());
      }
    }
  };

  auto draw_duration_vs_interhit_pages = [&]() {
    std::vector<int> channels_with_entries;
    channels_with_entries.reserve(timing_diagnostic_channels.size());
    for (int ch : timing_diagnostic_channels) {
      TH2D *hist = FindRunHist2D(result, "h_duration_vs_prev_interhit_", ch);
      if (hist && hist->GetEntries() > 0.0) {
        channels_with_entries.push_back(ch);
      }
    }
    constexpr size_t pads_per_page = 8;
    for (size_t start = 0; start < channels_with_entries.size(); start += pads_per_page) {
      canvas.Clear();
      canvas.SetRightMargin(0.05);
      canvas.Divide(4, 2, 0.001, 0.001);
      bool has_page = false;
      const size_t stop = std::min(start + pads_per_page, channels_with_entries.size());
      for (size_t i = start; i < stop; ++i) {
        const int ch = channels_with_entries[i];
        TH2D *hist = FindRunHist2D(result, "h_duration_vs_prev_interhit_", ch);
        canvas.cd(static_cast<int>(i - start + 1));
        if (auto *pad = gPad) {
          pad->SetLogx(true);
          pad->SetLogz(true);
          pad->SetRightMargin(0.14);
        }
        if (!hist || hist->GetEntries() <= 0.0) {
          continue;
        }
        has_page = true;
        hist->SetTitle((result.config.label + " " + g_tot_quantity_label + " vs previous hit interval ch" +
                        std::to_string(ch) + ";t_{i} - t_{i-1} [ns];" + g_tot_axis_label + ";entries")
                           .c_str());
        AutoZoomTotAxis(hist, hist->GetYaxis(), 2);
        hist->Draw("colz");
      }
      if (has_page) {
        canvas.Print(out_pdf.c_str());
      }
    }
  };

  if (draw_diagnostics) {
    draw_no_selection_tot_pages();
    draw_interhit_overlay_pages();
    draw_duration_vs_interhit_pages();
  }

  TH1D *trigger_candidates = FindRunHist1D(result, "h_trigger_candidate_interhit_", trigger_channel);
  TH1D *trigger_after_veto = FindRunHist1D(result, "h_trigger_after_veto_interhit_", trigger_channel);
  TH1D *trigger_clean = FindRunHist1D(result, "h_trigger_clean_interhit_", trigger_channel);
  if (draw_diagnostics && ((trigger_candidates && trigger_candidates->GetEntries() > 0.0) ||
                           (trigger_after_veto && trigger_after_veto->GetEntries() > 0.0) ||
                           (trigger_clean && trigger_clean->GetEntries() > 0.0))) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    if (auto *pad = static_cast<TPad *>(canvas.cd())) {
      pad->SetLogx(true);
      pad->SetLogy(true);
    }
    auto *stack = new THStack(("stack_trigger_interhit_" + SafeName(result.config.label)).c_str(),
                              (result.config.label + " ch" + std::to_string(trigger_channel) +
                               " trigger inter-hit interval;t_{i} - t_{i-1} [ns];entries")
                                  .c_str());
    stack->SetBit(kCanDelete);
    auto *legend = new TLegend(0.62, 0.74, 0.90, 0.90);
    legend->SetBit(kCanDelete);
    legend->SetBorderSize(0);
    legend->SetFillStyle(0);
    auto add_trigger_hist = [&](TH1D *hist, int color, const std::string &label) {
      if (!hist || hist->GetEntries() <= 0.0) {
        return;
      }
      auto *draw_hist = static_cast<TH1D *>(hist->Clone());
      draw_hist->SetDirectory(nullptr);
      draw_hist->SetLineColor(color);
      draw_hist->SetLineWidth(2);
      stack->Add(draw_hist, "hist");
      legend->AddEntry(draw_hist, label.c_str(), "l");
    };
    add_trigger_hist(trigger_candidates, kGray + 2, "candidates before cleanup");
    add_trigger_hist(trigger_after_veto, kOrange + 7, "after veto");
    add_trigger_hist(trigger_clean, kRed + 1, "clean trigger");
    if (stack->GetHists()) {
      stack->GetHists()->SetOwner(kTRUE);
    }
    stack->Draw("nostack hist");
    legend->Draw();
    canvas.Print(out_pdf.c_str());
  }

  bool has_tot_vs_spill = false;
  if (draw_diagnostics) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(static_cast<int>(tot_channels.size()), 1, 0.001, 0.001);
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
      set_linear_2d_pad();
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      has_tot_vs_spill = true;
      auto *draw_hist = static_cast<TH2D *>(hist->Clone());
      draw_hist->SetDirectory(nullptr);
      draw_hist->SetBit(kCanDelete);
      AutoZoomTotAxis(draw_hist, draw_hist->GetYaxis(), 2);
      draw_hist->Draw("colz");
    }
    if (has_tot_vs_spill) {
      canvas.Print(out_pdf.c_str());
    }
    if (has_tot_vs_spill) {
      canvas.Clear();
      canvas.SetRightMargin(0.05);
      canvas.Divide(static_cast<int>(tot_channels.size()), 1, 0.001, 0.001);
      bool has_profile_group = false;
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
        set_linear_profile_pad();
        if (!hist || hist->GetEntries() <= 0.0) {
          continue;
        }
        auto profile = MakeTotVsSpillProfile(*hist);
        if (!profile) {
          continue;
        }
        has_profile_group = true;
        profile->SetTitle(("[PROFILE ONLY] " + result.config.label +
                           " I=" + std::to_string(result.config.intensity) + " " + g_tot_quantity_label +
                           " vs spill ch" + std::to_string(ch) + ";spill;mean " + g_tot_axis_label)
                              .c_str());
        set_tot_profile_y_range(profile.get(), *hist);
        profile->Draw("E1");
        tot_vs_spill_profiles.push_back(std::move(profile));
      }
      if (has_profile_group) {
        canvas.Print(out_pdf.c_str());
      }
    }
  }

  auto draw_sensor_tot_vs_spill_group = [&](const std::string &prefix, const std::string &label) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
    bool has_group = false;
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
      set_linear_2d_pad();
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      has_group = true;
      auto *draw_hist = static_cast<TH2D *>(hist->Clone());
      draw_hist->SetDirectory(nullptr);
      draw_hist->SetBit(kCanDelete);
      draw_hist->SetTitle((result.config.label + " I=" + std::to_string(result.config.intensity) + " " + label +
                      " " + g_tot_quantity_label + " vs spill ch" + std::to_string(ch) + ";spill;" +
                      g_tot_axis_label + ";entries")
	                         .c_str());
      AutoZoomTotAxis(draw_hist, draw_hist->GetYaxis(), 2);
      draw_hist->Draw("colz");
    }
    if (has_group) {
      canvas.Print(out_pdf.c_str());
    }
    if (!has_group) {
      return;
    }

    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
    bool has_profile_group = false;
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
      set_linear_profile_pad();
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      auto profile = MakeTotVsSpillProfile(*hist);
      if (!profile) {
        continue;
      }
      has_profile_group = true;
      profile->SetTitle(("[PROFILE ONLY] " + result.config.label +
                         " I=" + std::to_string(result.config.intensity) + " " + label +
                         " " + g_tot_quantity_label + " vs spill ch" + std::to_string(ch) + ";spill;mean " +
                         g_tot_axis_label)
                            .c_str());
      set_tot_profile_y_range(profile.get(), *hist);
      profile->Draw("E1");
      profiles.push_back(std::move(profile));
    }
    if (has_profile_group) {
      canvas.Print(out_pdf.c_str());
    }
  };

  if (draw_results) {
    draw_sensor_tot_vs_spill_group("h_tot_vs_spill_selected_", "[FULL SELECTION]");
  }
  if (draw_diagnostics) {
    draw_sensor_tot_vs_spill_group("h_tot_vs_spill_rejected_", "[CUT DIAGNOSTIC: rejected by dt/ToT cut]");
  }

  auto draw_sensor_dt_vs_spill_group = [&](const std::string &prefix, const std::string &label) {
    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
    bool has_group = false;
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
      set_linear_2d_pad();
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      has_group = true;
      hist->SetTitle((result.config.label + " I=" + std::to_string(result.config.intensity) + " " + label +
                      " #Deltat vs spill ch" + std::to_string(ch) +
                      ";spill;" + dt_axis_title + ";entries")
	                         .c_str());
      hist->Draw("colz");
    }
    if (has_group) {
      canvas.Print(out_pdf.c_str());
    }
    if (!has_group) {
      return;
    }

    canvas.Clear();
    canvas.SetRightMargin(0.05);
    canvas.Divide(static_cast<int>(sensor_channels.size()), 1, 0.001, 0.001);
    bool has_profile_group = false;
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
      set_linear_profile_pad();
      if (!hist || hist->GetEntries() <= 0.0) {
        continue;
      }
      auto profile = MakeDtVsSpillProfile(*hist);
      if (!profile) {
        continue;
      }
      has_profile_group = true;
      profile->SetTitle(("[PROFILE ONLY] " + result.config.label +
                         " I=" + std::to_string(result.config.intensity) + " " + label +
                         " #Deltat vs spill ch" + std::to_string(ch) +
                         ";spill;mean " + dt_axis_title)
                            .c_str());
      set_profile_y_range(profile.get(), *hist);
      profile->Draw("E1");
      profiles.push_back(std::move(profile));
    }
    if (has_profile_group) {
      canvas.Print(out_pdf.c_str());
    }
  };

  if (draw_diagnostics) {
    draw_sensor_dt_vs_spill_group("h_raw_dt_vs_spill_", "[CUT DIAGNOSTIC: before dt/ToT cut]");
  }
  if (draw_results) {
    draw_sensor_dt_vs_spill_group("h_dt_vs_spill_", "[FULL SELECTION]");
  }
  if (draw_diagnostics) {
    draw_sensor_dt_vs_spill_group("h_rejected_dt_vs_spill_", "[CUT DIAGNOSTIC: rejected by dt/ToT cut]");
  }
  canvas.Clear();

		  if (draw_results && sensor_channels.size() >= 2) {
	    const int ch_a = sensor_channels[0];
	    const int ch_b = sensor_channels[1];
	    const std::string safe_label = SafeName(result.config.label);
	    const std::string raw_name =
	        "h_dt_uncorr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" + safe_label;
	    const std::string corr_name =
	        "h_dt_corr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" + safe_label;
	    TH1D *raw_hist = nullptr;
	    TH1D *corr_hist = nullptr;
	    for (const auto &owned : result.histograms) {
	      const std::string hist_name = owned ? std::string(owned->GetName()) : "";
	      if (hist_name == raw_name) {
	        raw_hist = owned.get();
	      } else if (hist_name == corr_name) {
	        corr_hist = owned.get();
	      }
	    }
		    if (raw_hist || corr_hist) {
		      canvas.Clear();
		      canvas.SetRightMargin(0.05);
		      if (auto *pad = static_cast<TPad *>(canvas.cd())) {
		        pad->SetLogx(false);
		        pad->SetLogy(false);
		        pad->SetLogz(false);
		      }
		      auto *stack = new THStack(("stack_dt_timewalk_compare_ch" + std::to_string(ch_a) + "_ch" +
	                                 std::to_string(ch_b) + "_" + safe_label)
	                                    .c_str(),
	                                (result.config.label + " I=" + std::to_string(result.config.intensity) +
	                                 " coincidence ch" + std::to_string(ch_a) + "-ch" + std::to_string(ch_b) +
	                                 " before/after timewalk;t_{" + std::to_string(ch_a) + "} - t_{" +
	                                 std::to_string(ch_b) + "} [ns];entries")
	                                    .c_str());
	      stack->SetBit(kCanDelete);
	      auto *legend = new TLegend(0.62, 0.74, 0.92, 0.90);
	      legend->SetBit(kCanDelete);
	      legend->SetBorderSize(0);
	      legend->SetFillStyle(0);
	      auto add_hist = [&](TH1D *hist, int color, const char *label) {
	        if (!hist) {
	          return;
	        }
	        auto *draw_hist = static_cast<TH1D *>(hist->Clone());
	        draw_hist->SetDirectory(nullptr);
	        draw_hist->SetStats(false);
	        draw_hist->SetLineColor(color);
	        draw_hist->SetLineWidth(2);
	        stack->Add(draw_hist, "hist");
	        legend->AddEntry(draw_hist, label, "l");
	      };
	      add_hist(raw_hist, kGray + 2, "before timewalk");
	      add_hist(corr_hist, kRed + 1, "after timewalk");
	      if (stack->GetHists()) {
	        stack->GetHists()->SetOwner(kTRUE);
	      }
	      stack->Draw("nostack hist");
	      legend->Draw();
	      canvas.Print(out_pdf.c_str());
	    }
	  }
  canvas.SetRightMargin(0.05);
}

void DrawAccumulatedCoincidenceBeforeAfter(TCanvas &canvas,
                                           const std::vector<RunResult> &results,
                                           const std::vector<int> &sensor_channels,
                                           const std::string &out_pdf)
{
  if (sensor_channels.size() < 2) {
    return;
  }
  const int ch_a = sensor_channels[0];
  const int ch_b = sensor_channels[1];
  std::unique_ptr<TH1D> raw_sum;
  std::unique_ptr<TH1D> corr_sum;

  auto add_to_sum = [](std::unique_ptr<TH1D> &sum, const TH1D *hist, const std::string &name) {
    if (!hist || hist->GetEntries() <= 0.0) {
      return;
    }
    if (!sum) {
      sum.reset(static_cast<TH1D *>(hist->Clone(name.c_str())));
      sum->SetDirectory(nullptr);
      return;
    }
    sum->Add(hist);
  };

  for (const auto &result : results) {
    const std::string safe_label = SafeName(result.config.label);
    const std::string raw_name =
        "h_dt_uncorr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" + safe_label;
    const std::string corr_name =
        "h_dt_corr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_" + safe_label;
    add_to_sum(raw_sum,
               FindRunHist1DByName(result, raw_name),
               "h_dt_uncorr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_accumulated");
    add_to_sum(corr_sum,
               FindRunHist1DByName(result, corr_name),
               "h_dt_corr_ch" + std::to_string(ch_a) + "_ch" + std::to_string(ch_b) + "_accumulated");
  }

  if (!raw_sum && !corr_sum) {
    return;
  }

  canvas.Clear();
  canvas.SetRightMargin(0.05);
  if (auto *pad = static_cast<TPad *>(canvas.cd())) {
    pad->SetLogx(false);
    pad->SetLogy(false);
    pad->SetLogz(false);
  }
  auto *stack = new THStack(("stack_dt_timewalk_compare_ch" + std::to_string(ch_a) + "_ch" +
                             std::to_string(ch_b) + "_accumulated")
                                .c_str(),
                            ("Accumulated coincidence ch" + std::to_string(ch_a) + "-ch" +
                             std::to_string(ch_b) + " before/after timewalk;t_{" +
                             std::to_string(ch_a) + "} - t_{" + std::to_string(ch_b) +
                             "} [ns];entries")
                                .c_str());
  stack->SetBit(kCanDelete);
  auto *legend = new TLegend(0.62, 0.74, 0.92, 0.90);
  legend->SetBit(kCanDelete);
  legend->SetBorderSize(0);
  legend->SetFillStyle(0);
  auto add_hist = [&](TH1D *hist, int color, const char *label) {
    if (!hist || hist->GetEntries() <= 0.0) {
      return;
    }
    auto *draw_hist = static_cast<TH1D *>(hist->Clone());
    draw_hist->SetDirectory(nullptr);
    draw_hist->SetStats(false);
    draw_hist->SetLineColor(color);
    draw_hist->SetLineWidth(3);
    stack->Add(draw_hist, "hist");
    legend->AddEntry(draw_hist, label, "l");
  };
  add_hist(raw_sum.get(), kGray + 2, "before timewalk");
  add_hist(corr_sum.get(), kRed + 1, "after timewalk");
  if (stack->GetHists()) {
    stack->GetHists()->SetOwner(kTRUE);
  }
  stack->Draw("nostack hist");
  legend->Draw();
  canvas.Print(out_pdf.c_str());
}

void DrawAccumulatedCorrectionOverlay(TCanvas &canvas,
                                      const std::map<int, TimewalkCorrection> &timewalk_corrections,
                                      const std::vector<int> &sensor_channels,
                                      double max_duration_ns,
                                      const std::string &out_pdf)
{
  const double xmax = std::max(1.0, max_duration_ns);
  auto *multi = new TMultiGraph();
  multi->SetBit(kCanDelete);
  multi->SetTitle(("Accumulated timewalk correction;" + g_tot_axis_label + ";correction f_{ch}(" +
                   g_tot_quantity_label + ") [ns]")
                      .c_str());
  auto *legend = new TLegend(0.68, 0.68, 0.92, 0.90);
  legend->SetBit(kCanDelete);
  legend->SetBorderSize(0);
  legend->SetFillStyle(0);

  int ngraphs = 0;
  for (size_t i = 0; i < sensor_channels.size(); ++i) {
    const int ch = sensor_channels[i];
    auto correction_it = timewalk_corrections.find(ch);
    if (correction_it == timewalk_corrections.end() || !correction_it->second.valid) {
      continue;
    }
    auto graph = std::make_unique<TGraph>();
    graph->SetName(("g_accumulated_timewalk_correction_ch" + std::to_string(ch)).c_str());
    graph->SetLineColor(ColorForIndex(i));
    graph->SetLineWidth(3);
    constexpr int npoints = 200;
    for (int point = 0; point < npoints; ++point) {
      const double frac = npoints > 1 ? static_cast<double>(point) / static_cast<double>(npoints - 1) : 0.0;
      const double tot = frac * xmax;
      graph->SetPoint(point, tot, correction_it->second.CorrectionNs(tot));
    }
    legend->AddEntry(graph.get(), ("ch " + std::to_string(ch)).c_str(), "l");
    multi->Add(graph.release(), "L");
    ++ngraphs;
  }

  if (ngraphs <= 0) {
    delete multi;
    delete legend;
    return;
  }
  if (multi->GetListOfGraphs()) {
    multi->GetListOfGraphs()->SetOwner(kTRUE);
  }
  canvas.Clear();
  canvas.SetRightMargin(0.05);
  if (auto *pad = static_cast<TPad *>(canvas.cd())) {
    pad->SetLogx(false);
    pad->SetLogy(false);
    pad->SetLogz(false);
  }
  multi->Draw("A");
  if (multi->GetXaxis()) {
    multi->GetXaxis()->SetLimits(0.0, xmax);
  }
  legend->Draw();
  canvas.Print(out_pdf.c_str());
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
    multi->SetTitle(
        ("Timewalk correction fits ch" + std::to_string(ch) + ";" + g_tot_axis_label + ";#Deltat fit [ns]").c_str());
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
                                 RunPlotGroup plot_group,
                                 const std::string &out_pdf)
{
  const bool draw_results = plot_group == RunPlotGroup::Results;
  const bool draw_diagnostics = plot_group == RunPlotGroup::Diagnostics;
  canvas.SetRightMargin(0.14);
  auto set_linear_canvas = [&]() {
    canvas.SetRightMargin(0.14);
    if (auto *pad = static_cast<TPad *>(canvas.cd())) {
      pad->SetLogx(false);
      pad->SetLogy(false);
      pad->SetLogz(false);
    }
  };
  for (int ch : sensor_channels) {
    const DtTotCut cut = DtTotCutForChannel(dt_tot_cuts, ch);
    auto raw_accumulated = MakeAccumulatedRawTimewalkHist(results, ch);
    if (draw_diagnostics && cut.enabled && raw_accumulated && raw_accumulated->GetEntries() > 0.0) {
      canvas.Clear();
      set_linear_canvas();
      auto *draw_raw = static_cast<TH2D *>(raw_accumulated->Clone());
      draw_raw->SetDirectory(nullptr);
      draw_raw->SetBit(kCanDelete);
      AutoZoomTotAxis(draw_raw, draw_raw->GetXaxis(), 1);
      draw_raw->Draw("colz");
      DrawDtTotCutLine(cut, draw_raw);
      canvas.Print(out_pdf.c_str());
    }

    auto accumulated = MakeAccumulatedTimewalkHist(results, ch);
    if (!accumulated || accumulated->GetEntries() <= 0.0) {
      auto rejected = MakeAccumulatedRejectedTimewalkHist(results, ch);
      if (draw_diagnostics && cut.enabled && rejected && rejected->GetEntries() > 0.0) {
        canvas.Clear();
        set_linear_canvas();
        auto *draw_rejected = static_cast<TH2D *>(rejected->Clone());
        draw_rejected->SetDirectory(nullptr);
        draw_rejected->SetBit(kCanDelete);
        draw_rejected->Draw("colz");
        DrawDtTotCutLine(cut, draw_rejected);
        canvas.Print(out_pdf.c_str());
      }
      continue;
    }

    if (draw_results) {
      canvas.Clear();
      set_linear_canvas();
      auto *draw_hist = static_cast<TH2D *>(accumulated->Clone());
      draw_hist->SetDirectory(nullptr);
      draw_hist->SetBit(kCanDelete);
      AutoZoomTotAxis(draw_hist, draw_hist->GetXaxis(), 1);
      draw_hist->Draw("colz");
      DrawDtTotCutLine(cut, draw_hist);
      canvas.Print(out_pdf.c_str());

      auto profile = MakeTimewalkProfile(*accumulated);
      auto fit = FitTimewalkProfile(profile.get(), FitRangeForChannel(fit_ranges, ch), fit_model);
      if (profile) {
        canvas.Clear();
        canvas.SetRightMargin(0.05);
        if (auto *pad = static_cast<TPad *>(canvas.cd())) {
          pad->SetLogx(false);
          pad->SetLogy(false);
          pad->SetLogz(false);
        }
        profile->SetMinimum(draw_hist->GetYaxis()->GetXmin());
        profile->SetMaximum(draw_hist->GetYaxis()->GetXmax());
        AutoZoomTotAxis(profile.get(), profile->GetXaxis(), 1);
        profile->Draw("E1");
        if (fit) {
          fit->Draw("same");
        }
        canvas.Print(out_pdf.c_str());
        canvas.SetRightMargin(0.14);
      }
    }

    auto rejected = MakeAccumulatedRejectedTimewalkHist(results, ch);
    if (draw_diagnostics && cut.enabled && rejected && rejected->GetEntries() > 0.0) {
      canvas.Clear();
      set_linear_canvas();
      auto *draw_rejected = static_cast<TH2D *>(rejected->Clone());
      draw_rejected->SetDirectory(nullptr);
      draw_rejected->SetBit(kCanDelete);
      AutoZoomTotAxis(draw_rejected, draw_rejected->GetXaxis(), 1);
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
    AutoZoomTotAxis(draw_hist, draw_hist->GetXaxis(), 1);
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
	                      const TdcSelection &tdc_selection,
	                      double match_window_ns,
	                      double event_window_ns,
	                      double trigger_deadtime_ns,
	                      int edge_spill,
	                      const std::string &edge_channels_csv,
	                      double edge_phase_period_ns,
	                      double edge_spill_fraction,
	                      TimeReferenceMode reference_mode,
	                      int event_reference_min_channels,
	                      bool signed_dt)
{
  std::ofstream out(path);
  if (!out) {
    std::cerr << "Cannot write text summary: " << path << std::endl;
    return;
  }
  out << "# runlist: " << runlist_path << "\n";
  out << "# tot_quantity: " << g_tot_quantity_label
      << " (TDC-even/TDC-odd dt; ToT mode = pulse width, slew-rate mode = inter-threshold rise time)\n";
	  out << "# fine_calib: " << fine_calib_path << "\n";
		  out << "# channel_calib: " << chan_calib_path << "\n";
		  out << "# reference_mode: " << TimeReferenceModeName(reference_mode) << "\n";
		  out << "# match_window_ns: " << match_window_ns << "\n";
		  out << "# event_window_ns: " << event_window_ns << "\n";
		  if (reference_mode == TimeReferenceMode::EventMedian) {
	    out << "# dt is t_channel - median(event excluding channel), min_channels="
	        << event_reference_min_channels << "\n";
	  } else {
	    out << "# dt is t_channel - t_trigger for channels matched to trigger ch" << trigger_channel << "\n";
	    out << "# dt trigger matching: " << (signed_dt ? "nearest signed trigger" : "previous trigger, dt >= 0") << "\n";
	  }
	  out << "# ToT statistics use valid leading/trailing hits after optional spill and channel-ToT selections, before dt matching\n";
	  out << "# optional dt/ToT cut keeps events below or above DT0 + SLOPE*ToT according to dt_tot_cut_direction\n";
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
	  if (!tdc_selection.Empty()) {
	    std::vector<int> channels = sensor_channels;
	    channels.push_back(trigger_channel);
	    std::sort(channels.begin(), channels.end());
	    channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
	    out << "# leading_tdc_selection: " << tdc_selection.Description(channels)
	        << " (trailing partner kept for ToT)\n";
	  }
	  out << "# h_dt_vs_tot histograms correlate matched dt with ToT for each sensor channel, using |dt|/dt < "
	      << kTimewalkDtLimitNs << " ns range\n";
	  out << "# Plot stages: [NO ANALYSIS CUTS] excludes trigger veto/period cleanup, matching windows, ToT windows, and dt/ToT cuts\n";
	  out << "# Plot stages: [FULL SELECTION] is the sample accepted for the timewalk input after all configured selections\n";
	  out << "# Plot stages: [CUT DIAGNOSTIC] isolates an intermediate selection or rejection step\n";
	  out << "# h_dt_vs_tot ProfileX objects are fitted with the configured timewalk model and written as *_pfx plus TF1\n";
	  out << "# h_dt_vs_tot_accum_ch* objects sum all intensities per channel and are fitted in the same way\n";
		  out << "# h_tot_vs_spill_* histograms show ToT versus spill for each channel; PDF pages separate TH2 maps from *_pfx mean-ToT profiles\n";
		  out << "# h_*dt_vs_spill_* histograms show matched dt versus spill for raw/selected/rejected sensor events; PDF pages separate TH2 maps from *_pfx mean-dt profiles\n";
	  out << "# h_edge_time_* histograms show leading/trailing edge times within one spill for channels "
	      << edge_channels_csv << "; requested spill=" << edge_spill
	      << " (-1 means first selected spill), initial fraction=" << edge_spill_fraction << "\n";
	  out << "# h_edge_phase_* histograms fold the same single-spill edges modulo "
	      << edge_phase_period_ns << " ns\n";
	  out << "# h_interhit_leading_* histograms show leading-hit intervals t_i-t_{i-1} per channel before analysis cuts\n";
	  out << "# h_interhit_full_selection_* histograms show the same intervals after the full timewalk selection\n";
	  out << "# h_duration_vs_prev_interhit_* histograms show hit duration/ToT versus interval from the previous leading hit\n";
	  out << "# h_trigger_candidate_interhit_*, h_trigger_after_veto_interhit_*, and h_trigger_clean_interhit_* compare trigger-channel intervals before veto, after veto, and after period cleanup\n";
	  out << "# trigger_veto_ns: " << trigger_deadtime_ns
	      << " ns, applied after each accepted trigger candidate before period cleanup\n";
	  out << "# h_dt_corr_vs_tot_accum_ch* histograms use all corrected events accumulated over all intensities\n";
		  out << "# h_dt_uncorr_ch*_ch* and h_dt_corr_ch*_ch* histograms compare channel-channel coincidences before/after timewalk in ["
		      << kCorrectedCoincidenceDtMinNs << ", " << kCorrectedCoincidenceDtMaxNs
		      << "] ns, matched through the same clean trigger/event selection and overlaid in a THStack in the PDF\n";
	  out << "# trigger ToT statistics use the cleaned channel-" << trigger_channel << " trigger candidates\n";
	  out << "# trigger veto/dead-time is applied only to channel " << trigger_channel << "\n";
  out << "# trigger22 period cleanup keeps only candidates compatible with the expected trigger period\n";
  out << "# trigger22_clean_period is computed after channel-" << trigger_channel << " dead-time cleaning\n";
  for (int ch : sensor_channels) {
    auto cut_it = dt_tot_cuts.find(ch);
    if (cut_it != dt_tot_cuts.end() && cut_it->second.enabled) {
      const auto &cut = cut_it->second;
      out << "# dt_tot_cut_ch" << ch << ": keep dt "
          << (cut.direction == DtTotCutDirection::KeepAbove ? ">= " : "<= ")
          << cut.intercept << " + " << cut.slope << "*" << g_tot_quantity_label;
      if (std::isfinite(cut.tot_min) || std::isfinite(cut.tot_max)) {
        out << " for " << g_tot_quantity_label << " in [" << cut.tot_min << "," << cut.tot_max << "] ns";
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
	    if (correction.model == TimewalkFitModel::Pol1Plateau) {
	      out << " correction_ns=linear-to-plateau"
	          << " p0=" << correction.p0 << " p1=" << correction.p1 << " x0=" << correction.p2
	          << " plateau=" << correction.p4;
	    } else if (correction.model == TimewalkFitModel::LinExpPlateau) {
	      out << " correction_ns=linear/exponential-plateau"
	          << " p0=" << correction.p0 << " p1=" << correction.p1 << " x0=" << correction.p2
	          << " tau=" << correction.p3 << " plateau=" << correction.p4;
	    } else {
	      out << " correction_ns=(" << correction.p0 << " + " << correction.p1 << "*" << g_tot_quantity_label << ")";
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
                              double trigger_deadtime_ns = 0.0,
                              double trigger_period_ns = 1000000.0,
                              double trigger_period_tolerance_ns = 50000.0,
                              bool signed_dt = false,
                              const char *timewalk_fit_ranges_csv = "17:0:30,19:0:30",
                              const char *timewalk_fit_model_name = "pol1-plateau",
                              const char *dt_tot_cuts_csv = "",
                              const char *trigger_tot_window_csv = "",
                              const char *spill_range_csv = "",
                              const char *channel_tot_windows_csv = "",
                              int edge_spill = -1,
                              const char *edge_channels_csv = "all",
                              double edge_phase_period_ns = 0.0,
                              double edge_spill_fraction = 0.01,
                              const char *tdc_selection_csv = "",
                              const char *reference_mode_name = "trigger",
                              int event_reference_min_channels = 3,
                              double event_window_ns = 0.0,
                              const char *dt_tot_cut_direction_name = "below",
                              const char *opmode_name = "tot",
                              double sensor_duration_ns = 0.0)
{
  if (WantsHelp(runlist_path)) {
    PrintHelp();
    return;
  }
  const TdcOperatingMode tdc_operating_mode = ParseTdcOperatingMode(opmode_name ? opmode_name : "tot");
  ConfigureTotLabels(tdc_operating_mode);
  if (max_duration_ns <= 0.0) {
    max_duration_ns = 30.0;
  }
  if (sensor_duration_ns <= 0.0) {
    sensor_duration_ns = max_duration_ns;
  }
  if (match_window_ns <= 0.0) {
    match_window_ns = 100.0;
  }
  if (event_window_ns <= 0.0) {
    event_window_ns = match_window_ns;
  }
  if (trigger_period_ns < 0.0) {
    trigger_period_ns = 0.0;
  }
  if (trigger_period_tolerance_ns < 0.0) {
    trigger_period_tolerance_ns = 0.0;
  }
  if (trigger_deadtime_ns < 0.0) {
    trigger_deadtime_ns = 0.0;
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
  const TimeReferenceMode reference_mode = ParseTimeReferenceMode(reference_mode_name ? reference_mode_name : "");
  if (reference_mode == TimeReferenceMode::Trigger) {
    const auto old_size = sensor_channels.size();
    sensor_channels.erase(std::remove(sensor_channels.begin(), sensor_channels.end(), trigger_channel),
                          sensor_channels.end());
    if (sensor_channels.size() != old_size) {
      std::cout << "Reference mode trigger: removing ch" << trigger_channel
                << " from sensor channels; it is used only as trigger reference." << std::endl;
    }
    if (sensor_channels.empty()) {
      std::cerr << "No sensor channels left after removing trigger/reference channel " << trigger_channel << "."
                << std::endl;
      return;
    }
  }
  std::vector<int> analysis_channels = sensor_channels;
  analysis_channels.push_back(trigger_channel);
  std::sort(analysis_channels.begin(), analysis_channels.end());
  analysis_channels.erase(std::unique(analysis_channels.begin(), analysis_channels.end()), analysis_channels.end());
  auto edge_channels = ParseEdgeChannelsCsv(edge_channels_csv ? edge_channels_csv : "all", analysis_channels);
  auto timewalk_fit_ranges = ParseFitRangesCsv(timewalk_fit_ranges_csv ? timewalk_fit_ranges_csv : "");
  const auto dt_tot_cut_direction =
      ParseDtTotCutDirection(dt_tot_cut_direction_name ? dt_tot_cut_direction_name : "below");
  auto dt_tot_cuts = ParseDtTotCutsCsv(dt_tot_cuts_csv ? dt_tot_cuts_csv : "", dt_tot_cut_direction);
  auto trigger_tot_window = ParseTotWindow(trigger_tot_window_csv ? trigger_tot_window_csv : "");
  auto spill_range = ParseSpillRange(spill_range_csv ? spill_range_csv : "");
  auto channel_tot_windows = ParseChannelTotWindowsCsv(channel_tot_windows_csv ? channel_tot_windows_csv : "");
  auto tdc_selection = ParseTdcSelectionCsv(tdc_selection_csv ? tdc_selection_csv : "");
  event_reference_min_channels = std::max(1, event_reference_min_channels);
  const auto timewalk_fit_model =
      ParseTimewalkFitModel(timewalk_fit_model_name ? timewalk_fit_model_name : "pol1-plateau");

  analysis_time::FineCalib fine_calib;
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    if (!fine_calib.LoadFromFile(fine_calib_path)) {
      std::cerr << "Warning: fine calibration not loaded: " << fine_calib_path << std::endl;
    }
  }
  fine_calib.use_lut = use_lut;
  analysis_time::ChannelTdcOffsetCalib tdc_offset_calib;
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    tdc_offset_calib.LoadFromFile(fine_calib_path);
  }
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
  std::cout << "TDC operating mode: " << TdcOperatingModeName(tdc_operating_mode) << " (" << g_tot_quantity_label
            << ")" << std::endl;
  std::cout << "Laser trigger/reference channel: " << trigger_channel << std::endl;
  std::cout << "Sensor channels: " << ChannelsLabel(sensor_channels) << std::endl;
  std::cout << "Time reference mode: " << TimeReferenceModeName(reference_mode) << std::endl;
  if (reference_mode == TimeReferenceMode::EventMedian) {
    std::cout << "Event-median min channels: " << event_reference_min_channels << std::endl;
    std::cout << "Timewalk observable: dt(sensor - event median excluding sensor) vs sensor " << g_tot_quantity_label
              << std::endl;
  } else {
    std::cout << "Timewalk observable: dt(sensor - laser ch" << trigger_channel << ") vs sensor "
              << g_tot_quantity_label << std::endl;
  }
  std::cout << "Match window (ns): " << match_window_ns << std::endl;
  std::cout << "Event-building window (ns): " << event_window_ns << std::endl;
  std::cout << "Trigger veto/dead-time after accepted trigger (ns): " << trigger_deadtime_ns << std::endl;
  std::cout << "Trigger expected period (ns): " << trigger_period_ns << std::endl;
  std::cout << "Trigger period tolerance (ns): " << trigger_period_tolerance_ns << std::endl;
  std::cout << "Signed nearest-trigger dt: " << signed_dt << std::endl;
  std::cout << "Timewalk fit ranges: " << (timewalk_fit_ranges_csv ? timewalk_fit_ranges_csv : "") << std::endl;
  std::cout << "Timewalk fit model: " << TimewalkFitModelName(timewalk_fit_model) << std::endl;
  std::cout << "dt/ToT selection cuts: " << (dt_tot_cuts_csv ? dt_tot_cuts_csv : "") << std::endl;
  std::cout << "dt/ToT cut direction: " << DtTotCutDirectionName(dt_tot_cut_direction) << std::endl;
  std::cout << "Trigger ToT window: " << (trigger_tot_window_csv ? trigger_tot_window_csv : "") << std::endl;
  std::cout << "Spill range: " << (spill_range_csv ? spill_range_csv : "") << std::endl;
  std::cout << "Channel ToT windows: " << (channel_tot_windows_csv ? channel_tot_windows_csv : "") << std::endl;
  std::cout << "Leading TDC selection: " << (tdc_selection_csv ? tdc_selection_csv : "")
            << (tdc_selection.Empty() ? "" : " (trailing partner kept for ToT)") << std::endl;
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
  analysis_time::PrintChannelTdcOffsetSummary(tdc_offset_calib);

  std::vector<RunResult> results;
  results.reserve(runs.size());
  for (const auto &run : runs) {
    std::cout << "== Analyze " << run.label << " intensity=" << run.intensity << std::endl;
    results.push_back(AnalyzeRun(run,
                                 sensor_channels,
                                 trigger_channel,
                                 fine_calib,
                                 tdc_offset_calib,
                                 chan_calib,
	                                 reference_mode,
	                                 event_reference_min_channels,
	                                 match_window_ns,
	                                 event_window_ns,
	                                 trigger_deadtime_ns,
                                 trigger_period_ns,
                                 trigger_period_tolerance_ns,
                                 max_duration_ns,
                                 sensor_duration_ns,
                                 clock_mhz,
                                 use_fine,
                                 fine_cut,
                                 require_valid_tot,
                                 signed_dt,
                                 dt_tot_cuts,
                                 trigger_tot_window,
			        spill_range,
			        channel_tot_windows,
			        tdc_selection,
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
                                            trigger_channel,
                                            timewalk_corrections,
                                            match_window_ns,
                                            trigger_deadtime_ns,
                                            trigger_period_ns,
                                            trigger_period_tolerance_ns,
                                            max_duration_ns,
                                            sensor_duration_ns,
                                            clock_mhz,
                                            fine_calib,
                                            tdc_offset_calib,
                                            chan_calib,
                                            reference_mode,
                                            event_reference_min_channels,
                                            use_fine,
                                            fine_cut,
                                            require_valid_tot,
                                            signed_dt,
                                            dt_tot_cuts,
                                            trigger_tot_window,
                                            spill_range,
	                                            channel_tot_windows,
	                                            tdc_selection,
	                                            event_window_ns,
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
                                "mean " + g_tot_quantity_label + " ch" + std::to_string(ch),
                                color);
    tot_mean_graphs.push_back(g_tot_mean.get());
    graphs.push_back(std::move(g_tot_mean));

    auto g_tot_rms = MakeGraph(results,
                               ch,
                               false,
                               true,
                               "g_tot_rms_ch" + std::to_string(ch),
                               "RMS " + g_tot_quantity_label + " ch" + std::to_string(ch),
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
			        tdc_selection,
			        match_window_ns,
			        event_window_ns,
			        trigger_deadtime_ns,
		        edge_spill,
	        edge_channels_csv ? edge_channels_csv : "all",
	        edge_phase_period_ns > 0.0 ? edge_phase_period_ns : trigger_period_ns,
	        edge_spill_fraction,
	        reference_mode,
	        event_reference_min_channels,
	        signed_dt);
  }

  if (out_root && out_root[0] != '\0') {
    TFile fout(out_root, "RECREATE");
    TParameter<int>("trigger_channel", trigger_channel).Write();
    TParameter<int>("reference_mode", static_cast<int>(reference_mode)).Write();
    TParameter<int>("event_reference_min_channels", event_reference_min_channels).Write();
    TParameter<double>("match_window_ns", match_window_ns).Write();
    TParameter<double>("event_window_ns", event_window_ns).Write();
    TParameter<double>("trigger_deadtime_ns", trigger_deadtime_ns).Write();
    TParameter<double>("trigger_veto_ns", trigger_deadtime_ns).Write();
    TParameter<double>("trigger_period_ns", trigger_period_ns).Write();
    TParameter<double>("trigger_period_tolerance_ns", trigger_period_tolerance_ns).Write();
    TParameter<int>("signed_dt", signed_dt ? 1 : 0).Write();
    TParameter<int>("dt_tot_cut_direction", static_cast<int>(dt_tot_cut_direction)).Write();
    TParameter<double>("max_duration_ns", max_duration_ns).Write();
    TParameter<double>("clock_mhz", clock_mhz).Write();
    TParameter<int>("trigger_tot_window_enabled", trigger_tot_window.enabled ? 1 : 0).Write();
    TParameter<double>("trigger_tot_window_min", trigger_tot_window.min).Write();
    TParameter<double>("trigger_tot_window_max", trigger_tot_window.max).Write();
    TParameter<int>("spill_range_enabled", spill_range.enabled ? 1 : 0).Write();
    TParameter<int>("spill_range_min", spill_range.min).Write();
    TParameter<int>("spill_range_max", spill_range.max).Write();
    TParameter<int>("leading_tdc_default_enabled", tdc_selection.default_enabled ? 1 : 0).Write();
    TParameter<int>("leading_tdc_default", tdc_selection.default_leading_tdc).Write();
    for (const auto &kv : tdc_selection.leading_tdc_by_channel) {
      TParameter<int>(("leading_tdc_ch" + std::to_string(kv.first)).c_str(), kv.second).Write();
    }
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
	      TParameter<double>(("timewalk_corr_baseline_ch" + std::to_string(ch)).c_str(), correction.baseline).Write();
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
      TParameter<int>(("dt_tot_cut_direction_ch" + std::to_string(ch)).c_str(), static_cast<int>(cut.direction))
          .Write();
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
    const std::string dt_summary_title = reference_mode == TimeReferenceMode::EventMedian
                                             ? "Mean time difference to event median"
                                             : "Mean time difference to laser trigger";
    const std::string dt_rms_title = reference_mode == TimeReferenceMode::EventMedian
                                         ? "Time-difference RMS to event median"
                                         : "Time-difference RMS to laser trigger";
    const std::string dt_expression_title = DtExpressionTitle(reference_mode, trigger_channel);
    const std::string dt_axis_title = "mean(" + dt_expression_title + ") [ns]";
    const std::string dt_rms_axis_title = "RMS(" + dt_expression_title + ") [ns]";
    canvas.Print((std::string(out_pdf) + "[").c_str());
    DrawGraphs(canvas,
               dt_mean_graphs,
               dt_labels,
               dt_summary_title,
               dt_axis_title,
               out_pdf);
    DrawGraphs(canvas,
               dt_rms_graphs,
               dt_labels,
               dt_rms_title,
               dt_rms_axis_title,
               out_pdf);
	    DrawGraphs(canvas, tot_mean_graphs, tot_labels, "Mean " + g_tot_quantity_label, "mean " + g_tot_axis_label,
	               out_pdf);
	    DrawGraphs(canvas, tot_rms_graphs, tot_labels, g_tot_quantity_label + " RMS", "RMS " + g_tot_axis_label,
	               out_pdf);
	    DrawAccumulatedCorrectionOverlay(canvas, timewalk_corrections, sensor_channels, max_duration_ns, out_pdf);
	    DrawAccumulatedTimewalkFits(canvas,
	                                results,
	                                sensor_channels,
	                                timewalk_fit_ranges,
	                                timewalk_fit_model,
	                                dt_tot_cuts,
	                                RunPlotGroup::Results,
	                                out_pdf);
	    DrawCorrectedAccumulatedTimewalk(canvas, corrected_accumulated_timewalk_histograms, out_pdf);
	    DrawAccumulatedCoincidenceBeforeAfter(canvas, results, sensor_channels, out_pdf);
	    for (const auto &result : results) {
	      DrawRunHistograms(canvas,
	                        result,
	                        sensor_channels,
	                        trigger_channel,
	                        edge_channels,
	                        reference_mode,
	                        timewalk_fit_ranges,
	                        dt_tot_cuts,
	                        timewalk_fit_model,
	                        RunPlotGroup::Results,
	                        out_pdf);
	    }
	    DrawSectionPage(canvas,
	                    "DIAGNOSTICS",
	                    "Diagnostic pages are collected at the end: no-cut views, cut rejection checks, and trigger timing structure.",
	                    out_pdf);
	    DrawAccumulatedTimewalkFits(canvas,
	                                results,
	                                sensor_channels,
	                                timewalk_fit_ranges,
	                                timewalk_fit_model,
	                                dt_tot_cuts,
	                                RunPlotGroup::Diagnostics,
	                                out_pdf);
	    for (const auto &result : results) {
	      DrawRunHistograms(canvas,
	                        result,
	                        sensor_channels,
	                        trigger_channel,
	                        edge_channels,
	                        reference_mode,
	                        timewalk_fit_ranges,
	                        dt_tot_cuts,
	                        timewalk_fit_model,
	                        RunPlotGroup::Diagnostics,
	                        out_pdf);
	    }
    canvas.Print((std::string(out_pdf) + "]").c_str());
    std::cout << "Wrote PDF output: " << out_pdf << std::endl;
  }
}
