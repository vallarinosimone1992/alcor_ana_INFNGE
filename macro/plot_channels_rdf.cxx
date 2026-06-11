#include <ROOT/RDataFrame.hxx>
#include <TCanvas.h>
#include <TFile.h>
#include <TLegend.h>
#include <TStyle.h>
#include <TSystem.h>
#include <TH1D.h>
#include <THStack.h>

#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
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

void PrintPlotHelp()
{
  std::cout << "plot_channels_rdf usage:" << std::endl;
  std::cout
      << "  plot_channels_rdf(\"/path/to/decoded.root\", \"out.pdf\", \"17,19\", \"TDC_calibration.root\","
         " 1, 0, true, 320, 1, 15, \"\")"
            << std::endl;
  std::cout << "Inputs:" << std::endl;
  std::cout << "  decoded dir must contain alcdaq.fifo_*.root with TTree 'alcor'" << std::endl;
  std::cout << "  required branches: device,fifo,type,counter,column,pixel,tdc,rollover,coarse,fine" << std::endl;
  std::cout << "  channels are 0..31, computed as column*4 + pixel when missing" << std::endl;
  std::cout << "  channels list can be comma- or space-separated" << std::endl;
  std::cout << "  fine calibration file uses hFineMin/hFineMax; default formula used when missing" << std::endl;
  std::cout << "  use_lut=false disables LUT even if hFineLut is present" << std::endl;
  std::cout << "  optional channel calibration uses hChanCalib_chXX vs ToT when a file path is provided" << std::endl;
  std::cout << "  spill index is 0-based; default 1 (second spill)" << std::endl;
  std::cout << "  includes extra plot: leading-edge time distribution in selected spill" << std::endl;
  std::cout << "  with exactly two channels, adds Delta t histogram for same-index leading hits" << std::endl;
  std::cout << "  fine_cut excludes hits with |fine - cut| <= fine_cut (fine units)" << std::endl;
}

std::vector<int> ParseChannels(const std::string &channels_csv)
{
  std::string cleaned = channels_csv;
  for (char &ch : cleaned) {
    if (ch == ',') {
      ch = ' ';
    }
  }

  std::stringstream ss(cleaned);
  std::vector<int> channels;
  int value = 0;
  while (ss >> value) {
    if (value < 0 || value > 31) {
      continue;
    }
    channels.push_back(value);
  }
  std::sort(channels.begin(), channels.end());
  channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
  return channels;
}

int BinsForRange(double lo, double hi)
{
  double span = hi - lo;
  if (span <= 0.0) {
    return 1;
  }
  int bins = static_cast<int>(span);
  if (bins < 10) {
    bins = 10;
  }
  if (bins > 200) {
    bins = 200;
  }
  return bins;
}

std::vector<int> DefaultColors()
{
  return {
      kAzure + 2,
      kOrange + 7,
      kGreen + 2,
      kMagenta + 1,
      kRed + 1,
      kCyan + 2,
      kViolet + 1,
      kPink + 7,
  };
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
} // namespace

void plot_channels_rdf(const char *decoded_dir = "../raw_data/latest/kc705-196/decoded",
                       const char *out_pdf = "ch17_ch19_vars.pdf",
                       const char *channels = "17,19",
                       const char *fine_calib_path = "",
                       int spill_index = 1,
                       int fine_cut = analysis_time::kDefaultFineCut,
                       bool use_lut = true,
                       double clock_mhz = 320.0,
                       int use_fine = 1,
                       double max_duration_ns = 15.0,
                       const char *chan_calib_path = "")
{
  if (WantsHelp(decoded_dir)) {
    PrintPlotHelp();
    return;
  }

  ScopedTimer timer("plot_channels_rdf");

  auto input_spec = analysis_io::ResolveInputSpec(decoded_dir);
  if (input_spec.files.empty()) {
    std::cout << "No decoded ROOT files found under " << decoded_dir << std::endl;
    return;
  }

  auto channel_list = ParseChannels(channels);
  if (channel_list.empty()) {
    std::cout << "No valid channels specified." << std::endl;
    return;
  }

  const bool use_fine_flag = (use_fine != 0);
  if (max_duration_ns < 0.0) {
    max_duration_ns = 0.0;
  }

  std::cout << "Input: " << decoded_dir << std::endl;
  std::cout << "Output: " << out_pdf << std::endl;
  std::cout << "Channels: " << channels << std::endl;
  std::cout << "Spill index: " << spill_index << std::endl;
  std::cout << "Clock (MHz): " << clock_mhz << std::endl;
  std::cout << "Use fine: " << (use_fine_flag ? 1 : 0) << std::endl;
  std::cout << "Use LUT: " << (use_lut ? 1 : 0) << std::endl;
  std::cout << "Max duration (ns): " << max_duration_ns << std::endl;
  if (fine_cut > 0) {
    std::cout << "Fine cut (bins): " << fine_cut << std::endl;
  } else {
    std::cout << "Fine cut (bins): 0" << std::endl;
  }
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    std::cout << "Fine calib: " << fine_calib_path << std::endl;
  }
  if (chan_calib_path && chan_calib_path[0] != '\0') {
    std::cout << "Channel calib: " << chan_calib_path << std::endl;
  }

  auto fine_calib_ptr = std::make_unique<analysis_time::FineCalib>();
  auto &fine_calib = *fine_calib_ptr;
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    if (fine_calib.LoadFromFile(fine_calib_path)) {
      std::cout << "Loaded fine calibration: " << fine_calib_path << std::endl;
    } else {
      std::cout << "Failed to load fine calibration: " << fine_calib_path << " (using default formula)" << std::endl;
    }
  }
  fine_calib.use_lut = use_lut;
  analysis_time::PrintFineCalibConstants(fine_calib);
  auto chan_calib_ptr = std::make_unique<analysis_time::ChannelCalib>();
  auto &chan_calib = *chan_calib_ptr;
  if (chan_calib_path && chan_calib_path[0] != '\0') {
    if (chan_calib.LoadFromFile(chan_calib_path)) {
      std::cout << "Loaded channel calibration: " << chan_calib_path << std::endl;
    } else {
      std::cout << "Failed to load channel calibration: " << chan_calib_path << " (ignored)" << std::endl;
    }
  }
  analysis_time::PrintChannelCalibSummary(chan_calib);

  ROOT::EnableImplicitMT();
  ROOT::RDataFrame df(input_spec.tree_name.c_str(), input_spec.files);
  auto colnames = df.GetColumnNames();
  bool has_channel = std::find(colnames.begin(), colnames.end(), "channel") != colnames.end();
  bool has_time_tick = std::find(colnames.begin(), colnames.end(), "time_tick") != colnames.end();
  bool has_spill = std::find(colnames.begin(), colnames.end(), "spill") != colnames.end();

  // ALCOR hits only; EO channel index is column*4 + pixel.
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

  std::string sel_expr;
  for (size_t i = 0; i < channel_list.size(); ++i) {
    if (i > 0) {
      sel_expr += " || ";
    }
    sel_expr += "channel == " + std::to_string(channel_list[i]);
  }
  auto df_sel = df_hits_cut.Filter(sel_expr);
  auto n_sel = *df_sel.Count();
  if (n_sel == 0) {
    std::cout << "No entries found for channels: " << channels << std::endl;
    return;
  }

  struct ChannelView {
    int channel;
    ROOT::RDF::RNode df;
  };
  std::vector<ChannelView> views;
  views.reserve(channel_list.size());
  for (int ch : channel_list) {
    views.push_back({ch, df_hits_cut.Filter("channel == " + std::to_string(ch))});
  }

  std::vector<std::string> vars = {
      "device",
      "fifo",
      "type",
      "counter",
      "column",
      "pixel",
      "tdc",
      "rollover",
      "coarse",
      "fine",
  };

  auto colors = DefaultColors();
  gStyle->SetOptStat(0);
  std::string out_open = std::string(out_pdf) + "[";
  std::string out_close = std::string(out_pdf) + "]";

struct PlotGroup {
    std::string name;
    std::string title;
    std::vector<TH1D *> hists;
    std::vector<std::string> labels;
  };

  std::vector<PlotGroup> groups;
  // Keep RDF histograms alive for later drawing/stacking.
  std::vector<std::vector<ROOT::RDF::RResultPtr<TH1D>>> rdf_hists_store;
  groups.reserve(vars.size() + 1);
  rdf_hists_store.reserve(vars.size());

  for (const auto &var : vars) {
    auto min_val = *df_sel.Min<int>(var);
    auto max_val = *df_sel.Max<int>(var);
    double lo = std::floor(static_cast<double>(min_val)) - 0.5;
    double hi = std::ceil(static_cast<double>(max_val)) + 0.5;
    if (lo == hi) {
      lo -= 0.5;
      hi += 0.5;
    }
    int bins = BinsForRange(lo, hi);

    PlotGroup group;
    group.name = var;
    group.title = var + "; " + var + "; entries";
    group.hists.reserve(views.size());
    group.labels.reserve(views.size());

    std::vector<ROOT::RDF::RResultPtr<TH1D>> hists;
    hists.reserve(views.size());

    for (size_t i = 0; i < views.size(); ++i) {
      std::string name = "h_" + var + "_ch" + std::to_string(views[i].channel);
      std::string ch_title = var + " (channel " + std::to_string(views[i].channel) + "); " + var + "; entries";
      auto hist = views[i].df.Histo1D({name.c_str(), ch_title.c_str(), bins, lo, hi}, var);
      hist->SetLineColor(colors[i % colors.size()]);
      hist->SetLineWidth(2);
      hists.push_back(hist);
      group.hists.push_back(hist.GetPtr());
      group.labels.push_back("channel " + std::to_string(views[i].channel));
    }

    groups.push_back(group);
    rdf_hists_store.push_back(std::move(hists));
  }

  const double tick_ns = analysis_time::TickNs(clock_mhz);
  const double duration_max_ns = max_duration_ns;
  auto df_time = df_sel;
  if (!has_time_tick) {
    df_time = df_time.Define("time_tick", analysis_time::TimeTickLambda(), {"rollover", "coarse"});
  }
  auto ch_vals = df_time.Take<int>("channel");
  auto time_vals = df_time.Take<Long64_t>("time_tick");
  auto fifo_vals = df_time.Take<int>("fifo");
  auto column_vals = df_time.Take<int>("column");
  auto pixel_vals = df_time.Take<int>("pixel");
  auto tdc_vals = df_time.Take<int>("tdc");
  auto fine_vals = df_time.Take<int>("fine");
  RunGraphsCompat(ch_vals, time_vals, fifo_vals, column_vals, pixel_vals, tdc_vals, fine_vals);

  std::unordered_map<int, size_t> channel_index;
  channel_index.reserve(views.size());
  for (size_t i = 0; i < views.size(); ++i) {
    channel_index[views[i].channel] = i;
  }

  struct Hit {
    long long time_tick;
    int fine;
    int tdc;
    int fifo;
    int column;
    int pixel;
    int spill;
    double time_ns_raw;
    double time_ns;
    bool leading;
  };

  std::vector<std::vector<Hit>> hits_by_channel(views.size());
  const auto &channels_vec = ch_vals.GetValue();
  const auto &times_vec = time_vals.GetValue();
  const auto &fifos_vec = fifo_vals.GetValue();
  const auto &columns_vec = column_vals.GetValue();
  const auto &pixels_vec = pixel_vals.GetValue();
  const auto &tdc_vec = tdc_vals.GetValue();
  const auto &fine_vec = fine_vals.GetValue();
  std::vector<int> spills_vec;
  if (has_spill) {
    auto spills_take = df_time.Take<int>("spill");
    RunGraphsCompat(spills_take);
    spills_vec = spills_take.GetValue();
  }

  for (size_t i = 0; i < channels_vec.size(); ++i) {
    int ch = channels_vec[i];
    auto idx_it = channel_index.find(ch);
    if (idx_it == channel_index.end()) {
      continue;
    }
    int spill_val = -1;
    if (has_spill && i < spills_vec.size()) {
      spill_val = spills_vec[i];
    }
    int tdc_index = analysis_time::TdcIndex(fifos_vec[i], columns_vec[i], pixels_vec[i], tdc_vec[i]);
    double time_ns_raw =
        analysis_time::TimeNsFromTick(fine_calib, times_vec[i], fine_vec[i], tdc_index, tick_ns, use_fine_flag);
    hits_by_channel[idx_it->second].push_back({times_vec[i],
                                               fine_vec[i],
                                               tdc_vec[i],
                                               fifos_vec[i],
                                               columns_vec[i],
                                               pixels_vec[i],
                                               spill_val,
                                               time_ns_raw,
                                               time_ns_raw,
                                               false});
  }

  std::vector<std::vector<double>> durations(views.size());
  bool has_duration = false;
  bool skip_chan_calib = false;
  if (chan_calib.loaded && duration_max_ns <= 0.0) {
    skip_chan_calib = true;
    std::cout << "Channel calibration loaded but max_duration_ns <= 0; skipping correction." << std::endl;
  }

  for (size_t i = 0; i < hits_by_channel.size(); ++i) {
    auto &hits = hits_by_channel[i];
    if (hits.empty()) {
      continue;
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
      if (a.time_tick != b.time_tick) {
        return a.time_tick < b.time_tick;
      }
      return a.fine < b.fine;
    });

    std::vector<char> leading_mask(hits.size(), 0);
    std::vector<int> leading_to_trailing(hits.size(), -1);

    if (duration_max_ns > 0.0) {
      std::array<bool, 2> have_leading = {false, false};
      std::array<double, 2> leading_time_ns = {0.0, 0.0};
      std::array<int, 2> leading_tdc = {-1, -1};
      std::array<int, 2> leading_idx = {-1, -1};
      for (size_t j = 0; j < hits.size(); ++j) {
        const auto &hit = hits[j];
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
          leading_time_ns[pair] = hit.time_ns_raw;
          leading_tdc[pair] = hit.tdc;
          leading_idx[pair] = static_cast<int>(j);
          have_leading[pair] = true;
          continue;
        }

        if (!have_leading[pair]) {
          continue;
        }
        if (leading_tdc[pair] < 0 || hit.tdc != (leading_tdc[pair] ^ 0x1)) {
          continue;
        }
        double dt_ns = hit.time_ns_raw - leading_time_ns[pair];
        if (dt_ns >= 0.0 && dt_ns <= duration_max_ns) {
          durations[i].push_back(dt_ns);
          has_duration = true;
          int lead_idx = leading_idx[pair];
          if (lead_idx >= 0 && lead_idx < static_cast<int>(hits.size())) {
            leading_mask[lead_idx] = 1;
            leading_to_trailing[lead_idx] = static_cast<int>(j);
          }
        }
        have_leading[pair] = false;
      }
    } else {
      for (size_t j = 0; j < hits.size(); ++j) {
        if (IsLeadingTdc(hits[j].tdc)) {
          leading_mask[j] = 1;
        }
      }
    }

    for (size_t j = 0; j < hits.size(); ++j) {
      hits[j].leading = leading_mask[j] != 0;
    }

    if (chan_calib.loaded && !skip_chan_calib && duration_max_ns > 0.0) {
      const int channel = views[i].channel;
      for (size_t j = 0; j < hits.size(); ++j) {
        if (!leading_mask[j]) {
          continue;
        }
        int trailing = leading_to_trailing[j];
        if (trailing < 0 || trailing >= static_cast<int>(hits.size())) {
          continue;
        }
        double tot = hits[trailing].time_ns_raw - hits[j].time_ns_raw;
        if (tot <= 0.0 || tot > duration_max_ns) {
          continue;
        }
        hits[j].time_ns = hits[j].time_ns_raw - chan_calib.CorrectionNs(channel, tot);
      }
    }
  }

  std::unique_ptr<TH1D> indexed_dt_hist;
  if (views.size() == 2) {
    std::vector<double> leading_times_a;
    std::vector<double> leading_times_b;
    leading_times_a.reserve(hits_by_channel[0].size());
    leading_times_b.reserve(hits_by_channel[1].size());

    for (const auto &hit : hits_by_channel[0]) {
      if (hit.leading) {
        leading_times_a.push_back(hit.time_ns);
      }
    }
    for (const auto &hit : hits_by_channel[1]) {
      if (hit.leading) {
        leading_times_b.push_back(hit.time_ns);
      }
    }

    const size_t pair_count = std::min(leading_times_a.size(), leading_times_b.size());
    if (pair_count > 0) {
      std::vector<double> indexed_dt;
      indexed_dt.reserve(pair_count);
      for (size_t i = 0; i < pair_count; ++i) {
        indexed_dt.push_back(leading_times_b[i] - leading_times_a[i]);
      }

      const auto minmax = std::minmax_element(indexed_dt.begin(), indexed_dt.end());
      double lo = std::floor(*minmax.first) - 0.5;
      double hi = std::ceil(*minmax.second) + 0.5;
      if (lo == hi) {
        lo -= 0.5;
        hi += 0.5;
      }
      const int bins = BinsForRange(lo, hi);

      const int channel_a = views[0].channel;
      const int channel_b = views[1].channel;
      std::ostringstream title;
      title << "leading-edge time difference by hit index (channel " << channel_b << " - channel " << channel_a
            << "); t_{ch" << channel_b << "}[i] - t_{ch" << channel_a << "}[i] [ns]; entries";

      indexed_dt_hist = std::make_unique<TH1D>("h_indexed_dt", title.str().c_str(), bins, lo, hi);
      indexed_dt_hist->SetLineColor(kBlue + 1);
      indexed_dt_hist->SetLineWidth(2);
      indexed_dt_hist->SetDirectory(nullptr);
      for (double dt : indexed_dt) {
        indexed_dt_hist->Fill(dt);
      }

      PlotGroup dt_group;
      dt_group.name = "indexed_leading_dt";
      dt_group.title = title.str();
      dt_group.hists.push_back(indexed_dt_hist.get());
      dt_group.labels.push_back("same index");
      groups.push_back(std::move(dt_group));
    }
  }

  std::vector<std::unique_ptr<TH1D>> duration_hists;
  if (has_duration) {
    const double duration_hist_max_ns = std::max(duration_max_ns, 30.0);
    int bins = static_cast<int>(duration_hist_max_ns * 4.0);
    if (bins < 1) {
      bins = 1;
    }
    PlotGroup duration_group;
    duration_group.name = "duration";
    std::ostringstream duration_title;
    duration_title << "hit duration (leading-trailing, <= " << duration_max_ns << " ns); Delta t [ns]; entries";
    duration_group.title = duration_title.str();
    duration_group.hists.reserve(views.size());
    duration_group.labels.reserve(views.size());
    duration_hists.reserve(views.size());

    for (size_t i = 0; i < views.size(); ++i) {
      std::string name = "h_duration_ch" + std::to_string(views[i].channel);
      std::ostringstream ch_title;
      ch_title << "hit duration (leading-trailing, <= " << duration_max_ns << " ns) (channel "
               << views[i].channel << "); Delta t [ns]; entries";
      const std::string ch_title_str = ch_title.str();
      auto hist = std::make_unique<TH1D>(name.c_str(), ch_title_str.c_str(), bins, 0.0, duration_hist_max_ns);
      hist->SetLineColor(colors[i % colors.size()]);
      hist->SetLineWidth(2);
      hist->SetDirectory(nullptr);
      for (double dt : durations[i]) {
        hist->Fill(dt);
      }
      duration_group.hists.push_back(hist.get());
      duration_group.labels.push_back("channel " + std::to_string(views[i].channel));
      duration_hists.push_back(std::move(hist));
    }

    groups.push_back(duration_group);
  }

  struct FineTdcGroup {
    int channel = -1;
    std::array<TH1D *, 4> hists{};
  };
  std::vector<FineTdcGroup> fine_tdc_groups;
  std::vector<std::array<ROOT::RDF::RResultPtr<TH1D>, 4>> fine_tdc_store;

  {
    const int fine_bins = analysis_time::kFineBins;
    const double fine_lo = -0.5;
    const double fine_hi = static_cast<double>(analysis_time::kFineBins) - 0.5;
    fine_tdc_groups.reserve(views.size());
    fine_tdc_store.reserve(views.size());
    for (auto &view : views) {
      FineTdcGroup group;
      group.channel = view.channel;
      std::array<ROOT::RDF::RResultPtr<TH1D>, 4> hists;
      for (int tdc = 0; tdc < 4; ++tdc) {
        std::string name = "h_fine_ch" + std::to_string(view.channel) + "_tdc" + std::to_string(tdc);
        std::string title = "fine (channel " + std::to_string(view.channel) + ", tdc " + std::to_string(tdc) +
                            "); fine; entries";
        auto hist = view.df.Filter("tdc == " + std::to_string(tdc))
                        .Histo1D({name.c_str(), title.c_str(), fine_bins, fine_lo, fine_hi}, "fine");
        hist->SetLineColor(kBlue + 1);
        hist->SetLineWidth(2);
        hists[tdc] = hist;
        group.hists[tdc] = hist.GetPtr();
      }
      fine_tdc_store.push_back(std::move(hists));
      fine_tdc_groups.push_back(group);
    }
  }

  std::vector<std::vector<double>> spill_times(views.size());
  bool has_spill_times = false;

  if (spill_index >= 0) {
    ROOT::RDF::RNode df_all = df;
    if (!has_channel) {
      df_all = df_all.Define("channel", "column * 4 + pixel");
    }
    if (!has_time_tick) {
      df_all = df_all.Define("time_tick", analysis_time::TimeTickLambda(), {"rollover", "coarse"});
    }

    auto types_all = df_all.Take<int>("type");
    auto channels_all = df_all.Take<int>("channel");
    auto ticks_all = df_all.Take<Long64_t>("time_tick");
    auto fifos_all = df_all.Take<int>("fifo");
    auto columns_all = df_all.Take<int>("column");
    auto pixels_all = df_all.Take<int>("pixel");
    auto tdcs_all = df_all.Take<int>("tdc");
    auto fines_all = df_all.Take<int>("fine");
    RunGraphsCompat(types_all, channels_all, ticks_all, fifos_all, columns_all, pixels_all, tdcs_all, fines_all);

    std::vector<int> spills_all;
    if (has_spill) {
      auto spills_take = df_all.Take<int>("spill");
      RunGraphsCompat(spills_take);
      spills_all = spills_take.GetValue();
    }

    const auto &types_vals = types_all.GetValue();
    const auto &channels_vals = channels_all.GetValue();
    const auto &ticks_vals = ticks_all.GetValue();
    const auto &fifos_vals = fifos_all.GetValue();
    const auto &columns_vals = columns_all.GetValue();
    const auto &pixels_vals = pixels_all.GetValue();
    const auto &tdcs_vals = tdcs_all.GetValue();
    const auto &fines_vals = fines_all.GetValue();

    if (has_spill) {
      Long64_t spill_start_tick = std::numeric_limits<Long64_t>::max();
      for (size_t i = 0; i < types_vals.size(); ++i) {
        if (types_vals[i] != 1) {
          continue;
        }
        if (static_cast<size_t>(i) >= spills_all.size()) {
          continue;
        }
        if (spills_all[i] != spill_index) {
          continue;
        }
        spill_start_tick = std::min(spill_start_tick, ticks_vals[i]);
      }
      if (spill_start_tick != std::numeric_limits<Long64_t>::max()) {
        double start_ns = static_cast<double>(spill_start_tick) * tick_ns;
        for (size_t ch_idx = 0; ch_idx < hits_by_channel.size(); ++ch_idx) {
          for (const auto &hit : hits_by_channel[ch_idx]) {
            if (hit.spill != spill_index) {
              continue;
            }
            if (!hit.leading) {
              continue;
            }
            spill_times[ch_idx].push_back(hit.time_ns - start_ns);
            has_spill_times = true;
          }
        }
      }
    } else {
      std::unordered_map<int, int> spill_index_by_fifo;
      std::unordered_map<int, bool> saw_header_by_fifo;
      std::unordered_map<int, bool> has_start_by_fifo;
      std::unordered_map<int, Long64_t> spill_start_tick_by_fifo;

      auto get_spill_index = [&](int fifo) -> int & {
        auto it = spill_index_by_fifo.find(fifo);
        if (it == spill_index_by_fifo.end()) {
          it = spill_index_by_fifo.emplace(fifo, -1).first;
        }
        return it->second;
      };

      for (size_t i = 0; i < types_vals.size(); ++i) {
        int type = types_vals[i];
        int fifo = fifos_vals[i];
        int &fifo_spill_index = get_spill_index(fifo);

        if (type == 7) {
          ++fifo_spill_index;
          saw_header_by_fifo[fifo] = true;
          spill_start_tick_by_fifo[fifo] = ticks_vals[i];
          has_start_by_fifo[fifo] = true;
          continue;
        }
        if (type == 15) {
          if (!saw_header_by_fifo[fifo]) {
            ++fifo_spill_index;
          }
          has_start_by_fifo[fifo] = false;
          continue;
        }
        if (type != 1) {
          continue;
        }
        if (fifo_spill_index != spill_index) {
          continue;
        }
        int ch = channels_vals[i];
        auto idx_it = channel_index.find(ch);
        if (idx_it == channel_index.end()) {
          continue;
        }
        if (!IsLeadingTdc(tdcs_vals[i])) {
          continue;
        }
        int tdc_index = analysis_time::TdcIndex(fifo, columns_vals[i], pixels_vals[i], tdcs_vals[i]);
        double time_ns =
            analysis_time::TimeNsFromTick(fine_calib, ticks_vals[i], fines_vals[i], tdc_index, tick_ns, use_fine_flag);
        if (!has_start_by_fifo[fifo]) {
          continue;
        }
        double start_ns = static_cast<double>(spill_start_tick_by_fifo[fifo]) * tick_ns;
        spill_times[idx_it->second].push_back(time_ns - start_ns);
        has_spill_times = true;
      }
    }
  }

  std::vector<std::unique_ptr<TH1D>> spill_hists;
  if (has_spill_times) {
    double min_val = std::numeric_limits<double>::max();
    double max_val = std::numeric_limits<double>::lowest();
    for (const auto &vals : spill_times) {
      for (double v : vals) {
        min_val = std::min(min_val, v);
        max_val = std::max(max_val, v);
      }
    }
    if (min_val == std::numeric_limits<double>::max()) {
      has_spill_times = false;
    } else {
      double lo = std::floor(min_val) - 0.5;
      double hi = std::ceil(max_val) + 0.5;
      if (lo == hi) {
        lo -= 0.5;
        hi += 0.5;
      }
      int bins = BinsForRange(lo, hi);
      PlotGroup spill_group;
      spill_group.name = "spill_leading_time";
      spill_group.title =
          "spill " + std::to_string(spill_index) + " leading-edge time; t - t_{spill} [ns]; entries";
      spill_group.hists.reserve(views.size());
      spill_group.labels.reserve(views.size());
      spill_hists.reserve(views.size());

      for (size_t i = 0; i < views.size(); ++i) {
        std::string name = "h_spill_lead_s" + std::to_string(spill_index) + "_ch" + std::to_string(views[i].channel);
        std::string ch_title = "spill " + std::to_string(spill_index) + " leading-edge time (channel " +
                               std::to_string(views[i].channel) + "); t - t_{spill} [ns]; entries";
        auto hist = std::make_unique<TH1D>(name.c_str(), ch_title.c_str(), bins, lo, hi);
        hist->SetLineColor(colors[i % colors.size()]);
        hist->SetLineWidth(2);
        hist->SetDirectory(nullptr);
        for (double v : spill_times[i]) {
          hist->Fill(v);
        }
        spill_group.hists.push_back(hist.get());
        spill_group.labels.push_back("channel " + std::to_string(views[i].channel));
        spill_hists.push_back(std::move(hist));
      }

      groups.push_back(spill_group);
    }
  }

  size_t drawable_groups = 0;
  for (const auto &group : groups) {
    if (!group.hists.empty()) {
      ++drawable_groups;
    }
  }
  if (drawable_groups == 0) {
    std::cout << "No histograms to draw." << std::endl;
    return;
  }

  TCanvas c_open("c_open", "c_open", 1600, 900);
  c_open.Print(out_open.c_str());

  for (const auto &group : groups) {
    if (group.hists.empty()) {
      continue;
    }
    int cols = 1;
    int rows = 1;
    GridForCount(group.hists.size(), cols, rows);
    std::string canvas_name = "c_" + group.name;
    TCanvas c(canvas_name.c_str(), canvas_name.c_str(), 1600, 900);
    c.Divide(cols, rows, 0.001, 0.001);
    for (size_t i = 0; i < group.hists.size(); ++i) {
      c.cd(static_cast<int>(i + 1));
      group.hists[i]->Draw("hist");
    }

    c.Print(out_pdf);
  }

  for (const auto &group : fine_tdc_groups) {
    std::string cname = "c_fine_tdc_ch" + std::to_string(group.channel);
    TCanvas c(cname.c_str(), cname.c_str(), 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    for (int tdc = 0; tdc < 4; ++tdc) {
      c.cd(tdc + 1);
      if (group.hists[tdc]) {
        group.hists[tdc]->Draw("hist");
      }
    }
    c.Print(out_pdf);
  }

  if (!groups.empty()) {
    int cols = 1;
    int rows = 1;
    GridForCount(groups.size(), cols, rows);
    TCanvas c_summary("c_summary", "c_summary", 1600, 900);
    c_summary.Divide(cols, rows, 0.001, 0.001);

    std::vector<std::unique_ptr<THStack>> stacks;
    std::vector<std::unique_ptr<TLegend>> legends;
    stacks.reserve(groups.size());
    legends.reserve(groups.size());

    for (size_t g = 0; g < groups.size(); ++g) {
      const auto &group = groups[g];
      if (group.hists.empty()) {
        continue;
      }
      c_summary.cd(static_cast<int>(g + 1));
      std::string stack_name = "hs_" + group.name + "_summary";
      auto stack = std::make_unique<THStack>(stack_name.c_str(), group.title.c_str());
      for (auto *hist : group.hists) {
        stack->Add(hist, "hist");
      }
      stack->Draw("nostack");

      auto leg = std::make_unique<TLegend>(0.65, 0.75, 0.88, 0.88);
      for (size_t i = 0; i < group.hists.size(); ++i) {
        std::string label;
        if (i < group.labels.size() && !group.labels[i].empty()) {
          label = group.labels[i];
        } else if (i < views.size()) {
          label = "channel " + std::to_string(views[i].channel);
        } else {
          label = "hist " + std::to_string(i);
        }
        leg->AddEntry(group.hists[i], label.c_str(), "l");
      }
      leg->Draw();

      stacks.push_back(std::move(stack));
      legends.push_back(std::move(leg));
    }

    c_summary.Print(out_pdf);
  }

  c_open.Print(out_close.c_str());
}
