#include <ROOT/RDataFrame.hxx>
#include <TCanvas.h>
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
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
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

int TdcPairIndex(int tdc)
{
  return tdc >> 1;
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
}  // namespace

void fine_calibration_rdf(const char *input = "../data/calibration",
                          const char *out_root = "fine_calibration.root",
                          double q_low = 0.01,
                          double q_high = 0.99,
                          long long min_entries = 200,
                          const char *out_pdf = "",
                          double max_duration_ns = 0.0,
                          bool match_coincidence = false,
                          double clock_mhz = 320.0)
{
  if (WantsHelp(input) || WantsHelp(out_root)) {
    std::cout << "fine_calibration_rdf usage:\n";
    std::cout << "  fine_calibration_rdf(\"/path/to/decoded_or_parent\", \"fine_calibration.root\", 0.01, 0.99, 200, \"fine_calibration.pdf\", 0.0, false, 320.0)\n";
    std::cout << "  required branches: type,fifo,column,pixel,tdc,fine\n";
    std::cout << "  match_coincidence=true uses leading edges with valid ToT (needs rollover/coarse, optional spill/run_id)\n";
    std::cout << "  output histograms: hFineMin, hFineMax (bins=" << analysis_time::kFineCalibSize << ")\n";
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
        if (leading_tdc[pair] < 0 || edge.tdc != (leading_tdc[pair] ^ 0x1)) {
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
    if (channel == 17 || channel == 19) {
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
  }
  for (auto &item : tdc_profiles) {
    if (item.profile) {
      item.profile->Write();
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

    TCanvas c_summary("c_fine_calib_summary", "Fine calibration summary", 1200, 900);
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

    TCanvas c_tdc("c_fine_calib_tdc", "Fine calibration TDC distributions", 1200, 900);
    c_tdc.Divide(2, 2);
    for (int t = 0; t < 4; ++t) {
      c_tdc.cd(t + 1);
      htdc[t]->Draw("hist");
    }
    c_tdc.Print(pdf_path.c_str());

    TCanvas c_tdc_cdf("c_fine_calib_tdc_cdf", "Fine calibration TDC CDFs", 1200, 900);
    c_tdc_cdf.Divide(2, 2);
    for (int t = 0; t < 4; ++t) {
      c_tdc_cdf.cd(t + 1);
      htdc_cdf[t]->Draw("hist");
    }
    c_tdc_cdf.Print(pdf_path.c_str());

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

      TCanvas c_lut_profiles("c_fine_cdf_profiles", "Fine CDF profiles (ch 17/19)", 1000, 800);
      c_lut_profiles.Divide(1, 2);
      TLegend legend17(0.7, 0.2, 0.9, 0.4);
      TLegend legend19(0.7, 0.2, 0.9, 0.4);
      legend17.SetBorderSize(0);
      legend17.SetFillStyle(0);
      legend19.SetBorderSize(0);
      legend19.SetFillStyle(0);
      auto stack17 = build_stack(17, "stack_fine_cdf_17", "Fine CDF profiles (ch 17);fine;CDF", legend17);
      auto stack19 = build_stack(19, "stack_fine_cdf_19", "Fine CDF profiles (ch 19);fine;CDF", legend19);
      c_lut_profiles.cd(1);
      if (stack17 && stack17->GetHists() && stack17->GetHists()->GetSize() > 0) {
        stack17->Draw("nostack");
        legend17.Draw();
      }
      c_lut_profiles.cd(2);
      if (stack19 && stack19->GetHists() && stack19->GetHists()->GetSize() > 0) {
        stack19->Draw("nostack");
        legend19.Draw();
      }
      c_lut_profiles.Print(pdf_path.c_str());

      TCanvas c_lut_profiles_zoom("c_fine_cdf_profiles_zoom", "Fine CDF profiles (ch 17/19) zoom", 1000, 800);
      c_lut_profiles_zoom.Divide(1, 2);
      TLegend legend17_zoom(0.7, 0.2, 0.9, 0.4);
      TLegend legend19_zoom(0.7, 0.2, 0.9, 0.4);
      legend17_zoom.SetBorderSize(0);
      legend17_zoom.SetFillStyle(0);
      legend19_zoom.SetBorderSize(0);
      legend19_zoom.SetFillStyle(0);
      auto stack17_zoom = build_stack(17, "stack_fine_cdf_17_zoom", "Fine CDF profiles (ch 17);fine;CDF", legend17_zoom);
      auto stack19_zoom = build_stack(19, "stack_fine_cdf_19_zoom", "Fine CDF profiles (ch 19);fine;CDF", legend19_zoom);
      c_lut_profiles_zoom.cd(1);
      if (stack17_zoom && stack17_zoom->GetHists() && stack17_zoom->GetHists()->GetSize() > 0) {
        stack17_zoom->Draw("nostack");
        stack17_zoom->GetXaxis()->SetRangeUser(20.0, 120.0);
        legend17_zoom.Draw();
      }
      c_lut_profiles_zoom.cd(2);
      if (stack19_zoom && stack19_zoom->GetHists() && stack19_zoom->GetHists()->GetSize() > 0) {
        stack19_zoom->Draw("nostack");
        stack19_zoom->GetXaxis()->SetRangeUser(20.0, 120.0);
        legend19_zoom.Draw();
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
      TCanvas c_lut_single("c_fine_cdf_profiles_single", "Fine CDF profiles (single)", 900, 700);
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

      std::vector<const TdcProfileInfo *> ch17;
      std::vector<const TdcProfileInfo *> ch19;
      for (const auto &item : tdc_profiles) {
        if (!item.profile) {
          continue;
        }
        if (item.channel == 17) {
          ch17.push_back(&item);
        } else if (item.channel == 19) {
          ch19.push_back(&item);
        }
      }
      auto sort_profiles = [](const TdcProfileInfo *a, const TdcProfileInfo *b) {
        if (a->fifo != b->fifo) {
          return a->fifo < b->fifo;
        }
        return a->tdc < b->tdc;
      };
      std::sort(ch17.begin(), ch17.end(), sort_profiles);
      std::sort(ch19.begin(), ch19.end(), sort_profiles);
      const size_t rows = std::max(ch17.size(), ch19.size());
      if (rows > 0) {
        gStyle->SetOptStat(1110);
        gStyle->SetOptFit(1111);
        const int height = static_cast<int>(350 * rows);
        TCanvas c_grid("c_fine_cdf_profiles_grid", "Fine CDF profiles grid", 1000, height);
        c_grid.Divide(2, static_cast<int>(rows));
        for (size_t r = 0; r < rows; ++r) {
          for (int col = 0; col < 2; ++col) {
            const auto &vec = (col == 0) ? ch17 : ch19;
            c_grid.cd(static_cast<int>(r * 2 + col + 1));
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
