#include <ROOT/RDataFrame.hxx>
#include <TFile.h>
#include <TProfile.h>
#include <TParameter.h>
#include <TF1.h>
#include <TSystem.h>

#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
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

uint64_t GroupKey(int run_id, int channel, int spill)
{
  uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(run_id));
  key <<= 32;
  key |= static_cast<uint32_t>(spill);
  key ^= static_cast<uint64_t>(channel + 1) * 0x9e3779b97f4a7c15ULL;
  return key;
}

uint64_t RunSpillKey(int run_id, int spill)
{
  uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(run_id));
  key <<= 32;
  key |= static_cast<uint32_t>(spill);
  return key;
}

struct Hit {
  int run_id = 0;
  int channel = -1;
  int fifo = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int spill = 0;
  long long time_tick = 0;
  int fine = 0;
  double time_ns = 0.0;
};

struct DurationInfo {
  std::vector<char> leading_mask;
  std::vector<int> leading_to_trailing;
};

DurationInfo ComputeDurationInfo(const std::vector<Hit> &hits,
                                 const analysis_time::FineCalib &fine_calib,
                                 double tick_ns,
                                 double max_duration_ns,
                                 bool use_fine)
{
  DurationInfo info;
  info.leading_mask.assign(hits.size(), 0);
  info.leading_to_trailing.assign(hits.size(), -1);
  if (hits.empty()) {
    return info;
  }

  std::unordered_map<uint64_t, std::vector<size_t>> groups;
  groups.reserve(hits.size());
  for (size_t i = 0; i < hits.size(); ++i) {
    groups[GroupKey(hits[i].run_id, hits[i].channel, hits[i].spill)].push_back(i);
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
      if (!IsValidTdcId(edge.tdc)) {
        continue;
      }
      int pair = TdcPairIndex(edge.tdc);
      if (pair < 0 || pair > 1) {
        continue;
      }
      const auto &hit = hits[edge.index];
      int tdc_index = analysis_time::TdcIndex(hit.fifo, hit.column, hit.pixel, hit.tdc);
      double time_ns =
          analysis_time::TimeNsFromTick(fine_calib, edge.time_tick, edge.fine, tdc_index, tick_ns, use_fine);
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
      if (leading_tdc[pair] < 0 || edge.tdc != (leading_tdc[pair] ^ 0x1)) {
        continue;
      }
      double dt_ns = time_ns - leading_time_ns[pair];
      if (dt_ns > 0.0 && dt_ns <= max_duration_ns) {
        info.leading_mask[leading_idx[pair]] = 1;
        info.leading_to_trailing[leading_idx[pair]] = static_cast<int>(edge.index);
      }
      have_leading[pair] = false;
    }
  }

  return info;
}

struct RefHit {
  double time_ns = 0.0;
  double tot_ns = -1.0;
};

struct CalibHit {
  double time_ns = 0.0;
  double tot_ns = 0.0;
};

double ProfileValue(const TProfile *prof, double x)
{
  if (!prof) {
    return 0.0;
  }
  const int bins = prof->GetNbinsX();
  if (bins <= 0) {
    return 0.0;
  }
  if (x <= prof->GetXaxis()->GetXmin()) {
    return prof->GetBinContent(1);
  }
  if (x >= prof->GetXaxis()->GetXmax()) {
    return prof->GetBinContent(bins);
  }
  int bin = prof->GetXaxis()->FindBin(x);
  if (bin < 1) {
    bin = 1;
  } else if (bin > bins) {
    bin = bins;
  }
  return prof->GetBinContent(bin);
}

double FitGaussianMean(TH1D *hist, double range)
{
  if (!hist) {
    return 0.0;
  }
  if (hist->GetEntries() < 50) {
    return hist->GetMean();
  }
  const double r = range > 0.0 ? range : hist->GetXaxis()->GetXmax();
  TF1 gaus("gaus_fit", "gaus", -r, r);
  gaus.SetParameters(hist->GetMaximum(), hist->GetMean(), hist->GetRMS());
  int status = hist->Fit(&gaus, "Q0");
  if (status == 0) {
    return gaus.GetParameter(1);
  }
  return hist->GetMean();
}

bool NearestDelta(const std::vector<RefHit> &refs,
                  double time_ns,
                  double window_ns,
                  double &dt_out,
                  double &ref_tot_out)
{
  dt_out = 0.0;
  ref_tot_out = -1.0;
  if (refs.empty()) {
    return false;
  }
  auto it = std::lower_bound(refs.begin(), refs.end(), time_ns,
                             [](const RefHit &a, double v) { return a.time_ns < v; });
  double best_dt = 0.0;
  double best_tot = -1.0;
  double best_abs = std::numeric_limits<double>::max();
  if (it != refs.end()) {
    double dt = time_ns - it->time_ns;
    double adt = std::abs(dt);
    if (adt < best_abs) {
      best_abs = adt;
      best_dt = dt;
      best_tot = it->tot_ns;
    }
  }
  if (it != refs.begin()) {
    auto prev = std::prev(it);
    double dt = time_ns - prev->time_ns;
    double adt = std::abs(dt);
    if (adt < best_abs) {
      best_abs = adt;
      best_dt = dt;
      best_tot = prev->tot_ns;
    }
  }
  if (best_abs <= window_ns) {
    dt_out = best_dt;
    ref_tot_out = best_tot;
    return true;
  }
  return false;
}
}  // namespace

void channel_calibration_rdf(const char *input = "../data/calibration",
                             const char *out_root = "channel_calibration.root",
                             int ref_channel = 19,
                             double window_ns = 40.0,
                             double max_duration_ns = 20.0,
                             int tot_bins = 60,
                             double clock_mhz = 320.0,
                             bool use_fine = true,
                             const char *fine_calib_path = "",
                             bool use_lut = true,
                             bool symmetrize_ref = true)
{
  if (WantsHelp(input) || WantsHelp(out_root)) {
    std::cout << "channel_calibration_rdf usage:\n";
    std::cout << "  channel_calibration_rdf(\"/path/to/decoded_or_parent\", \"channel_calibration.root\", 19,"
              << " 40.0, 20.0, 60, 320.0, true, \"fine_calibration.root\", true, true)\n";
    std::cout << "  required branches: type,fifo,column,pixel,tdc,rollover,coarse,fine\n";
    std::cout << "  use_lut=false disables LUT even if hFineLut is present\n";
    std::cout << "  symmetrize_ref=true splits correction between ref and channel (mean reference)\n";
    return;
  }

  ScopedTimer timer("channel_calibration_rdf");
  ROOT::EnableImplicitMT();

  if (ref_channel < 0 || ref_channel > 31) {
    std::cout << "Invalid reference channel: " << ref_channel << std::endl;
    return;
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
  for (const auto &name : {"type", "fifo", "column", "pixel", "tdc", "rollover", "coarse", "fine"}) {
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

  bool has_channel = HasBranch(colnames, "channel");
  bool has_time_tick = HasBranch(colnames, "time_tick");
  bool has_spill = HasBranch(colnames, "spill");
  bool has_run_id = HasBranch(colnames, "run_id");

  ROOT::RDF::RNode df_time = df;
  if (!has_channel) {
    df_time = df_time.Define("channel", "column * 4 + pixel");
  }
  if (!has_time_tick) {
    df_time = df_time.Define("time_tick", analysis_time::TimeTickLambda(), {"rollover", "coarse"});
  }
  if (!has_spill) {
    df_time = df_time.Define("spill", "0");
  }
  if (!has_run_id) {
    df_time = df_time.Define("run_id", "0");
  }

  auto types = df_time.Take<int>("type");
  auto channels_v = df_time.Take<int>("channel");
  auto ticks = df_time.Take<Long64_t>("time_tick");
  auto fines = df_time.Take<int>("fine");
  auto spills = df_time.Take<int>("spill");
  auto run_ids = df_time.Take<int>("run_id");
  auto fifos = df_time.Take<int>("fifo");
  auto columns = df_time.Take<int>("column");
  auto pixels = df_time.Take<int>("pixel");
  auto tdcs = df_time.Take<int>("tdc");
  auto rollovers = df_time.Take<int>("rollover");
  auto coarses = df_time.Take<int>("coarse");
  ROOT::RDF::RunGraphs({types, channels_v, ticks, fines, spills, run_ids, fifos, columns, pixels, tdcs, rollovers,
                        coarses});

  const auto &types_val = types.GetValue();
  const auto &channels_val = channels_v.GetValue();
  const auto &ticks_val = ticks.GetValue();
  const auto &fines_val = fines.GetValue();
  const auto &spills_val = spills.GetValue();
  const auto &run_ids_val = run_ids.GetValue();
  const auto &fifos_val = fifos.GetValue();
  const auto &columns_val = columns.GetValue();
  const auto &pixels_val = pixels.GetValue();
  const auto &tdcs_val = tdcs.GetValue();
  const auto &rollovers_val = rollovers.GetValue();
  const auto &coarses_val = coarses.GetValue();

  const double tick_ns = analysis_time::TickNs(clock_mhz);
  analysis_time::FineCalib fine_calib;
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    if (fine_calib.LoadFromFile(fine_calib_path)) {
      std::cout << "Loaded fine calibration: " << fine_calib_path << std::endl;
    } else {
      std::cout << "Failed to load fine calibration: " << fine_calib_path << " (using default formula)" << std::endl;
    }
  }
  fine_calib.use_lut = use_lut;
  analysis_time::PrintFineCalibConstants(fine_calib);
  if (max_duration_ns < 0.0) {
    max_duration_ns = 0.0;
  }
  if (tot_bins < 1) {
    tot_bins = 1;
  }

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
    hit.channel = channels_val[i];
    hit.fifo = fifos_val[i];
    hit.column = columns_val[i];
    hit.pixel = pixels_val[i];
    hit.tdc = tdcs_val[i];
    hit.spill = spill;
    hit.time_tick = ticks_val[i];
    hit.fine = fines_val[i];
    int tdc_index = analysis_time::TdcIndex(hit.fifo, hit.column, hit.pixel, hit.tdc);
    hit.time_ns =
        analysis_time::TimeNsFromTick(fine_calib, ticks_val[i], fines_val[i], tdc_index, tick_ns, use_fine);
    hits.push_back(hit);
  }

  std::vector<char> leading_mask;
  std::vector<int> leading_to_trailing;
  if (max_duration_ns > 0.0) {
    auto duration_info = ComputeDurationInfo(hits, fine_calib, tick_ns, max_duration_ns, use_fine);
    leading_mask = std::move(duration_info.leading_mask);
    leading_to_trailing = std::move(duration_info.leading_to_trailing);
  } else {
    std::cout << "max_duration_ns <= 0, cannot compute ToT for calibration." << std::endl;
    return;
  }

  std::vector<double> leading_tot(hits.size(), -1.0);
  for (size_t i = 0; i < hits.size(); ++i) {
    if (i >= leading_mask.size() || !leading_mask[i]) {
      continue;
    }
    int trailing = (i < leading_to_trailing.size()) ? leading_to_trailing[i] : -1;
    if (trailing < 0 || trailing >= static_cast<int>(hits.size())) {
      continue;
    }
    double dt = hits[trailing].time_ns - hits[i].time_ns;
    if (dt <= 0.0 || dt > max_duration_ns) {
      continue;
    }
    leading_tot[i] = dt;
  }

  std::unordered_map<uint64_t, std::vector<RefHit>> ref_hits;
  std::array<std::unordered_map<uint64_t, std::vector<CalibHit>>, 32> hits_by_channel;

  for (size_t i = 0; i < hits.size(); ++i) {
    if (i >= leading_mask.size() || !leading_mask[i]) {
      continue;
    }
    double tot = leading_tot[i];
    if (tot <= 0.0 || tot > max_duration_ns) {
      continue;
    }
    const Hit &hit = hits[i];
    const uint64_t key = RunSpillKey(hit.run_id, hit.spill);
    if (hit.channel == ref_channel) {
      double ref_tot = tot;
      if (ref_tot <= 0.0 || ref_tot > max_duration_ns) {
        ref_tot = -1.0;
      }
      ref_hits[key].push_back({hit.time_ns, ref_tot});
    } else if (hit.channel >= 0 && hit.channel < 32) {
      hits_by_channel[hit.channel][key].push_back({hit.time_ns, tot});
    }
  }

  for (auto &kv : ref_hits) {
    auto &vec = kv.second;
    std::sort(vec.begin(), vec.end(), [](const RefHit &a, const RefHit &b) { return a.time_ns < b.time_ns; });
  }

  std::vector<std::unique_ptr<TProfile>> profiles;
  profiles.reserve(32);
  for (int ch = 0; ch < 32; ++ch) {
    std::string name = "hChanCalib_ch" + std::to_string(ch);
    std::string title = "Channel calibration ch" + std::to_string(ch) + ";ToT [ns];#Delta t [ns]";
    auto prof = std::make_unique<TProfile>(name.c_str(), title.c_str(), tot_bins, 0.0, max_duration_ns);
    prof->SetDirectory(nullptr);
    profiles.push_back(std::move(prof));
  }

  const double symm_scale = symmetrize_ref ? 0.5 : 1.0;
  for (int ch = 0; ch < 32; ++ch) {
    if (ch == ref_channel) {
      continue;
    }
    for (auto &kv : hits_by_channel[ch]) {
      auto it_ref = ref_hits.find(kv.first);
      if (it_ref == ref_hits.end()) {
        continue;
      }
      const auto &refs = it_ref->second;
      for (const auto &hit : kv.second) {
        double dt = 0.0;
        double ref_tot = -1.0;
        bool ok = NearestDelta(refs, hit.time_ns, window_ns, dt, ref_tot);
        if (!ok) {
          continue;
        }
        if (hit.tot_ns <= 0.0 || hit.tot_ns > max_duration_ns) {
          continue;
        }
        profiles[ch]->Fill(hit.tot_ns, dt * symm_scale);
        if (symmetrize_ref && ref_tot > 0.0 && ref_tot <= max_duration_ns) {
          profiles[ref_channel]->Fill(ref_tot, -dt * symm_scale);
        }
      }
    }
  }

  std::array<double, 32> offset_mu{};
  std::array<double, 32> offset_w{};
  std::array<std::unique_ptr<TH1D>, 32> h_offset{};
  const int offset_bins = 200;
  const double offset_range = window_ns;
  for (int ch = 0; ch < 32; ++ch) {
    std::string name = "hChanOffset_ch" + std::to_string(ch);
    std::string title = "Channel offset ch" + std::to_string(ch) + ";#Delta t residual [ns];entries";
    auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), offset_bins, -offset_range, offset_range);
    hist->SetDirectory(nullptr);
    h_offset[ch] = std::move(hist);
  }

  for (int ch = 0; ch < 32; ++ch) {
    if (ch == ref_channel) {
      continue;
    }
    for (auto &kv : hits_by_channel[ch]) {
      auto it_ref = ref_hits.find(kv.first);
      if (it_ref == ref_hits.end()) {
        continue;
      }
      const auto &refs = it_ref->second;
      for (const auto &hit : kv.second) {
        double dt = 0.0;
        double ref_tot = -1.0;
        bool ok = NearestDelta(refs, hit.time_ns, window_ns, dt, ref_tot);
        if (!ok) {
          continue;
        }
        if (hit.tot_ns <= 0.0 || hit.tot_ns > max_duration_ns) {
          continue;
        }
        if (symmetrize_ref && !(ref_tot > 0.0 && ref_tot <= max_duration_ns)) {
          continue;
        }
        const double corr_ch = ProfileValue(profiles[ch].get(), hit.tot_ns);
        const double corr_ref = symmetrize_ref ? ProfileValue(profiles[ref_channel].get(), ref_tot) : 0.0;
        const double dt_resid = dt - (corr_ch - corr_ref);
        h_offset[ch]->Fill(dt_resid);
      }
    }
  }

  for (int ch = 0; ch < 32; ++ch) {
    if (ch == ref_channel) {
      continue;
    }
    if (!h_offset[ch]) {
      continue;
    }
    const double mu = FitGaussianMean(h_offset[ch].get(), offset_range);
    const double entries = h_offset[ch]->GetEntries();
    offset_mu[ch] = mu;
    offset_w[ch] = entries;
  }

  double mean_mu = 0.0;
  double mean_w = 0.0;
  for (int ch = 0; ch < 32; ++ch) {
    if (ch == ref_channel) {
      continue;
    }
    if (offset_w[ch] <= 0.0) {
      continue;
    }
    mean_mu += offset_mu[ch] * offset_w[ch];
    mean_w += offset_w[ch];
  }
  if (mean_w > 0.0) {
    mean_mu /= mean_w;
  }

  std::array<double, 32> offsets{};
  for (int ch = 0; ch < 32; ++ch) {
    offsets[ch] = 0.0;
  }
  for (int ch = 0; ch < 32; ++ch) {
    if (ch == ref_channel) {
      continue;
    }
    if (offset_w[ch] <= 0.0) {
      continue;
    }
    if (symmetrize_ref) {
      offsets[ch] = 0.5 * offset_mu[ch];
    } else {
      offsets[ch] = offset_mu[ch];
    }
  }
  if (symmetrize_ref && mean_w > 0.0) {
    offsets[ref_channel] = -0.5 * mean_mu;
  }

  auto out = std::unique_ptr<TFile>(TFile::Open(out_root, "RECREATE"));
  if (!out || out->IsZombie()) {
    std::cout << "Failed to open output file: " << out_root << std::endl;
    return;
  }

  out->cd();
  auto h_offset_summary =
      std::make_unique<TH1D>("hChanOffset", "Channel time offsets;channel;offset [ns]", 32, -0.5, 31.5);
  h_offset_summary->SetDirectory(nullptr);
  for (int ch = 0; ch < 32; ++ch) {
    h_offset_summary->SetBinContent(ch + 1, offsets[ch]);
  }
  h_offset_summary->Write();
  for (auto &prof : profiles) {
    prof->Write();
  }
  for (int ch = 0; ch < 32; ++ch) {
    if (h_offset[ch]) {
      h_offset[ch]->Write();
    }
  }
  TParameter<int> p_ref("ref_channel", ref_channel);
  TParameter<double> p_window("window_ns", window_ns);
  TParameter<double> p_maxdur("max_duration_ns", max_duration_ns);
  TParameter<int> p_bins("tot_bins", tot_bins);
  TParameter<int> p_sym("symmetrize_ref", symmetrize_ref ? 1 : 0);
  TParameter<double> p_offset_mean("offset_mean_mu", mean_mu);
  p_ref.Write();
  p_window.Write();
  p_maxdur.Write();
  p_bins.Write();
  p_sym.Write();
  p_offset_mean.Write();
  out->Close();

  std::cout << "Wrote channel calibration to " << out_root << std::endl;
}
