#include <ROOT/RDataFrame.hxx>
#include <ROOT/RDFHelpers.hxx>
#include <TCanvas.h>
#include <TF1.h>
#include <TFile.h>
#include <TH1D.h>
#include <TH2D.h>
#include <THStack.h>
#include <TLegend.h>
#include <TProfile.h>
#include <TParameter.h>
#include <TPad.h>
#include <TPaveStats.h>
#include <TStyle.h>
#include <TTree.h>
#include <TSystem.h>

#include "analysis_io.h"
#include "analysis_events.h"
#include "analysis_tdc.h"
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace {
using analysis_tdc::GetTrailingPartner;
using analysis_tdc::IsLeadingTdc;
using analysis_tdc::IsTrailingTdc;
using analysis_tdc::TdcPairIndex;

bool WantsHelp(const char *arg)
{
  if (!arg) {
    return true;
  }
  std::string val(arg);
  return val == "-h" || val == "--help" || val == "help";
}

struct ScopedTimer {
  std::string label;
  std::chrono::steady_clock::time_point start;
  explicit ScopedTimer(std::string label_in)
      : label(std::move(label_in)),
        start(std::chrono::steady_clock::now())
  {}
  ~ScopedTimer()
  {
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::cout << "Elapsed time (" << label << "): " << elapsed.count() << " s" << std::endl;
  }
};

bool HasBranch(const std::vector<std::string> &cols, const std::string &name)
{
  return std::find(cols.begin(), cols.end(), name) != cols.end();
}

uint64_t RunSpillKey(int run_id, int spill)
{
  uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(run_id));
  key <<= 32;
  key |= static_cast<uint32_t>(spill);
  return key;
}

int QuantileBin(const long long *counts, int bins, long long total, double q)
{
  if (total <= 0) {
    return -1;
  }
  if (q < 0.0) {
    q = 0.0;
  }
  if (q > 1.0) {
    q = 1.0;
  }
  long long target = static_cast<long long>(std::ceil(q * static_cast<double>(total)));
  if (target < 1) {
    target = 1;
  }
  long long sum = 0;
  for (int i = 0; i < bins; ++i) {
    sum += counts[i];
    if (sum >= target) {
      return i;
    }
  }
  return bins - 1;
}

std::vector<int> ParseChannelsCsv(const char *csv)
{
  std::vector<int> channels;
  if (!csv || csv[0] == '\0') {
    return channels;
  }
  std::string text(csv);
  for (char &ch : text) {
    if (ch == ',' || ch == ';' || ch == ':') {
      ch = ' ';
    }
  }
  std::istringstream input(text);
  int channel = 0;
  while (input >> channel) {
    if (channel < 0 || channel >= 32) {
      continue;
    }
    if (std::find(channels.begin(), channels.end(), channel) == channels.end()) {
      channels.push_back(channel);
    }
  }
  std::sort(channels.begin(), channels.end());
  return channels;
}

std::string ChannelListLabel(const std::vector<int> &channels)
{
  if (channels.empty()) {
    return "all";
  }
  std::ostringstream out;
  for (size_t i = 0; i < channels.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << channels[i];
  }
  return out.str();
}

std::string ParentDir(const std::string &path)
{
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

std::string OffsetSourceKey(const std::string &path)
{
  const std::string decoded_token = "/decoded/";
  const auto decoded_pos = path.rfind(decoded_token);
  if (decoded_pos != std::string::npos) {
    return path.substr(0, decoded_pos + std::string("/decoded").size());
  }
  return ParentDir(path);
}

analysis_time::FineCalib BuildFineCalibFromValues(
    const std::array<double, analysis_time::kFineCalibSize> &min_vals,
    const std::array<double, analysis_time::kFineCalibSize> &max_vals,
    const std::array<int, analysis_time::kFineCalibSize> &valid_vals,
    const TH2D &hlut)
{
  analysis_time::FineCalib calib;
  calib.FillDefault();
  bool any_valid = false;
  for (int idx = 0; idx < analysis_time::kFineCalibSize; ++idx) {
    if (!valid_vals[idx] || max_vals[idx] <= min_vals[idx]) {
      continue;
    }
    calib.min[idx] = min_vals[idx];
    calib.cut[idx] = 0.5 * (min_vals[idx] + max_vals[idx]);
    calib.inv_range[idx] = 1.0 / (max_vals[idx] - min_vals[idx]);
    for (int b = 0; b < analysis_time::kFineBins; ++b) {
      calib.lut[idx * analysis_time::kFineBins + b] = static_cast<float>(hlut.GetBinContent(idx + 1, b + 1));
    }
    any_valid = true;
  }
  calib.loaded = any_valid;
  calib.lut_loaded = any_valid;
  calib.use_lut = true;
  return calib;
}

bool HasTreeBranch(TTree *tree, const char *name)
{
  return tree && tree->GetBranch(name) != nullptr;
}

struct OffsetHit {
  int run_id = 0;
  int spill = 0;
  int channel = 0;
  int tdc = 0;
  double time_raw_ns = 0.0;
  double time_ns = 0.0;
};

struct OffsetCursor {
  std::string path;
  int source_id = 0;
  std::unique_ptr<TFile> file;
  TTree *tree = nullptr;
  Long64_t entry = 0;
  Long64_t entries = 0;
  bool valid = false;
  bool has_spill = false;
  bool has_run_id = false;
  int current_spill = 0;

  int type = 0;
  int fifo = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int fine = 0;
  int rollover = 0;
  int coarse = 0;
  int spill = 0;
  int run_id = 0;
  OffsetHit hit;
};

void BindOffsetCursorBranches(OffsetCursor &cursor)
{
  if (!cursor.tree) {
    return;
  }
  cursor.tree->SetBranchAddress("type", &cursor.type);
  cursor.tree->SetBranchAddress("fifo", &cursor.fifo);
  cursor.tree->SetBranchAddress("column", &cursor.column);
  cursor.tree->SetBranchAddress("pixel", &cursor.pixel);
  cursor.tree->SetBranchAddress("tdc", &cursor.tdc);
  cursor.tree->SetBranchAddress("fine", &cursor.fine);
  cursor.tree->SetBranchAddress("rollover", &cursor.rollover);
  cursor.tree->SetBranchAddress("coarse", &cursor.coarse);
  if (cursor.has_spill) {
    cursor.tree->SetBranchAddress("spill", &cursor.spill);
  }
  if (cursor.has_run_id) {
    cursor.tree->SetBranchAddress("run_id", &cursor.run_id);
  }
}

bool InitOffsetCursor(const std::string &path,
                      const std::string &tree_name,
                      int source_id,
                      OffsetCursor &cursor)
{
  cursor = OffsetCursor{};
  cursor.path = path;
  cursor.source_id = source_id;
  cursor.file.reset(TFile::Open(path.c_str(), "READ"));
  if (!cursor.file || cursor.file->IsZombie()) {
    std::cout << "Offset study: skipping " << path << " (cannot open)" << std::endl;
    return false;
  }
  cursor.tree = dynamic_cast<TTree *>(cursor.file->Get(tree_name.c_str()));
  if (!cursor.tree) {
    std::cout << "Offset study: skipping " << path << " (missing tree " << tree_name << ")" << std::endl;
    return false;
  }
  for (const auto *name : {"type", "fifo", "column", "pixel", "tdc", "fine", "rollover", "coarse"}) {
    if (!HasTreeBranch(cursor.tree, name)) {
      std::cout << "Offset study: skipping " << path << " (missing branch " << name << ")" << std::endl;
      return false;
    }
  }
  cursor.has_spill = HasTreeBranch(cursor.tree, "spill");
  cursor.has_run_id = HasTreeBranch(cursor.tree, "run_id");
  BindOffsetCursorBranches(cursor);
  cursor.entries = cursor.tree->GetEntries();
  return cursor.entries > 0;
}

bool AdvanceOffsetCursor(OffsetCursor &cursor, const analysis_time::FineCalib &calib, double tick_ns)
{
  cursor.valid = false;
  while (cursor.entry < cursor.entries) {
    cursor.tree->GetEntry(cursor.entry++);
    if (!cursor.has_spill && cursor.type == 15) {
      ++cursor.current_spill;
      continue;
    }
    if (cursor.type != 1) {
      continue;
    }
    if (cursor.tdc < 0 || cursor.tdc >= analysis_time::kTdcPerPixel) {
      continue;
    }
    if (cursor.fine < 0 || cursor.fine >= analysis_time::kFineBins) {
      continue;
    }
    const int tdc_index = analysis_time::TdcIndex(cursor.fifo, cursor.column, cursor.pixel, cursor.tdc);
    if (tdc_index < 0 || tdc_index >= analysis_time::kFineCalibSize || calib.inv_range[tdc_index] <= 0.0) {
      continue;
    }
    const Long64_t time_tick = analysis_time::TimeTick(cursor.rollover, cursor.coarse);
    cursor.hit.run_id = cursor.has_run_id ? cursor.run_id : cursor.source_id;
    cursor.hit.spill = cursor.has_spill ? cursor.spill : cursor.current_spill;
    cursor.hit.channel = cursor.column * analysis_time::kPixelsPerColumn + cursor.pixel;
    cursor.hit.tdc = cursor.tdc;
    cursor.hit.time_raw_ns = analysis_time::TimeNsFromTick(time_tick, cursor.fine, tick_ns, true);
    cursor.hit.time_ns = analysis_time::TimeNsFromTick(calib, time_tick, cursor.fine, tdc_index, tick_ns, true);
    cursor.valid = true;
    return true;
  }
  return false;
}

struct ChannelTdcOffsetSummary {
  int channel = 0;
  int tdc = 0;
  int ref_channel = 0;
  int ref_tdc = 0;
  long long entries = 0;
  double offset_ns = 0.0;
  double mean_ns = 0.0;
  double rms_ns = 0.0;
  bool valid = false;
};

struct ChannelTdcOffsetStudy {
  bool attempted = false;
  bool available = false;
  int ref_channel = 22;
  int reference_mode = 0;
  int min_channels = 3;
  double match_window_ns = 0.0;
  double event_window_ns = 0.0;
  std::array<bool, 32> target_channels{};
  std::vector<int> target_channel_list;
  std::array<std::array<long long, analysis_time::kTdcPerPixel>, 32> edge_counts{};
  std::map<std::pair<int, int>, std::unique_ptr<TH1D>> raw_dt_hists;
  std::map<std::pair<int, int>, std::unique_ptr<TH1D>> dt_hists;
  std::unique_ptr<TH1D> h_coinc_raw;
  std::unique_ptr<TH1D> h_coinc_tdc;
  std::unique_ptr<TH1D> h_coinc_tdc_offset;
  std::unique_ptr<TH2D> h_offset;
  std::unique_ptr<TH2D> h_mean;
  std::unique_ptr<TH2D> h_rms;
  std::unique_ptr<TH2D> h_entries;
  std::unique_ptr<TH1D> h_leading_offset;
  std::unique_ptr<TH1D> h_leading_entries;
  std::unique_ptr<TTree> tree;
  std::vector<ChannelTdcOffsetSummary> summaries;
};

bool IsTargetOffsetChannel(const ChannelTdcOffsetStudy &study, int channel)
{
  return channel >= 0 && channel < static_cast<int>(study.target_channels.size()) && study.target_channels[channel];
}

bool IsReferenceOffsetChannel(const ChannelTdcOffsetStudy &study, int channel)
{
  return study.reference_mode == 0 && channel == study.ref_channel;
}

bool KeepOffsetInputChannel(const ChannelTdcOffsetStudy &study, int channel)
{
  return IsTargetOffsetChannel(study, channel) || IsReferenceOffsetChannel(study, channel);
}

bool ChannelHasValidTdc(const std::array<int, analysis_time::kFineCalibSize> &valid_vals, int channel)
{
  for (int idx = 0; idx < analysis_time::kFineCalibSize; ++idx) {
    if (!valid_vals[idx]) {
      continue;
    }
    const int local = idx % analysis_time::kTdcPerFifo;
    const int column = local / (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
    const int rem = local % (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
    const int pixel = rem / analysis_time::kTdcPerPixel;
    const int ch = column * analysis_time::kPixelsPerColumn + pixel;
    if (ch == channel) {
      return true;
    }
  }
  return false;
}

int ChooseOffsetReferenceChannel(const std::array<int, analysis_time::kFineCalibSize> &valid_vals,
                                 int requested_channel)
{
  if (requested_channel >= 0) {
    return requested_channel;
  }
  if (ChannelHasValidTdc(valid_vals, 22)) {
    return 22;
  }
  for (int ch = 0; ch < 32; ++ch) {
    if (ChannelHasValidTdc(valid_vals, ch)) {
      return ch;
    }
  }
  return -1;
}

TH1D *OffsetDtHist(ChannelTdcOffsetStudy &study, int channel, int tdc)
{
  const auto key = std::make_pair(channel, tdc);
  auto it = study.dt_hists.find(key);
  if (it != study.dt_hists.end()) {
    return it->second.get();
  }
  const int bins = std::max(100, static_cast<int>(std::ceil(4.0 * study.match_window_ns)));
  const std::string ref_tag = study.reference_mode == 1 ? "eventMedian" : "refCh" + std::to_string(study.ref_channel);
  const std::string ref_title = study.reference_mode == 1
                                    ? "event median"
                                    : "ref ch " + std::to_string(study.ref_channel) + " TDC " + std::to_string(tdc);
  std::string name = "hOffsetDt_ch" + std::to_string(channel) + "_tdc" + std::to_string(tdc) + "_" + ref_tag;
  std::string title = "Offset #Deltat ch " + std::to_string(channel) + " TDC " + std::to_string(tdc) +
                      " - " + ref_title + ";#Deltat [ns];entries";
  auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), bins, -study.match_window_ns, study.match_window_ns);
  hist->SetDirectory(nullptr);
  TH1D *ptr = hist.get();
  study.dt_hists[key] = std::move(hist);
  return ptr;
}

TH1D *OffsetRawDtHist(ChannelTdcOffsetStudy &study, int channel, int tdc)
{
  const auto key = std::make_pair(channel, tdc);
  auto it = study.raw_dt_hists.find(key);
  if (it != study.raw_dt_hists.end()) {
    return it->second.get();
  }
  const int bins = std::max(100, static_cast<int>(std::ceil(4.0 * study.match_window_ns)));
  const std::string ref_tag = "refCh" + std::to_string(study.ref_channel);
  const std::string ref_title = "ref ch " + std::to_string(study.ref_channel) + " TDC " + std::to_string(tdc);
  std::string name = "hOffsetRawDt_ch" + std::to_string(channel) + "_tdc" + std::to_string(tdc) + "_" + ref_tag;
  std::string title = "Uncalibrated offset #Deltat ch " + std::to_string(channel) + " TDC " + std::to_string(tdc) +
                      " - " + ref_title + ";#Deltat [ns];entries";
  auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), bins, -study.match_window_ns, study.match_window_ns);
  hist->SetDirectory(nullptr);
  TH1D *ptr = hist.get();
  study.raw_dt_hists[key] = std::move(hist);
  return ptr;
}

void ProcessChannelReferenceOffsetSpill(const std::vector<OffsetHit> &spill_hits, ChannelTdcOffsetStudy &study)
{
  if (study.ref_channel < 0 || spill_hits.empty()) {
    return;
  }
  std::array<std::vector<size_t>, analysis_time::kTdcPerPixel> ref_indices;
  std::array<std::vector<size_t>, analysis_time::kTdcPerPixel> target_indices;
  for (size_t i = 0; i < spill_hits.size(); ++i) {
    const auto &hit = spill_hits[i];
    if (hit.channel < 0 || hit.channel >= 32 ||
        hit.tdc < 0 || hit.tdc >= analysis_time::kTdcPerPixel) {
      continue;
    }
    if (!KeepOffsetInputChannel(study, hit.channel)) {
      continue;
    }
    ++study.edge_counts[hit.channel][hit.tdc];
    if (hit.channel == study.ref_channel) {
      ref_indices[hit.tdc].push_back(i);
      continue;
    }
    if (IsTargetOffsetChannel(study, hit.channel)) {
      target_indices[hit.tdc].push_back(i);
    }
  }

  for (int tdc = 0; tdc < analysis_time::kTdcPerPixel; ++tdc) {
    auto events = analysis_events::BuildReferenceEvents(spill_hits,
                                                        ref_indices[tdc],
                                                        target_indices[tdc],
                                                        study.event_window_ns,
                                                        true);
    for (const auto &event : events) {
      if (event.reference_index >= spill_hits.size()) {
        continue;
      }
      const auto &ref = spill_hits[event.reference_index];
      for (const auto &event_hit : event.hits) {
        if (event_hit.index >= spill_hits.size() || !IsTargetOffsetChannel(study, event_hit.channel)) {
          continue;
        }
        if (std::abs(event_hit.dt_ns) > study.match_window_ns) {
          continue;
        }
        const auto &target = spill_hits[event_hit.index];
        auto *hist = OffsetDtHist(study, event_hit.channel, tdc);
        auto *raw_hist = OffsetRawDtHist(study, event_hit.channel, tdc);
        hist->Fill(event_hit.dt_ns);
        raw_hist->Fill(target.time_raw_ns - ref.time_raw_ns);
      }
    }
  }
}

double MedianValue(std::vector<double> values)
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

struct OffsetEventEntry {
  int channel = 0;
  double time_ns = 0.0;
};

void ProcessOffsetEventCluster(std::vector<OffsetEventEntry> cluster, int tdc, ChannelTdcOffsetStudy &study)
{
  if (cluster.empty()) {
    return;
  }
  std::vector<double> all_times;
  all_times.reserve(cluster.size());
  for (const auto &entry : cluster) {
    all_times.push_back(entry.time_ns);
  }
  const double preliminary_median = MedianValue(all_times);
  if (!std::isfinite(preliminary_median)) {
    return;
  }

  std::array<bool, 32> seen{};
  std::array<OffsetEventEntry, 32> best_by_channel{};
  for (const auto &entry : cluster) {
    if (entry.channel < 0 || entry.channel >= static_cast<int>(seen.size())) {
      continue;
    }
    if (!seen[entry.channel] ||
        std::abs(entry.time_ns - preliminary_median) < std::abs(best_by_channel[entry.channel].time_ns - preliminary_median)) {
      seen[entry.channel] = true;
      best_by_channel[entry.channel] = entry;
    }
  }

  std::vector<OffsetEventEntry> unique_entries;
  unique_entries.reserve(cluster.size());
  std::vector<double> unique_times;
  for (int ch = 0; ch < static_cast<int>(seen.size()); ++ch) {
    if (!seen[ch]) {
      continue;
    }
    unique_entries.push_back(best_by_channel[ch]);
    unique_times.push_back(best_by_channel[ch].time_ns);
  }
  if (static_cast<int>(unique_entries.size()) < std::max(1, study.min_channels)) {
    return;
  }

  for (const auto &entry : unique_entries) {
    std::vector<double> reference_times;
    reference_times.reserve(unique_times.size());
    for (const auto &other : unique_entries) {
      if (other.channel == entry.channel) {
        continue;
      }
      reference_times.push_back(other.time_ns);
    }
    const double reference = reference_times.empty() ? MedianValue(unique_times) : MedianValue(reference_times);
    if (!std::isfinite(reference)) {
      continue;
    }
    OffsetDtHist(study, entry.channel, tdc)->Fill(entry.time_ns - reference);
  }
}

void ProcessEventMedianOffsetSpill(const std::vector<OffsetHit> &spill_hits, ChannelTdcOffsetStudy &study)
{
  if (spill_hits.empty()) {
    return;
  }
  std::array<std::vector<size_t>, analysis_time::kTdcPerPixel> by_tdc;
  for (size_t i = 0; i < spill_hits.size(); ++i) {
    const auto &hit = spill_hits[i];
    if (hit.channel < 0 || hit.channel >= 32 || hit.tdc < 0 || hit.tdc >= analysis_time::kTdcPerPixel) {
      continue;
    }
    if (!IsTargetOffsetChannel(study, hit.channel)) {
      continue;
    }
    by_tdc[hit.tdc].push_back(i);
    ++study.edge_counts[hit.channel][hit.tdc];
  }

  for (int tdc = 0; tdc < analysis_time::kTdcPerPixel; ++tdc) {
    auto events = analysis_events::BuildClusterEvents(spill_hits, by_tdc[tdc], study.event_window_ns, study.min_channels);
    for (const auto &event : events) {
      for (const auto &event_hit : event.hits) {
        if (event_hit.index >= spill_hits.size() || !IsTargetOffsetChannel(study, event_hit.channel)) {
          continue;
        }
        if (std::abs(event_hit.dt_ns) > study.match_window_ns) {
          continue;
        }
        OffsetDtHist(study, event_hit.channel, tdc)->Fill(event_hit.dt_ns);
      }
    }
  }
}

void FinalizeOffsetStudy(ChannelTdcOffsetStudy &study)
{
  const std::string ref_title = study.reference_mode == 1
                                    ? "event median reference"
                                    : "reference channel " + std::to_string(study.ref_channel);
  study.h_offset = std::make_unique<TH2D>("hChannelTdcOffset",
                                          ("Channel/TDC offset relative to " + ref_title + ";channel;TDC").c_str(),
                                          32,
                                          -0.5,
                                          31.5,
                                          analysis_time::kTdcPerPixel,
                                          -0.5,
                                          analysis_time::kTdcPerPixel - 0.5);
  study.h_mean = std::make_unique<TH2D>("hChannelTdcOffsetMean",
                                        ("Channel/TDC #Deltat mean relative to " + ref_title + ";channel;TDC").c_str(),
                                        32,
                                        -0.5,
                                        31.5,
                                        analysis_time::kTdcPerPixel,
                                        -0.5,
                                          analysis_time::kTdcPerPixel - 0.5);
  study.h_rms = std::make_unique<TH2D>("hChannelTdcOffsetRms",
                                       "Channel/TDC #Deltat RMS relative to reference;channel;TDC",
                                       32,
                                       -0.5,
                                       31.5,
                                       analysis_time::kTdcPerPixel,
                                       -0.5,
                                       analysis_time::kTdcPerPixel - 0.5);
  study.h_entries = std::make_unique<TH2D>("hChannelTdcOffsetEntries",
                                           "Channel/TDC offset matched entries;channel;TDC",
                                           32,
                                           -0.5,
                                           31.5,
                                           analysis_time::kTdcPerPixel,
                                           -0.5,
                                           analysis_time::kTdcPerPixel - 0.5);
  study.h_leading_offset = std::make_unique<TH1D>("hChannelLeadingOffset",
                                                  "Weighted leading-TDC offset summary;channel;offset [ns]",
                                                  32,
                                                  -0.5,
                                                  31.5);
  study.h_leading_entries = std::make_unique<TH1D>("hChannelLeadingOffsetEntries",
                                                   "Weighted leading-TDC offset matched entries;channel;entries",
                                                   32,
                                                   -0.5,
                                                   31.5);
  const int coincidence_bins = std::max(100, static_cast<int>(std::ceil(4.0 * study.match_window_ns)));
  const std::string coincidence_title =
      "Coincidence #Deltat to reference ch " + std::to_string(study.ref_channel) +
      " on calibration run;#Deltat = t_{ch} - t_{22} [ns];entries";
  study.h_coinc_raw = std::make_unique<TH1D>("hChannelOffsetCoincidenceRawCh22",
                                             (coincidence_title + " (uncalibrated fine time)").c_str(),
                                             coincidence_bins,
                                             -study.match_window_ns,
                                             study.match_window_ns);
  study.h_coinc_tdc = std::make_unique<TH1D>("hChannelOffsetCoincidenceTdcCh22",
                                             (coincidence_title + " (TDC calibrated)").c_str(),
                                             coincidence_bins,
                                             -study.match_window_ns,
                                             study.match_window_ns);
  study.h_coinc_tdc_offset = std::make_unique<TH1D>("hChannelOffsetCoincidenceTdcOffsetCh22",
                                                    (coincidence_title + " (TDC + channel/TDC offset)").c_str(),
                                                    coincidence_bins,
                                                    -study.match_window_ns,
                                                    study.match_window_ns);
  const std::array<TH1 *, 9> offset_summary_hists = {
      static_cast<TH1 *>(study.h_offset.get()),
      static_cast<TH1 *>(study.h_mean.get()),
      static_cast<TH1 *>(study.h_rms.get()),
      static_cast<TH1 *>(study.h_entries.get()),
      static_cast<TH1 *>(study.h_leading_offset.get()),
      static_cast<TH1 *>(study.h_leading_entries.get()),
      static_cast<TH1 *>(study.h_coinc_raw.get()),
      static_cast<TH1 *>(study.h_coinc_tdc.get()),
      static_cast<TH1 *>(study.h_coinc_tdc_offset.get())};
  for (auto *obj : offset_summary_hists) {
    if (obj) {
      obj->SetDirectory(nullptr);
    }
  }

  study.summaries.clear();
  for (auto &kv : study.dt_hists) {
    TH1D *hist = kv.second.get();
    if (!hist || hist->GetEntries() <= 0.0) {
      continue;
    }
    ChannelTdcOffsetSummary summary;
    summary.channel = kv.first.first;
    summary.tdc = kv.first.second;
    summary.ref_channel = study.ref_channel;
    summary.ref_tdc = summary.tdc;
    summary.entries = static_cast<long long>(hist->GetEntries());
    summary.mean_ns = hist->GetMean();
    summary.rms_ns = hist->GetRMS();
    double q = 0.5;
    double median = 0.0;
    hist->GetQuantiles(1, &median, &q);
    summary.offset_ns = median;
    summary.valid = summary.entries >= 10;
    study.summaries.push_back(summary);

    const int xbin = summary.channel + 1;
    const int ybin = summary.tdc + 1;
    study.h_offset->SetBinContent(xbin, ybin, summary.offset_ns);
    study.h_mean->SetBinContent(xbin, ybin, summary.mean_ns);
    study.h_rms->SetBinContent(xbin, ybin, summary.rms_ns);
    study.h_entries->SetBinContent(xbin, ybin, static_cast<double>(summary.entries));
    if (summary.valid) {
      study.available = true;
    }
    auto raw_it = study.raw_dt_hists.find(kv.first);
    if (raw_it != study.raw_dt_hists.end() && raw_it->second) {
      study.h_coinc_raw->Add(raw_it->second.get());
    }
    study.h_coinc_tdc->Add(hist);
    for (int bin = 1; bin <= hist->GetNbinsX(); ++bin) {
      const double entries_in_bin = hist->GetBinContent(bin);
      if (entries_in_bin <= 0.0) {
        continue;
      }
      study.h_coinc_tdc_offset->Fill(hist->GetBinCenter(bin) - summary.offset_ns, entries_in_bin);
    }
  }
  if (study.h_coinc_tdc && study.h_coinc_tdc_offset) {
    study.h_coinc_tdc_offset->SetEntries(study.h_coinc_tdc->GetEntries());
  }

  if (study.reference_mode == 0 && IsTargetOffsetChannel(study, study.ref_channel)) {
    for (int tdc = 0; tdc < analysis_time::kTdcPerPixel; ++tdc) {
      const long long count = study.edge_counts[study.ref_channel][tdc];
      if (count <= 0) {
        continue;
      }
      const int xbin = study.ref_channel + 1;
      const int ybin = tdc + 1;
      study.h_offset->SetBinContent(xbin, ybin, 0.0);
      study.h_mean->SetBinContent(xbin, ybin, 0.0);
      study.h_rms->SetBinContent(xbin, ybin, 0.0);
      study.h_entries->SetBinContent(xbin, ybin, static_cast<double>(count));
    }
  }

  for (int ch = 0; ch < 32; ++ch) {
    double weighted = 0.0;
    double weight = 0.0;
    for (int tdc : {0, 2}) {
      const double entries = study.h_entries->GetBinContent(ch + 1, tdc + 1);
      if (entries <= 0.0) {
        continue;
      }
      weighted += entries * study.h_offset->GetBinContent(ch + 1, tdc + 1);
      weight += entries;
    }
    if (weight > 0.0) {
      study.h_leading_offset->SetBinContent(ch + 1, weighted / weight);
      study.h_leading_entries->SetBinContent(ch + 1, weight);
    }
  }

  study.tree = std::make_unique<TTree>("channel_tdc_offsets", "Per-channel per-TDC relative timing offsets");
  study.tree->SetDirectory(nullptr);
  int channel = 0;
  int tdc = 0;
  int ref_channel = study.ref_channel;
  int ref_tdc = 0;
  int reference_mode = study.reference_mode;
  int valid = 0;
  long long entries = 0;
  double offset_ns = 0.0;
  double mean_ns = 0.0;
  double rms_ns = 0.0;
  study.tree->Branch("channel", &channel, "channel/I");
  study.tree->Branch("tdc", &tdc, "tdc/I");
  study.tree->Branch("ref_channel", &ref_channel, "ref_channel/I");
  study.tree->Branch("ref_tdc", &ref_tdc, "ref_tdc/I");
  study.tree->Branch("reference_mode", &reference_mode, "reference_mode/I");
  study.tree->Branch("entries", &entries, "entries/L");
  study.tree->Branch("offset_ns", &offset_ns, "offset_ns/D");
  study.tree->Branch("mean_ns", &mean_ns, "mean_ns/D");
  study.tree->Branch("rms_ns", &rms_ns, "rms_ns/D");
  study.tree->Branch("valid", &valid, "valid/I");

  for (const auto &summary : study.summaries) {
    channel = summary.channel;
    tdc = summary.tdc;
    ref_channel = summary.ref_channel;
    ref_tdc = summary.ref_tdc;
    reference_mode = study.reference_mode;
    entries = summary.entries;
    offset_ns = summary.offset_ns;
    mean_ns = summary.mean_ns;
    rms_ns = summary.rms_ns;
    valid = summary.valid ? 1 : 0;
    study.tree->Fill();
  }
}

ChannelTdcOffsetStudy StudyChannelTdcOffsets(
    const analysis_io::InputSpec &input_spec,
    const analysis_time::FineCalib &calib,
    const std::array<int, analysis_time::kFineCalibSize> &valid_vals,
    double clock_mhz,
    int requested_ref_channel,
    double match_window_ns,
    double event_window_ns,
    int min_channels,
    const std::vector<int> &requested_channels)
{
  ChannelTdcOffsetStudy study;
  study.attempted = true;
  study.match_window_ns = match_window_ns > 0.0 ? match_window_ns : 200.0;
  study.event_window_ns = event_window_ns > 0.0 ? event_window_ns : study.match_window_ns;
  study.reference_mode = requested_ref_channel >= 0 ? 0 : 1;
  study.min_channels = std::max(1, min_channels);
  if (requested_channels.empty()) {
    for (int ch = 0; ch < static_cast<int>(study.target_channels.size()); ++ch) {
      study.target_channels[ch] = true;
      study.target_channel_list.push_back(ch);
    }
  } else {
    for (int ch : requested_channels) {
      if (ch < 0 || ch >= static_cast<int>(study.target_channels.size()) || study.target_channels[ch]) {
        continue;
      }
      study.target_channels[ch] = true;
      study.target_channel_list.push_back(ch);
    }
  }
  if (study.reference_mode == 0) {
    study.ref_channel = ChooseOffsetReferenceChannel(valid_vals, requested_ref_channel);
    if (study.ref_channel < 0) {
      std::cout << "Offset study: no valid channel found for reference selection." << std::endl;
      return study;
    }
  } else {
    study.ref_channel = -1;
  }
  if (!calib.loaded || !calib.lut_loaded) {
    std::cout << "Offset study: skipped because no fine-TDC LUT is available." << std::endl;
    return study;
  }

  std::unordered_map<std::string, int> source_ids;
  std::vector<OffsetCursor> cursors;
  cursors.reserve(input_spec.files.size());
  for (const auto &path : input_spec.files) {
    const std::string source = OffsetSourceKey(path);
    auto source_it = source_ids.find(source);
    if (source_it == source_ids.end()) {
      const int next_id = static_cast<int>(source_ids.size());
      source_it = source_ids.emplace(source, next_id).first;
    }
    OffsetCursor cursor;
    if (!InitOffsetCursor(path, input_spec.tree_name, source_it->second, cursor)) {
      continue;
    }
    if (AdvanceOffsetCursor(cursor, calib, analysis_time::TickNs(clock_mhz))) {
      cursors.push_back(std::move(cursor));
      // ROOT stores branch addresses as raw pointers; moving the cursor changes those addresses.
      BindOffsetCursorBranches(cursors.back());
    }
  }
  if (cursors.empty()) {
    std::cout << "Offset study: no readable timing hits found." << std::endl;
    FinalizeOffsetStudy(study);
    return study;
  }

  if (study.reference_mode == 1) {
    std::cout << "Offset study: event median reference, min channels " << study.min_channels
              << ", event window +/-" << study.event_window_ns
              << " ns, match window +/-" << study.match_window_ns << " ns" << std::endl;
  } else {
    std::cout << "Offset study: reference channel " << study.ref_channel
              << ", event window +/-" << study.event_window_ns
              << " ns, match window +/-" << study.match_window_ns << " ns" << std::endl;
  }
  std::cout << "Offset study target channels: " << ChannelListLabel(study.target_channel_list) << std::endl;

  std::vector<OffsetHit> spill_hits;
  while (true) {
    bool any_valid = false;
    uint64_t min_key = std::numeric_limits<uint64_t>::max();
    for (const auto &cursor : cursors) {
      if (!cursor.valid) {
        continue;
      }
      any_valid = true;
      min_key = std::min(min_key, RunSpillKey(cursor.hit.run_id, cursor.hit.spill));
    }
    if (!any_valid) {
      break;
    }
    spill_hits.clear();
    for (auto &cursor : cursors) {
      while (cursor.valid && RunSpillKey(cursor.hit.run_id, cursor.hit.spill) == min_key) {
        spill_hits.push_back(cursor.hit);
        AdvanceOffsetCursor(cursor, calib, analysis_time::TickNs(clock_mhz));
      }
    }
    if (study.reference_mode == 1) {
      ProcessEventMedianOffsetSpill(spill_hits, study);
    } else {
      ProcessChannelReferenceOffsetSpill(spill_hits, study);
    }
  }

  FinalizeOffsetStudy(study);
  if (study.available) {
    std::cout << "Offset study results (median dt = channel - reference):" << std::endl;
    for (const auto &summary : study.summaries) {
      if (!summary.valid) {
        continue;
      }
      const std::string ref_label =
          study.reference_mode == 1 ? " ref event-median" : " ref ch " + std::to_string(summary.ref_channel);
      std::cout << "  ch " << summary.channel << " tdc " << summary.tdc
                << ref_label << " offset=" << summary.offset_ns
                << " ns mean=" << summary.mean_ns << " rms=" << summary.rms_ns
                << " entries=" << summary.entries << std::endl;
    }
  } else {
    std::cout << "Offset study: no channel/TDC pair had enough matched entries." << std::endl;
  }
  return study;
}
}  // namespace

void fine_calibration_rdf(const char *input = "../data/calibration",
                          const char *out_root = "fine_calibration.root",
                          double q_low = 0.01,
                          double q_high = 0.99,
                          long long min_entries = 200,
                          const char *out_pdf = "",
                          double max_duration_ns = 0.0,
                          bool match_coincidence = false,
                          double clock_mhz = 320.0,
                          bool study_channel_offsets = true,
                          int offset_reference_channel = 22,
                          double offset_match_window_ns = 100.0,
                          int offset_min_channels = 3,
                          const char *offset_channels_csv = "",
                          double offset_event_window_ns = 0.0)
{
  if (WantsHelp(input) || WantsHelp(out_root)) {
    std::cout << "fine_calibration_rdf usage:\n";
    std::cout << "  fine_calibration_rdf(\"/path/to/decoded_or_parent\", \"fine_calibration.root\", 0.01, 0.99, 200, \"fine_calibration.pdf\", 0.0, false, 320.0, true, 22, 100.0, 3, \"17,19,22\", 0.0)\n";
    std::cout << "  required branches: type,fifo,column,pixel,tdc,fine\n";
    std::cout << "  match_coincidence=true uses leading edges with valid ToT (needs rollover/coarse, optional spill/run_id)\n";
    std::cout << "  output histograms: hFineMin, hFineMax (bins=" << analysis_time::kFineCalibSize << ")\n";
    std::cout << "  study_channel_offsets=true also writes channel_tdc_offsets and hChannelTdcOffset using calibrated TDC times\n";
    std::cout << "  offset_reference_channel defaults to reference time on ch22; -1 uses an event-median reference\n";
    std::cout << "  offset_event_window_ns defines the event-building window; 0 uses offset_match_window_ns\n";
    std::cout << "  offset_channels_csv limits the offset study to selected channels; empty means all channels\n";
    return;
  }

  ScopedTimer timer("fine_calibration_rdf");
  ROOT::EnableImplicitMT();

  if (q_high < q_low) {
    std::swap(q_high, q_low);
  }

  analysis_io::InputSpec input_spec = analysis_io::ResolveInputSpec(input ? input : "");
  if (input_spec.files.empty()) {
    std::cout << "No decoded ROOT files found under " << (input ? input : "") << std::endl;
    return;
  }

  ROOT::RDataFrame df(input_spec.tree_name.c_str(), input_spec.files);
  auto colnames = df.GetColumnNames();
  if (colnames.empty()) {
    std::cout << "Error: no branches found in tree '" << input_spec.tree_name
              << "'. The decoded file may be empty. Re-run the decoder." << std::endl;
    return;
  }
  std::vector<std::string> missing;
  for (const auto &name : {"type", "fifo", "column", "pixel", "tdc", "fine"}) {
    if (!HasBranch(colnames, name)) {
      missing.emplace_back(name);
    }
  }
  if (!missing.empty()) {
    std::cout << "Error: missing required branches in tree '" << input_spec.tree_name << "': ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i) {
        std::cout << ", ";
      }
      std::cout << missing[i];
    }
    std::cout << std::endl;
    return;
  }

  bool do_match = match_coincidence && max_duration_ns > 0.0;
  if (do_match) {
    std::vector<std::string> extra_missing;
    for (const auto &name : {"rollover", "coarse"}) {
      if (!HasBranch(colnames, name)) {
        extra_missing.emplace_back(name);
      }
    }
    if (!extra_missing.empty()) {
      std::cout << "Warning: match_coincidence requested but missing branches: ";
      for (size_t i = 0; i < extra_missing.size(); ++i) {
        if (i) {
          std::cout << ", ";
        }
        std::cout << extra_missing[i];
      }
      std::cout << ". Falling back to full-hit calibration." << std::endl;
      do_match = false;
    }
  }

  const int bins = analysis_time::kFineBins;
  const int size = analysis_time::kFineCalibSize;
  std::vector<long long> counts(size * bins, 0);
  std::vector<long long> counts_tdc(4 * bins, 0);

  if (do_match) {
    std::cout << "Fine calibration: using leading+trailing edges with valid ToT (max_duration_ns="
              << max_duration_ns << " ns)" << std::endl;
    const double tick_ns = analysis_time::TickNs(clock_mhz);
    bool has_spill = HasBranch(colnames, "spill");
    bool has_run_id = HasBranch(colnames, "run_id");

    ROOT::RDF::RNode df_time = df;
    if (!has_spill) {
      df_time = df_time.Define("spill", "0");
    }
    if (!has_run_id) {
      df_time = df_time.Define("run_id", "0");
    }

    auto types = df_time.Take<int>("type");
    auto fifos = df_time.Take<int>("fifo");
    auto columns = df_time.Take<int>("column");
    auto pixels = df_time.Take<int>("pixel");
    auto tdcs = df_time.Take<int>("tdc");
    auto fines = df_time.Take<int>("fine");
    auto rollovers = df_time.Take<int>("rollover");
    auto coarses = df_time.Take<int>("coarse");
    auto spills = df_time.Take<int>("spill");
    auto run_ids = df_time.Take<int>("run_id");
    ROOT::RDF::RunGraphs({types, fifos, columns, pixels, tdcs, fines, rollovers, coarses, spills, run_ids});

    const auto &types_val = types.GetValue();
    const auto &fifos_val = fifos.GetValue();
    const auto &columns_val = columns.GetValue();
    const auto &pixels_val = pixels.GetValue();
    const auto &tdcs_val = tdcs.GetValue();
    const auto &fines_val = fines.GetValue();
    const auto &rollovers_val = rollovers.GetValue();
    const auto &coarses_val = coarses.GetValue();
    const auto &spills_val = spills.GetValue();
    const auto &run_ids_val = run_ids.GetValue();

    struct Hit {
      int run_id = 0;
      int spill = 0;
      int channel = 0;
      int fifo = 0;
      int column = 0;
      int pixel = 0;
      int tdc = 0;
      int fine = 0;
      long long time_tick = 0;
    };
    std::vector<Hit> hits;
    hits.reserve(types_val.size());

    int current_spill = 0;
    for (size_t i = 0; i < types_val.size(); ++i) {
      const int type = types_val[i];
      int spill = 0;
      int run_id = 0;
      if (!has_spill) {
        if (type == 15) {
          ++current_spill;
          continue;
        }
        if (type != 1) {
          continue;
        }
        spill = current_spill;
      } else {
        if (type != 1) {
          continue;
        }
        spill = spills_val[i];
      }
      if (has_run_id) {
        run_id = run_ids_val[i];
      }
      Hit hit;
      hit.run_id = run_id;
      hit.spill = spill;
      hit.fifo = fifos_val[i];
      hit.column = columns_val[i];
      hit.pixel = pixels_val[i];
      hit.tdc = tdcs_val[i];
      hit.fine = fines_val[i];
      hit.channel = hit.column * 4 + hit.pixel;
      hit.time_tick = analysis_time::TimeTick(rollovers_val[i], coarses_val[i]);
      hits.push_back(hit);
    }

    std::vector<char> leading_mask(hits.size(), 0);
    std::vector<int> leading_to_trailing(hits.size(), -1);
    std::unordered_map<uint64_t, std::vector<size_t>> groups;
    groups.reserve(hits.size());
    for (size_t i = 0; i < hits.size(); ++i) {
      groups[RunSpillKey(hits[i].run_id, hits[i].spill) ^ (static_cast<uint64_t>(hits[i].channel + 1) * 0x9e3779b97f4a7c15ULL)].push_back(i);
    }
    struct EdgeRef {
      long long time_tick = 0;
      int fine = 0;
      int tdc = 0;
      size_t index = 0;
    };
    for (auto &kv : groups) {
      auto &indices = kv.second;
      std::vector<EdgeRef> edges;
      edges.reserve(indices.size());
      for (size_t idx : indices) {
        edges.push_back({hits[idx].time_tick, hits[idx].fine, hits[idx].tdc, idx});
      }
      std::sort(edges.begin(), edges.end(), [](const EdgeRef &a, const EdgeRef &b) {
        if (a.time_tick != b.time_tick) {
          return a.time_tick < b.time_tick;
        }
        return a.fine < b.fine;
      });

      std::array<bool, 2> have_leading = {false, false};
      std::array<double, 2> leading_time_ns = {0.0, 0.0};
      std::array<size_t, 2> leading_idx = {0, 0};
      std::array<int, 2> leading_tdc = {-1, -1};

      for (const auto &edge : edges) {
        if (!IsLeadingTdc(edge.tdc) && !IsTrailingTdc(edge.tdc)) {
          continue;
        }
        if (edge.tdc < 0 || edge.tdc > 3) {
          continue;
        }
        int pair = TdcPairIndex(edge.tdc);
        if (pair < 0 || pair > 1) {
          continue;
        }
        double time_ns = analysis_time::TimeNsFromTick(edge.time_tick, edge.fine, tick_ns, true);
        if (IsLeadingTdc(edge.tdc)) {
          leading_time_ns[pair] = time_ns;
          leading_idx[pair] = edge.index;
          leading_tdc[pair] = edge.tdc;
          have_leading[pair] = true;
          continue;
        }
        if (!have_leading[pair]) {
          continue;
        }
        if (leading_tdc[pair] < 0 || edge.tdc != GetTrailingPartner(leading_tdc[pair])) {
          continue;
        }
        double dt_ns = time_ns - leading_time_ns[pair];
        if (dt_ns > 0.0 && dt_ns <= max_duration_ns) {
          leading_mask[leading_idx[pair]] = 1;
          leading_to_trailing[leading_idx[pair]] = static_cast<int>(edge.index);
        }
        have_leading[pair] = false;
      }
    }

    for (size_t i = 0; i < hits.size(); ++i) {
      if (!leading_mask[i]) {
        continue;
      }
      auto add_hit = [&](size_t idx_hit) {
        const int fine = hits[idx_hit].fine;
        const int tdc = hits[idx_hit].tdc;
        if (fine < 0 || fine >= bins) {
          return;
        }
        if (tdc < 0 || tdc > 3) {
          return;
        }
        const int idx = analysis_time::TdcIndex(hits[idx_hit].fifo, hits[idx_hit].column, hits[idx_hit].pixel,
                                                hits[idx_hit].tdc);
        if (idx < 0) {
          return;
        }
        counts[idx * bins + fine] += 1;
        counts_tdc[tdc * bins + fine] += 1;
      };

      add_hit(i);
      int trailing = (i < leading_to_trailing.size()) ? leading_to_trailing[i] : -1;
      if (trailing >= 0 && trailing < static_cast<int>(hits.size())) {
        add_hit(static_cast<size_t>(trailing));
      }
    }
  } else {
    auto df_hits = df.Filter([](int type) { return type == 1; }, {"type"});
    const unsigned int nslots = df_hits.GetNSlots();
    std::vector<std::vector<int>> slot_counts(nslots, std::vector<int>(size * bins, 0));
    std::vector<std::vector<int>> slot_tdc_counts(nslots, std::vector<int>(4 * bins, 0));

    df_hits.ForeachSlot(
        [&](unsigned int slot, int fifo, int column, int pixel, int tdc, int fine) {
          if (fine < 0 || fine >= bins) {
            return;
          }
          if (tdc < 0 || tdc > 3) {
            return;
          }
          const int idx = analysis_time::TdcIndex(fifo, column, pixel, tdc);
          if (idx < 0) {
            return;
          }
          slot_counts[slot][idx * bins + fine] += 1;
          slot_tdc_counts[slot][tdc * bins + fine] += 1;
        },
        {"fifo", "column", "pixel", "tdc", "fine"});

    for (const auto &slot : slot_counts) {
      for (size_t i = 0; i < counts.size(); ++i) {
        counts[i] += slot[i];
      }
    }
    for (const auto &slot : slot_tdc_counts) {
      for (size_t i = 0; i < counts_tdc.size(); ++i) {
        counts_tdc[i] += slot[i];
      }
    }
  }

  auto out = std::unique_ptr<TFile>(TFile::Open(out_root, "RECREATE"));
  if (!out || out->IsZombie()) {
    std::cout << "Failed to open output file: " << out_root << std::endl;
    return;
  }

  auto hmin = std::make_unique<TH1D>("hFineMin", "Fine calibration min;TDC index;fine min", size, 0.5, size + 0.5);
  auto hmax = std::make_unique<TH1D>("hFineMax", "Fine calibration max;TDC index;fine max", size, 0.5, size + 0.5);
  auto hcount =
      std::make_unique<TH1D>("hFineEntries", "Fine calibration entries;TDC index;entries", size, 0.5, size + 0.5);
  auto hlut = std::make_unique<TH2D>("hFineLut",
                                     "Fine LUT (CDF);TDC index;fine raw;fraction",
                                     size,
                                     0.5,
                                     size + 0.5,
                                     bins,
                                     0.5,
                                     bins + 0.5);
  hmin->SetDirectory(nullptr);
  hmax->SetDirectory(nullptr);
  hcount->SetDirectory(nullptr);
  hlut->SetDirectory(nullptr);
  std::array<std::unique_ptr<TH1D>, 4> htdc{};
  std::array<std::unique_ptr<TH1D>, 4> htdc_cdf{};
  std::array<std::unique_ptr<TH1D>, 4> hfraction_raw_tdc{};
  std::array<std::unique_ptr<TH1D>, 4> hfraction_lut_tdc{};
  for (int t = 0; t < 4; ++t) {
    std::string name = "hFineTdc" + std::to_string(t);
    std::string title = "Fine distribution (TDC " + std::to_string(t) + ");fine;entries";
    auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), bins, 0.5, bins + 0.5);
    hist->SetDirectory(nullptr);
    htdc[t] = std::move(hist);

    std::string cdf_name = "hFineCdfTdc" + std::to_string(t);
    std::string cdf_title = "Fine CDF (TDC " + std::to_string(t) + ");fine;CDF";
    auto cdf_hist = std::make_unique<TH1D>(cdf_name.c_str(), cdf_title.c_str(), bins, 0.5, bins + 0.5);
    cdf_hist->SetDirectory(nullptr);
    htdc_cdf[t] = std::move(cdf_hist);

    std::string raw_name = "hFineFractionRawTdc" + std::to_string(t);
    std::string raw_title = "Fine fraction before LUT (TDC " + std::to_string(t) + ");fine fraction;entries";
    auto raw_hist = std::make_unique<TH1D>(raw_name.c_str(), raw_title.c_str(), bins, -0.5, 0.5);
    raw_hist->SetDirectory(nullptr);
    raw_hist->SetStats(false);
    hfraction_raw_tdc[t] = std::move(raw_hist);

    std::string lut_name = "hFineFractionLutTdc" + std::to_string(t);
    std::string lut_title = "Fine fraction after LUT (TDC " + std::to_string(t) + ");fine fraction;entries";
    auto lut_hist = std::make_unique<TH1D>(lut_name.c_str(), lut_title.c_str(), bins, -0.5, 0.5);
    lut_hist->SetDirectory(nullptr);
    lut_hist->SetStats(false);
    hfraction_lut_tdc[t] = std::move(lut_hist);
  }

  std::array<double, analysis_time::kFineCalibSize> min_vals{};
  std::array<double, analysis_time::kFineCalibSize> max_vals{};
  std::array<long long, analysis_time::kFineCalibSize> entries{};
  std::array<int, analysis_time::kFineCalibSize> valid_vals{};

  long long total_hits = 0;
  int valid = 0;
  for (int idx = 0; idx < size; ++idx) {
    const long long *cptr = counts.data() + idx * bins;
    long long total = 0;
    for (int b = 0; b < bins; ++b) {
      total += cptr[b];
    }
    total_hits += total;
    hcount->SetBinContent(idx + 1, static_cast<double>(total));
    entries[idx] = total;

    if (total < min_entries) {
      hmin->SetBinContent(idx + 1, 0.0);
      hmax->SetBinContent(idx + 1, 0.0);
      min_vals[idx] = 0.0;
      max_vals[idx] = 0.0;
      valid_vals[idx] = 0;
      continue;
    }

    int low_bin = QuantileBin(cptr, bins, total, q_low);
    int high_bin = QuantileBin(cptr, bins, total, q_high);
    if (low_bin < 0 || high_bin < 0 || high_bin <= low_bin) {
      hmin->SetBinContent(idx + 1, 0.0);
      hmax->SetBinContent(idx + 1, 0.0);
      min_vals[idx] = 0.0;
      max_vals[idx] = 0.0;
      valid_vals[idx] = 0;
      continue;
    }

    double min_val = static_cast<double>(std::max(1, low_bin));
    double max_val = static_cast<double>(std::max(min_val + 1.0, static_cast<double>(high_bin)));
    hmin->SetBinContent(idx + 1, min_val);
    hmax->SetBinContent(idx + 1, max_val);
    min_vals[idx] = min_val;
    max_vals[idx] = max_val;
    valid_vals[idx] = 1;
    ++valid;

    long long cum = 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = cptr[b];
      cum += count;
      double frac = 0.0;
      if (total > 0) {
        frac = (static_cast<double>(cum) - 0.5 * static_cast<double>(count)) / static_cast<double>(total);
      }
      if (frac < 0.0) {
        frac = 0.0;
      } else if (frac > 1.0) {
        frac = 1.0;
      }
      const double val = frac - 0.5;
      hlut->SetBinContent(idx + 1, b + 1, val);
    }
  }

  struct TdcProfileInfo {
    int idx;
    int fifo;
    int column;
    int pixel;
    int tdc;
    int channel;
    long long entries;
    std::unique_ptr<TProfile> profile;
  };
  std::vector<TdcProfileInfo> tdc_profiles;
  std::vector<int> candidate_idx;
  const std::vector<int> diagnostic_channels = {17, 19, 22};
  std::vector<std::unique_ptr<TH1D>> hfraction_raw_channel;
  std::vector<std::unique_ptr<TH1D>> hfraction_lut_channel;
  hfraction_raw_channel.reserve(diagnostic_channels.size());
  hfraction_lut_channel.reserve(diagnostic_channels.size());
  for (int channel : diagnostic_channels) {
    std::string raw_name = "hFineFractionRawCh" + std::to_string(channel);
    std::string raw_title = "Fine fraction before LUT (ch " + std::to_string(channel) + ");fine fraction;entries";
    auto raw_hist = std::make_unique<TH1D>(raw_name.c_str(), raw_title.c_str(), bins, -0.5, 0.5);
    raw_hist->SetDirectory(nullptr);
    raw_hist->SetStats(false);
    hfraction_raw_channel.push_back(std::move(raw_hist));

    std::string lut_name = "hFineFractionLutCh" + std::to_string(channel);
    std::string lut_title = "Fine fraction after LUT (ch " + std::to_string(channel) + ");fine fraction;entries";
    auto lut_hist = std::make_unique<TH1D>(lut_name.c_str(), lut_title.c_str(), bins, -0.5, 0.5);
    lut_hist->SetDirectory(nullptr);
    lut_hist->SetStats(false);
    hfraction_lut_channel.push_back(std::move(lut_hist));
  }
  candidate_idx.reserve(size);
  for (int idx = 0; idx < size; ++idx) {
    if (!valid_vals[idx]) {
      continue;
    }
    const int fifo = idx / analysis_time::kTdcPerFifo;
    const int rem = idx % analysis_time::kTdcPerFifo;
    const int column = rem / (analysis_time::kTdcPerPixel * analysis_time::kPixelsPerColumn);
    const int rem2 = rem % (analysis_time::kTdcPerPixel * analysis_time::kPixelsPerColumn);
    const int pixel = rem2 / analysis_time::kTdcPerPixel;
    const int channel = column * 4 + pixel;
    if (std::find(diagnostic_channels.begin(), diagnostic_channels.end(), channel) != diagnostic_channels.end()) {
      candidate_idx.push_back(idx);
    }
  }
  std::sort(candidate_idx.begin(), candidate_idx.end(), [&](int a, int b) {
    return entries[a] > entries[b];
  });
  tdc_profiles.reserve(candidate_idx.size());
  for (int idx : candidate_idx) {
    const int fifo = idx / analysis_time::kTdcPerFifo;
    const int rem = idx % analysis_time::kTdcPerFifo;
    const int column = rem / (analysis_time::kTdcPerPixel * analysis_time::kPixelsPerColumn);
    const int rem2 = rem % (analysis_time::kTdcPerPixel * analysis_time::kPixelsPerColumn);
    const int pixel = rem2 / analysis_time::kTdcPerPixel;
    const int tdc = rem2 % analysis_time::kTdcPerPixel;
    const int channel = column * 4 + pixel;

    std::string name = "pFineCdfTdc" + std::to_string(idx);
    std::string title = "Fine CDF profile (ch " + std::to_string(channel) + ", TDC " + std::to_string(tdc) +
                        ", fifo " + std::to_string(fifo) + ");fine;CDF";
    auto prof = std::make_unique<TProfile>(name.c_str(), title.c_str(), bins, 0.5, bins + 0.5);
    prof->SetDirectory(nullptr);
    prof->SetErrorOption("s");
    for (int b = 0; b < bins; ++b) {
      const double val = hlut->GetBinContent(idx + 1, b + 1);
      const double cdf_val = val + 0.5;
      prof->Fill(static_cast<double>(b + 1), cdf_val);
    }
    tdc_profiles.push_back({idx, fifo, column, pixel, tdc, channel, entries[idx], std::move(prof)});
  }

  for (int idx = 0; idx < size; ++idx) {
    if (!valid_vals[idx]) {
      continue;
    }
    const int fifo = idx / analysis_time::kTdcPerFifo;
    const int local = idx % analysis_time::kTdcPerFifo;
    const int column = local / (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
    const int rem = local % (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
    const int pixel = rem / analysis_time::kTdcPerPixel;
    const int tdc = rem % analysis_time::kTdcPerPixel;
    const int channel = column * 4 + pixel;
    (void)fifo;

    const auto channel_it = std::find(diagnostic_channels.begin(), diagnostic_channels.end(), channel);
    const bool is_diagnostic_channel = channel_it != diagnostic_channels.end();
    const size_t channel_pos = is_diagnostic_channel
                                   ? static_cast<size_t>(std::distance(diagnostic_channels.begin(), channel_it))
                                   : 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = counts[idx * bins + b];
      if (count <= 0) {
        continue;
      }
      const double raw_fraction = (static_cast<double>(b) + 0.5) / static_cast<double>(bins) - 0.5;
      const double lut_fraction = hlut->GetBinContent(idx + 1, b + 1);
      hfraction_raw_tdc[tdc]->Fill(raw_fraction, static_cast<double>(count));
      hfraction_lut_tdc[tdc]->Fill(lut_fraction, static_cast<double>(count));
      if (is_diagnostic_channel) {
        hfraction_raw_channel[channel_pos]->Fill(raw_fraction, static_cast<double>(count));
        hfraction_lut_channel[channel_pos]->Fill(lut_fraction, static_cast<double>(count));
      }
    }
  }

  ChannelTdcOffsetStudy offset_study;
  if (study_channel_offsets) {
    auto derived_fine_calib = BuildFineCalibFromValues(min_vals, max_vals, valid_vals, *hlut);
    const auto offset_channels = ParseChannelsCsv(offset_channels_csv);
    offset_study = StudyChannelTdcOffsets(input_spec,
                                          derived_fine_calib,
                                          valid_vals,
                                          clock_mhz,
                                          offset_reference_channel,
                                          offset_match_window_ns,
                                          offset_event_window_ns,
                                          offset_min_channels,
                                          offset_channels);
  }

  auto tree = std::make_unique<TTree>("fine_calib", "Fine calibration values");
  tree->SetDirectory(nullptr);
  int tdc_index = 0;
  double min_val_tree = 0.0;
  double max_val_tree = 0.0;
  long long entries_tree = 0;
  int valid_tree = 0;
  tree->Branch("tdc_index", &tdc_index, "tdc_index/I");
  tree->Branch("min", &min_val_tree, "min/D");
  tree->Branch("max", &max_val_tree, "max/D");
  tree->Branch("entries", &entries_tree, "entries/L");
  tree->Branch("valid", &valid_tree, "valid/I");

  TParameter<double> p_q_low("q_low", q_low);
  TParameter<double> p_q_high("q_high", q_high);
  TParameter<long long> p_min_entries("min_entries", min_entries);
  TParameter<long long> p_total_hits("total_hits", total_hits);
  TParameter<int> p_valid("valid_tdcs", valid);

  out->cd();
  hmin->Write();
  hmax->Write();
  hcount->Write();
  hlut->Write();
  for (int t = 0; t < 4; ++t) {
    long long total_tdc = 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = counts_tdc[t * bins + b];
      htdc[t]->SetBinContent(b + 1, static_cast<double>(count));
      total_tdc += count;
    }
    long long cum = 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = counts_tdc[t * bins + b];
      cum += count;
      double frac = 0.0;
      if (total_tdc > 0) {
        frac = (static_cast<double>(cum) - 0.5 * static_cast<double>(count)) / static_cast<double>(total_tdc);
      }
      if (frac < 0.0) {
        frac = 0.0;
      } else if (frac > 1.0) {
        frac = 1.0;
      }
      htdc_cdf[t]->SetBinContent(b + 1, frac);
    }
    htdc[t]->Write();
    htdc_cdf[t]->Write();
    hfraction_raw_tdc[t]->Write();
    hfraction_lut_tdc[t]->Write();
  }
  for (size_t i = 0; i < diagnostic_channels.size(); ++i) {
    hfraction_raw_channel[i]->Write();
    hfraction_lut_channel[i]->Write();
  }
  for (auto &item : tdc_profiles) {
    if (item.profile) {
      item.profile->Write();
    }
  }
  if (offset_study.attempted) {
    TParameter<int>("channel_offset_study_enabled", study_channel_offsets ? 1 : 0).Write();
    TParameter<int>("channel_offset_ref_channel", offset_study.ref_channel).Write();
    TParameter<int>("channel_offset_reference_mode", offset_study.reference_mode).Write();
    TParameter<double>("channel_offset_match_window_ns", offset_study.match_window_ns).Write();
    TParameter<double>("channel_offset_event_window_ns", offset_study.event_window_ns).Write();
    TParameter<int>("channel_offset_min_channels", offset_study.min_channels).Write();
    TParameter<int>("channel_offset_available", offset_study.available ? 1 : 0).Write();
    TParameter<int>("channel_offset_n_target_channels",
                    static_cast<int>(offset_study.target_channel_list.size()))
        .Write();
    for (size_t i = 0; i < offset_study.target_channel_list.size(); ++i) {
      TParameter<int>(("channel_offset_target_" + std::to_string(i)).c_str(), offset_study.target_channel_list[i])
          .Write();
    }
    if (offset_study.h_offset) {
      offset_study.h_offset->Write();
    }
    if (offset_study.h_mean) {
      offset_study.h_mean->Write();
    }
    if (offset_study.h_rms) {
      offset_study.h_rms->Write();
    }
    if (offset_study.h_entries) {
      offset_study.h_entries->Write();
    }
    if (offset_study.h_leading_offset) {
      offset_study.h_leading_offset->Write();
    }
    if (offset_study.h_leading_entries) {
      offset_study.h_leading_entries->Write();
    }
    if (offset_study.h_coinc_raw) {
      offset_study.h_coinc_raw->Write();
    }
    if (offset_study.h_coinc_tdc) {
      offset_study.h_coinc_tdc->Write();
    }
    if (offset_study.h_coinc_tdc_offset) {
      offset_study.h_coinc_tdc_offset->Write();
    }
    if (offset_study.tree) {
      offset_study.tree->Write();
    }
    for (auto &kv : offset_study.raw_dt_hists) {
      if (kv.second) {
        kv.second->Write();
      }
    }
    for (auto &kv : offset_study.dt_hists) {
      if (kv.second) {
        kv.second->Write();
      }
    }
  }
  for (int idx = 0; idx < size; ++idx) {
    tdc_index = idx;
    min_val_tree = min_vals[idx];
    max_val_tree = max_vals[idx];
    entries_tree = entries[idx];
    valid_tree = valid_vals[idx];
    tree->Fill();
  }
  tree->Write();
  p_q_low.Write();
  p_q_high.Write();
  p_min_entries.Write();
  p_total_hits.Write();
  p_valid.Write();
  out->Close();

  std::string pdf_path = out_pdf ? out_pdf : "";
  if (!pdf_path.empty()) {
    gStyle->SetOptStat(1110);
    gStyle->SetOptFit(1111);

    auto find_profile_range = [](TProfile *prof,
                                 double y_low,
                                 double y_high,
                                 double xmin,
                                 double xmax,
                                 double &xlow,
                                 double &xhigh) -> bool {
      if (!prof) {
        return false;
      }
      const int nbins = prof->GetNbinsX();
      int first = -1;
      for (int b = 1; b <= nbins; ++b) {
        const double x = prof->GetBinCenter(b);
        if (x < xmin || x > xmax) {
          continue;
        }
        const double y = prof->GetBinContent(b);
        if (y > y_low) {
          first = b;
          break;
        }
      }
      if (first < 0) {
        return false;
      }
      int upper = -1;
      for (int b = first; b <= nbins; ++b) {
        const double x = prof->GetBinCenter(b);
        if (x < xmin || x > xmax) {
          continue;
        }
        const double y = prof->GetBinContent(b);
        if (y > y_high) {
          upper = b;
          break;
        }
      }
      if (upper < 0 || upper <= first) {
        return false;
      }
      xlow = prof->GetBinLowEdge(first);
      xhigh = prof->GetBinLowEdge(upper) + prof->GetBinWidth(upper);
      return true;
    };

    TCanvas c_summary("c_fine_calib_summary", "Fine calibration summary", 1600, 900);
    c_summary.Divide(2, 2);
    c_summary.cd(1);
    hmin->Draw("hist");
    c_summary.cd(2);
    hmax->Draw("hist");
    c_summary.cd(3);
    hcount->Draw("hist");
    c_summary.cd(4);
    gPad->SetLogz(true);
    hlut->Draw("colz");
    c_summary.Print((pdf_path + "[").c_str());
    c_summary.Print(pdf_path.c_str());

    TCanvas c_tdc("c_fine_calib_tdc", "Fine calibration TDC distributions", 1600, 900);
    c_tdc.Divide(2, 2);
    for (int t = 0; t < 4; ++t) {
      c_tdc.cd(t + 1);
      htdc[t]->Draw("hist");
    }
    c_tdc.Print(pdf_path.c_str());

    TCanvas c_tdc_cdf("c_fine_calib_tdc_cdf", "Fine calibration TDC CDFs", 1600, 900);
    c_tdc_cdf.Divide(2, 2);
    for (int t = 0; t < 4; ++t) {
      c_tdc_cdf.cd(t + 1);
      htdc_cdf[t]->Draw("hist");
    }
    c_tdc_cdf.Print(pdf_path.c_str());

    auto draw_before_after_stack = [](TH1D *raw_hist,
                                      TH1D *lut_hist,
                                      const std::string &stack_name,
                                      const std::string &title) {
      auto stack = std::make_unique<THStack>(stack_name.c_str(), title.c_str());
      auto legend = std::make_unique<TLegend>(0.68, 0.76, 0.9, 0.9);
      legend->SetBorderSize(0);
      legend->SetFillStyle(0);
      auto has_content = [](const TH1D *hist) {
        return hist && (hist->GetEntries() > 0.0 || hist->Integral(0, hist->GetNbinsX() + 1) > 0.0);
      };
      int added = 0;
      if (has_content(raw_hist)) {
        raw_hist->SetLineColor(kRed + 1);
        raw_hist->SetLineWidth(2);
        raw_hist->SetStats(false);
        stack->Add(raw_hist, "hist");
        legend->AddEntry(raw_hist, "raw", "l");
        ++added;
      }
      if (has_content(lut_hist)) {
        lut_hist->SetLineColor(kBlue + 1);
        lut_hist->SetLineWidth(2);
        lut_hist->SetStats(false);
        stack->Add(lut_hist, "hist");
        legend->AddEntry(lut_hist, "LUT corrected", "l");
        ++added;
      }
      if (added == 0) {
        gPad->DrawFrame(-0.5, 0.0, 0.5, 1.0, title.c_str());
        return std::make_pair(std::move(stack), std::move(legend));
      }
      stack->Draw("nostack hist");
      legend->Draw();
      return std::make_pair(std::move(stack), std::move(legend));
    };

    TCanvas c_tdc_before_after("c_fine_fraction_before_after_tdc",
                               "Fine fraction before/after TDC LUT",
                               1600,
                               900);
    c_tdc_before_after.Divide(2, 2);
    std::vector<std::unique_ptr<THStack>> tdc_before_after_stacks;
    std::vector<std::unique_ptr<TLegend>> tdc_before_after_legends;
    tdc_before_after_stacks.reserve(4);
    tdc_before_after_legends.reserve(4);
    for (int t = 0; t < 4; ++t) {
      c_tdc_before_after.cd(t + 1);
      auto drawn = draw_before_after_stack(hfraction_raw_tdc[t].get(),
                                           hfraction_lut_tdc[t].get(),
                                           "stack_fine_fraction_before_after_tdc" + std::to_string(t),
                                           "TDC " + std::to_string(t) + ";fine fraction;entries");
      tdc_before_after_stacks.push_back(std::move(drawn.first));
      tdc_before_after_legends.push_back(std::move(drawn.second));
    }
    c_tdc_before_after.Print(pdf_path.c_str());

    TCanvas c_channel_before_after("c_fine_fraction_before_after_channels",
                                   "Fine fraction before/after TDC LUT (channels)",
                                   1600,
                                   900);
    c_channel_before_after.Divide(static_cast<int>(diagnostic_channels.size()), 1);
    std::vector<std::unique_ptr<THStack>> channel_before_after_stacks;
    std::vector<std::unique_ptr<TLegend>> channel_before_after_legends;
    channel_before_after_stacks.reserve(diagnostic_channels.size());
    channel_before_after_legends.reserve(diagnostic_channels.size());
    for (size_t i = 0; i < diagnostic_channels.size(); ++i) {
      c_channel_before_after.cd(static_cast<int>(i + 1));
      auto drawn = draw_before_after_stack(hfraction_raw_channel[i].get(),
                                           hfraction_lut_channel[i].get(),
                                           "stack_fine_fraction_before_after_ch" +
                                               std::to_string(diagnostic_channels[i]),
                                           "channel " + std::to_string(diagnostic_channels[i]) +
                                               ";fine fraction;entries");
      channel_before_after_stacks.push_back(std::move(drawn.first));
      channel_before_after_legends.push_back(std::move(drawn.second));
    }
    c_channel_before_after.Print(pdf_path.c_str());

    if (offset_study.attempted && offset_study.h_offset && offset_study.h_entries) {
      TCanvas c_offset("c_channel_tdc_offsets", "Channel/TDC timing offsets", 1600, 900);
      c_offset.Divide(2, 2);
      c_offset.cd(1);
      gPad->SetRightMargin(0.15);
      offset_study.h_offset->SetMarkerSize(0.8);
      offset_study.h_offset->Draw("colz text");
      c_offset.cd(2);
      gPad->SetRightMargin(0.15);
      gPad->SetLogz(true);
      offset_study.h_entries->Draw("colz");
      c_offset.cd(3);
      if (offset_study.h_leading_offset) {
        offset_study.h_leading_offset->Draw("hist");
      }
      c_offset.cd(4);
      if (offset_study.h_leading_entries) {
        gPad->SetLogy(true);
        offset_study.h_leading_entries->Draw("hist");
      }
      c_offset.Print(pdf_path.c_str());

      if (offset_study.h_coinc_raw && offset_study.h_coinc_tdc && offset_study.h_coinc_tdc_offset &&
          (offset_study.h_coinc_raw->GetEntries() > 0.0 || offset_study.h_coinc_tdc->GetEntries() > 0.0 ||
           offset_study.h_coinc_tdc_offset->GetEntries() > 0.0)) {
        TCanvas c_offset_coinc("c_channel_tdc_offset_coincidences",
                               "Coincidences before/after TDC and offset corrections",
                               1600,
                               900);
        c_offset_coinc.SetLogy(true);
        auto stack = std::make_unique<THStack>("stack_channel_tdc_offset_coincidences",
                                               "Calibration-run coincidences to ch22;#Deltat = t_{ch} - t_{22} [ns];entries");
        auto legend = std::make_unique<TLegend>(0.60, 0.70, 0.90, 0.90);
        legend->SetBorderSize(0);
        legend->SetFillStyle(0);
        auto add_coinc_hist = [&](TH1D *hist, int color, const std::string &label) {
          if (!hist || hist->GetEntries() <= 0.0) {
            return;
          }
          hist->SetStats(false);
          hist->SetLineColor(color);
          hist->SetLineWidth(2);
          stack->Add(hist, "hist");
          std::ostringstream entry;
          entry << label << " RMS=" << std::fixed << std::setprecision(3) << hist->GetRMS() << " ns";
          legend->AddEntry(hist, entry.str().c_str(), "l");
        };
        add_coinc_hist(offset_study.h_coinc_raw.get(), kGray + 2, "raw");
        add_coinc_hist(offset_study.h_coinc_tdc.get(), kBlue + 1, "TDC");
        add_coinc_hist(offset_study.h_coinc_tdc_offset.get(), kRed + 1, "TDC+offset");
        stack->Draw("nostack hist");
        legend->Draw();
        c_offset_coinc.Print(pdf_path.c_str());
      }
    }

    if (!tdc_profiles.empty()) {
      auto build_stack = [&](int channel, const std::string &name, const std::string &title, TLegend &legend) {
        auto stack = std::make_unique<THStack>(name.c_str(), title.c_str());
        const std::array<int, 8> colors = {kBlue + 1, kRed + 1, kGreen + 2, kMagenta + 1, kOrange + 7, kCyan + 2, kViolet + 1, kGray + 2};
        size_t color_idx = 0;
        for (auto &item : tdc_profiles) {
          if (!item.profile || item.channel != channel) {
            continue;
          }
          const int color = colors[color_idx % colors.size()];
          ++color_idx;
          item.profile->SetLineColor(color);
          item.profile->SetLineWidth(2);
          stack->Add(item.profile.get(), "hist");
          std::string label = "ch " + std::to_string(item.channel) + " fifo " + std::to_string(item.fifo) +
                              " tdc " + std::to_string(item.tdc);
          legend.AddEntry(item.profile.get(), label.c_str(), "l");
        }
        return stack;
      };

      TCanvas c_lut_profiles("c_fine_cdf_profiles", "Fine CDF profiles (ch 17/19/22)", 1600, 900);
      c_lut_profiles.Divide(static_cast<int>(diagnostic_channels.size()), 1);
      std::vector<std::unique_ptr<TLegend>> legends;
      std::vector<std::unique_ptr<THStack>> stacks;
      legends.reserve(diagnostic_channels.size());
      stacks.reserve(diagnostic_channels.size());
      for (int channel : diagnostic_channels) {
        auto legend = std::make_unique<TLegend>(0.7, 0.2, 0.9, 0.4);
        legend->SetBorderSize(0);
        legend->SetFillStyle(0);
        auto stack = build_stack(channel,
                                 "stack_fine_cdf_" + std::to_string(channel),
                                 "Fine CDF profiles (ch " + std::to_string(channel) + ");fine;CDF",
                                 *legend);
        legends.push_back(std::move(legend));
        stacks.push_back(std::move(stack));
      }
      for (size_t i = 0; i < diagnostic_channels.size(); ++i) {
        c_lut_profiles.cd(static_cast<int>(i + 1));
        if (stacks[i] && stacks[i]->GetHists() && stacks[i]->GetHists()->GetSize() > 0) {
          stacks[i]->Draw("nostack");
          legends[i]->Draw();
        }
      }
      c_lut_profiles.Print(pdf_path.c_str());

      TCanvas c_lut_profiles_zoom("c_fine_cdf_profiles_zoom", "Fine CDF profiles (ch 17/19/22) zoom", 1600, 900);
      c_lut_profiles_zoom.Divide(static_cast<int>(diagnostic_channels.size()), 1);
      std::vector<std::unique_ptr<TLegend>> legends_zoom;
      std::vector<std::unique_ptr<THStack>> stacks_zoom;
      legends_zoom.reserve(diagnostic_channels.size());
      stacks_zoom.reserve(diagnostic_channels.size());
      for (int channel : diagnostic_channels) {
        auto legend = std::make_unique<TLegend>(0.7, 0.2, 0.9, 0.4);
        legend->SetBorderSize(0);
        legend->SetFillStyle(0);
        auto stack = build_stack(channel,
                                 "stack_fine_cdf_" + std::to_string(channel) + "_zoom",
                                 "Fine CDF profiles (ch " + std::to_string(channel) + ");fine;CDF",
                                 *legend);
        legends_zoom.push_back(std::move(legend));
        stacks_zoom.push_back(std::move(stack));
      }
      for (size_t i = 0; i < diagnostic_channels.size(); ++i) {
        c_lut_profiles_zoom.cd(static_cast<int>(i + 1));
        if (stacks_zoom[i] && stacks_zoom[i]->GetHists() && stacks_zoom[i]->GetHists()->GetSize() > 0) {
          stacks_zoom[i]->Draw("nostack");
          stacks_zoom[i]->GetXaxis()->SetRangeUser(20.0, 120.0);
          legends_zoom[i]->Draw();
        }
      }
      c_lut_profiles_zoom.Print(pdf_path.c_str());
    }

    if (!tdc_profiles.empty()) {
      const std::string png_dir = "report/images";
      gSystem->mkdir(png_dir.c_str(), true);
      gStyle->SetStatX(0.9);
      gStyle->SetStatY(0.4);
      gStyle->SetStatW(0.2);
      gStyle->SetStatH(0.2);
      TCanvas c_lut_single("c_fine_cdf_profiles_single", "Fine CDF profiles (single)", 1600, 900);
      for (auto &item : tdc_profiles) {
        if (!item.profile) {
          continue;
        }
        c_lut_single.cd();
        item.profile->SetLineColor(kBlue + 1);
        item.profile->SetLineWidth(2);
        const double xmin = 20.0;
        const double xmax = 120.0;
        double xlow = xmin;
        double xhigh = xmax;
        bool has_range = find_profile_range(item.profile.get(), 0.03, 0.97, xmin, xmax, xlow, xhigh);
        item.profile->GetXaxis()->SetRangeUser(xmin, xmax);
        item.profile->Draw("E1");
        if (has_range) {
          TF1 fit_func(Form("fit_line_%d", item.idx), "pol1", xlow, xhigh);
          fit_func.SetLineColor(kRed + 1);
          fit_func.SetLineWidth(2);
          fit_func.SetNpx(200);
          item.profile->Fit(&fit_func, "QR");
        }
        gPad->Update();
        if (auto *stats = dynamic_cast<TPaveStats *>(item.profile->FindObject("stats"))) {
          stats->SetX1NDC(0.7);
          stats->SetX2NDC(0.9);
          stats->SetY1NDC(0.2);
          stats->SetY2NDC(0.4);
        }
        gPad->Modified();
        gPad->Update();
        c_lut_single.Print(pdf_path.c_str());
        std::string png_path = png_dir + "/fine_cdf_fit_ch" + std::to_string(item.channel) +
                               "_fifo" + std::to_string(item.fifo) + "_tdc" + std::to_string(item.tdc) + ".png";
        c_lut_single.Print(png_path.c_str());
      }

      std::vector<std::vector<const TdcProfileInfo *>> profiles_by_channel(diagnostic_channels.size());
      for (const auto &item : tdc_profiles) {
        if (!item.profile) {
          continue;
        }
        auto ch_it = std::find(diagnostic_channels.begin(), diagnostic_channels.end(), item.channel);
        if (ch_it != diagnostic_channels.end()) {
          const auto pos = static_cast<size_t>(std::distance(diagnostic_channels.begin(), ch_it));
          profiles_by_channel[pos].push_back(&item);
        }
      }
      auto sort_profiles = [](const TdcProfileInfo *a, const TdcProfileInfo *b) {
        if (a->fifo != b->fifo) {
          return a->fifo < b->fifo;
        }
        return a->tdc < b->tdc;
      };
      size_t rows = 0;
      for (auto &profiles : profiles_by_channel) {
        std::sort(profiles.begin(), profiles.end(), sort_profiles);
        rows = std::max(rows, profiles.size());
      }
      if (rows > 0) {
        gStyle->SetOptStat(1110);
        gStyle->SetOptFit(1111);
        TCanvas c_grid("c_fine_cdf_profiles_grid", "Fine CDF profiles grid", 1600, 900);
        c_grid.Divide(static_cast<int>(diagnostic_channels.size()), static_cast<int>(rows));
        for (size_t r = 0; r < rows; ++r) {
          for (size_t col = 0; col < diagnostic_channels.size(); ++col) {
            const auto &vec = profiles_by_channel[col];
            c_grid.cd(static_cast<int>(r * diagnostic_channels.size() + col + 1));
            if (r >= vec.size()) {
              continue;
            }
            auto *item = vec[r];
            if (!item || !item->profile) {
              continue;
            }
            item->profile->SetLineColor(kBlue + 1);
            item->profile->SetLineWidth(2);
            const double xmin = 20.0;
            const double xmax = 120.0;
            double xlow = xmin;
            double xhigh = xmax;
            bool has_range = find_profile_range(item->profile.get(), 0.03, 0.97, xmin, xmax, xlow, xhigh);
            item->profile->GetXaxis()->SetRangeUser(xmin, xmax);
            item->profile->Draw("E1");
            if (has_range) {
              TF1 fit_func(Form("fit_line_grid_%d", item->idx), "pol1", xlow, xhigh);
              fit_func.SetLineColor(kRed + 1);
              fit_func.SetLineWidth(2);
              fit_func.SetNpx(200);
              item->profile->Fit(&fit_func, "QR");
            }
            gPad->Update();
            if (auto *stats = dynamic_cast<TPaveStats *>(item->profile->FindObject("stats"))) {
              stats->SetX1NDC(0.7);
              stats->SetX2NDC(0.9);
              stats->SetY1NDC(0.2);
              stats->SetY2NDC(0.4);
            }
            gPad->Modified();
            gPad->Update();
          }
        }
        std::string grid_path = png_dir + "/fine_cdf_fit_grid.png";
        c_grid.Print(grid_path.c_str());
      }
    }
    c_tdc.Print((pdf_path + "]").c_str());

    std::cout << "Wrote fine calibration PDF to " << pdf_path << std::endl;
  }

  std::cout << "Wrote fine calibration to " << out_root << std::endl;
  std::cout << "Total hits: " << total_hits << ", valid TDCs: " << valid << " / " << size << std::endl;
  std::cout << "Fine calibration values (valid TDCs only):" << std::endl;
  for (int idx = 0; idx < size; ++idx) {
    if (!valid_vals[idx]) {
      continue;
    }
    std::cout << "  idx " << idx << " min=" << min_vals[idx] << " max=" << max_vals[idx]
              << " entries=" << entries[idx] << std::endl;
  }

  int invalid = 0;
  for (int idx = 0; idx < size; ++idx) {
    if (valid_vals[idx]) {
      continue;
    }
    ++invalid;
  }
  if (invalid > 0) {
    std::cout << "TDC indices with insufficient statistics (min_entries=" << min_entries << "):" << std::endl;
    const int n_fifo = analysis_time::kFineCalibSize / analysis_time::kTdcPerFifo;
    std::vector<long long> fifo_entries(n_fifo, 0);
    for (int idx = 0; idx < size; ++idx) {
      const int fifo = idx / analysis_time::kTdcPerFifo;
      if (fifo >= 0 && fifo < n_fifo) {
        fifo_entries[fifo] += entries[idx];
      }
    }
    std::vector<bool> fifo_empty(n_fifo, false);
    for (int fifo = 0; fifo < n_fifo; ++fifo) {
      if (fifo_entries[fifo] == 0) {
        fifo_empty[fifo] = true;
        std::cout << "  fifo " << fifo << " entries=0 (all TDC indices skipped)" << std::endl;
      }
    }
    for (int idx = 0; idx < size; ++idx) {
      if (valid_vals[idx]) {
        continue;
      }
      const int fifo = idx / analysis_time::kTdcPerFifo;
      if (fifo >= 0 && fifo < n_fifo && fifo_empty[fifo]) {
        continue;
      }
      const int local = idx % analysis_time::kTdcPerFifo;
      const int column = local / (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
      const int rem = local % (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
      const int pixel = rem / analysis_time::kTdcPerPixel;
      const int tdc = rem % analysis_time::kTdcPerPixel;
      std::cout << "  idx " << idx
                << " fifo=" << fifo
                << " column=" << column
                << " pixel=" << pixel
                << " tdc=" << tdc
                << " entries=" << entries[idx] << std::endl;
    }
  }
}
