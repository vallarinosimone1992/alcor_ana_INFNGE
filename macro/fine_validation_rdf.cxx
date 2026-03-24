#include <ROOT/RDataFrame.hxx>
#include <TCanvas.h>
#include <TFile.h>
#include <TH1D.h>
#include <TMath.h>
#include <TParameter.h>
#include <TLegend.h>
#include <TLatex.h>
#include <TLine.h>
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
  double a2_ad = std::numeric_limits<double>::quiet_NaN();
};

std::vector<long long> BuildCountsFromValues(const std::vector<float> &values, int bins, double xmin, double xmax)
{
  std::vector<long long> counts(static_cast<size_t>(bins), 0);
  if (values.empty()) {
    return counts;
  }
  const double width = xmax - xmin;
  if (width <= 0.0) {
    return counts;
  }
  for (float v : values) {
    if (v < xmin || v >= xmax) {
      continue;
    }
    const double pos = (static_cast<double>(v) - xmin) / width;
    int bin = static_cast<int>(pos * bins);
    if (bin < 0) {
      bin = 0;
    } else if (bin >= bins) {
      bin = bins - 1;
    }
    counts[static_cast<size_t>(bin)] += 1;
  }
  return counts;
}

void ComputeDnlInl(const std::vector<long long> &counts, std::vector<double> &dnl, std::vector<double> &inl)
{
  const int bins = static_cast<int>(counts.size());
  dnl.assign(static_cast<size_t>(bins), 0.0);
  inl.assign(static_cast<size_t>(bins), 0.0);
  long long total = 0;
  for (long long c : counts) {
    total += c;
  }
  if (bins <= 0 || total <= 0) {
    return;
  }
  const double avg = static_cast<double>(total) / static_cast<double>(bins);
  long long cum = 0;
  for (int b = 0; b < bins; ++b) {
    const long long c = counts[static_cast<size_t>(b)];
    dnl[static_cast<size_t>(b)] = (avg > 0.0) ? (static_cast<double>(c) / avg - 1.0) : 0.0;
    cum += c;
    const double f_emp = static_cast<double>(cum) / static_cast<double>(total);
    const double f_ideal = (static_cast<double>(b) + 0.5) / static_cast<double>(bins);
    inl[static_cast<size_t>(b)] = f_emp - f_ideal;
  }
}

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

std::vector<float> BuildLinearMap(const long long *counts, int bins, double q_low, double q_high)
{
  std::vector<float> lut(bins, 0.0f);
  long long total = 0;
  for (int b = 0; b < bins; ++b) {
    total += counts[b];
  }
  if (total <= 0) {
    return lut;
  }
  auto quantile = [&](double q) {
    long long target = static_cast<long long>(std::floor(q * static_cast<double>(total)));
    long long cum = 0;
    for (int b = 0; b < bins; ++b) {
      cum += counts[b];
      if (cum >= target) {
        return b + 1;
      }
    }
    return bins;
  };
  int low_bin = quantile(q_low);
  int high_bin = quantile(q_high);
  if (low_bin < 1) {
    low_bin = 1;
  }
  if (high_bin <= low_bin) {
    high_bin = low_bin + 1;
  }
  double min_val = static_cast<double>(low_bin);
  double max_val = static_cast<double>(high_bin);
  double range = max_val - min_val;
  if (range <= 0.0) {
    return lut;
  }
  const double cut = 0.5 * (min_val + max_val);
  for (int b = 0; b < bins; ++b) {
    double fine = (static_cast<double>(b + 1) - min_val) / range;
    if (static_cast<double>(b + 1) > cut) {
      fine -= 1.0;
    }
    lut[b] = static_cast<float>(fine);
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

  const double eps = 1.0 / (static_cast<double>(n) * 100.0);
  double ad_sum = 0.0;
  for (long long i = 1; i <= n; ++i) {
    double ui = values[static_cast<size_t>(i - 1)] + 0.5;
    double uj = values[static_cast<size_t>(n - i)] + 0.5;
    ui = std::min(1.0 - eps, std::max(eps, ui));
    uj = std::min(1.0 - eps, std::max(eps, uj));
    ad_sum += (2.0 * static_cast<double>(i) - 1.0) * (std::log(ui) + std::log(1.0 - uj));
  }
  m.a2_ad = -static_cast<double>(n) - ad_sum / static_cast<double>(n);
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
            << " D=" << m.d_ks << " p=" << m.p_ks << " AD=" << m.a2_ad << std::endl;
}

void DrawMetricsBox(double x, double y, const char *label, const Metrics &m)
{
  TLatex latex;
  latex.SetNDC(true);
  latex.SetTextFont(42);
  latex.SetTextSize(0.035);
  std::ostringstream line1;
  line1 << label << "  D=" << std::fixed << std::setprecision(3) << m.d_ks
        << "  p=" << std::fixed << std::setprecision(3) << m.p_ks
        << "  AD=" << std::fixed << std::setprecision(3) << m.a2_ad;
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
  const std::array<int, 2> watch_channels = {17, 19};
  std::array<std::vector<long long>, 2> raw_counts_ch;
  for (auto &vec : raw_counts_ch) {
    vec.assign(static_cast<size_t>(bins), 0);
  }
  std::array<std::array<std::vector<long long>, 4>, 2> raw_counts_ch_tdc;
  for (auto &arr : raw_counts_ch_tdc) {
    for (auto &vec : arr) {
      vec.assign(static_cast<size_t>(bins), 0);
    }
  }

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
    const int column = columns_val[i];
    const int pixel = pixels_val[i];
    const int channel = column * 4 + pixel;
    const int idx = analysis_time::TdcIndex(fifos_val[i], column, pixel, tdc);
    if (idx < 0 || idx >= size) {
      continue;
    }
    const int offset = idx * bins + fine;
    counts_full[offset] += 1;
    totals_full[idx] += 1;
    for (size_t c = 0; c < watch_channels.size(); ++c) {
      if (channel == watch_channels[c]) {
        raw_counts_ch[c][static_cast<size_t>(fine)] += 1;
        raw_counts_ch_tdc[c][tdc][static_cast<size_t>(fine)] += 1;
      }
    }
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
  std::vector<std::vector<float>> lut_linear(size);
  std::vector<char> valid_linear(size, 0);
  std::vector<char> valid_full(size, 0);
  std::vector<char> valid_even(size, 0);
  std::vector<char> valid_odd(size, 0);

  for (int idx = 0; idx < size; ++idx) {
    if (totals_full[idx] >= min_entries) {
      lut_full[idx] = BuildLut(&counts_full[idx * bins], bins, totals_full[idx]);
      valid_full[idx] = 1;
      lut_linear[idx] = BuildLinearMap(&counts_full[idx * bins], bins, 0.05, 0.95);
      valid_linear[idx] = 1;
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

  std::array<std::vector<float>, 5> raw_vals;
  std::array<std::vector<float>, 5> linear_vals;
  std::array<std::vector<float>, 5> intrinsic_vals;
  std::array<std::vector<float>, 5> cross_vals;
  for (auto &v : raw_vals) {
    v.reserve(n_hits / 4 + 1);
  }
  for (auto &v : linear_vals) {
    v.reserve(n_hits / 4 + 1);
  }
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
    const float fine_raw = static_cast<float>((static_cast<double>(fine) + 0.5) / static_cast<double>(bins) - 0.5);
    raw_vals[tdc].push_back(fine_raw);
    raw_vals[4].push_back(fine_raw);

    const float fine_lin = valid_linear[idx] ? lut_linear[idx][fine] : fine_raw;
    linear_vals[tdc].push_back(fine_lin);
    linear_vals[4].push_back(fine_lin);

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

  std::array<Metrics, 5> raw_metrics;
  std::array<Metrics, 5> linear_metrics;
  std::array<Metrics, 5> intrinsic_metrics;
  std::array<Metrics, 5> cross_metrics;
  for (int i = 0; i < 5; ++i) {
    raw_metrics[i] = ComputeKsUniform(raw_vals[i]);
    linear_metrics[i] = ComputeKsUniform(linear_vals[i]);
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

  std::array<std::unique_ptr<TH1D>, 5> h_raw;
  std::array<std::unique_ptr<TH1D>, 5> h_lin;
  std::array<std::unique_ptr<TH1D>, 5> h_intr;
  std::array<std::unique_ptr<TH1D>, 5> h_cross;
  for (int i = 0; i < 5; ++i) {
    std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
    std::string name_raw = "hFineFraction_raw_" + tag;
    std::string name_lin = "hFineFraction_lin_" + tag;
    std::string name_intr = "hFineFraction_intr_" + tag;
    std::string name_cross = "hFineFraction_cross_" + tag;
    h_raw[i] = std::make_unique<TH1D>(name_raw.c_str(), ("Fine fraction (raw) " + tag + ";fine_fraction;entries").c_str(),
                                      100, -0.5, 0.5);
    h_lin[i] = std::make_unique<TH1D>(name_lin.c_str(), ("Fine fraction (linear) " + tag + ";fine_fraction;entries").c_str(),
                                      100, -0.5, 0.5);
    h_intr[i] = std::make_unique<TH1D>(name_intr.c_str(), ("Fine fraction (intrinsic vs cross) " + tag + ";fine_fraction;entries").c_str(),
                                       100, -0.5, 0.5);
    h_cross[i] = std::make_unique<TH1D>(name_cross.c_str(), ("Fine fraction (intrinsic vs cross) " + tag + ";fine_fraction;entries").c_str(),
                                        100, -0.5, 0.5);
    h_raw[i]->SetDirectory(nullptr);
    h_lin[i]->SetDirectory(nullptr);
    h_intr[i]->SetDirectory(nullptr);
    h_cross[i]->SetDirectory(nullptr);
    for (float v : raw_vals[i]) {
      h_raw[i]->Fill(v);
    }
    for (float v : linear_vals[i]) {
      h_lin[i]->Fill(v);
    }
    for (float v : intrinsic_vals[i]) {
      h_intr[i]->Fill(v);
    }
    for (float v : cross_vals[i]) {
      h_cross[i]->Fill(v);
    }
  }

  const int cdf_bins = 100;
  std::array<std::unique_ptr<TH1D>, 5> h_cdf_raw;
  std::array<std::unique_ptr<TH1D>, 5> h_cdf_lin;
  std::array<std::unique_ptr<TH1D>, 5> h_cdf_intr;
  std::array<std::unique_ptr<TH1D>, 5> h_cdf_cross;
  for (int i = 0; i < 5; ++i) {
    h_cdf_raw[i] = BuildCdfResidualHist(raw_vals[i], "hCdfResidual_raw_" + std::to_string(i), cdf_bins);
    h_cdf_lin[i] = BuildCdfResidualHist(linear_vals[i], "hCdfResidual_lin_" + std::to_string(i), cdf_bins);
    h_cdf_intr[i] = BuildCdfResidualHist(intrinsic_vals[i], "hCdfResidual_intr_" + std::to_string(i), cdf_bins);
    h_cdf_cross[i] = BuildCdfResidualHist(cross_vals[i], "hCdfResidual_cross_" + std::to_string(i), cdf_bins);
  }

  std::array<std::vector<long long>, 5> raw_counts;
  for (auto &v : raw_counts) {
    v.assign(static_cast<size_t>(bins), 0);
  }
  for (int idx = 0; idx < size; ++idx) {
    if (!valid_full[idx]) {
      continue;
    }
    const int local = idx % analysis_time::kTdcPerFifo;
    const int tdc = local % analysis_time::kTdcPerPixel;
    for (int b = 0; b < bins; ++b) {
      const long long count = counts_full[idx * bins + b];
      raw_counts[tdc][static_cast<size_t>(b)] += count;
      raw_counts[4][static_cast<size_t>(b)] += count;
    }
  }

  const int lut_bins = 100;
  std::array<std::vector<long long>, 5> lin_counts;
  std::array<std::vector<long long>, 5> lut_counts;
  for (int i = 0; i < 5; ++i) {
    lin_counts[i] = BuildCountsFromValues(linear_vals[i], lut_bins, -0.5, 0.5);
    lut_counts[i] = BuildCountsFromValues(intrinsic_vals[i], lut_bins, -0.5, 0.5);
  }

  std::array<std::unique_ptr<TH1D>, 2> h_raw_counts_ch;
  std::array<std::unique_ptr<TH1D>, 2> h_raw_cdf_ch;
  for (size_t i = 0; i < watch_channels.size(); ++i) {
    const int ch = watch_channels[i];
    h_raw_counts_ch[i] = std::make_unique<TH1D>(Form("hFineCounts_raw_ch%d", ch),
                                                Form("Fine counts (raw) ch %d;fine code;entries", ch),
                                                bins,
                                                0.5,
                                                bins + 0.5);
    h_raw_cdf_ch[i] = std::make_unique<TH1D>(Form("hFineCdf_raw_ch%d", ch),
                                             Form("Fine CDF (raw) ch %d;fine code;CDF", ch),
                                             bins,
                                             0.5,
                                             bins + 0.5);
    h_raw_counts_ch[i]->SetDirectory(nullptr);
    h_raw_cdf_ch[i]->SetDirectory(nullptr);
    long long total = 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = raw_counts_ch[i][static_cast<size_t>(b)];
      h_raw_counts_ch[i]->SetBinContent(b + 1, static_cast<double>(count));
      total += count;
    }
    long long cum = 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = raw_counts_ch[i][static_cast<size_t>(b)];
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
      h_raw_cdf_ch[i]->SetBinContent(b + 1, frac);
    }
  }

  std::array<std::unique_ptr<TH1D>, 5> h_raw_counts;
  std::array<std::unique_ptr<TH1D>, 5> h_raw_cdf;
  for (int i = 0; i < 5; ++i) {
    std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
    h_raw_counts[i] = std::make_unique<TH1D>(("hFineCounts_raw_" + tag).c_str(),
                                             ("Fine counts (raw) " + tag + ";fine code;entries").c_str(),
                                             bins,
                                             0.5,
                                             bins + 0.5);
    h_raw_cdf[i] = std::make_unique<TH1D>(("hFineCdf_raw_" + tag).c_str(),
                                          ("Fine CDF (raw) " + tag + ";fine code;CDF").c_str(),
                                          bins,
                                          0.5,
                                          bins + 0.5);
    h_raw_counts[i]->SetDirectory(nullptr);
    h_raw_cdf[i]->SetDirectory(nullptr);
    long long total = 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = raw_counts[i][static_cast<size_t>(b)];
      h_raw_counts[i]->SetBinContent(b + 1, static_cast<double>(count));
      total += count;
    }
    long long cum = 0;
    for (int b = 0; b < bins; ++b) {
      const long long count = raw_counts[i][static_cast<size_t>(b)];
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
      h_raw_cdf[i]->SetBinContent(b + 1, frac);
    }
  }

  std::array<std::unique_ptr<TH1D>, 5> h_dnl_raw;
  std::array<std::unique_ptr<TH1D>, 5> h_inl_raw;
  std::array<std::unique_ptr<TH1D>, 5> h_dnl_lin;
  std::array<std::unique_ptr<TH1D>, 5> h_inl_lin;
  std::array<std::unique_ptr<TH1D>, 5> h_dnl_lut;
  std::array<std::unique_ptr<TH1D>, 5> h_inl_lut;
  for (int i = 0; i < 5; ++i) {
    std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
    std::vector<double> dnl_raw;
    std::vector<double> inl_raw;
    std::vector<double> dnl_lin;
    std::vector<double> inl_lin;
    std::vector<double> dnl_lut;
    std::vector<double> inl_lut;
    ComputeDnlInl(raw_counts[i], dnl_raw, inl_raw);
    ComputeDnlInl(lin_counts[i], dnl_lin, inl_lin);
    ComputeDnlInl(lut_counts[i], dnl_lut, inl_lut);

    h_dnl_raw[i] = std::make_unique<TH1D>(("hFineDNL_raw_" + tag).c_str(),
                                          ("DNL (raw) " + tag + ";fine code;DNL").c_str(),
                                          bins,
                                          0.5,
                                          bins + 0.5);
    h_inl_raw[i] = std::make_unique<TH1D>(("hFineINL_raw_" + tag).c_str(),
                                          ("INL (raw) " + tag + ";fine code;INL").c_str(),
                                          bins,
                                          0.5,
                                          bins + 0.5);
    h_dnl_lin[i] = std::make_unique<TH1D>(("hFineDNL_lin_" + tag).c_str(),
                                          ("DNL (linear) " + tag + ";fine fraction;DNL").c_str(),
                                          lut_bins,
                                          -0.5,
                                          0.5);
    h_inl_lin[i] = std::make_unique<TH1D>(("hFineINL_lin_" + tag).c_str(),
                                          ("INL (linear) " + tag + ";fine fraction;INL").c_str(),
                                          lut_bins,
                                          -0.5,
                                          0.5);
    h_dnl_lut[i] = std::make_unique<TH1D>(("hFineDNL_lut_" + tag).c_str(),
                                          ("DNL (LUT) " + tag + ";fine fraction;DNL").c_str(),
                                          lut_bins,
                                          -0.5,
                                          0.5);
    h_inl_lut[i] = std::make_unique<TH1D>(("hFineINL_lut_" + tag).c_str(),
                                          ("INL (LUT) " + tag + ";fine fraction;INL").c_str(),
                                          lut_bins,
                                          -0.5,
                                          0.5);
    h_dnl_raw[i]->SetDirectory(nullptr);
    h_inl_raw[i]->SetDirectory(nullptr);
    h_dnl_lin[i]->SetDirectory(nullptr);
    h_inl_lin[i]->SetDirectory(nullptr);
    h_dnl_lut[i]->SetDirectory(nullptr);
    h_inl_lut[i]->SetDirectory(nullptr);
    for (int b = 0; b < bins && b < static_cast<int>(dnl_raw.size()); ++b) {
      h_dnl_raw[i]->SetBinContent(b + 1, dnl_raw[static_cast<size_t>(b)]);
      h_inl_raw[i]->SetBinContent(b + 1, inl_raw[static_cast<size_t>(b)]);
    }
    for (int b = 0; b < lut_bins && b < static_cast<int>(dnl_lin.size()); ++b) {
      h_dnl_lin[i]->SetBinContent(b + 1, dnl_lin[static_cast<size_t>(b)]);
      h_inl_lin[i]->SetBinContent(b + 1, inl_lin[static_cast<size_t>(b)]);
    }
    for (int b = 0; b < lut_bins && b < static_cast<int>(dnl_lut.size()); ++b) {
      h_dnl_lut[i]->SetBinContent(b + 1, dnl_lut[static_cast<size_t>(b)]);
      h_inl_lut[i]->SetBinContent(b + 1, inl_lut[static_cast<size_t>(b)]);
    }
  }

  out->cd();
  for (int i = 0; i < 5; ++i) {
    if (h_raw_counts[i]) h_raw_counts[i]->Write();
    if (h_raw_cdf[i]) h_raw_cdf[i]->Write();
    if (h_raw[i]) h_raw[i]->Write();
    if (h_lin[i]) h_lin[i]->Write();
    if (h_intr[i]) h_intr[i]->Write();
    if (h_cross[i]) h_cross[i]->Write();
    if (h_cdf_raw[i]) h_cdf_raw[i]->Write();
    if (h_cdf_lin[i]) h_cdf_lin[i]->Write();
    if (h_cdf_intr[i]) h_cdf_intr[i]->Write();
    if (h_cdf_cross[i]) h_cdf_cross[i]->Write();
    if (h_dnl_raw[i]) h_dnl_raw[i]->Write();
    if (h_inl_raw[i]) h_inl_raw[i]->Write();
    if (h_dnl_lin[i]) h_dnl_lin[i]->Write();
    if (h_inl_lin[i]) h_inl_lin[i]->Write();
    if (h_dnl_lut[i]) h_dnl_lut[i]->Write();
    if (h_inl_lut[i]) h_inl_lut[i]->Write();
  }
  for (size_t i = 0; i < watch_channels.size(); ++i) {
    if (h_raw_counts_ch[i]) h_raw_counts_ch[i]->Write();
    if (h_raw_cdf_ch[i]) h_raw_cdf_ch[i]->Write();
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
    txt << "# fine LUT validation (unbinned KS/AD vs uniform)\n";
    txt << "# input: " << (input ? input : "") << "\n";
    txt << "# min_entries: " << min_entries << "\n";
    txt << "# groups: tdc0..tdc3, all\n";
    auto dump = [&](const char *label, const Metrics &m) {
      txt << label << " n=" << m.n << " mean=" << m.mean << " std=" << m.stddev
          << " D=" << m.d_ks << " p=" << m.p_ks << " AD=" << m.a2_ad << "\n";
    };
    for (int i = 0; i < 5; ++i) {
      std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
      dump(("raw_" + tag).c_str(), raw_metrics[i]);
      dump(("linear_" + tag).c_str(), linear_metrics[i]);
      dump(("intrinsic_" + tag).c_str(), intrinsic_metrics[i]);
      dump(("cross_" + tag).c_str(), cross_metrics[i]);
    }
  }

  std::cout << "Uniformity metrics (KS/AD vs uniform):" << std::endl;
  for (int i = 0; i < 5; ++i) {
    std::string tag = (i < 4) ? ("tdc" + std::to_string(i)) : "all";
    PrintMetrics(("raw_" + tag).c_str(), raw_metrics[i]);
    PrintMetrics(("linear_" + tag).c_str(), linear_metrics[i]);
    PrintMetrics(("intrinsic_" + tag).c_str(), intrinsic_metrics[i]);
    PrintMetrics(("cross_" + tag).c_str(), cross_metrics[i]);
  }
  std::cout << "Metrics written to " << out_txt << std::endl;

  std::string out_open = std::string(out_pdf) + "[";
  std::string out_close = std::string(out_pdf) + "]";
  TCanvas c_open("c_open", "c_open", 1600, 900);
  c_open.Print(out_open.c_str());

  auto draw_raw_lut_page = [&](int idx_start, int idx_end) {
    TCanvas c("c_raw_lut", "c_raw_lut", 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = idx_start; i <= idx_end; ++i) {
      c.cd(pad++);
      if (!h_raw[i] || !h_lin[i] || !h_intr[i]) {
        continue;
      }
      h_raw[i]->SetLineColor(kRed + 1);
      h_raw[i]->SetLineWidth(2);
      h_lin[i]->SetLineColor(kOrange + 7);
      h_lin[i]->SetLineWidth(2);
      h_intr[i]->SetLineColor(kBlue + 1);
      h_intr[i]->SetLineWidth(2);
      h_raw[i]->Draw("hist");
      h_lin[i]->Draw("hist same");
      h_intr[i]->Draw("hist same");
      TLegend leg(0.58, 0.72, 0.88, 0.88);
      leg.AddEntry(h_raw[i].get(), "raw", "l");
      leg.AddEntry(h_lin[i].get(), "linear", "l");
      leg.AddEntry(h_intr[i].get(), "LUT", "l");
      leg.Draw();
      DrawMetricsBox(0.6, 0.86, "raw", raw_metrics[i]);
      DrawMetricsBox(0.6, 0.74, "lin", linear_metrics[i]);
      DrawMetricsBox(0.6, 0.62, "lut", intrinsic_metrics[i]);
    }
    c.Print(out_pdf);
  };

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
      DrawMetricsBox(0.6, 0.86, "intr", intrinsic_metrics[i]);
      DrawMetricsBox(0.6, 0.74, "cross", cross_metrics[i]);
    }
    c.Print(out_pdf);
  };

  draw_hist_page(0, 3);

  {
    TCanvas c("c_raw_lut_all", "c_raw_lut_all", 1600, 900);
    h_raw[4]->SetLineColor(kRed + 1);
    h_raw[4]->SetLineWidth(2);
    h_lin[4]->SetLineColor(kOrange + 7);
    h_lin[4]->SetLineWidth(2);
    h_intr[4]->SetLineColor(kBlue + 1);
    h_intr[4]->SetLineWidth(2);
    h_raw[4]->Draw("hist");
    h_lin[4]->Draw("hist same");
    h_intr[4]->Draw("hist same");
    TLegend leg(0.58, 0.72, 0.88, 0.88);
    leg.AddEntry(h_raw[4].get(), "raw", "l");
    leg.AddEntry(h_lin[4].get(), "linear", "l");
    leg.AddEntry(h_intr[4].get(), "LUT", "l");
    leg.Draw();
    DrawMetricsBox(0.6, 0.86, "raw", raw_metrics[4]);
    DrawMetricsBox(0.6, 0.74, "lin", linear_metrics[4]);
    DrawMetricsBox(0.6, 0.62, "lut", intrinsic_metrics[4]);
    c.Print(out_pdf);
  }

  draw_raw_lut_page(0, 3);

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
    DrawMetricsBox(0.6, 0.86, "intr", intrinsic_metrics[4]);
    DrawMetricsBox(0.6, 0.74, "cross", cross_metrics[4]);
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

  auto draw_uniform_line = [](TH1D *hist, int color) {
    if (!hist) {
      return;
    }
    const double mean = (hist->GetNbinsX() > 0)
                            ? (hist->GetSumOfWeights() / static_cast<double>(hist->GetNbinsX()))
                            : 0.0;
    auto *line = new TLine(hist->GetXaxis()->GetXmin(), mean, hist->GetXaxis()->GetXmax(), mean);
    line->SetLineColor(color);
    line->SetLineStyle(2);
    line->SetLineWidth(2);
    line->Draw("same");
  };

  draw_cdf_page(0, 3);

  {
    TCanvas c("c_raw_counts", "c_raw_counts", 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = 0; i < 4; ++i) {
      c.cd(pad++);
      if (!h_raw_counts[i]) {
        continue;
      }
      h_raw_counts[i]->SetLineColor(kBlue + 1);
      h_raw_counts[i]->SetLineWidth(2);
      h_raw_counts[i]->Draw("hist");
      draw_uniform_line(h_raw_counts[i].get(), kRed + 1);
    }
    c.Print(out_pdf);
  }

  {
    TCanvas c("c_raw_counts_ch", "c_raw_counts_ch", 1600, 900);
    c.Divide(2, 1, 0.001, 0.001);
    for (size_t i = 0; i < watch_channels.size(); ++i) {
      c.cd(static_cast<int>(i + 1));
      if (!h_raw_counts_ch[i]) {
        continue;
      }
      h_raw_counts_ch[i]->SetLineColor(kBlue + 1);
      h_raw_counts_ch[i]->SetLineWidth(2);
      h_raw_counts_ch[i]->Draw("hist");
      draw_uniform_line(h_raw_counts_ch[i].get(), kRed + 1);
    }
    c.Print(out_pdf);
  }

  {
    TCanvas c("c_raw_cdf", "c_raw_cdf", 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = 0; i < 4; ++i) {
      c.cd(pad++);
      if (!h_raw_cdf[i]) {
        continue;
      }
      h_raw_cdf[i]->SetLineColor(kBlue + 1);
      h_raw_cdf[i]->SetLineWidth(2);
      h_raw_cdf[i]->Draw("hist");
      TF1 ideal(Form("ideal_cdf_%d", i), Form("(x-0.5)/%d", bins), 0.5, bins + 0.5);
      ideal.SetLineColor(kRed + 1);
      ideal.SetLineStyle(2);
      ideal.Draw("same");
    }
    c.Print(out_pdf);
  }

  {
    TCanvas c("c_raw_cdf_ch", "c_raw_cdf_ch", 1600, 900);
    c.Divide(2, 1, 0.001, 0.001);
    for (size_t i = 0; i < watch_channels.size(); ++i) {
      c.cd(static_cast<int>(i + 1));
      if (!h_raw_cdf_ch[i]) {
        continue;
      }
      h_raw_cdf_ch[i]->SetLineColor(kBlue + 1);
      h_raw_cdf_ch[i]->SetLineWidth(2);
      h_raw_cdf_ch[i]->Draw("hist");
      TF1 ideal(Form("ideal_cdf_ch_%zu", i), Form("(x-0.5)/%d", bins), 0.5, bins + 0.5);
      ideal.SetLineColor(kRed + 1);
      ideal.SetLineStyle(2);
      ideal.Draw("same");
    }
    c.Print(out_pdf);
  }

  {
    TCanvas c("c_lut_uniform", "c_lut_uniform", 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = 0; i < 4; ++i) {
      c.cd(pad++);
      if (!h_intr[i]) {
        continue;
      }
      h_intr[i]->SetLineColor(kBlue + 1);
      h_intr[i]->SetLineWidth(2);
      h_intr[i]->Draw("hist");
      draw_uniform_line(h_intr[i].get(), kRed + 1);
    }
    c.Print(out_pdf);
  }

  {
    TCanvas c("c_lut_uniform_all", "c_lut_uniform_all", 1600, 900);
    if (h_intr[4]) {
      h_intr[4]->SetLineColor(kBlue + 1);
      h_intr[4]->SetLineWidth(2);
      h_intr[4]->Draw("hist");
      draw_uniform_line(h_intr[4].get(), kRed + 1);
    }
    c.Print(out_pdf);
  }

  {
    TCanvas c("c_raw_cdf_all", "c_raw_cdf_all", 1600, 900);
    if (h_raw_cdf[4]) {
      h_raw_cdf[4]->SetLineColor(kBlue + 1);
      h_raw_cdf[4]->SetLineWidth(2);
      h_raw_cdf[4]->Draw("hist");
      TF1 ideal("ideal_cdf_all", Form("(x-0.5)/%d", bins), 0.5, bins + 0.5);
      ideal.SetLineColor(kRed + 1);
      ideal.SetLineStyle(2);
      ideal.Draw("same");
    }
    c.Print(out_pdf);
  }

  {
    gStyle->SetOptStat(0);
    gStyle->SetOptFit(0);
    auto add_fraction = [&](std::vector<long long> &out, double val, long long count) {
      if (val < -0.5 || val >= 0.5) {
        return;
      }
      const double pos = (val + 0.5);
      int bin = static_cast<int>(pos * static_cast<double>(lut_bins));
      if (bin < 0) {
        bin = 0;
      } else if (bin >= lut_bins) {
        bin = lut_bins - 1;
      }
      out[static_cast<size_t>(bin)] += count;
    };

    std::array<std::array<std::vector<long long>, 4>, 2> frac_raw{};
    std::array<std::array<std::vector<long long>, 4>, 2> frac_lin{};
    std::array<std::array<std::vector<long long>, 4>, 2> frac_lut{};
    for (size_t ci = 0; ci < watch_channels.size(); ++ci) {
      for (int tdc = 0; tdc < 4; ++tdc) {
        frac_raw[ci][tdc].assign(static_cast<size_t>(lut_bins), 0);
        frac_lin[ci][tdc].assign(static_cast<size_t>(lut_bins), 0);
        frac_lut[ci][tdc].assign(static_cast<size_t>(lut_bins), 0);
      }
    }

    for (int idx = 0; idx < size; ++idx) {
      if (totals_full[idx] <= 0) {
        continue;
      }
      int fifo = 0;
      int column = 0;
      int pixel = 0;
      int tdc = 0;
      int channel = 0;
      {
        fifo = idx / analysis_time::kTdcPerFifo;
        const int local = idx % analysis_time::kTdcPerFifo;
        column = local / (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
        const int rem = local % (analysis_time::kPixelsPerColumn * analysis_time::kTdcPerPixel);
        pixel = rem / analysis_time::kTdcPerPixel;
        tdc = rem % analysis_time::kTdcPerPixel;
        channel = column * 4 + pixel;
      }
      int ci = -1;
      if (channel == watch_channels[0]) {
        ci = 0;
      } else if (channel == watch_channels[1]) {
        ci = 1;
      } else {
        continue;
      }
      const long long *cptr = &counts_full[idx * bins];
      for (int b = 0; b < bins; ++b) {
        const long long count = cptr[b];
        if (count <= 0) {
          continue;
        }
        const double raw_val = (static_cast<double>(b) + 0.5) / static_cast<double>(bins) - 0.5;
        const double lin_val = valid_linear[idx] ? static_cast<double>(lut_linear[idx][b]) : raw_val;
        const double lut_val = valid_full[idx] ? static_cast<double>(lut_full[idx][b]) : raw_val;
        add_fraction(frac_raw[ci][tdc], raw_val, count);
        add_fraction(frac_lin[ci][tdc], lin_val, count);
        add_fraction(frac_lut[ci][tdc], lut_val, count);
      }
    }

    for (size_t ci = 0; ci < watch_channels.size(); ++ci) {
      const int channel = watch_channels[ci];
      std::array<std::unique_ptr<TH1D>, 4> h_counts{};
      std::array<std::unique_ptr<TH1D>, 4> h_cdf{};
      std::array<std::unique_ptr<TH1D>, 4> h_dnl_raw{};
      std::array<std::unique_ptr<TH1D>, 4> h_dnl_lin{};
      std::array<std::unique_ptr<TH1D>, 4> h_dnl_lut{};
      std::array<std::unique_ptr<TH1D>, 4> h_inl_raw{};
      std::array<std::unique_ptr<TH1D>, 4> h_inl_lin{};
      std::array<std::unique_ptr<TH1D>, 4> h_inl_lut{};

      for (int tdc = 0; tdc < 4; ++tdc) {
        h_counts[tdc] = std::make_unique<TH1D>(Form("h_counts_ch%d_tdc%d", channel, tdc),
                                               Form("Raw fine counts (ch %d tdc %d);fine code;entries", channel, tdc),
                                               bins,
                                               0.5,
                                               bins + 0.5);
        h_cdf[tdc] = std::make_unique<TH1D>(Form("h_cdf_ch%d_tdc%d", channel, tdc),
                                            Form("Raw CDF (ch %d tdc %d);fine code;CDF", channel, tdc),
                                            bins,
                                            0.5,
                                            bins + 0.5);
        h_counts[tdc]->SetDirectory(nullptr);
        h_cdf[tdc]->SetDirectory(nullptr);
        long long total = 0;
        for (int b = 0; b < bins; ++b) {
          const long long count = raw_counts_ch_tdc[ci][tdc][static_cast<size_t>(b)];
          h_counts[tdc]->SetBinContent(b + 1, static_cast<double>(count));
          total += count;
        }
        long long cum = 0;
        for (int b = 0; b < bins; ++b) {
          const long long count = raw_counts_ch_tdc[ci][tdc][static_cast<size_t>(b)];
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
          h_cdf[tdc]->SetBinContent(b + 1, frac);
        }

        std::vector<double> dnl_raw;
        std::vector<double> inl_raw;
        std::vector<double> dnl_lin;
        std::vector<double> inl_lin;
        std::vector<double> dnl_lut;
        std::vector<double> inl_lut;
        ComputeDnlInl(frac_raw[ci][tdc], dnl_raw, inl_raw);
        ComputeDnlInl(frac_lin[ci][tdc], dnl_lin, inl_lin);
        ComputeDnlInl(frac_lut[ci][tdc], dnl_lut, inl_lut);

        h_dnl_raw[tdc] = std::make_unique<TH1D>(Form("h_dnl_raw_ch%d_tdc%d", channel, tdc),
                                                Form("DNL (ch %d tdc %d);fine fraction;DNL", channel, tdc),
                                                lut_bins,
                                                -0.5,
                                                0.5);
        h_dnl_lin[tdc] = std::make_unique<TH1D>(Form("h_dnl_lin_ch%d_tdc%d", channel, tdc),
                                                Form("DNL (ch %d tdc %d);fine fraction;DNL", channel, tdc),
                                                lut_bins,
                                                -0.5,
                                                0.5);
        h_dnl_lut[tdc] = std::make_unique<TH1D>(Form("h_dnl_lut_ch%d_tdc%d", channel, tdc),
                                                Form("DNL (ch %d tdc %d);fine fraction;DNL", channel, tdc),
                                                lut_bins,
                                                -0.5,
                                                0.5);
        h_inl_raw[tdc] = std::make_unique<TH1D>(Form("h_inl_raw_ch%d_tdc%d", channel, tdc),
                                                Form("INL (ch %d tdc %d);fine fraction;INL", channel, tdc),
                                                lut_bins,
                                                -0.5,
                                                0.5);
        h_inl_lin[tdc] = std::make_unique<TH1D>(Form("h_inl_lin_ch%d_tdc%d", channel, tdc),
                                                Form("INL (ch %d tdc %d);fine fraction;INL", channel, tdc),
                                                lut_bins,
                                                -0.5,
                                                0.5);
        h_inl_lut[tdc] = std::make_unique<TH1D>(Form("h_inl_lut_ch%d_tdc%d", channel, tdc),
                                                Form("INL (ch %d tdc %d);fine fraction;INL", channel, tdc),
                                                lut_bins,
                                                -0.5,
                                                0.5);
        for (int b = 0; b < lut_bins; ++b) {
          if (b < static_cast<int>(dnl_raw.size())) {
            h_dnl_raw[tdc]->SetBinContent(b + 1, dnl_raw[static_cast<size_t>(b)]);
            h_inl_raw[tdc]->SetBinContent(b + 1, inl_raw[static_cast<size_t>(b)]);
          }
          if (b < static_cast<int>(dnl_lin.size())) {
            h_dnl_lin[tdc]->SetBinContent(b + 1, dnl_lin[static_cast<size_t>(b)]);
            h_inl_lin[tdc]->SetBinContent(b + 1, inl_lin[static_cast<size_t>(b)]);
          }
          if (b < static_cast<int>(dnl_lut.size())) {
            h_dnl_lut[tdc]->SetBinContent(b + 1, dnl_lut[static_cast<size_t>(b)]);
            h_inl_lut[tdc]->SetBinContent(b + 1, inl_lut[static_cast<size_t>(b)]);
          }
        }
      }

      auto draw_channel_canvas = [&](const char *name,
                                     const char *title,
                                     const std::array<std::unique_ptr<TH1D>, 4> &hist,
                                     bool draw_ideal,
                                     bool overlay,
                                     const std::array<std::unique_ptr<TH1D>, 4> *hist2,
                                     const std::array<std::unique_ptr<TH1D>, 4> *hist3,
                                     bool add_legend) {
        TCanvas c(name, title, 1600, 900);
        c.Divide(2, 2, 0.001, 0.001);
        for (int tdc = 0; tdc < 4; ++tdc) {
          c.cd(tdc + 1);
          if (!hist[tdc]) {
            continue;
          }
          if (overlay && hist2 && (*hist2)[tdc] && hist3 && (*hist3)[tdc]) {
            hist[tdc]->SetLineColor(2);
            hist[tdc]->SetLineWidth(1);
            (*hist2)[tdc]->SetLineColor(3);
            (*hist2)[tdc]->SetLineWidth(1);
            (*hist3)[tdc]->SetLineColor(4);
            (*hist3)[tdc]->SetLineWidth(1);
            hist[tdc]->Draw("hist");
            (*hist2)[tdc]->Draw("hist same");
            (*hist3)[tdc]->Draw("hist same");
          } else {
            hist[tdc]->SetLineColor(kBlue + 1);
            hist[tdc]->SetLineWidth(2);
            hist[tdc]->Draw("hist");
          }
          if (draw_ideal) {
            TF1 ideal(Form("ideal_cdf_ch_%d_tdc_%d", channel, tdc), Form("(x-0.5)/%d", bins), 0.5, bins + 0.5);
            ideal.SetLineColor(kRed + 1);
            ideal.SetLineStyle(2);
            ideal.Draw("same");
          }
        }
        if (add_legend && overlay && hist2 && hist3 && hist[0] && (*hist2)[0] && (*hist3)[0]) {
          c.cd(1);
          auto *leg = new TLegend(0.65, 0.65, 0.88, 0.88);
          leg->AddEntry(hist[0].get(), "raw", "l");
          leg->AddEntry((*hist2)[0].get(), "linear", "l");
          leg->AddEntry((*hist3)[0].get(), "LUT", "l");
          leg->Draw();
          gPad->Modified();
          gPad->Update();
        }
        c.Print(out_pdf);
      };

      draw_channel_canvas(Form("c_raw_counts_ch%d_tdc", channel),
                          Form("Raw fine counts ch %d", channel),
                          h_counts,
                          false,
                          false,
                          nullptr,
                          nullptr,
                          false);
      draw_channel_canvas(Form("c_raw_cdf_ch%d_tdc", channel),
                          Form("Raw CDF ch %d", channel),
                          h_cdf,
                          true,
                          false,
                          nullptr,
                          nullptr,
                          false);
      draw_channel_canvas(Form("c_dnl_ch%d_tdc", channel),
                          Form("DNL ch %d", channel),
                          h_dnl_raw,
                          false,
                          true,
                          &h_dnl_lin,
                          &h_dnl_lut,
                          true);
      draw_channel_canvas(Form("c_inl_ch%d_tdc", channel),
                          Form("INL ch %d", channel),
                          h_inl_raw,
                          false,
                          true,
                          &h_inl_lin,
                          &h_inl_lut,
                          true);
    }
  }

  {
    TCanvas c("c_cdf_raw_lut", "c_cdf_raw_lut", 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = 0; i < 4; ++i) {
      c.cd(pad++);
      if (h_cdf_raw[i]) {
        h_cdf_raw[i]->SetLineColor(kRed + 1);
        h_cdf_raw[i]->SetLineWidth(2);
        h_cdf_raw[i]->Draw("hist");
      }
      if (h_cdf_intr[i]) {
        h_cdf_intr[i]->SetLineColor(kBlue + 1);
        h_cdf_intr[i]->SetLineWidth(2);
        h_cdf_intr[i]->Draw("hist same");
      }
      TLegend leg(0.62, 0.75, 0.88, 0.88);
      if (h_cdf_raw[i]) leg.AddEntry(h_cdf_raw[i].get(), "raw", "l");
      if (h_cdf_intr[i]) leg.AddEntry(h_cdf_intr[i].get(), "LUT", "l");
      leg.Draw();
    }
    c.Print(out_pdf);
  }

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

  auto draw_dnl_inl = [&](const char *name,
                          const std::array<std::unique_ptr<TH1D>, 5> &raw,
                          const std::array<std::unique_ptr<TH1D>, 5> &lin,
                          const std::array<std::unique_ptr<TH1D>, 5> &lut,
                          int idx_start,
                          int idx_end) {
    TCanvas c(name, name, 1600, 900);
    c.Divide(2, 2, 0.001, 0.001);
    int pad = 1;
    for (int i = idx_start; i <= idx_end; ++i) {
      c.cd(pad++);
      if (!raw[i] || !lut[i]) {
        continue;
      }
      raw[i]->SetLineColor(kRed + 1);
      raw[i]->SetLineWidth(2);
      lin[i]->SetLineColor(kOrange + 7);
      lin[i]->SetLineWidth(2);
      lut[i]->SetLineColor(kBlue + 1);
      lut[i]->SetLineWidth(2);
      raw[i]->Draw("hist");
      lin[i]->Draw("hist same");
      lut[i]->Draw("hist same");
      TLegend leg(0.62, 0.75, 0.88, 0.88);
      leg.AddEntry(raw[i].get(), "raw", "l");
      leg.AddEntry(lin[i].get(), "linear", "l");
      leg.AddEntry(lut[i].get(), "LUT", "l");
      leg.Draw();
    }
    c.Print(out_pdf);
  };

  draw_dnl_inl("c_dnl", h_dnl_raw, h_dnl_lin, h_dnl_lut, 0, 3);
  draw_dnl_inl("c_inl", h_inl_raw, h_inl_lin, h_inl_lut, 0, 3);

  {
    TCanvas c("c_dnl_all", "c_dnl_all", 1600, 900);
    h_dnl_raw[4]->SetLineColor(kRed + 1);
    h_dnl_raw[4]->SetLineWidth(2);
    h_dnl_lin[4]->SetLineColor(kOrange + 7);
    h_dnl_lin[4]->SetLineWidth(2);
    h_dnl_lut[4]->SetLineColor(kBlue + 1);
    h_dnl_lut[4]->SetLineWidth(2);
    h_dnl_raw[4]->Draw("hist");
    h_dnl_lin[4]->Draw("hist same");
    h_dnl_lut[4]->Draw("hist same");
    TLegend leg(0.62, 0.75, 0.88, 0.88);
    leg.AddEntry(h_dnl_raw[4].get(), "raw", "l");
    leg.AddEntry(h_dnl_lin[4].get(), "linear", "l");
    leg.AddEntry(h_dnl_lut[4].get(), "LUT", "l");
    leg.Draw();
    c.Print(out_pdf);
  }

  {
    TCanvas c("c_inl_all", "c_inl_all", 1600, 900);
    h_inl_raw[4]->SetLineColor(kRed + 1);
    h_inl_raw[4]->SetLineWidth(2);
    h_inl_lin[4]->SetLineColor(kOrange + 7);
    h_inl_lin[4]->SetLineWidth(2);
    h_inl_lut[4]->SetLineColor(kBlue + 1);
    h_inl_lut[4]->SetLineWidth(2);
    h_inl_raw[4]->Draw("hist");
    h_inl_lin[4]->Draw("hist same");
    h_inl_lut[4]->Draw("hist same");
    TLegend leg(0.62, 0.75, 0.88, 0.88);
    leg.AddEntry(h_inl_raw[4].get(), "raw", "l");
    leg.AddEntry(h_inl_lin[4].get(), "linear", "l");
    leg.AddEntry(h_inl_lut[4].get(), "LUT", "l");
    leg.Draw();
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
