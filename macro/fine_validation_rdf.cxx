#include <ROOT/RDataFrame.hxx>
#include <TCanvas.h>
#include <TFile.h>
#include <TH1D.h>
#include <TMath.h>
#include <TParameter.h>
#include <TLegend.h>
#include <TLatex.h>
#include <TStyle.h>
#include <TSystem.h>

#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
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

int BinsForWindow(double window_ns)
{
  int bins = static_cast<int>(std::ceil(window_ns * 4.0));
  if (bins < 50) {
    bins = 50;
  }
  if (bins > 400) {
    bins = 400;
  }
  return bins;
}

struct Metrics {
  long long n = 0;
  double mean = std::numeric_limits<double>::quiet_NaN();
  double stddev = std::numeric_limits<double>::quiet_NaN();
  double d_ks = std::numeric_limits<double>::quiet_NaN();
  double p_ks = std::numeric_limits<double>::quiet_NaN();
};

std::vector<float> BuildLut(const long long *counts, int bins, long long total)
{
  std::vector<float> lut(bins, 0.0f);
  if (total <= 0) {
    return lut;
  }
  long long cum = 0;
  for (int b = 0; b < bins; ++b) {
    const long long c = counts[b];
    cum += c;
    double frac = (static_cast<double>(cum) - 0.5 * static_cast<double>(c)) / static_cast<double>(total);
    if (frac < 0.0) {
      frac = 0.0;
    } else if (frac > 1.0) {
      frac = 1.0;
    }
    lut[b] = static_cast<float>(frac - 0.5);
  }
  return lut;
}

Metrics ComputeKsUniform(std::vector<float> values)
{
  Metrics m;
  if (values.empty()) {
    return m;
  }
  std::sort(values.begin(), values.end());
  const long long n = static_cast<long long>(values.size());
  m.n = n;

  double sum = 0.0;
  double sum2 = 0.0;
  for (float v : values) {
    sum += v;
    sum2 += v * v;
  }
  m.mean = sum / static_cast<double>(n);
  double var = sum2 / static_cast<double>(n) - m.mean * m.mean;
  m.stddev = var > 0.0 ? std::sqrt(var) : 0.0;

  double d = 0.0;
  for (long long i = 0; i < n; ++i) {
    const double x = values[static_cast<size_t>(i)];
    const double f_theory = std::min(1.0, std::max(0.0, x + 0.5));
    const double f_emp = (static_cast<double>(i) + 1.0) / static_cast<double>(n);
    const double diff = std::abs(f_emp - f_theory);
    if (diff > d) {
      d = diff;
    }
  }
  m.d_ks = d;
  const double en = std::sqrt(static_cast<double>(n));
  const double z = (en + 0.12 + 0.11 / en) * d;
  m.p_ks = TMath::KolmogorovProb(z);
  return m;
}

std::unique_ptr<TH1D> BuildCdfResidualHist(const std::vector<float> &values, const std::string &name, int bins)
{
  if (values.empty()) {
    return {};
  }
  auto hist = std::make_unique<TH1D>(name.c_str(), "CDF residual (intrinsic vs cross);fine_fraction;F_{emp}-F_{uni}",
                                     bins, -0.5, 0.5);
  hist->SetDirectory(nullptr);
  std::vector<long long> counts(static_cast<size_t>(bins), 0);
  for (float v : values) {
    int bin = hist->GetXaxis()->FindBin(v);
    if (bin < 1 || bin > bins) {
      continue;
    }
    counts[static_cast<size_t>(bin - 1)] += 1;
  }
  const long long total = static_cast<long long>(values.size());
  long long cum = 0;
  for (int b = 1; b <= bins; ++b) {
    const long long count = counts[static_cast<size_t>(b - 1)];
    cum += count;
    double f_emp = 0.0;
    if (total > 0) {
      f_emp = (static_cast<double>(cum) - 0.5 * static_cast<double>(count)) / static_cast<double>(total);
    }
    const double x = hist->GetBinCenter(b);
    const double f_theory = std::min(1.0, std::max(0.0, x + 0.5));
    hist->SetBinContent(b, f_emp - f_theory);
  }
  return hist;
}

void PrintMetrics(const char *label, const Metrics &m)
{
  std::cout.setf(std::ios::fixed);
  std::cout << std::setprecision(4);
  std::cout << label << " n=" << m.n << " mean=" << m.mean << " std=" << m.stddev
            << " D=" << m.d_ks << " p=" << m.p_ks << std::endl;
}

void DrawMetricsBox(double x, double y, const char *label, const Metrics &m)
{
  TLatex latex;
  latex.SetNDC(true);
  latex.SetTextFont(42);
  latex.SetTextSize(0.035);
  std::ostringstream line1;
  line1 << label << "  D=" << std::fixed << std::setprecision(3) << m.d_ks
        << "  p=" << std::fixed << std::setprecision(3) << m.p_ks;
  std::ostringstream line2;
  line2 << "mean=" << std::fixed << std::setprecision(3) << m.mean
        << "  std=" << std::fixed << std::setprecision(3) << m.stddev;
  latex.DrawLatex(x, y, line1.str().c_str());
  latex.DrawLatex(x, y - 0.05, line2.str().c_str());
}
}  // namespace

void fine_validation_rdf(const char *input = "../data/calibration",
                         const char *out_pdf = "fine_validation.pdf",
                         const char *out_root = "fine_validation.root",
                         const char *out_txt = "fine_validation.txt",
                         long long min_entries = 200)
{
  if (WantsHelp(input) || WantsHelp(out_pdf)) {
    std::cout << "fine_validation_rdf usage:\n";
    std::cout << "  fine_validation_rdf(\"/path/to/decoded_or_parent\", \"out.pdf\", \"out.root\", \"out.txt\", 200)\n";
    std::cout << "  required branches: type,fifo,column,pixel,tdc,fine\n";
    return;
  }

  ScopedTimer timer("fine_validation_rdf");
  ROOT::EnableImplicitMT();
  gStyle->SetOptStat(0);

  auto input_spec = analysis_io::ResolveInputSpec(input ? input : "");
  if (input_spec.files.empty()) {
    std::cout << "No decoded ROOT files found under " << (input ? input : "") << std::endl;
    return;
  }

  ROOT::RDataFrame df(input_spec.tree_name.c_str(), input_spec.files);
  auto colnames = df.GetColumnNames();
  if (colnames.empty()) {
    std::cout << "Error: no branches found in tree '" << input_spec.tree_name << "'." << std::endl;
    return;
  }

  std::vector<std::string> missing;
  for (const auto &name : {"type", "fifo", "column", "pixel", "tdc", "fine", "rollover", "coarse"}) {
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
  auto fifos = df_hits.Take<int>("fifo");
  auto columns = df_hits.Take<int>("column");
  auto pixels = df_hits.Take<int>("pixel");
  auto tdcs = df_hits.Take<int>("tdc");
  auto fines = df_hits.Take<int>("fine");
  auto rollovers = df_hits.Take<int>("rollover");
  auto coarses = df_hits.Take<int>("coarse");
  ROOT::RDF::RunGraphs({fifos, columns, pixels, tdcs, fines, rollovers, coarses});

  const auto &fifos_val = fifos.GetValue();
  const auto &columns_val = columns.GetValue();
  const auto &pixels_val = pixels.GetValue();
  const auto &tdcs_val = tdcs.GetValue();
  const auto &fines_val = fines.GetValue();
  const auto &rollovers_val = rollovers.GetValue();
  const auto &coarses_val = coarses.GetValue();

  const int bins = analysis_time::kFineBins;
  const int size = analysis_time::kFineCalibSize;

  std::vector<long long> counts_full(size * bins, 0);
  std::vector<long long> counts_even(size * bins, 0);
  std::vector<long long> counts_odd(size * bins, 0);
  std::vector<long long> totals_full(size, 0);
  std::vector<long long> totals_even(size, 0);
  std::vector<long long> totals_odd(size, 0);

  const size_t n_hits = fines_val.size();
  for (size_t i = 0; i < n_hits; ++i) {
    const int fine = fines_val[i];
    const int tdc = tdcs_val[i];
    if (fine < 0 || fine >= bins) {
      continue;
    }
    if (tdc < 0 || tdc > 3) {
      continue;
    }
    const int idx = analysis_time::TdcIndex(fifos_val[i], columns_val[i], pixels_val[i], tdc);
    if (idx < 0 || idx >= size) {
      continue;
    }
    const int offset = idx * bins + fine;
    counts_full[offset] += 1;
    totals_full[idx] += 1;
    if ((i & 1U) == 0U) {
      counts_even[offset] += 1;
      totals_even[idx] += 1;
    } else {
      counts_odd[offset] += 1;
      totals_odd[idx] += 1;
    }
  }

  std::vector<std::vector<float>> lut_full(size);
  std::vector<std::vector<float>> lut_even(size);
  std::vector<std::vector<float>> lut_odd(size);
  std::vector<char> valid_full(size, 0);
  std::vector<char> valid_even(size, 0);
  std::vector<char> valid_odd(size, 0);

  for (int idx = 0; idx < size; ++idx) {
    if (totals_full[idx] >= min_entries) {
      lut_full[idx] = BuildLut(&counts_full[idx * bins], bins, totals_full[idx]);
      valid_full[idx] = 1;
    }
    if (totals_even[idx] >= min_entries) {
      lut_even[idx] = BuildLut(&counts_even[idx * bins], bins, totals_even[idx]);
      valid_even[idx] = 1;
    }
    if (totals_odd[idx] >= min_entries) {
      lut_odd[idx] = BuildLut(&counts_odd[idx * bins], bins, totals_odd[idx]);
      valid_odd[idx] = 1;
    }
  }

  std::array<std::vector<float>, 5> intrinsic_vals;
  std::array<std::vector<float>, 5> cross_vals;
  for (auto &v : intrinsic_vals) {
    v.reserve(n_hits / 4 + 1);
  }
  for (auto &v : cross_vals) {
    v.reserve(n_hits / 4 + 1);
  }

  for (size_t i = 0; i < n_hits; ++i) {
    const int fine = fines_val[i];
    const int tdc = tdcs_val[i];
    if (fine < 0 || fine >= bins) {
      continue;
    }
    if (tdc < 0 || tdc > 3) {
      continue;
    }
    const int idx = analysis_time::TdcIndex(fifos_val[i], columns_val[i], pixels_val[i], tdc);
    if (idx < 0 || idx >= size) {
      continue;
    }
    if (!valid_full[idx]) {
      continue;
    }
    const float fine_intr = lut_full[idx][fine];
    intrinsic_vals[tdc].push_back(fine_intr);
    intrinsic_vals[4].push_back(fine_intr);

    bool even = ((i & 1U) == 0U);
    if (even && valid_odd[idx]) {
      const float fine_cross = lut_odd[idx][fine];
      cross_vals[tdc].push_back(fine_cross);
      cross_vals[4].push_back(fine_cross);
    } else if (!even && valid_even[idx]) {
      const float fine_cross = lut_even[idx][fine];
      cross_vals[tdc].push_back(fine_cross);
      cross_vals[4].push_back(fine_cross);
    }
  }

  std::array<Metrics, 5> intrinsic_metrics;
  std::array<Metrics, 5> cross_metrics;
  for (int i = 0; i < 5; ++i) {
    intrinsic_metrics[i] = ComputeKsUniform(intrinsic_vals[i]);
    cross_metrics[i] = ComputeKsUniform(cross_vals[i]);
  }

  std::unique_ptr<TH1D> h_dt_raw;
  std::unique_ptr<TH1D> h_dt_lut;
  std::unique_ptr<TH1D> h_dt_raw_search;
  std::unique_ptr<TH1D> h_dt_lut_search;

  {
    // Calibration comparison: LUT vs no-LUT (default formula) using calibration data.
    const double tick_ns = analysis_time::TickNs(320.0);
    const int ch_a = 17;
    const int ch_b = 19;
    std::vector<double> times_a_lut;
    std::vector<double> times_b_lut;
    std::vector<double> times_a_raw;
    std::vector<double> times_b_raw;
    times_a_lut.reserve(n_hits / 8 + 1);
    times_b_lut.reserve(n_hits / 8 + 1);
    times_a_raw.reserve(n_hits / 8 + 1);
    times_b_raw.reserve(n_hits / 8 + 1);

    for (size_t i = 0; i < n_hits; ++i) {
      const int fine = fines_val[i];
      const int tdc = tdcs_val[i];
      if (fine < 0 || fine >= bins) {
        continue;
      }
      if (tdc < 0 || tdc > 3) {
        continue;
      }
      const int column = columns_val[i];
      const int pixel = pixels_val[i];
      const int channel = column * 4 + pixel;
      if (channel != ch_a && channel != ch_b) {
        continue;
      }
      const int idx = analysis_time::TdcIndex(fifos_val[i], column, pixel, tdc);
      if (idx < 0 || idx >= size) {
        continue;
      }
      const Long64_t time_tick = analysis_time::TimeTick(rollovers_val[i], coarses_val[i]);
      const double fine_raw = analysis_time::FineFractionDefault(fine);
      double fine_lut = fine_raw;
      if (valid_full[idx]) {
        fine_lut = lut_full[idx][fine];
      }
      const double time_raw = (static_cast<double>(time_tick) - fine_raw) * tick_ns;
      const double time_lut = (static_cast<double>(time_tick) - fine_lut) * tick_ns;
      if (channel == ch_a) {
        times_a_raw.push_back(time_raw);
        times_a_lut.push_back(time_lut);
      } else {
        times_b_raw.push_back(time_raw);
        times_b_lut.push_back(time_lut);
      }
    }

    auto fill_dt = [](const std::vector<double> &a, const std::vector<double> &b, double window, TH1D &hist) {
      if (a.empty() || b.empty()) {
        return;
      }
      size_t j = 0;
      for (size_t i = 0; i < a.size(); ++i) {
        const double t = a[i];
        while (j < b.size() && b[j] < t - window) {
          ++j;
        }
        size_t k = j;
        while (k < b.size() && b[k] <= t + window) {
          hist.Fill(b[k] - t);
          ++k;
        }
      }
    };

    std::sort(times_a_raw.begin(), times_a_raw.end());
    std::sort(times_b_raw.begin(), times_b_raw.end());
    std::sort(times_a_lut.begin(), times_a_lut.end());
    std::sort(times_b_lut.begin(), times_b_lut.end());

    const double window_ns = 20.0;
    const double search_window_ns = std::max(window_ns * 15.0, 100.0 + 4.0 * window_ns);
    const int bins_coinc = BinsForWindow(window_ns);
    const int bins_search = BinsForWindow(search_window_ns);

    if (!times_a_raw.empty() && !times_b_raw.empty()) {
      std::cout << "Calibration Δt comparison: ch " << ch_a << " vs " << ch_b
                << " window=" << window_ns << " ns" << std::endl;
      h_dt_raw = std::make_unique<TH1D>("h_dt_raw",
                                        "Calibration data #Deltat (no LUT);#Deltat [ns];pairs",
                                        bins_coinc,
                                        -window_ns,
                                        window_ns);
      h_dt_lut = std::make_unique<TH1D>("h_dt_lut",
                                        "Calibration data #Deltat (LUT);#Deltat [ns];pairs",
                                        bins_coinc,
                                        -window_ns,
                                        window_ns);
      h_dt_raw_search = std::make_unique<TH1D>("h_dt_raw_search",
                                               "Calibration data #Deltat search (no LUT);#Deltat [ns];pairs",
                                               bins_search,
                                               -search_window_ns,
                                               search_window_ns);
      h_dt_lut_search = std::make_unique<TH1D>("h_dt_lut_search",
                                               "Calibration data #Deltat search (LUT);#Deltat [ns];pairs",
                                               bins_search,
                                               -search_window_ns,
                                               search_window_ns);
      h_dt_raw->SetDirectory(nullptr);
      h_dt_lut->SetDirectory(nullptr);
      h_dt_raw_search->SetDirectory(nullptr);
      h_dt_lut_search->SetDirectory(nullptr);

      fill_dt(times_a_raw, times_b_raw, window_ns, *h_dt_raw);
      fill_dt(times_a_lut, times_b_lut, window_ns, *h_dt_lut);
      fill_dt(times_a_raw, times_b_raw, search_window_ns, *h_dt_raw_search);
      fill_dt(times_a_lut, times_b_lut, search_window_ns, *h_dt_lut_search);
    } else {
      std::cout << "Calibration Δt comparison skipped (missing channels " << ch_a << " or " << ch_b << ")"
                << std::endl;
    }
  }

  auto out = std::unique_ptr<TFile>(TFile::Open(out_root, "RECREATE"));
  if (!out || out->IsZombie()) {
    std::cout << "Failed to open output ROOT file: " << out_root << std::endl;
    return;
  }

  std::array<std::unique_ptr<TH1D>, 5> h_intr;
  std::array<std::unique_ptr<TH1D>, 5> h_cross;
  for (int i = 0; i < 5; ++i) {
    std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
    std::string name_intr = "hFineFraction_intr_" + tag;
    std::string name_cross = "hFineFraction_cross_" + tag;
    h_intr[i] = std::make_unique<TH1D>(name_intr.c_str(), ("Fine fraction (intrinsic vs cross) " + tag + ";fine_fraction;entries").c_str(),
                                       100, -0.5, 0.5);
    h_cross[i] = std::make_unique<TH1D>(name_cross.c_str(), ("Fine fraction (intrinsic vs cross) " + tag + ";fine_fraction;entries").c_str(),
                                        100, -0.5, 0.5);
    h_intr[i]->SetDirectory(nullptr);
    h_cross[i]->SetDirectory(nullptr);
    for (float v : intrinsic_vals[i]) {
      h_intr[i]->Fill(v);
    }
    for (float v : cross_vals[i]) {
      h_cross[i]->Fill(v);
    }
  }

  const int cdf_bins = 100;
  std::array<std::unique_ptr<TH1D>, 5> h_cdf_intr;
  std::array<std::unique_ptr<TH1D>, 5> h_cdf_cross;
  for (int i = 0; i < 5; ++i) {
    h_cdf_intr[i] = BuildCdfResidualHist(intrinsic_vals[i], "hCdfResidual_intr_" + std::to_string(i), cdf_bins);
    h_cdf_cross[i] = BuildCdfResidualHist(cross_vals[i], "hCdfResidual_cross_" + std::to_string(i), cdf_bins);
  }

  out->cd();
  for (int i = 0; i < 5; ++i) {
    if (h_intr[i]) h_intr[i]->Write();
    if (h_cross[i]) h_cross[i]->Write();
    if (h_cdf_intr[i]) h_cdf_intr[i]->Write();
    if (h_cdf_cross[i]) h_cdf_cross[i]->Write();
  }
  if (h_dt_raw) h_dt_raw->Write();
  if (h_dt_lut) h_dt_lut->Write();
  if (h_dt_raw_search) h_dt_raw_search->Write();
  if (h_dt_lut_search) h_dt_lut_search->Write();
  TParameter<long long> p_min_entries("min_entries", min_entries);
  p_min_entries.Write();
  out->Close();

  std::ofstream txt(out_txt);
  if (txt) {
    txt << "# fine LUT validation (unbinned KS vs uniform)\n";
    txt << "# input: " << (input ? input : "") << "\n";
    txt << "# min_entries: " << min_entries << "\n";
    txt << "# groups: tdc0..tdc3, all\n";
    auto dump = [&](const char *label, const Metrics &m) {
      txt << label << " n=" << m.n << " mean=" << m.mean << " std=" << m.stddev
          << " D=" << m.d_ks << " p=" << m.p_ks << "\n";
    };
    for (int i = 0; i < 5; ++i) {
      std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
      dump(("intrinsic_" + tag).c_str(), intrinsic_metrics[i]);
      dump(("cross_" + tag).c_str(), cross_metrics[i]);
    }
  }

  std::cout << "Intrinsic/Cross-validation metrics (KS vs uniform):" << std::endl;
  for (int i = 0; i < 5; ++i) {
    std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
    PrintMetrics(("intrinsic_" + tag).c_str(), intrinsic_metrics[i]);
    PrintMetrics(("cross_" + tag).c_str(), cross_metrics[i]);
  }
  std::cout << "Metrics written to " << out_txt << std::endl;

  std::string out_open = std::string(out_pdf) + "[";
  std::string out_close = std::string(out_pdf) + "]";
  TCanvas c_open("c_open", "c_open", 1600, 900);
  c_open.Print(out_open.c_str());

  auto draw_hist_page = [&](int idx_start, int idx_end) {
    TCanvas c("c_hist", "c_hist", 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = idx_start; i <= idx_end; ++i) {
      c.cd(pad++);
      if (!h_intr[i] || !h_cross[i]) {
        continue;
      }
      h_intr[i]->SetLineColor(kBlue + 1);
      h_intr[i]->SetLineWidth(2);
      h_cross[i]->SetLineColor(kRed + 1);
      h_cross[i]->SetLineWidth(2);
      h_intr[i]->Draw("hist");
      h_cross[i]->Draw("hist same");
      TLegend leg(0.62, 0.75, 0.88, 0.88);
      leg.AddEntry(h_intr[i].get(), "intrinsic", "l");
      leg.AddEntry(h_cross[i].get(), "cross", "l");
      leg.Draw();
      DrawMetricsBox(0.12, 0.88, "intr", intrinsic_metrics[i]);
      DrawMetricsBox(0.12, 0.76, "cross", cross_metrics[i]);
    }
    c.Print(out_pdf);
  };

  draw_hist_page(0, 3);

  {
    TCanvas c("c_hist_all", "c_hist_all", 1600, 900);
    h_intr[4]->SetLineColor(kBlue + 1);
    h_intr[4]->SetLineWidth(2);
    h_cross[4]->SetLineColor(kRed + 1);
    h_cross[4]->SetLineWidth(2);
    h_intr[4]->Draw("hist");
    h_cross[4]->Draw("hist same");
    TLegend leg(0.62, 0.75, 0.88, 0.88);
    leg.AddEntry(h_intr[4].get(), "intrinsic", "l");
    leg.AddEntry(h_cross[4].get(), "cross", "l");
    leg.Draw();
    DrawMetricsBox(0.12, 0.88, "intr", intrinsic_metrics[4]);
    DrawMetricsBox(0.12, 0.76, "cross", cross_metrics[4]);
    c.Print(out_pdf);
  }

  auto draw_cdf_page = [&](int idx_start, int idx_end) {
    TCanvas c("c_cdf", "c_cdf", 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = idx_start; i <= idx_end; ++i) {
      c.cd(pad++);
      if (h_cdf_intr[i]) {
        h_cdf_intr[i]->SetLineColor(kBlue + 1);
        h_cdf_intr[i]->SetLineWidth(2);
        h_cdf_intr[i]->Draw("hist");
      }
      if (h_cdf_cross[i]) {
        h_cdf_cross[i]->SetLineColor(kRed + 1);
        h_cdf_cross[i]->SetLineWidth(2);
        h_cdf_cross[i]->Draw("hist same");
      }
      TLegend leg(0.62, 0.75, 0.88, 0.88);
      if (h_cdf_intr[i]) leg.AddEntry(h_cdf_intr[i].get(), "intrinsic", "l");
      if (h_cdf_cross[i]) leg.AddEntry(h_cdf_cross[i].get(), "cross", "l");
      leg.Draw();
      DrawMetricsBox(0.12, 0.88, "intr", intrinsic_metrics[i]);
      DrawMetricsBox(0.12, 0.76, "cross", cross_metrics[i]);
    }
    c.Print(out_pdf);
  };

  draw_cdf_page(0, 3);

  {
    TCanvas c("c_cdf_all", "c_cdf_all", 1600, 900);
    if (h_cdf_intr[4]) {
      h_cdf_intr[4]->SetLineColor(kBlue + 1);
      h_cdf_intr[4]->SetLineWidth(2);
      h_cdf_intr[4]->Draw("hist");
    }
    if (h_cdf_cross[4]) {
      h_cdf_cross[4]->SetLineColor(kRed + 1);
      h_cdf_cross[4]->SetLineWidth(2);
      h_cdf_cross[4]->Draw("hist same");
    }
    TLegend leg(0.62, 0.75, 0.88, 0.88);
    if (h_cdf_intr[4]) leg.AddEntry(h_cdf_intr[4].get(), "intrinsic", "l");
    if (h_cdf_cross[4]) leg.AddEntry(h_cdf_cross[4].get(), "cross", "l");
    leg.Draw();
    DrawMetricsBox(0.12, 0.88, "intr", intrinsic_metrics[4]);
    DrawMetricsBox(0.12, 0.76, "cross", cross_metrics[4]);
    c.Print(out_pdf);
  }

  if (h_dt_raw_search && h_dt_lut_search) {
    TCanvas c("c_calib_dt_search", "c_calib_dt_search", 1600, 900);
    h_dt_raw_search->SetLineColor(kRed + 1);
    h_dt_raw_search->SetLineWidth(2);
    h_dt_lut_search->SetLineColor(kBlue + 1);
    h_dt_lut_search->SetLineWidth(2);
    h_dt_raw_search->Draw("hist");
    h_dt_lut_search->Draw("hist same");
    TLegend leg(0.62, 0.75, 0.88, 0.88);
    leg.AddEntry(h_dt_raw_search.get(), "no LUT", "l");
    leg.AddEntry(h_dt_lut_search.get(), "LUT", "l");
    leg.Draw();
    c.Print(out_pdf);
  }

  if (h_dt_raw && h_dt_lut) {
    TCanvas c("c_calib_dt_coinc", "c_calib_dt_coinc", 1600, 900);
    h_dt_raw->SetLineColor(kRed + 1);
    h_dt_raw->SetLineWidth(2);
    h_dt_lut->SetLineColor(kBlue + 1);
    h_dt_lut->SetLineWidth(2);
    h_dt_raw->Draw("hist");
    h_dt_lut->Draw("hist same");
    TLegend leg(0.62, 0.75, 0.88, 0.88);
    leg.AddEntry(h_dt_raw.get(), "no LUT", "l");
    leg.AddEntry(h_dt_lut.get(), "LUT", "l");
    leg.Draw();
    c.Print(out_pdf);
  }

  c_open.Print(out_close.c_str());
}
