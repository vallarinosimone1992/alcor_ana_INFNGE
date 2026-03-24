#include <ROOT/RDataFrame.hxx>
#include <TCanvas.h>
#include <TH1D.h>
#include <TH2D.h>
#include <THStack.h>
#include <TLegend.h>
#include <TStyle.h>
#include <TSystem.h>

#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
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

template <typename... Ts>
void RunGraphsCompat(Ts &...results)
{
  int dummy[] = {(results.GetValue(), 0)...};
  (void)dummy;
}

bool WantsHelp(const char *arg)
{
  if (!arg) {
    return true;
  }
  std::string val(arg);
  return val == "-h" || val == "--help" || val == "help";
}

struct PairConfig {
  int ch_a = -1;
  int ch_b = -1;
  double window_ns = 10.0;
};

bool LoadPairs(const std::string &path,
               double default_window_ns,
               std::vector<PairConfig> &pairs,
               std::unordered_set<int> &channels)
{
  std::ifstream in(path);
  if (!in) {
    std::cerr << "Failed to open pairs file: " << path << std::endl;
    return false;
  }
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::stringstream ss(line);
    std::string first;
    if (!(ss >> first)) {
      continue;
    }
    if (first == "group" || first == "multi" || first == "nfold") {
      continue;
    }
    int ch1 = -1;
    int ch2 = -1;
    try {
      ch1 = std::stoi(first);
    } catch (...) {
      std::cerr << "Skipping line " << line_no << " (invalid channel)" << std::endl;
      continue;
    }
    if (!(ss >> ch2)) {
      std::cerr << "Skipping line " << line_no << " (missing channel)" << std::endl;
      continue;
    }
    double window_ns = default_window_ns;
    if (ss >> window_ns) {
      if (window_ns <= 0.0) {
        window_ns = default_window_ns;
      }
    }
    PairConfig cfg;
    cfg.ch_a = ch1;
    cfg.ch_b = ch2;
    cfg.window_ns = window_ns;
    pairs.push_back(cfg);
    channels.insert(ch1);
    channels.insert(ch2);
  }
  return !pairs.empty();
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

struct LeadHit {
  double time_raw = 0.0;
  double time_corr = 0.0;
  double tot = -1.0;
};

struct Hit {
  int channel = -1;
  int fifo = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int spill = 0;
  long long time_tick = 0;
  int fine = 0;
  double time_ns_raw = 0.0;
  double time_ns_corr = 0.0;
  double tot = -1.0;
  bool leading = false;
};

bool FindNearestInWindow(const std::vector<LeadHit> &hits,
                          double t,
                          double window_ns,
                          LeadHit &out,
                          double *best_abs_out = nullptr)
{
  if (hits.empty()) {
    return false;
  }
  auto it = std::lower_bound(hits.begin(), hits.end(), t, [](const LeadHit &h, double val) {
    return h.time_raw < val;
  });
  double best_abs = window_ns + 1.0;
  const LeadHit *best = nullptr;
  if (it != hits.end()) {
    double dt = it->time_raw - t;
    double adt = std::abs(dt);
    if (adt < best_abs) {
      best_abs = adt;
      best = &(*it);
    }
  }
  if (it != hits.begin()) {
    auto prev = std::prev(it);
    double dt = prev->time_raw - t;
    double adt = std::abs(dt);
    if (adt < best_abs) {
      best_abs = adt;
      best = &(*prev);
    }
  }
  if (best_abs_out) {
    *best_abs_out = best_abs;
  }
  if (best && best_abs <= window_ns) {
    out = *best;
    return true;
  }
  return false;
}

void GridForCount(size_t count, int &cols, int &rows)
{
  if (count == 0) {
    cols = 1;
    rows = 1;
    return;
  }
  cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
  rows = static_cast<int>(std::ceil(static_cast<double>(count) / cols));
}
}  // namespace

void tw_validation_rdf(const char *input = "../data/calibration",
                       const char *pairs_file = "../config/coincidence_pairs.txt",
                       const char *out_pdf = "tw_validation.pdf",
                       double default_window_ns = 10.0,
                       double clock_mhz = 320.0,
                       bool use_fine = true,
                       double max_duration_ns = 15.0,
                       const char *fine_calib_path = "",
                       const char *chan_calib_path = "",
                       int fine_cut = analysis_time::kDefaultFineCut,
                       bool use_lut = true)
{
  if (WantsHelp(input) || WantsHelp(pairs_file)) {
    std::cout << "tw_validation_rdf usage:\n";
    std::cout << "  tw_validation_rdf(\"/path/to/decoded\", \"pairs.txt\", \"tw_validation.pdf\", 10, 320, true, 15, \"fine_calibration.root\", \"channel_calibration.root\", 0, true)\n";
    return;
  }

  ScopedTimer timer("tw_validation_rdf");
  ROOT::EnableImplicitMT();

  analysis_io::InputSpec input_spec = analysis_io::ResolveInputSpec(input ? input : "");
  if (input_spec.files.empty()) {
    std::cout << "No decoded ROOT files found under " << (input ? input : "") << std::endl;
    return;
  }

  std::vector<PairConfig> pairs;
  std::unordered_set<int> channels_set;
  if (!LoadPairs(pairs_file ? pairs_file : "", default_window_ns, pairs, channels_set)) {
    std::cout << "No valid channel pairs found in " << (pairs_file ? pairs_file : "") << std::endl;
    return;
  }

  const bool use_fine_flag = (use_fine != 0);
  if (max_duration_ns < 0.0) {
    max_duration_ns = 0.0;
  }

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

  analysis_time::ChannelCalib chan_calib;
  if (chan_calib_path && chan_calib_path[0] != '\0') {
    if (chan_calib.LoadFromFile(chan_calib_path)) {
      std::cout << "Loaded channel calibration: " << chan_calib_path << std::endl;
    } else {
      std::cout << "Failed to load channel calibration: " << chan_calib_path << " (ignored)" << std::endl;
    }
  }
  analysis_time::PrintChannelCalibSummary(chan_calib);

  ROOT::RDataFrame df(input_spec.tree_name.c_str(), input_spec.files);
  auto cols = df.GetColumnNames();
  if (cols.empty()) {
    std::cout << "Error: no branches found in tree '" << input_spec.tree_name
              << "'. The decoded file may be empty. Re-run the decoder." << std::endl;
    return;
  }

  bool has_channel = std::find(cols.begin(), cols.end(), "channel") != cols.end();
  bool has_spill = std::find(cols.begin(), cols.end(), "spill") != cols.end();

  auto df_hits = df.Filter("type == 1");
  auto df_hits_ch = df_hits;
  if (!has_channel) {
    df_hits_ch = df_hits.Define("channel", "column * 4 + pixel");
  }

  ROOT::RDF::RNode df_hits_cut = df_hits_ch;
  if (use_fine_flag && fine_cut > 0) {
    auto fine_cut_lambda = [&fine_calib, fine_cut](int fine, int fifo, int column, int pixel, int tdc) -> bool {
      const int tdc_index = analysis_time::TdcIndex(fifo, column, pixel, tdc);
      return analysis_time::PassFineCut(fine_calib, fine, tdc_index, fine_cut);
    };
    df_hits_cut = df_hits_ch.Filter(fine_cut_lambda, {"fine", "fifo", "column", "pixel", "tdc"});
  }

  auto channels = df_hits_cut.Take<int>("channel");
  auto ticks = df_hits_cut.Define("time_tick", analysis_time::TimeTickLambda(), {"rollover", "coarse"}).Take<Long64_t>("time_tick");
  auto fifos = df_hits_cut.Take<int>("fifo");
  auto columns = df_hits_cut.Take<int>("column");
  auto pixels = df_hits_cut.Take<int>("pixel");
  auto tdcs = df_hits_cut.Take<int>("tdc");
  auto fines = df_hits_cut.Take<int>("fine");
  RunGraphsCompat(channels, ticks, fifos, columns, pixels, tdcs, fines);

  std::vector<int> spills_vec;
  if (has_spill) {
    auto spills_take = df_hits_cut.Take<int>("spill");
    RunGraphsCompat(spills_take);
    spills_vec = spills_take.GetValue();
  }

  const auto &channels_val = channels.GetValue();
  const auto &ticks_val = ticks.GetValue();
  const auto &fifos_val = fifos.GetValue();
  const auto &columns_val = columns.GetValue();
  const auto &pixels_val = pixels.GetValue();
  const auto &tdcs_val = tdcs.GetValue();
  const auto &fines_val = fines.GetValue();

  const double tick_ns = analysis_time::TickNs(clock_mhz);

  std::vector<Hit> hits;
  hits.reserve(channels_val.size());
  int current_spill = 0;
  for (size_t i = 0; i < channels_val.size(); ++i) {
    int ch = channels_val[i];
    if (channels_set.find(ch) == channels_set.end()) {
      continue;
    }
    int spill = 0;
    if (has_spill) {
      spill = (i < spills_vec.size()) ? spills_vec[i] : 0;
    } else {
      spill = current_spill;
    }
    Hit hit;
    hit.channel = ch;
    hit.fifo = fifos_val[i];
    hit.column = columns_val[i];
    hit.pixel = pixels_val[i];
    hit.tdc = tdcs_val[i];
    hit.spill = spill;
    hit.time_tick = ticks_val[i];
    hit.fine = fines_val[i];
    int tdc_index = analysis_time::TdcIndex(hit.fifo, hit.column, hit.pixel, hit.tdc);
    hit.time_ns_raw = analysis_time::TimeNsFromTick(fine_calib, hit.time_tick, hit.fine, tdc_index,
                                                    tick_ns, use_fine_flag);
    hit.time_ns_corr = hit.time_ns_raw;
    hits.push_back(hit);
  }

  std::unordered_map<long long, std::vector<size_t>> groups;
  groups.reserve(hits.size());
  for (size_t i = 0; i < hits.size(); ++i) {
    long long key = static_cast<long long>(hits[i].channel) * 1000000LL + hits[i].spill;
    groups[key].push_back(i);
  }

  for (auto &kv : groups) {
    auto &indices = kv.second;
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
      if (hits[a].time_tick != hits[b].time_tick) {
        return hits[a].time_tick < hits[b].time_tick;
      }
      return hits[a].fine < hits[b].fine;
    });
    std::array<bool, 2> have_leading = {false, false};
    std::array<double, 2> leading_time = {0.0, 0.0};
    std::array<int, 2> leading_tdc = {-1, -1};
    std::array<size_t, 2> leading_idx = {0, 0};
    for (size_t idx : indices) {
      const auto &hit = hits[idx];
      if (!IsLeadingTdc(hit.tdc) && !IsTrailingTdc(hit.tdc)) {
        continue;
      }
      if (!IsValidTdcId(hit.tdc)) {
        continue;
      }
      int pair = TdcPairIndex(hit.tdc);
      if (pair < 0 || pair > 1) {
        continue;
      }
      if (IsLeadingTdc(hit.tdc)) {
        have_leading[pair] = true;
        leading_time[pair] = hit.time_ns_raw;
        leading_tdc[pair] = hit.tdc;
        leading_idx[pair] = idx;
        hits[idx].leading = true;
        continue;
      }
      if (!have_leading[pair]) {
        continue;
      }
      if (leading_tdc[pair] < 0 || hit.tdc != (leading_tdc[pair] ^ 0x1)) {
        continue;
      }
      double dt = hit.time_ns_raw - leading_time[pair];
      if (dt > 0.0) {
        hits[leading_idx[pair]].tot = dt;
      }
      have_leading[pair] = false;
    }
  }

  if (chan_calib.loaded && max_duration_ns > 0.0) {
    for (auto &hit : hits) {
      if (!hit.leading || hit.tot <= 0.0 || hit.tot > max_duration_ns) {
        continue;
      }
      hit.time_ns_corr = hit.time_ns_raw - chan_calib.CorrectionNs(hit.channel, hit.tot);
    }
  }

  std::unordered_map<int, std::unordered_map<int, std::vector<LeadHit>>> lead_hits;
  std::unordered_map<int, std::vector<LeadHit>> lead_hits_all;
  for (const auto &hit : hits) {
    if (!hit.leading) {
      continue;
    }
    LeadHit lh;
    lh.time_raw = hit.time_ns_raw;
    lh.time_corr = hit.time_ns_corr;
    lh.tot = hit.tot;
    lead_hits[hit.spill][hit.channel].push_back(lh);
    lead_hits_all[hit.channel].push_back(lh);
  }
  for (auto &spill_kv : lead_hits) {
    for (auto &ch_kv : spill_kv.second) {
      auto &vec = ch_kv.second;
      std::sort(vec.begin(), vec.end(), [](const LeadHit &a, const LeadHit &b) { return a.time_raw < b.time_raw; });
    }
  }
  for (auto &ch_kv : lead_hits_all) {
    auto &vec = ch_kv.second;
    std::sort(vec.begin(), vec.end(), [](const LeadHit &a, const LeadHit &b) { return a.time_raw < b.time_raw; });
  }

  std::unordered_map<int, size_t> lead_counts;
  for (const auto &spill_kv : lead_hits) {
    for (const auto &ch_kv : spill_kv.second) {
      lead_counts[ch_kv.first] += ch_kv.second.size();
    }
  }
  for (const auto &cfg : pairs) {
    std::cout << "Leading hits ch" << cfg.ch_a << ": " << lead_counts[cfg.ch_a]
              << " | ch" << cfg.ch_b << ": " << lead_counts[cfg.ch_b] << std::endl;
  }

  struct PairHists {
    PairConfig cfg;
    std::unique_ptr<TH1D> h_search_raw;
    std::unique_ptr<TH1D> h_search_corr;
    std::unique_ptr<TH1D> h_coinc_raw;
    std::unique_ptr<TH1D> h_coinc_corr;
    std::unique_ptr<TH2D> h_dt_tot_raw_a;
    std::unique_ptr<TH2D> h_dt_tot_corr_a;
    std::unique_ptr<TH2D> h_dt_tot_raw_b;
    std::unique_ptr<TH2D> h_dt_tot_corr_b;
  };

  std::vector<PairHists> pair_hists;
  pair_hists.reserve(pairs.size());
  for (const auto &cfg : pairs) {
    const double search_window = cfg.window_ns * 15.0;
    PairHists ph;
    ph.cfg = cfg;
    std::string label = std::to_string(cfg.ch_a) + "_" + std::to_string(cfg.ch_b);
    ph.h_search_raw = std::make_unique<TH1D>(
        ("h_search_raw_" + label).c_str(),
        ("search #Delta t raw ch" + label + ";#Delta t [ns];entries").c_str(),
        200, -search_window, search_window);
    ph.h_search_corr = std::make_unique<TH1D>(
        ("h_search_corr_" + label).c_str(),
        ("search #Delta t corrected ch" + label + ";#Delta t [ns];entries").c_str(),
        200, -search_window, search_window);
    ph.h_coinc_raw = std::make_unique<TH1D>(
        ("h_coinc_raw_" + label).c_str(),
        ("coincidence #Delta t raw ch" + label + ";#Delta t [ns];entries").c_str(),
        200, -cfg.window_ns, cfg.window_ns);
    ph.h_coinc_corr = std::make_unique<TH1D>(
        ("h_coinc_corr_" + label).c_str(),
        ("coincidence #Delta t corrected ch" + label + ";#Delta t [ns];entries").c_str(),
        200, -cfg.window_ns, cfg.window_ns);
    ph.h_dt_tot_raw_a = std::make_unique<TH2D>(
        ("h_dt_tot_raw_a_" + label).c_str(),
        ("#Delta t vs ToT raw ch" + std::to_string(cfg.ch_a) + ";ToT [ns];#Delta t [ns]").c_str(),
        60, 0.0, max_duration_ns, 120, -search_window, search_window);
    ph.h_dt_tot_corr_a = std::make_unique<TH2D>(
        ("h_dt_tot_corr_a_" + label).c_str(),
        ("#Delta t vs ToT corrected ch" + std::to_string(cfg.ch_a) + ";ToT [ns];#Delta t [ns]").c_str(),
        60, 0.0, max_duration_ns, 120, -search_window, search_window);
    ph.h_dt_tot_raw_b = std::make_unique<TH2D>(
        ("h_dt_tot_raw_b_" + label).c_str(),
        ("#Delta t vs ToT raw ch" + std::to_string(cfg.ch_b) + ";ToT [ns];#Delta t [ns]").c_str(),
        60, 0.0, max_duration_ns, 120, -search_window, search_window);
    ph.h_dt_tot_corr_b = std::make_unique<TH2D>(
        ("h_dt_tot_corr_b_" + label).c_str(),
        ("#Delta t vs ToT corrected ch" + std::to_string(cfg.ch_b) + ";ToT [ns];#Delta t [ns]").c_str(),
        60, 0.0, max_duration_ns, 120, -search_window, search_window);
    pair_hists.push_back(std::move(ph));
  }

  auto fill_pair = [&](const std::vector<LeadHit> &hits_a,
                       const std::vector<LeadHit> &hits_b,
                       PairHists &ph) -> size_t {
    const double search_window = ph.cfg.window_ns * 15.0;
    size_t match_count = 0;
    for (const auto &ha : hits_a) {
      LeadHit hb;
      if (!FindNearestInWindow(hits_b, ha.time_raw, search_window, hb, nullptr)) {
        continue;
      }
      ++match_count;
      double dt_raw = hb.time_raw - ha.time_raw;
      double dt_corr = hb.time_corr - ha.time_corr;
      if (std::abs(dt_raw) <= search_window) {
        ph.h_search_raw->Fill(dt_raw);
      }
      if (std::abs(dt_corr) <= search_window) {
        ph.h_search_corr->Fill(dt_corr);
      }
      if (std::abs(dt_raw) <= ph.cfg.window_ns) {
        ph.h_coinc_raw->Fill(dt_raw);
      }
      if (std::abs(dt_corr) <= ph.cfg.window_ns) {
        ph.h_coinc_corr->Fill(dt_corr);
      }
      if (ha.tot > 0.0 && max_duration_ns > 0.0) {
        ph.h_dt_tot_raw_a->Fill(ha.tot, dt_raw);
        ph.h_dt_tot_corr_a->Fill(ha.tot, dt_corr);
      }
      if (hb.tot > 0.0 && max_duration_ns > 0.0) {
        ph.h_dt_tot_raw_b->Fill(hb.tot, dt_raw);
        ph.h_dt_tot_corr_b->Fill(hb.tot, dt_corr);
      }
    }
    return match_count;
  };

  for (auto &ph : pair_hists) {
    size_t total_matches = 0;
    for (auto &spill_kv : lead_hits) {
      const auto &by_channel = spill_kv.second;
      auto it_a = by_channel.find(ph.cfg.ch_a);
      auto it_b = by_channel.find(ph.cfg.ch_b);
      if (it_a == by_channel.end() || it_b == by_channel.end()) {
        continue;
      }
      total_matches += fill_pair(it_a->second, it_b->second, ph);
    }

    if (total_matches == 0) {
      auto it_a_all = lead_hits_all.find(ph.cfg.ch_a);
      auto it_b_all = lead_hits_all.find(ph.cfg.ch_b);
      if (it_a_all != lead_hits_all.end() && it_b_all != lead_hits_all.end() &&
          !it_a_all->second.empty() && !it_b_all->second.empty()) {
        std::cout << "No matches found per-spill for pair " << ph.cfg.ch_a << "," << ph.cfg.ch_b
                  << "; retrying without spill grouping." << std::endl;
        fill_pair(it_a_all->second, it_b_all->second, ph);
      } else {
        std::cout << "No leading hits found for pair " << ph.cfg.ch_a << "," << ph.cfg.ch_b << std::endl;
      }
    }
  }

  gStyle->SetOptStat(0);
  std::string open_pdf = std::string(out_pdf) + "[";
  std::string close_pdf = std::string(out_pdf) + "]";
  TCanvas c_open("c_open", "c_open", 1600, 900);
  c_open.Print(open_pdf.c_str());

  for (auto &ph : pair_hists) {
    std::string label = std::to_string(ph.cfg.ch_a) + " vs " + std::to_string(ph.cfg.ch_b);
    TCanvas c_search("c_search", "c_search", 1600, 900);
    auto stack = std::make_unique<THStack>("hs_search", ("#Delta t search window " + label).c_str());
    ph.h_search_raw->SetLineColor(kRed + 1);
    ph.h_search_corr->SetLineColor(kBlue + 1);
    ph.h_search_raw->SetLineWidth(2);
    ph.h_search_corr->SetLineWidth(2);
    stack->Add(ph.h_search_raw.get(), "hist");
    stack->Add(ph.h_search_corr.get(), "hist");
    stack->Draw("nostack");
    auto leg = std::make_unique<TLegend>(0.6, 0.75, 0.88, 0.88);
    leg->AddEntry(ph.h_search_raw.get(), "raw", "l");
    leg->AddEntry(ph.h_search_corr.get(), "TW corrected", "l");
    leg->Draw();
    c_search.Print(out_pdf);

    TCanvas c_coinc("c_coinc", "c_coinc", 1600, 900);
    auto stack2 = std::make_unique<THStack>("hs_coinc", ("#Delta t coincidence window " + label).c_str());
    ph.h_coinc_raw->SetLineColor(kRed + 1);
    ph.h_coinc_corr->SetLineColor(kBlue + 1);
    ph.h_coinc_raw->SetLineWidth(2);
    ph.h_coinc_corr->SetLineWidth(2);
    stack2->Add(ph.h_coinc_raw.get(), "hist");
    stack2->Add(ph.h_coinc_corr.get(), "hist");
    stack2->Draw("nostack");
    auto leg2 = std::make_unique<TLegend>(0.6, 0.75, 0.88, 0.88);
    leg2->AddEntry(ph.h_coinc_raw.get(), "raw", "l");
    leg2->AddEntry(ph.h_coinc_corr.get(), "TW corrected", "l");
    leg2->Draw();
    c_coinc.Print(out_pdf);

    TCanvas c_dt_tot("c_dt_tot", "c_dt_tot", 1600, 900);
    c_dt_tot.Divide(2, 2, 0.001, 0.001);
    c_dt_tot.cd(1);
    ph.h_dt_tot_raw_a->Draw("colz");
    c_dt_tot.cd(2);
    ph.h_dt_tot_corr_a->Draw("colz");
    c_dt_tot.cd(3);
    ph.h_dt_tot_raw_b->Draw("colz");
    c_dt_tot.cd(4);
    ph.h_dt_tot_corr_b->Draw("colz");
    c_dt_tot.Print(out_pdf);
  }

  c_open.Print(close_pdf.c_str());
  std::cout << "Wrote TW validation PDF to " << out_pdf << std::endl;
}
