#include <ROOT/RDataFrame.hxx>
#include <TCanvas.h>
#include <TFile.h>
#include <TH1D.h>
#include <TH2D.h>
#include <TParameter.h>
#include <TPad.h>
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
                          const char *out_pdf = "")
{
  if (WantsHelp(input) || WantsHelp(out_root)) {
    std::cout << "fine_calibration_rdf usage:\n";
    std::cout << "  fine_calibration_rdf(\"/path/to/decoded_or_parent\", \"fine_calibration.root\", 0.01, 0.99, 200, \"fine_calibration.pdf\")\n";
    std::cout << "  required branches: type,fifo,column,pixel,tdc,fine\n";
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

  auto df_hits = df.Filter([](int type) { return type == 1; }, {"type"});
  const unsigned int nslots = df_hits.GetNSlots();
  const int bins = analysis_time::kFineBins;
  const int size = analysis_time::kFineCalibSize;
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

  std::vector<long long> counts(size * bins, 0);
  for (const auto &slot : slot_counts) {
    for (size_t i = 0; i < counts.size(); ++i) {
      counts[i] += slot[i];
    }
  }
  std::vector<long long> counts_tdc(4 * bins, 0);
  for (const auto &slot : slot_tdc_counts) {
    for (size_t i = 0; i < counts_tdc.size(); ++i) {
      counts_tdc[i] += slot[i];
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
  for (int t = 0; t < 4; ++t) {
    std::string name = "hFineTdc" + std::to_string(t);
    std::string title = "Fine distribution (TDC " + std::to_string(t) + ");fine;entries";
    auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), bins, 0.5, bins + 0.5);
    hist->SetDirectory(nullptr);
    htdc[t] = std::move(hist);
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
    for (int b = 0; b < bins; ++b) {
      htdc[t]->SetBinContent(b + 1, static_cast<double>(counts_tdc[t * bins + b]));
    }
    htdc[t]->Write();
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
}
