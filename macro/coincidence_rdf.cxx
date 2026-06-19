#include <TCanvas.h>
#include <TFile.h>
#include <TH1.h>
#include <TH1D.h>
#include <TH2D.h>
#include <TF1.h>
#include <TFitResult.h>
#include <TFitResultPtr.h>
#include <THStack.h>
#include <TLegend.h>
#include <TLatex.h>
#include <TPad.h>
#include <TParameter.h>
#include <TRandom3.h>
#include <TError.h>
#include <TStyle.h>
#include <TSystem.h>
#include <TTree.h>

#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

void PrintCoincidenceHelp()
{
  std::cout << "coincidence_rdf usage:" << std::endl;
  std::cout << "  coincidence_rdf(\"/path/to/decoded.root\", \"pairs.txt\", \"out.pdf\", 10.0, 320.0, true,"
               " 15.0, 0.0, \"TDC_calibration.root\", false, \"\", 0, \"out.root\", \"out.txt\", true, 100,"
               " \"laser_timewalk.root\")"
            << std::endl;
  std::cout << "Inputs:" << std::endl;
  std::cout << "  decoded dir must contain alcdaq.fifo_*.root with TTree 'alcor'" << std::endl;
  std::cout << "  required branches: type,column,pixel,tdc,fifo,rollover,coarse,fine" << std::endl;
  std::cout << "  channels are 0..31, computed as column*4 + pixel when missing" << std::endl;
  std::cout << "Pairs file format: chA chB [window_ns], '#' for comments" << std::endl;
  std::cout << "  group lines: group ch1 ch2 ch3 [window=ns] (3+ channels)" << std::endl;
  std::cout << "  duration-min lines: dmin CH NS (per-channel minimum ToT)" << std::endl;
  std::cout << "  use window=10 or 10.0 for integer windows to avoid ambiguity" << std::endl;
  std::cout << "  use the 'group' prefix for 3+ channel coincidences" << std::endl;
  std::cout << "Notes: type==1 hits, type==15 spill boundary, clock_mhz sets tick size" << std::endl;
  std::cout << "  spill markers (type==15) are always used when spill is not provided" << std::endl;
  std::cout << "  coincidence uses leading-edge timestamps with valid duration (ToT)" << std::endl;
  std::cout << "  max duration can be set; <=0 disables duration filter (leading edges only)" << std::endl;
  std::cout << "  group coincidence is evaluated per hit of the first channel in the group" << std::endl;
  std::cout << "  group plots show t_i - mean for the matched timestamps" << std::endl;
  std::cout << "  pair-mean plot: mean(t17,t19) - mean(t22,t23) using leading edges" << std::endl;
  std::cout << "  coincidence-hit distributions are appended to the same PDF" << std::endl;
  std::cout << "  only hits with min_duration <= ToT <= max_duration_ns are considered" << std::endl;
  std::cout << "  fine calibration file uses hFineMin/hFineMax; default formula used when missing" << std::endl;
  std::cout << "  TDC calibration may also provide hChannelTdcOffset, applied per channel/TDC before ToT" << std::endl;
  std::cout << "  optional channel calibration uses hChanCalib_chXX vs ToT when a file path is provided" << std::endl;
  std::cout << "  timewalk calibration reads signed timewalk_corr_*_chXX parameters from a laser analysis ROOT file" << std::endl;
  std::cout << "  force_window=true ignores per-line windows in the pairs file" << std::endl;
  std::cout << "  fine_cut excludes hits with |fine - cut| <= fine_cut (fine units)" << std::endl;
  std::cout << "  out_root writes histograms to a ROOT file when non-empty" << std::endl;
  std::cout << "  out_txt writes search/coincidence histograms and FWHM info when non-empty" << std::endl;
  std::cout << "  use_lut=false disables LUT even if hFineLut is present" << std::endl;
  std::cout << "  preview_hits prints the first N leading-hit timestamps per channel after corrections" << std::endl;
}

struct Hit {
  int run_id = 0;
  int device = 0;
  int fifo = 0;
  int type = 0;
  int counter = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int rollover = 0;
  int coarse = 0;
  int fine = 0;
  int channel = -1;
  long long time_tick = 0;
  double time_ns_raw = 0.0;
  double time_ns = 0.0;
  int spill = 0;
};

struct HitRef {
  double time_ns = 0.0;
  size_t index = 0;
};

struct DurationInfo {
  std::vector<char> leading_mask;
  std::vector<int> leading_to_trailing;
};

struct ScopedErrorIgnoreLevel {
  int previous = 0;
  explicit ScopedErrorIgnoreLevel(int level)
      : previous(gErrorIgnoreLevel)
  {
    gErrorIgnoreLevel = level;
  }
  ~ScopedErrorIgnoreLevel()
  {
    gErrorIgnoreLevel = previous;
  }
};

std::unordered_map<int, std::vector<size_t>> CollectLeadingIndicesByChannel(const std::vector<int> &channel_list,
                                                                            const std::vector<Hit> &hits,
                                                                            const std::vector<char> &leading_mask)
{
  std::unordered_map<int, std::vector<size_t>> indices_by_channel;
  indices_by_channel.reserve(channel_list.size());
  for (int ch : channel_list) {
    indices_by_channel.emplace(ch, std::vector<size_t>{});
  }

  for (size_t i = 0; i < hits.size(); ++i) {
    if (i >= leading_mask.size() || !leading_mask[i]) {
      continue;
    }
    auto it = indices_by_channel.find(hits[i].channel);
    if (it == indices_by_channel.end()) {
      continue;
    }
    it->second.push_back(i);
  }

  for (auto &kv : indices_by_channel) {
    auto &indices = kv.second;
    std::sort(indices.begin(), indices.end(), [&hits](size_t lhs, size_t rhs) {
      if (hits[lhs].time_ns != hits[rhs].time_ns) {
        return hits[lhs].time_ns < hits[rhs].time_ns;
      }
      return lhs < rhs;
    });
  }

  return indices_by_channel;
}

void PrintLeadingEventPreview(const std::vector<int> &channel_list,
                              const std::vector<Hit> &hits,
                              const std::vector<char> &leading_mask,
                              size_t max_events = 100)
{
  if (max_events == 0) {
    return;
  }
  const auto indices_by_channel = CollectLeadingIndicesByChannel(channel_list, hits, leading_mask);

  std::cout << "First " << max_events
            << " leading-hit timestamps per channel, ordered by time (after timing corrections / duration filter)"
            << std::endl;
  for (int ch : channel_list) {
    const auto it = indices_by_channel.find(ch);
    const size_t total = (it != indices_by_channel.end()) ? it->second.size() : 0;
    const size_t shown = std::min(max_events, total);
    std::cout << "  channel " << ch << " -> showing " << shown << " / " << total << std::endl;
    if (it == indices_by_channel.end() || it->second.empty()) {
      continue;
    }

    for (size_t order = 0; order < shown; ++order) {
      const size_t idx = it->second[order];
      const auto &hit = hits[idx];
      std::ostringstream line;
      line << std::fixed << std::setprecision(3);
      line << "    [" << order << "] time_ns=" << hit.time_ns;
      std::cout << line.str() << std::endl;
    }
  }
}

void PrintLeadingTotPreview(const std::vector<int> &channel_list,
                            const std::vector<Hit> &hits,
                            const std::vector<char> &leading_mask,
                            const std::vector<double> &tot_per_hit,
                            size_t max_events = 100)
{
  if (max_events == 0) {
    return;
  }
  const auto indices_by_channel = CollectLeadingIndicesByChannel(channel_list, hits, leading_mask);

  std::cout << "First " << max_events
            << " leading-hit ToT per channel, ordered by time (after duration filter)" << std::endl;
  for (int ch : channel_list) {
    const auto it = indices_by_channel.find(ch);
    size_t total_with_tot = 0;
    if (it != indices_by_channel.end()) {
      for (size_t idx : it->second) {
        if (idx < tot_per_hit.size() && tot_per_hit[idx] > 0.0) {
          ++total_with_tot;
        }
      }
    }
    const size_t shown = std::min(max_events, total_with_tot);
    std::cout << "  channel " << ch << " -> showing " << shown << " / " << total_with_tot << std::endl;
    if (it == indices_by_channel.end() || it->second.empty()) {
      continue;
    }

    size_t printed = 0;
    for (size_t idx : it->second) {
      if (idx >= tot_per_hit.size() || tot_per_hit[idx] <= 0.0) {
        continue;
      }
      std::ostringstream line;
      line << std::fixed << std::setprecision(3);
      line << "    [" << printed << "] tot_ns=" << tot_per_hit[idx];
      std::cout << line.str() << std::endl;
      ++printed;
      if (printed >= max_events) {
        break;
      }
    }
  }
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


std::string SanitizeTag(std::string value)
{
  for (auto &ch : value) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
      ch = '_';
    }
  }
  return value;
}

double ComputeFwhmFromHist(const TH1D &hist, double bkg, double &x_left, double &x_right)
{
  x_left = std::numeric_limits<double>::quiet_NaN();
  x_right = std::numeric_limits<double>::quiet_NaN();
  const int nbins = hist.GetNbinsX();
  if (nbins < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const int ib_max = hist.GetMaximumBin();
  const double peak = hist.GetBinContent(ib_max);
  const double peak_sub = peak - bkg;
  if (peak_sub <= 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double half = bkg + 0.5 * peak_sub;

  int i = ib_max;
  while (i > 1 && hist.GetBinContent(i) > half) {
    --i;
  }
  if (i < ib_max) {
    const double y1 = hist.GetBinContent(i);
    const double y2 = hist.GetBinContent(i + 1);
    const double x1 = hist.GetBinCenter(i);
    const double x2 = hist.GetBinCenter(i + 1);
    if (y2 != y1) {
      x_left = x1 + (half - y1) * (x2 - x1) / (y2 - y1);
    } else {
      x_left = x1;
    }
  }

  i = ib_max;
  while (i < nbins && hist.GetBinContent(i) > half) {
    ++i;
  }
  if (i > ib_max) {
    const double y1 = hist.GetBinContent(i - 1);
    const double y2 = hist.GetBinContent(i);
    const double x1 = hist.GetBinCenter(i - 1);
    const double x2 = hist.GetBinCenter(i);
    if (y2 != y1) {
      x_right = x1 + (half - y1) * (x2 - x1) / (y2 - y1);
    } else {
      x_right = x2;
    }
  }

  if (std::isfinite(x_left) && std::isfinite(x_right) && x_right > x_left) {
    return x_right - x_left;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

double FitBackgroundFromSidebandsCore(TH1D &hist_search, double window_ns)
{
  if (window_ns <= 0.0) {
    return 0.0;
  }
  const double pad = 100.0;
  const double band = window_ns * 4.0;
  double left_lo = -pad - band;
  double left_hi = -pad;
  double right_lo = pad;
  double right_hi = pad + band;

  const double hist_lo = hist_search.GetXaxis()->GetXmin();
  const double hist_hi = hist_search.GetXaxis()->GetXmax();
  left_lo = std::max(left_lo, hist_lo);
  left_hi = std::min(left_hi, hist_hi);
  right_lo = std::max(right_lo, hist_lo);
  right_hi = std::min(right_hi, hist_hi);

  double bkg_left = 0.0;
  double err_left = 0.0;
  bool ok_left = false;
  if (left_hi > left_lo) {
    TF1 f_left("f_sideband_left", "pol0", left_lo, left_hi);
    TFitResultPtr res_left = hist_search.Fit(&f_left, "RQN");
    if (res_left.Get() && res_left->IsValid()) {
      bkg_left = f_left.GetParameter(0);
      err_left = f_left.GetParError(0);
      ok_left = std::isfinite(bkg_left);
    }
  }

  double bkg_right = 0.0;
  double err_right = 0.0;
  bool ok_right = false;
  if (right_hi > right_lo) {
    TF1 f_right("f_sideband_right", "pol0", right_lo, right_hi);
    TFitResultPtr res_right = hist_search.Fit(&f_right, "RQN");
    if (res_right.Get() && res_right->IsValid()) {
      bkg_right = f_right.GetParameter(0);
      err_right = f_right.GetParError(0);
      ok_right = std::isfinite(bkg_right);
    }
  }

  double weight_left = 0.0;
  double weight_right = 0.0;
  if (ok_left) {
    weight_left = (err_left > 0.0) ? 1.0 / (err_left * err_left) : 1.0;
  }
  if (ok_right) {
    weight_right = (err_right > 0.0) ? 1.0 / (err_right * err_right) : 1.0;
  }

  if (weight_left + weight_right > 0.0) {
    return (bkg_left * weight_left + bkg_right * weight_right) / (weight_left + weight_right);
  }
  return 0.0;
}

double FitBackgroundFromSidebands(const TH1D &hist_search, double window_ns)
{
  std::unique_ptr<TH1D> tmp(static_cast<TH1D *>(hist_search.Clone("h_sideband_tmp")));
  tmp->SetDirectory(nullptr);
  return FitBackgroundFromSidebandsCore(*tmp, window_ns);
}

bool ComputeFwhmBootstrap(const TH1D &hist_coinc,
                          const TH1D &hist_search,
                          double window_ns,
                          int trials,
                          double &fwhm_mean,
                          double &fwhm_sigma,
                          double &bkg_mean,
                          double &bkg_sigma)
{
  if (trials < 1) {
    return false;
  }
  TRandom3 rng(0);
  auto tmp_coinc = std::unique_ptr<TH1D>(static_cast<TH1D *>(hist_coinc.Clone("h_boot_coinc")));
  auto tmp_search = std::unique_ptr<TH1D>(static_cast<TH1D *>(hist_search.Clone("h_boot_search")));
  tmp_coinc->SetDirectory(nullptr);
  tmp_search->SetDirectory(nullptr);
  ScopedErrorIgnoreLevel silence_fit_warnings(kError);

  std::vector<double> fwhm_vals;
  std::vector<double> bkg_vals;
  fwhm_vals.reserve(static_cast<size_t>(trials));
  bkg_vals.reserve(static_cast<size_t>(trials));

  const int bins_coinc = hist_coinc.GetNbinsX();
  const int bins_search = hist_search.GetNbinsX();

  for (int t = 0; t < trials; ++t) {
    for (int b = 1; b <= bins_coinc; ++b) {
      double mu = hist_coinc.GetBinContent(b);
      if (mu < 0.0) {
        mu = 0.0;
      }
      tmp_coinc->SetBinContent(b, rng.PoissonD(mu));
    }
    for (int b = 1; b <= bins_search; ++b) {
      double mu = hist_search.GetBinContent(b);
      if (mu < 0.0) {
        mu = 0.0;
      }
      tmp_search->SetBinContent(b, rng.PoissonD(mu));
    }

    const double bkg = FitBackgroundFromSidebandsCore(*tmp_search, window_ns);
    double x_left = 0.0;
    double x_right = 0.0;
    const double fwhm = ComputeFwhmFromHist(*tmp_coinc, bkg, x_left, x_right);
    if (!std::isfinite(fwhm)) {
      continue;
    }
    fwhm_vals.push_back(fwhm);
    bkg_vals.push_back(bkg);
  }

  if (fwhm_vals.empty()) {
    return false;
  }

  auto mean_std = [](const std::vector<double> &vals, double &mean, double &sigma) {
    double sum = 0.0;
    for (double v : vals) {
      sum += v;
    }
    mean = sum / static_cast<double>(vals.size());
    double var = 0.0;
    for (double v : vals) {
      double d = v - mean;
      var += d * d;
    }
    var /= static_cast<double>(vals.size());
    sigma = std::sqrt(std::max(0.0, var));
  };

  mean_std(fwhm_vals, fwhm_mean, fwhm_sigma);
  mean_std(bkg_vals, bkg_mean, bkg_sigma);
  return true;
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

bool StartsWith(const std::string &value, const std::string &prefix)
{
  return value.rfind(prefix, 0) == 0;
}

bool ParseChannelToken(const std::string &token, int &value)
{
  char *end = nullptr;
  long parsed = std::strtol(token.c_str(), &end, 10);
  if (!end || *end != '\0') {
    return false;
  }
  if (parsed < 0 || parsed > 31) {
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

bool ParseDoubleToken(const std::string &token, double &value)
{
  char *end = nullptr;
  value = std::strtod(token.c_str(), &end);
  if (!end || *end != '\0') {
    return false;
  }
  return true;
}

bool ParseWindowToken(const std::string &token, double &value)
{
  std::string trimmed = token;
  if (StartsWith(trimmed, "w=")) {
    trimmed = trimmed.substr(2);
  } else if (StartsWith(trimmed, "window=")) {
    trimmed = trimmed.substr(7);
  } else {
    return false;
  }
  if (!ParseDoubleToken(trimmed, value)) {
    return false;
  }
  return value > 0.0;
}

bool IsDurationMinDirective(const std::string &token)
{
  return token == "dmin" || token == "min_duration" || token == "duration_min" || token == "minduration";
}

double ChannelMinDurationNs(const std::unordered_map<int, double> &per_channel_min_duration_ns,
                            int channel,
                            double default_min_duration_ns)
{
  auto it = per_channel_min_duration_ns.find(channel);
  if (it != per_channel_min_duration_ns.end()) {
    return it->second;
  }
  return default_min_duration_ns;
}

struct TimewalkCorrection {
  bool valid = false;
  int model = 0;
  double p0 = 0.0;
  double p1 = 0.0;
  double p2 = 0.0;
  double p3 = 1.0;
  double p4 = 0.0;
  double baseline = 0.0;

  double EvalNs(double tot) const
  {
    if (!std::isfinite(tot)) {
      return 0.0;
    }
    if (model == 1) {
      if (tot <= p2 || p3 <= 0.0) {
        return p0 + p1 * tot;
      } else {
        return p4 + (p0 + p1 * p2 - p4) * std::exp(-(tot - p2) / p3);
      }
    }
    if (model == 2) {
      return p0 + p1 * std::min(tot, p2);
    }
    if (model == 3) {
      const double base = tot - p2;
      if (base <= 0.0 || p3 <= 0.0) {
        return 0.0;
      }
      return p0 + p1 / std::pow(base, p3);
    }
    return p0 + p1 * tot;
  }

  double CorrectionNs(double tot) const
  {
    if (!valid || !std::isfinite(tot)) {
      return 0.0;
    }
    const double value = EvalNs(tot);
    return std::isfinite(value) ? value : 0.0;
  }
};

template <typename T>
bool ReadTParameter(TFile &file, const std::string &name, T &value)
{
  auto *param = dynamic_cast<TParameter<T> *>(file.Get(name.c_str()));
  if (!param) {
    return false;
  }
  value = param->GetVal();
  return true;
}

std::unordered_map<int, TimewalkCorrection> LoadTimewalkCorrections(const char *path,
                                                                    const std::vector<int> &channel_list)
{
  std::unordered_map<int, TimewalkCorrection> corrections;
  if (!path || path[0] == '\0') {
    return corrections;
  }

  std::unique_ptr<TFile> file(TFile::Open(path, "READ"));
  if (!file || file->IsZombie()) {
    std::cout << "Failed to load timewalk calibration: " << path << " (ignored)" << std::endl;
    return corrections;
  }

  for (int ch : channel_list) {
    TimewalkCorrection correction;
    int valid = 0;
    if (!ReadTParameter(*file, "timewalk_corr_valid_ch" + std::to_string(ch), valid) || valid == 0) {
      continue;
    }
    ReadTParameter(*file, "timewalk_corr_model_ch" + std::to_string(ch), correction.model);
    if (!ReadTParameter(*file, "timewalk_corr_p0_ch" + std::to_string(ch), correction.p0) ||
        !ReadTParameter(*file, "timewalk_corr_p1_ch" + std::to_string(ch), correction.p1)) {
      continue;
    }
    ReadTParameter(*file, "timewalk_corr_p2_ch" + std::to_string(ch), correction.p2);
    ReadTParameter(*file, "timewalk_corr_p3_ch" + std::to_string(ch), correction.p3);
    ReadTParameter(*file, "timewalk_corr_p4_ch" + std::to_string(ch), correction.p4);
    if (!ReadTParameter(*file, "timewalk_corr_baseline_ch" + std::to_string(ch), correction.baseline)) {
      if (correction.model == 1 || correction.model == 2) {
        correction.baseline = correction.p4;
      } else if (correction.model == 3) {
        correction.baseline = correction.p0;
      }
    }
    correction.valid = true;
    corrections[ch] = correction;

    std::cout << "Loaded timewalk correction ch" << ch << " model=" << correction.model << " p0="
              << correction.p0 << " p1=" << correction.p1 << " p2=" << correction.p2 << " p3="
              << correction.p3 << " p4=" << correction.p4 << " baseline=" << correction.baseline << std::endl;
  }

  if (corrections.empty()) {
    std::cout << "No valid timewalk corrections found in " << path << std::endl;
  } else {
    std::cout << "Loaded timewalk calibration: " << path << std::endl;
  }
  return corrections;
}

uint64_t GroupKey(int run_id, int channel, int spill)
{
  uint64_t key = static_cast<uint64_t>(run_id);
  key = (key << 40) | (static_cast<uint64_t>(channel & 0xff) << 32) | static_cast<uint32_t>(spill);
  return key;
}

uint64_t RunSpillKey(int run_id, int spill)
{
  return (static_cast<uint64_t>(run_id) << 32) | static_cast<uint32_t>(spill);
}

int ValueForVar(const Hit &hit, const std::string &var)
{
  if (var == "device") {
    return hit.device;
  }
  if (var == "fifo") {
    return hit.fifo;
  }
  if (var == "type") {
    return hit.type;
  }
  if (var == "counter") {
    return hit.counter;
  }
  if (var == "column") {
    return hit.column;
  }
  if (var == "pixel") {
    return hit.pixel;
  }
  if (var == "tdc") {
    return hit.tdc;
  }
  if (var == "rollover") {
    return hit.rollover;
  }
  if (var == "coarse") {
    return hit.coarse;
  }
  if (var == "fine") {
    return hit.fine;
  }
  return 0;
}

DurationInfo ComputeDurationInfo(const std::vector<Hit> &hits,
                                 const analysis_time::FineCalib &fine_calib,
                                 double tick_ns,
                                 double min_duration_ns,
                                 double max_duration_ns,
                                 const std::unordered_map<int, double> &per_channel_min_duration_ns,
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
    double time_ns = 0.0;
    int tdc = 0;
    size_t index = 0;
  };

  for (auto &kv : groups) {
    auto &indices = kv.second;
    const int channel = hits[indices.front()].channel;
    const double channel_min_duration_ns =
        std::max(0.0, ChannelMinDurationNs(per_channel_min_duration_ns, channel, min_duration_ns));
    std::vector<EdgeRef> edges;
    edges.reserve(indices.size());
    for (size_t idx : indices) {
      edges.push_back({hits[idx].time_ns_raw, hits[idx].tdc, idx});
    }
    std::sort(edges.begin(), edges.end(), [](const EdgeRef &a, const EdgeRef &b) {
      if (a.time_ns != b.time_ns) {
        return a.time_ns < b.time_ns;
      }
      return a.tdc < b.tdc;
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
      const double time_ns = edge.time_ns;
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
      const bool pass_min = dt_ns >= channel_min_duration_ns;
      const bool pass_max = (max_duration_ns <= 0.0) || (dt_ns <= max_duration_ns);
      if (dt_ns > 0.0 && pass_min && pass_max) {
        info.leading_mask[leading_idx[pair]] = 1;
        info.leading_to_trailing[leading_idx[pair]] = static_cast<int>(edge.index);
      }
      have_leading[pair] = false;
    }
  }

  return info;
}

struct PairConfig {
  int channel_a = -1;
  int channel_b = -1;
  double window_ns = 0.0;
  double search_window_ns = 0.0;
  long long count = 0;
  std::unique_ptr<TH1D> hist_search;
  std::unique_ptr<TH1D> hist_coinc;
  std::unique_ptr<TH2D> hist_dt_fine_a;
  std::unique_ptr<TH2D> hist_dt_fine_b;
  std::unique_ptr<TH2D> hist_dt_fine_coinc_a;
  std::unique_ptr<TH2D> hist_dt_fine_coinc_b;
  std::unique_ptr<TH2D> hist_dt_fine_coinc_a_tdc0;
  std::unique_ptr<TH2D> hist_dt_fine_coinc_b_tdc0;
  std::unique_ptr<TH2D> hist_dt_fine_coinc_a_tdc2;
  std::unique_ptr<TH2D> hist_dt_fine_coinc_b_tdc2;
  std::array<std::unique_ptr<TH2D>, 4> hist_tot_fine_a{};
  std::array<std::unique_ptr<TH2D>, 4> hist_tot_fine_b{};
  std::vector<char> coincident_hits;
};

struct GroupConfig {
  std::vector<int> channels;
  double window_ns = 0.0;
  long long count = 0;
  std::vector<std::unique_ptr<TH1D>> hists;
};

struct CoincPlotGroup {
  std::string kind;
  std::string name;
  std::string title;
  std::vector<std::unique_ptr<TH1D>> hists;
};

struct CoincPlotSet {
  std::string label;
  std::string title_prefix;
  std::vector<int> channels;
  std::vector<CoincPlotGroup> groups;
};

struct PairStats {
  double fwhm = std::numeric_limits<double>::quiet_NaN();
  double fwhm_err = std::numeric_limits<double>::quiet_NaN();
  double bkg = std::numeric_limits<double>::quiet_NaN();
  double bkg_err = std::numeric_limits<double>::quiet_NaN();
  double gaus_sigma = std::numeric_limits<double>::quiet_NaN();
  double gaus_sigma_err = std::numeric_limits<double>::quiet_NaN();
  double x_left = std::numeric_limits<double>::quiet_NaN();
  double x_right = std::numeric_limits<double>::quiet_NaN();
  long long entries = 0;
  int boot_trials = 0;
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
  bool has_run_id = false;
  bool has_device = false;
  bool has_counter = false;
  bool has_pending = false;
  bool eof = false;
  Hit pending;

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
  int channel = 0;
  int run_id = 0;
  Long64_t time_tick = 0;
};

bool HasTreeBranch(TTree *tree, const char *name)
{
  return tree && tree->GetBranch(name) != nullptr;
}

void BindTreeCursorBranches(TreeCursor &cursor)
{
  cursor.tree->SetBranchAddress("type", &cursor.type);
  cursor.tree->SetBranchAddress("fifo", &cursor.fifo);
  cursor.tree->SetBranchAddress("column", &cursor.column);
  cursor.tree->SetBranchAddress("pixel", &cursor.pixel);
  cursor.tree->SetBranchAddress("tdc", &cursor.tdc);
  cursor.tree->SetBranchAddress("rollover", &cursor.rollover);
  cursor.tree->SetBranchAddress("coarse", &cursor.coarse);
  cursor.tree->SetBranchAddress("fine", &cursor.fine);
  if (cursor.has_device) {
    cursor.tree->SetBranchAddress("device", &cursor.device);
  }
  if (cursor.has_counter) {
    cursor.tree->SetBranchAddress("counter", &cursor.counter);
  }
  if (cursor.has_spill) {
    cursor.tree->SetBranchAddress("spill", &cursor.spill);
  }
  if (cursor.has_run_id) {
    cursor.tree->SetBranchAddress("run_id", &cursor.run_id);
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
    std::cout << "Failed to open decoded ROOT file: " << path << std::endl;
    return false;
  }
  cursor.tree = dynamic_cast<TTree *>(cursor.file->Get(tree_name.c_str()));
  if (!cursor.tree) {
    std::cout << "Missing tree '" << tree_name << "' in " << path << std::endl;
    return false;
  }

  std::vector<std::string> missing;
  for (const auto &name : {"type", "fifo", "column", "pixel", "tdc", "rollover", "coarse", "fine"}) {
    if (!HasTreeBranch(cursor.tree, name)) {
      missing.emplace_back(name);
    }
  }
  if (!missing.empty()) {
    std::cout << "Error: missing required branches in " << path << ": ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i) {
        std::cout << ", ";
      }
      std::cout << missing[i];
    }
    std::cout << std::endl;
    return false;
  }

  cursor.has_channel = HasTreeBranch(cursor.tree, "channel");
  cursor.has_time_tick = HasTreeBranch(cursor.tree, "time_tick");
  cursor.has_spill = HasTreeBranch(cursor.tree, "spill");
  cursor.has_run_id = HasTreeBranch(cursor.tree, "run_id");
  cursor.has_device = HasTreeBranch(cursor.tree, "device");
  cursor.has_counter = HasTreeBranch(cursor.tree, "counter");

  cursor.tree->SetBranchStatus("*", 0);
  auto enable = [&cursor](const char *name) {
    if (HasTreeBranch(cursor.tree, name)) {
      cursor.tree->SetBranchStatus(name, 1);
    }
  };
  for (const auto &name : {"type", "fifo", "column", "pixel", "tdc", "rollover", "coarse", "fine"}) {
    enable(name);
  }
  for (const auto &name : {"device", "counter", "spill", "run_id", "channel", "time_tick"}) {
    enable(name);
  }

  BindTreeCursorBranches(cursor);

  cursor.entries = cursor.tree->GetEntries();
  return true;
}

bool AdvanceCursor(TreeCursor &cursor,
                   const std::unordered_set<int> &channels,
                   const analysis_time::FineCalib &fine_calib,
                   const analysis_time::ChannelTdcOffsetCalib &tdc_offset_calib,
                   double tick_ns,
                   bool use_fine,
                   int fine_cut)
{
  cursor.has_pending = false;
  while (cursor.entry < cursor.entries) {
    cursor.tree->GetEntry(cursor.entry++);
    if (!cursor.has_spill && cursor.type == 15) {
      ++cursor.current_spill;
      continue;
    }
    if (cursor.type != 1) {
      continue;
    }

    const int ch = cursor.has_channel ? cursor.channel : cursor.column * 4 + cursor.pixel;
    if (channels.find(ch) == channels.end()) {
      continue;
    }

    const int tdc_index = analysis_time::TdcIndex(cursor.fifo, cursor.column, cursor.pixel, cursor.tdc);
    if (use_fine && !analysis_time::PassFineCut(fine_calib, cursor.fine, tdc_index, fine_cut)) {
      continue;
    }

    Hit hit;
    hit.run_id = cursor.has_run_id ? cursor.run_id : 0;
    hit.device = cursor.has_device ? cursor.device : 0;
    hit.fifo = cursor.fifo;
    hit.type = cursor.type;
    hit.counter = cursor.has_counter ? cursor.counter : 0;
    hit.column = cursor.column;
    hit.pixel = cursor.pixel;
    hit.tdc = cursor.tdc;
    hit.rollover = cursor.rollover;
    hit.coarse = cursor.coarse;
    hit.fine = cursor.fine;
    hit.channel = ch;
    hit.time_tick = cursor.has_time_tick ? cursor.time_tick : analysis_time::TimeTick(cursor.rollover, cursor.coarse);
    hit.spill = cursor.has_spill ? cursor.spill : cursor.current_spill;
    hit.time_ns_raw = analysis_time::TimeNsFromTick(fine_calib, hit.time_tick, hit.fine, tdc_index, tick_ns, use_fine);
    hit.time_ns = hit.time_ns_raw - tdc_offset_calib.CorrectionNs(ch, hit.tdc);
    cursor.pending = hit;
    cursor.has_pending = true;
    return true;
  }

  cursor.eof = true;
  return false;
}

uint64_t HitRunSpillKey(const Hit &hit)
{
  return RunSpillKey(hit.run_id, hit.spill);
}

struct PreviewStore {
  size_t max_events = 0;
  std::unordered_map<int, size_t> total_leading;
  std::unordered_map<int, size_t> total_tot;
  std::unordered_map<int, std::vector<double>> times_by_channel;
  std::unordered_map<int, std::vector<double>> tots_by_channel;

  explicit PreviewStore(size_t max_events_in = 0)
      : max_events(max_events_in)
  {}

  void AddLeading(const Hit &hit, double tot_ns)
  {
    ++total_leading[hit.channel];
    auto &times = times_by_channel[hit.channel];
    if (times.size() < max_events) {
      times.push_back(hit.time_ns);
    }
    if (tot_ns > 0.0) {
      ++total_tot[hit.channel];
      auto &tots = tots_by_channel[hit.channel];
      if (tots.size() < max_events) {
        tots.push_back(tot_ns);
      }
    }
  }
};

void PrintPreviewStore(const std::vector<int> &channel_list, const PreviewStore &preview)
{
  if (preview.max_events == 0) {
    return;
  }

  std::cout << "First " << preview.max_events
            << " leading-hit timestamps per channel, ordered by spill stream (after timing corrections / duration filter)"
            << std::endl;
  for (int ch : channel_list) {
    auto total_it = preview.total_leading.find(ch);
    const size_t total = (total_it != preview.total_leading.end()) ? total_it->second : 0;
    auto values_it = preview.times_by_channel.find(ch);
    const size_t shown = (values_it != preview.times_by_channel.end()) ? values_it->second.size() : 0;
    std::cout << "  channel " << ch << " -> showing " << shown << " / " << total << std::endl;
    if (values_it == preview.times_by_channel.end()) {
      continue;
    }
    for (size_t i = 0; i < values_it->second.size(); ++i) {
      std::ostringstream line;
      line << std::fixed << std::setprecision(3);
      line << "    [" << i << "] time_ns=" << values_it->second[i];
      std::cout << line.str() << std::endl;
    }
  }

  std::cout << "First " << preview.max_events
            << " leading-hit ToT per channel, ordered by spill stream (after duration filter)" << std::endl;
  for (int ch : channel_list) {
    auto total_it = preview.total_tot.find(ch);
    const size_t total = (total_it != preview.total_tot.end()) ? total_it->second : 0;
    auto values_it = preview.tots_by_channel.find(ch);
    const size_t shown = (values_it != preview.tots_by_channel.end()) ? values_it->second.size() : 0;
    std::cout << "  channel " << ch << " -> showing " << shown << " / " << total << std::endl;
    if (values_it == preview.tots_by_channel.end()) {
      continue;
    }
    for (size_t i = 0; i < values_it->second.size(); ++i) {
      std::ostringstream line;
      line << std::fixed << std::setprecision(3);
      line << "    [" << i << "] tot_ns=" << values_it->second[i];
      std::cout << line.str() << std::endl;
    }
  }
}

bool FindNearestInWindow(const std::vector<HitRef> &hits, double time_ns, double window_ns, size_t &index)
{
  if (hits.empty()) {
    return false;
  }
  const double lo = time_ns - window_ns;
  const double hi = time_ns + window_ns;
  auto it = std::lower_bound(hits.begin(), hits.end(), lo,
                             [](const HitRef &hit, double value) { return hit.time_ns < value; });
  bool found = false;
  double best_dt = 0.0;
  for (; it != hits.end() && it->time_ns <= hi; ++it) {
    double dt = std::abs(it->time_ns - time_ns);
    if (!found || dt < best_dt) {
      found = true;
      best_dt = dt;
      index = it->index;
    }
  }
  return found;
}

std::string ChannelListLabel(const std::vector<int> &channels)
{
  std::ostringstream label;
  for (size_t i = 0; i < channels.size(); ++i) {
    if (i > 0) {
      label << ",";
    }
    label << channels[i];
  }
  return label.str();
}

int BinsForWindow(double window_ns)
{
  if (window_ns <= 0.0) {
    return 60;
  }
  int bins = static_cast<int>(std::lround(window_ns * 20.0));
  if (bins < 6) {
    bins = 6;
  }
  return bins;
}

bool LoadCoincidenceConfig(const std::string &path,
                           double default_window_ns,
                           bool force_window,
                           std::vector<PairConfig> &pairs,
                           std::vector<GroupConfig> &groups,
                           std::unordered_set<int> &channels,
                           std::unordered_map<int, double> &per_channel_min_duration_ns)
{
  std::ifstream fin(path);
  if (!fin) {
    std::cerr << "Failed to open pairs file: " << path << std::endl;
    return false;
  }

  std::string line;
  int line_no = 0;
  while (std::getline(fin, line)) {
    ++line_no;
    auto comment_pos = line.find('#');
    if (comment_pos != std::string::npos) {
      line = line.substr(0, comment_pos);
    }
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
      tokens.push_back(token);
    }
    if (tokens.empty()) {
      continue;
    }

    if (IsDurationMinDirective(tokens[0])) {
      if (tokens.size() != 3) {
        std::cerr << "Skipping invalid duration-min line " << line_no << std::endl;
        continue;
      }
      int ch = -1;
      double val = 0.0;
      if (!ParseChannelToken(tokens[1], ch) || !ParseDoubleToken(tokens[2], val) || val < 0.0) {
        std::cerr << "Skipping invalid duration-min line " << line_no << std::endl;
        continue;
      }
      per_channel_min_duration_ns[ch] = val;
      continue;
    }

    bool force_group = false;
    if (tokens[0] == "group" || tokens[0] == "multi" || tokens[0] == "nfold") {
      force_group = true;
      tokens.erase(tokens.begin());
    }
    if (tokens.empty()) {
      std::cerr << "Skipping invalid line " << line_no << std::endl;
      continue;
    }

    double window_ns = default_window_ns;
    double parsed_window = 0.0;
    if (!tokens.empty()) {
      const std::string &last = tokens.back();
      if (ParseWindowToken(last, parsed_window)) {
        window_ns = parsed_window;
        tokens.pop_back();
      } else {
        int tmp = 0;
        if (!ParseChannelToken(last, tmp)) {
          if (ParseDoubleToken(last, parsed_window)) {
            window_ns = parsed_window;
            tokens.pop_back();
          }
        }
      }
    }
    if (force_window) {
      window_ns = default_window_ns;
    }
    if (window_ns <= 0.0) {
      window_ns = default_window_ns;
    }

    std::vector<int> channel_list;
    channel_list.reserve(tokens.size());
    std::unordered_set<int> local_seen;
    bool bad_line = false;
    for (const auto &tok : tokens) {
      int ch = -1;
      if (!ParseChannelToken(tok, ch)) {
        bad_line = true;
        break;
      }
      if (local_seen.find(ch) != local_seen.end()) {
        bad_line = true;
        break;
      }
      local_seen.insert(ch);
      channel_list.push_back(ch);
    }
    if (bad_line || channel_list.size() < 2) {
      std::cerr << "Skipping invalid line " << line_no << std::endl;
      continue;
    }

    if (force_group) {
      if (channel_list.size() < 3) {
        std::cerr << "Skipping invalid group on line " << line_no << std::endl;
        continue;
      }
      GroupConfig cfg;
      cfg.channels = channel_list;
      cfg.window_ns = window_ns;
      groups.push_back(std::move(cfg));
      for (int ch : channel_list) {
        channels.insert(ch);
      }
      continue;
    }

    if (channel_list.size() > 2) {
      std::cerr << "Skipping group on line " << line_no << " (use 'group' prefix)" << std::endl;
      continue;
    }

    if (channel_list.size() != 2 || channel_list[0] == channel_list[1]) {
      std::cerr << "Skipping invalid pair on line " << line_no << std::endl;
      continue;
    }
    PairConfig cfg;
    cfg.channel_a = channel_list[0];
    cfg.channel_b = channel_list[1];
    cfg.window_ns = window_ns;
    pairs.push_back(std::move(cfg));
    channels.insert(channel_list[0]);
    channels.insert(channel_list[1]);
  }
  return true;
}

void InitGroupHists(std::vector<GroupConfig> &groups)
{
  if (groups.empty()) {
    return;
  }
  auto colors = DefaultColors();
  for (size_t g = 0; g < groups.size(); ++g) {
    auto &group = groups[g];
    if (group.channels.empty()) {
      continue;
    }
    const double hist_window_ns = group.window_ns * 2.2;
    const int bins = BinsForWindow(hist_window_ns);
    const std::string label = ChannelListLabel(group.channels);

    group.hists.clear();
    group.hists.reserve(group.channels.size());
    for (size_t i = 0; i < group.channels.size(); ++i) {
      std::ostringstream title;
      title << "group ch " << label << " (t_i - mean, window " << group.window_ns << " ns);"
            << "#Deltat to mean [ns]; entries";
      std::string name = "h_group_dtmean_g" + std::to_string(g) + "_ch" + std::to_string(group.channels[i]);
      auto hist = std::make_unique<TH1D>(name.c_str(),
                                         title.str().c_str(),
                                         bins,
                                         -hist_window_ns,
                                         hist_window_ns);
      hist->SetLineColor(colors[i % colors.size()]);
      hist->SetLineWidth(2);
      hist->SetDirectory(nullptr);
      group.hists.push_back(std::move(hist));
    }
  }
}

void MarkCoincident(size_t index,
                    const std::vector<int> &leading_to_trailing,
                    std::vector<char> &coincident_hits)
{
  if (index >= coincident_hits.size()) {
    return;
  }
  coincident_hits[index] = 1;
  if (index < leading_to_trailing.size()) {
    int trailing = leading_to_trailing[index];
    if (trailing >= 0 && static_cast<size_t>(trailing) < coincident_hits.size()) {
      coincident_hits[static_cast<size_t>(trailing)] = 1;
    }
  }
}

void ProcessSpill(std::unordered_map<int, std::vector<HitRef>> &hits_by_channel,
                  std::vector<PairConfig> &pairs,
                  const std::vector<Hit> &hits,
                  const std::vector<double> &tot_per_hit,
                  std::unordered_map<int, long long> &hit_counts,
                  const std::vector<int> &leading_to_trailing,
                  std::vector<char> &coincident_hits)
{
  if (hits_by_channel.empty()) {
    return;
  }

  for (auto &kv : hits_by_channel) {
    hit_counts[kv.first] += static_cast<long long>(kv.second.size());
    std::sort(kv.second.begin(), kv.second.end(), [](const HitRef &a, const HitRef &b) {
      return a.time_ns < b.time_ns;
    });
  }

  for (auto &pair : pairs) {
    auto it_a = hits_by_channel.find(pair.channel_a);
    auto it_b = hits_by_channel.find(pair.channel_b);
    if (it_a == hits_by_channel.end() || it_b == hits_by_channel.end()) {
      continue;
    }
    const auto &times_a = it_a->second;
    const auto &times_b = it_b->second;
    if (times_a.empty() || times_b.empty()) {
      continue;
    }

    const double window = pair.window_ns;
    const double search_window = pair.search_window_ns > 0.0 ? pair.search_window_ns : pair.window_ns * 15.0;
    if (pair.hist_search && search_window > 0.0) {
      size_t j_search = 0;
      for (size_t i = 0; i < times_a.size(); ++i) {
        const double t = times_a[i].time_ns;
        while (j_search < times_b.size() && times_b[j_search].time_ns < t - search_window) {
          ++j_search;
        }
        size_t k = j_search;
        while (k < times_b.size() && times_b[k].time_ns <= t + search_window) {
          const double dt = times_b[k].time_ns - t;
          pair.hist_search->Fill(dt);
          if (pair.hist_dt_fine_a || pair.hist_dt_fine_b) {
            const size_t idx_a = times_a[i].index;
            const size_t idx_b = times_b[k].index;
            if (idx_a < hits.size() && idx_b < hits.size()) {
              if (pair.hist_dt_fine_a) {
                pair.hist_dt_fine_a->Fill(dt, hits[idx_a].fine);
              }
              if (pair.hist_dt_fine_b) {
                pair.hist_dt_fine_b->Fill(dt, hits[idx_b].fine);
              }
            }
          }
          ++k;
        }
      }
    }
    size_t j = 0;
    for (size_t i = 0; i < times_a.size(); ++i) {
      const double t = times_a[i].time_ns;
      while (j < times_b.size() && times_b[j].time_ns < t - window) {
        ++j;
      }
      size_t k = j;
      while (k < times_b.size() && times_b[k].time_ns <= t + window) {
        const double dt = times_b[k].time_ns - t;
        const size_t idx_a = times_a[i].index;
        const size_t idx_b = times_b[k].index;
        const bool idx_valid = (idx_a < hits.size() && idx_b < hits.size());
        pair.hist_coinc->Fill(dt);
        ++pair.count;
        if (pair.hist_dt_fine_coinc_a || pair.hist_dt_fine_coinc_b ||
            pair.hist_dt_fine_coinc_a_tdc0 || pair.hist_dt_fine_coinc_b_tdc0 ||
            pair.hist_dt_fine_coinc_a_tdc2 || pair.hist_dt_fine_coinc_b_tdc2) {
          if (idx_valid) {
            if (pair.hist_dt_fine_coinc_a) {
              pair.hist_dt_fine_coinc_a->Fill(hits[idx_a].fine, dt);
            }
            if (pair.hist_dt_fine_coinc_b) {
              pair.hist_dt_fine_coinc_b->Fill(hits[idx_b].fine, dt);
            }
            if (hits[idx_a].tdc == 0 && pair.hist_dt_fine_coinc_a_tdc0) {
              pair.hist_dt_fine_coinc_a_tdc0->Fill(hits[idx_a].fine, dt);
            } else if (hits[idx_a].tdc == 2 && pair.hist_dt_fine_coinc_a_tdc2) {
              pair.hist_dt_fine_coinc_a_tdc2->Fill(hits[idx_a].fine, dt);
            }
            if (hits[idx_b].tdc == 0 && pair.hist_dt_fine_coinc_b_tdc0) {
              pair.hist_dt_fine_coinc_b_tdc0->Fill(hits[idx_b].fine, dt);
            } else if (hits[idx_b].tdc == 2 && pair.hist_dt_fine_coinc_b_tdc2) {
              pair.hist_dt_fine_coinc_b_tdc2->Fill(hits[idx_b].fine, dt);
            }
          }
        }
        if (!pair.hist_tot_fine_a.empty() || !pair.hist_tot_fine_b.empty()) {
          if (idx_valid && idx_a < tot_per_hit.size()) {
            const double tot_a = tot_per_hit[idx_a];
            const int tdc_a = hits[idx_a].tdc;
            if (tot_a > 0.0 && tdc_a >= 0 && tdc_a < 4) {
              auto &hist_a = pair.hist_tot_fine_a[tdc_a];
              if (hist_a) {
                hist_a->Fill(hits[idx_a].fine, tot_a);
              }
            }
          }
          if (idx_valid && idx_b < tot_per_hit.size()) {
            const double tot_b = tot_per_hit[idx_b];
            const int tdc_b = hits[idx_b].tdc;
            if (tot_b > 0.0 && tdc_b >= 0 && tdc_b < 4) {
              auto &hist_b = pair.hist_tot_fine_b[tdc_b];
              if (hist_b) {
                hist_b->Fill(hits[idx_b].fine, tot_b);
              }
            }
          }
        }
        MarkCoincident(times_a[i].index, leading_to_trailing, coincident_hits);
        MarkCoincident(times_b[k].index, leading_to_trailing, coincident_hits);
        if (!pair.coincident_hits.empty()) {
          MarkCoincident(times_a[i].index, leading_to_trailing, pair.coincident_hits);
          MarkCoincident(times_b[k].index, leading_to_trailing, pair.coincident_hits);
        }
        ++k;
      }
    }
  }
}

void ProcessGroupSpill(const std::unordered_map<int, std::vector<HitRef>> &hits_by_channel,
                       std::vector<GroupConfig> &groups,
                       const std::vector<Hit> &hits,
                       const std::vector<int> &leading_to_trailing,
                       std::vector<char> &coincident_hits)
{
  if (hits_by_channel.empty() || groups.empty()) {
    return;
  }

  for (auto &group : groups) {
    if (group.channels.size() < 3) {
      continue;
    }
    int ref_channel = group.channels[0];
    auto it_ref = hits_by_channel.find(ref_channel);
    if (it_ref == hits_by_channel.end() || it_ref->second.empty()) {
      continue;
    }
    const auto &ref_hits = it_ref->second;

    for (const auto &ref_hit : ref_hits) {
      std::vector<size_t> matched_indices;
      matched_indices.reserve(group.channels.size());
      matched_indices.push_back(ref_hit.index);

      bool ok = true;
      for (size_t i = 1; i < group.channels.size(); ++i) {
        int ch = group.channels[i];
        auto it = hits_by_channel.find(ch);
        if (it == hits_by_channel.end() || it->second.empty()) {
          ok = false;
          break;
        }
        size_t match_index = 0;
        if (!FindNearestInWindow(it->second, ref_hit.time_ns, group.window_ns, match_index)) {
          ok = false;
          break;
        }
        matched_indices.push_back(match_index);
      }

      if (!ok) {
        continue;
      }
      ++group.count;
      for (size_t idx : matched_indices) {
        MarkCoincident(idx, leading_to_trailing, coincident_hits);
      }

      if (group.hists.size() == matched_indices.size()) {
        double sum = 0.0;
        for (size_t idx : matched_indices) {
          sum += hits[idx].time_ns;
        }
        double mean = sum / static_cast<double>(matched_indices.size());
        for (size_t i = 0; i < matched_indices.size(); ++i) {
          double dt = hits[matched_indices[i]].time_ns - mean;
          group.hists[i]->Fill(dt);
        }
      }
    }
  }
}

bool FindPairWindow(const std::vector<PairConfig> &pairs,
                    int channel_a,
                    int channel_b,
                    double default_window_ns,
                    double &window_ns)
{
  for (const auto &pair : pairs) {
    if ((pair.channel_a == channel_a && pair.channel_b == channel_b) ||
        (pair.channel_a == channel_b && pair.channel_b == channel_a)) {
      window_ns = pair.window_ns;
      return true;
    }
  }
  window_ns = default_window_ns;
  return false;
}

bool CollectPairMeanTimes(const std::unordered_map<int, std::vector<HitRef>> &hits_by_channel,
                          int channel_a,
                          int channel_b,
                          double window_ns,
                          std::vector<double> &means)
{
  means.clear();
  auto it_a = hits_by_channel.find(channel_a);
  auto it_b = hits_by_channel.find(channel_b);
  if (it_a == hits_by_channel.end() || it_b == hits_by_channel.end()) {
    return false;
  }
  const auto &times_a = it_a->second;
  const auto &times_b = it_b->second;
  if (times_a.empty() || times_b.empty()) {
    return false;
  }

  size_t j = 0;
  for (size_t i = 0; i < times_a.size(); ++i) {
    const double t = times_a[i].time_ns;
    while (j < times_b.size() && times_b[j].time_ns < t - window_ns) {
      ++j;
    }
    size_t k = j;
    while (k < times_b.size() && times_b[k].time_ns <= t + window_ns) {
      means.push_back(0.5 * (t + times_b[k].time_ns));
      ++k;
    }
  }
  return !means.empty();
}

void FillMeanTimeDiffs(const std::vector<double> &means_a,
                       const std::vector<double> &means_b,
                       TH1D *hist)
{
  if (!hist) {
    return;
  }
  for (double ta : means_a) {
    for (double tb : means_b) {
      hist->Fill(ta - tb);
    }
  }
}

void PlotGroupCoincidencePlots(const std::vector<GroupConfig> &groups, const char *out_pdf)
{
  if (groups.empty()) {
    return;
  }

  for (size_t g = 0; g < groups.size(); ++g) {
    const auto &group = groups[g];
    if (group.hists.empty()) {
      continue;
    }
    std::string label = ChannelListLabel(group.channels);
    std::ostringstream title;
    title << "group ch " << label << " (t_i - mean, window " << group.window_ns << " ns);"
          << "#Deltat to mean [ns]; entries";

    std::string canvas_name = "c_group_" + std::to_string(g);
    std::string stack_name = "hs_group_" + std::to_string(g);
    TCanvas c(canvas_name.c_str(), canvas_name.c_str(), 1600, 900);
    THStack stack(stack_name.c_str(), title.str().c_str());
    for (const auto &hist : group.hists) {
      stack.Add(hist.get(), "hist");
    }
    gPad->SetLogy(1);
    stack.Draw("nostack");

    TLegend leg(0.65, 0.75, 0.88, 0.88);
    for (size_t i = 0; i < group.hists.size(); ++i) {
      std::string entry = "channel " + std::to_string(group.channels[i]);
      leg.AddEntry(group.hists[i].get(), entry.c_str(), "l");
    }
    leg.Draw();

    c.Print(out_pdf);
  }
}

bool BuildCoincidentHitDistributions(const std::vector<Hit> &hits,
                                     const analysis_time::FineCalib &fine_calib,
                                     const std::vector<char> &coincident_hits,
                                     const std::vector<int> &channel_list,
                                     double tick_ns,
                                     bool use_fine,
                                     const std::vector<double> &tot_per_hit,
                                     double min_duration_ns,
                                     const std::unordered_map<int, double> &per_channel_min_duration_ns,
                                     double duration_plot_max_ns,
                                     const std::string &label,
                                     const std::string &title_prefix,
                                     CoincPlotSet &out_set)
{
  if (hits.empty() || coincident_hits.empty() || channel_list.empty()) {
    return false;
  }

  std::unordered_map<int, size_t> channel_index;
  channel_index.reserve(channel_list.size());
  for (size_t i = 0; i < channel_list.size(); ++i) {
    channel_index[channel_list[i]] = i;
  }

  std::vector<std::vector<const Hit *>> hits_by_channel(channel_list.size());
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

  struct MinMax {
    int min = 0;
    int max = 0;
    bool has = false;
  };
  std::vector<MinMax> minmax(vars.size());

  size_t selected_hits = 0;
  for (size_t i = 0; i < hits.size(); ++i) {
    if (!coincident_hits[i]) {
      continue;
    }
    auto idx_it = channel_index.find(hits[i].channel);
    if (idx_it == channel_index.end()) {
      continue;
    }
    hits_by_channel[idx_it->second].push_back(&hits[i]);
    ++selected_hits;
    for (size_t v = 0; v < vars.size(); ++v) {
      int val = ValueForVar(hits[i], vars[v]);
      if (!minmax[v].has) {
        minmax[v].min = val;
        minmax[v].max = val;
        minmax[v].has = true;
      } else {
        minmax[v].min = std::min(minmax[v].min, val);
        minmax[v].max = std::max(minmax[v].max, val);
      }
    }
  }

  if (selected_hits == 0) {
    if (!label.empty()) {
      std::cout << "No coincidence hits available for additional plots (" << label << ")." << std::endl;
    } else {
      std::cout << "No coincidence hits available for additional plots." << std::endl;
    }
    return false;
  }

  auto colors = DefaultColors();
  CoincPlotSet set;
  set.label = label;
  set.title_prefix = title_prefix;
  set.channels = channel_list;
  set.groups.reserve(vars.size() + 1);

  for (size_t v = 0; v < vars.size(); ++v) {
    if (!minmax[v].has) {
      continue;
    }
    const std::string &var = vars[v];
    double lo = std::floor(static_cast<double>(minmax[v].min)) - 0.5;
    double hi = std::ceil(static_cast<double>(minmax[v].max)) + 0.5;
    if (lo == hi) {
      lo -= 0.5;
      hi += 0.5;
    }
    int bins = BinsForRange(lo, hi);

    CoincPlotGroup group;
    group.kind = var;
    group.name = "coinc_" + label + "_" + var;
    if (!title_prefix.empty()) {
      group.title = title_prefix + ": " + var + "; " + var + "; entries";
    } else {
      group.title = "coincident hits: " + var + "; " + var + "; entries";
    }
    group.hists.reserve(channel_list.size());

    for (size_t i = 0; i < channel_list.size(); ++i) {
      std::string name = "h_coinc_" + label + "_" + var + "_ch" + std::to_string(channel_list[i]);
      std::string ch_title;
      if (!title_prefix.empty()) {
        ch_title = title_prefix + " " + var + " (channel " + std::to_string(channel_list[i]) + "); " + var +
                   "; entries";
      } else {
        ch_title = var + " (channel " + std::to_string(channel_list[i]) +
                   ", coincident hits); " + var + "; entries";
      }
      auto hist = std::make_unique<TH1D>(name.c_str(), ch_title.c_str(), bins, lo, hi);
      hist->SetLineColor(colors[i % colors.size()]);
      hist->SetLineWidth(2);
      hist->SetDirectory(nullptr);
      for (const auto *hit : hits_by_channel[i]) {
        hist->Fill(ValueForVar(*hit, var));
      }
      group.hists.push_back(std::move(hist));
    }

    set.groups.push_back(std::move(group));
  }

  const double duration_max_ns = duration_plot_max_ns > 0.0 ? duration_plot_max_ns : 0.0;
  std::vector<std::vector<double>> durations(channel_list.size());

  for (size_t i = 0; i < hits.size(); ++i) {
    if (i >= coincident_hits.size() || !coincident_hits[i]) {
      continue;
    }
    if (i >= tot_per_hit.size() || !IsLeadingTdc(hits[i].tdc)) {
      continue;
    }
    auto idx_it = channel_index.find(hits[i].channel);
    if (idx_it == channel_index.end()) {
      continue;
    }
    const double dt_ns = tot_per_hit[i];
    if (dt_ns <= 0.0) {
      continue;
    }
    const double channel_min_duration =
        std::max(0.0, ChannelMinDurationNs(per_channel_min_duration_ns, hits[i].channel, min_duration_ns));
    if (dt_ns < channel_min_duration) {
      continue;
    }
    if (duration_max_ns > 0.0 && dt_ns > duration_max_ns) {
      continue;
    }
    durations[idx_it->second].push_back(dt_ns);
  }

  if (duration_max_ns > 0.0) {
    int bins = static_cast<int>(duration_max_ns * 4.0);
    if (bins < 1) {
      bins = 1;
    }
    if (bins > 400) {
      bins = 400;
    }
    CoincPlotGroup duration_group;
    duration_group.kind = "duration";
    duration_group.name = "coinc_" + label + "_duration";
    if (!title_prefix.empty()) {
      std::ostringstream title;
      title << title_prefix << " Time-over-Threshold (ToT) (<= " << duration_max_ns
            << " ns); ToT [ns]; entries";
      duration_group.title = title.str();
    } else {
      std::ostringstream title;
      title << "coincident hit Time-over-Threshold (ToT) (<= " << duration_max_ns
            << " ns); ToT [ns]; entries";
      duration_group.title = title.str();
    }
    duration_group.hists.reserve(channel_list.size());

    for (size_t i = 0; i < channel_list.size(); ++i) {
      std::string name = "h_coinc_" + label + "_duration_ch" + std::to_string(channel_list[i]);
      std::string ch_title;
      if (!title_prefix.empty()) {
        std::ostringstream title;
        title << title_prefix << " Time-over-Threshold (ToT) (channel " << channel_list[i]
              << ", <= " << duration_max_ns << " ns); ToT [ns]; entries";
        ch_title = title.str();
      } else {
        std::ostringstream title;
        title << "coincident hit Time-over-Threshold (ToT) (leading-trailing, <= " << duration_max_ns
              << " ns) (channel " << channel_list[i] << "); ToT [ns]; entries";
        ch_title = title.str();
      }
      auto hist = std::make_unique<TH1D>(name.c_str(), ch_title.c_str(), bins, 0.0, duration_max_ns);
      hist->SetLineColor(colors[i % colors.size()]);
      hist->SetLineWidth(2);
      hist->SetDirectory(nullptr);
      for (double dt : durations[i]) {
        hist->Fill(dt);
      }
      duration_group.hists.push_back(std::move(hist));
    }

    set.groups.push_back(std::move(duration_group));
  }

  if (!set.groups.empty()) {
    std::stable_partition(set.groups.begin(), set.groups.end(), [](const CoincPlotGroup &grp) {
      return grp.kind == "duration";
    });
  }

  if (set.groups.empty()) {
    std::cout << "No coincidence histograms to draw." << std::endl;
    return false;
  }

  out_set = std::move(set);
  return true;
}

std::vector<std::string> CoincidentPlotVars()
{
  return {
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
}

std::unique_ptr<TH1D> MakeStreamingVarHist(const std::string &name,
                                           const std::string &title,
                                           const std::string &var)
{
  int bins = 64;
  double lo = -0.5;
  double hi = 63.5;

  if (var == "fifo") {
    bins = 25;
    hi = 24.5;
  } else if (var == "type") {
    bins = 20;
    hi = 19.5;
  } else if (var == "column") {
    bins = 8;
    hi = 7.5;
  } else if (var == "pixel") {
    bins = 4;
    hi = 3.5;
  } else if (var == "tdc") {
    bins = 4;
    hi = 3.5;
  } else if (var == "fine") {
    bins = BinsForRange(0.0, static_cast<double>(analysis_time::kFineBins));
    lo = -0.5;
    hi = static_cast<double>(analysis_time::kFineBins) - 0.5;
  } else if (var == "counter") {
    bins = 200;
    hi = 4095.5;
  } else if (var == "rollover") {
    bins = 200;
    hi = 65535.5;
  } else if (var == "coarse") {
    bins = 200;
    hi = 32767.5;
  } else if (var == "device") {
    bins = 16;
    hi = 15.5;
  }

  auto hist = std::make_unique<TH1D>(name.c_str(), title.c_str(), bins, lo, hi);
  hist->SetDirectory(nullptr);
  hist->SetLineWidth(2);
  return hist;
}

std::vector<CoincPlotSet> InitStreamingCoincidentPlotSets(const std::vector<PairConfig> &pairs,
                                                          double max_duration_ns)
{
  std::vector<CoincPlotSet> sets;
  sets.reserve(pairs.size());
  auto vars = CoincidentPlotVars();
  auto colors = DefaultColors();

  for (size_t p = 0; p < pairs.size(); ++p) {
    const auto &pair = pairs[p];
    std::vector<int> pair_channels = {pair.channel_a, pair.channel_b};
    std::ostringstream label;
    label << "pair" << p << "_ch" << pair.channel_a << "_" << pair.channel_b << "_w" << pair.window_ns;
    std::string label_tag = SanitizeTag(label.str());
    std::ostringstream title_prefix;
    title_prefix << "coincident hits ch " << pair.channel_a << " vs " << pair.channel_b
                 << " (window " << pair.window_ns << " ns)";

    CoincPlotSet set;
    set.label = label_tag;
    set.title_prefix = title_prefix.str();
    set.channels = pair_channels;

    const double duration_plot_max = std::max(pair.window_ns, max_duration_ns > 0.0 ? max_duration_ns : 0.0);
    if (duration_plot_max > 0.0) {
      int bins = static_cast<int>(duration_plot_max * 4.0);
      if (bins < 1) {
        bins = 1;
      }
      if (bins > 400) {
        bins = 400;
      }
      CoincPlotGroup duration_group;
      duration_group.kind = "duration";
      duration_group.name = "coinc_" + label_tag + "_duration";
      std::ostringstream duration_title;
      duration_title << set.title_prefix << " Time-over-Threshold (ToT) (<= " << duration_plot_max
                     << " ns); ToT [ns]; entries";
      duration_group.title = duration_title.str();
      duration_group.hists.reserve(pair_channels.size());
      for (size_t i = 0; i < pair_channels.size(); ++i) {
        std::string name = "h_coinc_" + label_tag + "_duration_ch" + std::to_string(pair_channels[i]);
        std::ostringstream ch_title;
        ch_title << set.title_prefix << " Time-over-Threshold (ToT) (channel " << pair_channels[i]
                 << ", <= " << duration_plot_max << " ns); ToT [ns]; entries";
        auto hist = std::make_unique<TH1D>(name.c_str(), ch_title.str().c_str(), bins, 0.0, duration_plot_max);
        hist->SetLineColor(colors[i % colors.size()]);
        hist->SetLineWidth(2);
        hist->SetDirectory(nullptr);
        duration_group.hists.push_back(std::move(hist));
      }
      set.groups.push_back(std::move(duration_group));
    }

    for (const auto &var : vars) {
      CoincPlotGroup group;
      group.kind = var;
      group.name = "coinc_" + label_tag + "_" + var;
      group.title = set.title_prefix + ": " + var + "; " + var + "; entries";
      group.hists.reserve(pair_channels.size());
      for (size_t i = 0; i < pair_channels.size(); ++i) {
        std::string name = "h_coinc_" + label_tag + "_" + var + "_ch" + std::to_string(pair_channels[i]);
        std::string ch_title =
            set.title_prefix + " " + var + " (channel " + std::to_string(pair_channels[i]) + "); " + var +
            "; entries";
        auto hist = MakeStreamingVarHist(name, ch_title, var);
        hist->SetLineColor(colors[i % colors.size()]);
        group.hists.push_back(std::move(hist));
      }
      set.groups.push_back(std::move(group));
    }

    sets.push_back(std::move(set));
  }

  return sets;
}

void FillCoincidentPlotSetFromMask(const std::vector<Hit> &hits,
                                   const std::vector<char> &coincident_hits,
                                   CoincPlotSet &set,
                                   const std::vector<double> &tot_per_hit,
                                   double min_duration_ns,
                                   const std::unordered_map<int, double> &per_channel_min_duration_ns,
                                   double duration_plot_max_ns)
{
  if (hits.empty() || coincident_hits.empty() || set.groups.empty()) {
    return;
  }

  std::unordered_map<int, size_t> channel_index;
  channel_index.reserve(set.channels.size());
  for (size_t i = 0; i < set.channels.size(); ++i) {
    channel_index[set.channels[i]] = i;
  }

  for (size_t i = 0; i < hits.size(); ++i) {
    if (i >= coincident_hits.size() || !coincident_hits[i]) {
      continue;
    }
    auto idx_it = channel_index.find(hits[i].channel);
    if (idx_it == channel_index.end()) {
      continue;
    }
    const size_t channel_pos = idx_it->second;

    for (auto &group : set.groups) {
      if (channel_pos >= group.hists.size() || !group.hists[channel_pos]) {
        continue;
      }
      if (group.kind == "duration") {
        if (i >= tot_per_hit.size() || !IsLeadingTdc(hits[i].tdc)) {
          continue;
        }
        const double dt_ns = tot_per_hit[i];
        if (dt_ns <= 0.0) {
          continue;
        }
        const double channel_min_duration =
            std::max(0.0, ChannelMinDurationNs(per_channel_min_duration_ns, hits[i].channel, min_duration_ns));
        if (dt_ns < channel_min_duration) {
          continue;
        }
        if (duration_plot_max_ns > 0.0 && dt_ns > duration_plot_max_ns) {
          continue;
        }
        group.hists[channel_pos]->Fill(dt_ns);
      } else {
        group.hists[channel_pos]->Fill(ValueForVar(hits[i], group.kind));
      }
    }
  }
}

void ProcessStreamingSpill(std::vector<Hit> &hits,
                           std::vector<PairConfig> &pairs,
                           std::vector<GroupConfig> &groups,
                           TH1D *pair_mean_hist,
                           double pair_mean_window_a,
                           double pair_mean_window_b,
                           std::vector<CoincPlotSet> &pair_plot_sets,
                           std::unordered_map<int, long long> &hit_counts,
                           const analysis_time::FineCalib &fine_calib,
                           double tick_ns,
                           bool use_fine,
                           double min_duration_ns,
                           double max_duration_ns,
                           const std::unordered_map<int, double> &per_channel_min_duration_ns,
                           const analysis_time::ChannelCalib &chan_calib,
                           const std::unordered_map<int, TimewalkCorrection> &timewalk_corrections,
                           long long &timewalk_corrected_hits,
                           PreviewStore &preview)
{
  if (hits.empty()) {
    return;
  }

  std::vector<char> leading_mask;
  std::vector<int> leading_to_trailing;
  const bool use_duration_filter =
      max_duration_ns > 0.0 || min_duration_ns > 0.0 || !per_channel_min_duration_ns.empty();
  if (use_duration_filter) {
    auto duration_info = ComputeDurationInfo(
        hits, fine_calib, tick_ns, min_duration_ns, max_duration_ns, per_channel_min_duration_ns, use_fine);
    leading_mask = std::move(duration_info.leading_mask);
    leading_to_trailing = std::move(duration_info.leading_to_trailing);
  } else {
    leading_mask.assign(hits.size(), 0);
    leading_to_trailing.assign(hits.size(), -1);
    for (size_t i = 0; i < hits.size(); ++i) {
      if (IsLeadingTdc(hits[i].tdc)) {
        leading_mask[i] = 1;
      }
    }
  }

  std::vector<double> tot_per_hit(hits.size(), -1.0);
  if (use_duration_filter) {
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i >= leading_mask.size() || !leading_mask[i]) {
        continue;
      }
      int trailing = (i < leading_to_trailing.size()) ? leading_to_trailing[i] : -1;
      if (trailing < 0 || trailing >= static_cast<int>(hits.size())) {
        continue;
      }
      double dt = hits[trailing].time_ns_raw - hits[i].time_ns_raw;
      const double channel_min_duration =
          std::max(0.0, ChannelMinDurationNs(per_channel_min_duration_ns, hits[i].channel, min_duration_ns));
      if (dt <= 0.0 || dt < channel_min_duration || (max_duration_ns > 0.0 && dt > max_duration_ns)) {
        continue;
      }
      tot_per_hit[i] = dt;
      tot_per_hit[static_cast<size_t>(trailing)] = dt;
    }
  }

  if (chan_calib.loaded) {
    if (max_duration_ns > 0.0) {
      for (size_t i = 0; i < hits.size(); ++i) {
        if (i >= leading_mask.size() || !leading_mask[i]) {
          continue;
        }
        double tot = tot_per_hit[i];
        if (tot <= 0.0) {
          continue;
        }
        hits[i].time_ns -= chan_calib.CorrectionNs(hits[i].channel, tot);
      }
    }
  }

  if (!timewalk_corrections.empty()) {
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i >= leading_mask.size() || !leading_mask[i]) {
        continue;
      }
      const double tot = tot_per_hit[i];
      if (tot <= 0.0) {
        continue;
      }
      auto correction_it = timewalk_corrections.find(hits[i].channel);
      if (correction_it == timewalk_corrections.end()) {
        continue;
      }
      const double correction_ns = correction_it->second.CorrectionNs(tot);
      if (!std::isfinite(correction_ns)) {
        continue;
      }
      hits[i].time_ns -= correction_ns;
      ++timewalk_corrected_hits;
    }
  }

  if (preview.max_events > 0) {
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i < leading_mask.size() && leading_mask[i]) {
        const double tot = (i < tot_per_hit.size()) ? tot_per_hit[i] : -1.0;
        preview.AddLeading(hits[i], tot);
      }
    }
  }

  for (auto &pair : pairs) {
    pair.coincident_hits.assign(hits.size(), 0);
  }
  std::vector<char> all_coincident_hits(hits.size(), 0);

  std::unordered_map<int, std::vector<HitRef>> spill_hits;
  for (size_t i = 0; i < hits.size(); ++i) {
    if (i >= leading_mask.size() || !leading_mask[i]) {
      continue;
    }
    spill_hits[hits[i].channel].push_back({hits[i].time_ns, i});
  }

  ProcessSpill(spill_hits, pairs, hits, tot_per_hit, hit_counts, leading_to_trailing, all_coincident_hits);
  ProcessGroupSpill(spill_hits, groups, hits, leading_to_trailing, all_coincident_hits);
  if (pair_mean_hist) {
    std::vector<double> means_a;
    std::vector<double> means_b;
    if (CollectPairMeanTimes(spill_hits, 17, 19, pair_mean_window_a, means_a) &&
        CollectPairMeanTimes(spill_hits, 22, 23, pair_mean_window_b, means_b)) {
      FillMeanTimeDiffs(means_a, means_b, pair_mean_hist);
    }
  }

  for (size_t p = 0; p < pairs.size() && p < pair_plot_sets.size(); ++p) {
    const double duration_plot_max = std::max(pairs[p].window_ns, max_duration_ns > 0.0 ? max_duration_ns : 0.0);
    FillCoincidentPlotSetFromMask(hits,
                                  pairs[p].coincident_hits,
                                  pair_plot_sets[p],
                                  tot_per_hit,
                                  min_duration_ns,
                                  per_channel_min_duration_ns,
                                  duration_plot_max);
    pairs[p].coincident_hits.clear();
  }
}

void FitAndAnnotateDurationStack(const CoincPlotSet &set,
                                 const CoincPlotGroup &group,
                                 std::vector<std::unique_ptr<TF1>> &fits,
                                 std::vector<std::unique_ptr<TLatex>> &labels,
                                 double x0 = 0.15,
                                 double y0 = 0.85);

void DrawCoincidentStacksPerPair(const std::vector<CoincPlotSet> &sets, const char *out_pdf)
{
  if (sets.empty()) {
    return;
  }

  for (const auto &set : sets) {
    if (set.groups.empty()) {
      continue;
    }
    int cols = 1;
    int rows = 1;
    GridForCount(set.groups.size(), cols, rows);
    std::string name = "c_coinc_summary";
    if (!set.label.empty()) {
      name += "_" + set.label;
    }
    TCanvas c_summary(name.c_str(), name.c_str(), 1600, 900);
    c_summary.Divide(cols, rows, 0.001, 0.001);

    std::vector<std::unique_ptr<THStack>> stacks;
    std::vector<std::unique_ptr<TLegend>> legends;
    std::vector<std::unique_ptr<TF1>> duration_fits;
    std::vector<std::unique_ptr<TLatex>> duration_labels;
    stacks.reserve(set.groups.size());
    legends.reserve(set.groups.size());

    for (size_t g = 0; g < set.groups.size(); ++g) {
      const auto &group = set.groups[g];
      if (group.hists.empty()) {
        continue;
      }
      c_summary.cd(static_cast<int>(g + 1));
      gPad->SetLogy(1);
      std::string stack_name = "hs_" + group.name + "_summary";
      auto stack = std::make_unique<THStack>(stack_name.c_str(), group.title.c_str());
      for (const auto &hist : group.hists) {
        stack->Add(hist.get(), "hist");
      }
      stack->Draw("nostack");

      auto leg = std::make_unique<TLegend>(0.65, 0.75, 0.88, 0.88);
      for (size_t i = 0; i < group.hists.size(); ++i) {
        std::string label = "channel " + std::to_string(set.channels[i]);
        leg->AddEntry(group.hists[i].get(), label.c_str(), "l");
      }
      leg->Draw();

      FitAndAnnotateDurationStack(set, group, duration_fits, duration_labels);

      stacks.push_back(std::move(stack));
      legends.push_back(std::move(leg));
    }

    c_summary.Print(out_pdf);
  }
}

void FitAndAnnotateDurationStack(const CoincPlotSet &set,
                                 const CoincPlotGroup &group,
                                 std::vector<std::unique_ptr<TF1>> &fits,
                                 std::vector<std::unique_ptr<TLatex>> &labels,
                                 double x0,
                                 double y0)
{
  if (group.kind != "duration" || group.hists.empty()) {
    return;
  }

  size_t printed_labels = 0;
  for (size_t i = 0; i < group.hists.size(); ++i) {
    TH1D *hist = group.hists[i].get();
    if (!hist || i >= set.channels.size() || hist->GetEntries() < 5) {
      continue;
    }

    const double mean_seed = hist->GetMean();
    const double sigma_seed = hist->GetRMS();
    if (!std::isfinite(mean_seed) || !std::isfinite(sigma_seed) || sigma_seed <= 0.0) {
      continue;
    }

    const double x_min = hist->GetXaxis()->GetXmin();
    const double x_max = hist->GetXaxis()->GetXmax();
    double fit_min = std::max(x_min, mean_seed - 6.0 * sigma_seed);
    double fit_max = std::min(x_max, mean_seed + 6.0 * sigma_seed);
    if (fit_max <= fit_min) {
      fit_min = x_min;
      fit_max = x_max;
    }

    auto fit = std::make_unique<TF1>(
        Form("f_%s_ch%d_gaus", group.name.c_str(), set.channels[i]),
        "gaus",
        fit_min,
        fit_max);
    fit->SetParameters(hist->GetMaximum(), mean_seed, sigma_seed);
    fit->SetParLimits(2, 1e-6, std::max(1e-6, x_max - x_min));
    fit->SetLineColor(kRed);
    fit->SetLineWidth(1);

    hist->Fit(fit.get(), "RQ0");
    const double mean = fit->GetParameter(1);
    const double sigma = std::abs(fit->GetParameter(2));
    if (!std::isfinite(mean) || !std::isfinite(sigma) || sigma <= 0.0) {
      continue;
    }

    fit->Draw("same");

    const double npe = (mean / sigma) * (mean / sigma);
    auto text = std::make_unique<TLatex>();
    text->SetNDC(true);
    text->SetTextFont(42);
    text->SetTextSize(0.04);
    text->SetTextColor(hist->GetLineColor());
    std::ostringstream line;
    line << std::fixed << std::setprecision(2)
         << "ch " << set.channels[i] << ": N_{P.E.} = " << npe;
    text->DrawLatex(x0, y0 - 0.06 * printed_labels, line.str().c_str());
    ++printed_labels;

    fits.push_back(std::move(fit));
    labels.push_back(std::move(text));
  }

  gPad->Modified();
  gPad->Update();
}

void DrawCoincidentStacksPerVariable(const std::vector<CoincPlotSet> &sets, const char *out_pdf)
{
  if (sets.empty()) {
    return;
  }
  const auto &ref_groups = sets.front().groups;
  if (ref_groups.empty()) {
    return;
  }
  std::vector<std::string> kinds;
  kinds.reserve(ref_groups.size());
  for (const auto &grp : ref_groups) {
    kinds.push_back(grp.kind);
  }

  for (const auto &kind : kinds) {
    int cols = 1;
    int rows = 1;
    GridForCount(sets.size(), cols, rows);
    std::string canvas_name = "c_coinc_var_" + kind;
    TCanvas c_var(canvas_name.c_str(), canvas_name.c_str(), 1600, 900);
    c_var.Divide(cols, rows, 0.001, 0.001);

    std::vector<std::unique_ptr<THStack>> stacks;
    std::vector<std::unique_ptr<TLegend>> legends;
    std::vector<std::unique_ptr<TF1>> duration_fits;
    std::vector<std::unique_ptr<TLatex>> duration_labels;
    stacks.reserve(sets.size());
    legends.reserve(sets.size());

    size_t pad = 1;
    for (const auto &set : sets) {
      if (pad > sets.size()) {
        break;
      }
      const CoincPlotGroup *group_ptr = nullptr;
      for (const auto &grp : set.groups) {
        if (grp.kind == kind) {
          group_ptr = &grp;
          break;
        }
      }
      if (!group_ptr || group_ptr->hists.empty()) {
        ++pad;
        continue;
      }
      c_var.cd(static_cast<int>(pad));
      gPad->SetLogy(1);
      std::string stack_name = "hs_" + group_ptr->name + "_all";
      auto stack = std::make_unique<THStack>(stack_name.c_str(), group_ptr->title.c_str());
      for (const auto &hist : group_ptr->hists) {
        stack->Add(hist.get(), "hist");
      }
      stack->Draw("nostack");

      auto leg = std::make_unique<TLegend>(0.65, 0.75, 0.88, 0.88);
      for (size_t i = 0; i < group_ptr->hists.size(); ++i) {
        std::string label = "channel " + std::to_string(set.channels[i]);
        leg->AddEntry(group_ptr->hists[i].get(), label.c_str(), "l");
      }
      leg->Draw();

      FitAndAnnotateDurationStack(set, *group_ptr, duration_fits, duration_labels);

      stacks.push_back(std::move(stack));
      legends.push_back(std::move(leg));
      ++pad;
    }

    c_var.Print(out_pdf);
  }
}

void WriteHistToTxt(std::ostream &out, const TH1D &hist)
{
  const int bins = hist.GetNbinsX();
  for (int b = 1; b <= bins; ++b) {
    double lo = hist.GetXaxis()->GetBinLowEdge(b);
    double hi = hist.GetXaxis()->GetBinUpEdge(b);
    double c = hist.GetBinContent(b);
    out << lo << " " << hi << " " << c << "\n";
  }
}

void WriteCoincidenceTxt(const char *out_txt,
                         const std::vector<PairConfig> &pairs,
                         const std::vector<PairStats> &pair_stats,
                         double clock_mhz,
                         bool use_fine,
                         double min_duration_ns,
                         double max_duration_ns,
                         int fine_cut,
                         const char *pairs_file,
                         const std::unordered_map<int, double> &per_channel_min_duration_ns)
{
  if (!out_txt || out_txt[0] == '\0') {
    return;
  }
  std::ofstream out(out_txt);
  if (!out) {
    std::cout << "Failed to open TXT output file: " << out_txt << std::endl;
    return;
  }
  out << "# coincidence txt output\n";
  out << "# pairs_file: " << (pairs_file ? pairs_file : "") << "\n";
  out << "# clock_mhz: " << clock_mhz << "\n";
  out << "# use_fine: " << (use_fine ? 1 : 0) << "\n";
  out << "# min_duration_ns: " << min_duration_ns << "\n";
  out << "# max_duration_ns: " << max_duration_ns << "\n";
  out << "# fine_cut: " << fine_cut << "\n";
  for (const auto &kv : per_channel_min_duration_ns) {
    out << "# channel_min_duration_ns ch=" << kv.first << " value=" << kv.second << "\n";
  }

  for (size_t i = 0; i < pairs.size(); ++i) {
    const auto &pair = pairs[i];
    const auto &stats = (i < pair_stats.size()) ? pair_stats[i] : PairStats{};
    out << "# pair " << i << " ch_a=" << pair.channel_a << " ch_b=" << pair.channel_b
        << " window_ns=" << pair.window_ns << " search_window_ns=" << pair.search_window_ns << "\n";
    out << "# fwhm_bkg_sub=" << stats.fwhm << " fwhm_err=" << stats.fwhm_err
        << " bkg_sideband=" << stats.bkg << " bkg_err=" << stats.bkg_err
        << " gaus_sigma=" << stats.gaus_sigma << " gaus_sigma_err=" << stats.gaus_sigma_err
        << " x_left=" << stats.x_left << " x_right=" << stats.x_right
        << " entries=" << stats.entries << " boot_trials=" << stats.boot_trials << "\n";
    if (pair.hist_search) {
      out << "# hist_search: bin_low bin_high count\n";
      WriteHistToTxt(out, *pair.hist_search);
    }
    if (pair.hist_coinc) {
      out << "# hist_coinc: bin_low bin_high count\n";
      WriteHistToTxt(out, *pair.hist_coinc);
    }
  }
  std::cout << "Wrote coincidence TXT output to " << out_txt << std::endl;
}

void WriteCoincidenceRoot(const char *out_root,
                          const std::vector<PairConfig> &pairs,
                          const std::vector<GroupConfig> &groups,
                          const TH1D *pair_mean_hist,
                          const std::vector<CoincPlotSet> &pair_plot_sets,
                          double clock_mhz,
                          bool use_fine,
                          double min_duration_ns,
                          double max_duration_ns,
                          int fine_cut,
                          const char *pairs_file,
                          const std::unordered_map<int, double> &per_channel_min_duration_ns)
{
  if (!out_root || out_root[0] == '\0') {
    return;
  }
  std::unique_ptr<TFile> out(TFile::Open(out_root, "RECREATE"));
  if (!out || out->IsZombie()) {
    std::cout << "Failed to open ROOT output file: " << out_root << std::endl;
    return;
  }

  if (pairs_file && pairs_file[0] != '\0') {
    TNamed pairs_name("pairs_file", pairs_file);
    pairs_name.Write();
  }
  TParameter<double> p_clock("clock_mhz", clock_mhz);
  TParameter<int> p_use_fine("use_fine", use_fine ? 1 : 0);
  TParameter<double> p_mindur("min_duration_ns", min_duration_ns);
  TParameter<double> p_maxdur("max_duration_ns", max_duration_ns);
  TParameter<int> p_fine_cut("fine_cut", fine_cut);
  p_clock.Write();
  p_use_fine.Write();
  p_mindur.Write();
  p_maxdur.Write();
  p_fine_cut.Write();
  for (const auto &kv : per_channel_min_duration_ns) {
    const std::string name = "min_duration_ch" + std::to_string(kv.first);
    TParameter<double> p(name.c_str(), kv.second);
    p.Write();
  }

  for (const auto &pair : pairs) {
    if (pair.hist_search) {
      pair.hist_search->Write();
    }
    if (pair.hist_coinc) {
      pair.hist_coinc->Write();
    }
    if (pair.hist_dt_fine_a) {
      pair.hist_dt_fine_a->Write();
    }
    if (pair.hist_dt_fine_b) {
      pair.hist_dt_fine_b->Write();
    }
    if (pair.hist_dt_fine_coinc_a) {
      pair.hist_dt_fine_coinc_a->Write();
    }
    if (pair.hist_dt_fine_coinc_b) {
      pair.hist_dt_fine_coinc_b->Write();
    }
    if (pair.hist_dt_fine_coinc_a_tdc0) {
      pair.hist_dt_fine_coinc_a_tdc0->Write();
    }
    if (pair.hist_dt_fine_coinc_b_tdc0) {
      pair.hist_dt_fine_coinc_b_tdc0->Write();
    }
    if (pair.hist_dt_fine_coinc_a_tdc2) {
      pair.hist_dt_fine_coinc_a_tdc2->Write();
    }
    if (pair.hist_dt_fine_coinc_b_tdc2) {
      pair.hist_dt_fine_coinc_b_tdc2->Write();
    }
    for (auto &hist : pair.hist_tot_fine_a) {
      if (hist) {
        hist->Write();
      }
    }
    for (auto &hist : pair.hist_tot_fine_b) {
      if (hist) {
        hist->Write();
      }
    }
  }
  if (pair_mean_hist) {
    pair_mean_hist->Write();
  }
  for (const auto &group : groups) {
    for (const auto &hist : group.hists) {
      if (hist) {
        hist->Write();
      }
    }
  }
  for (const auto &set : pair_plot_sets) {
    for (const auto &group : set.groups) {
      for (const auto &hist : group.hists) {
        if (hist) {
          hist->Write();
        }
      }
    }
  }
  out->Close();
  std::cout << "Wrote coincidence ROOT output to " << out_root << std::endl;
}

}  // namespace

void coincidence_rdf(const char *decoded_dir = "../raw_data/latest/kc705-196/decoded",
                     const char *pairs_file = "pairs.txt",
                     const char *out_pdf = "coincidences.pdf",
                     double default_window_ns = 10.0,
                     double clock_mhz = 320.0,
                     bool use_fine = true,
                     double max_duration_ns = 15.0,
                     double min_duration_ns = 0.0,
                     const char *fine_calib_path = "",
                     bool force_window = false,
                     const char *chan_calib_path = "",
                     int fine_cut = analysis_time::kDefaultFineCut,
                     const char *out_root = "",
                     const char *out_txt = "",
                     bool use_lut = true,
                     int preview_hits = 100,
                     const char *timewalk_calib_path = "")
{
  ScopedTimer timer("coincidence_rdf");
  ROOT::EnableImplicitMT();

  if (WantsHelp(decoded_dir)) {
    PrintCoincidenceHelp();
    return;
  }

  gStyle->SetOptStat(0);
  gStyle->SetOptFit(1111);
  auto input_spec = analysis_io::ResolveInputSpec(decoded_dir);
  if (input_spec.files.empty()) {
    std::cout << "No decoded ROOT files found under " << decoded_dir << std::endl;
    return;
  }

  std::unordered_set<int> channels;
  std::vector<PairConfig> pairs;
  std::vector<GroupConfig> groups;
  std::unordered_map<int, double> per_channel_min_duration_ns;
  if (!LoadCoincidenceConfig(
          pairs_file, default_window_ns, force_window, pairs, groups, channels, per_channel_min_duration_ns)) {
    return;
  }
  if (pairs.empty() && groups.empty()) {
    std::cout << "No valid channel pairs/groups found in " << pairs_file << std::endl;
    return;
  }

  analysis_time::ChannelCalib chan_calib;
  bool chan_calib_loaded = false;
  if (chan_calib_path && chan_calib_path[0] != '\0') {
    if (chan_calib.LoadFromFile(chan_calib_path)) {
      chan_calib_loaded = true;
    }
    if (chan_calib.meta_loaded && chan_calib.meta_maxdur > 0.0) {
      if (max_duration_ns <= 0.0 || std::abs(max_duration_ns - chan_calib.meta_maxdur) > 1e-6) {
        std::cout << "Warning: max_duration_ns=" << max_duration_ns
                  << " does not match channel calibration (" << chan_calib.meta_maxdur
                  << " ns). Using calibration value." << std::endl;
        max_duration_ns = chan_calib.meta_maxdur;
      }
      if (chan_calib.meta_bins > 0) {
        std::cout << "Channel calibration ToT bins: " << chan_calib.meta_bins << std::endl;
      }
      if (chan_calib.meta_sym >= 0) {
        std::cout << "Channel calibration symmetrize_ref: " << chan_calib.meta_sym << std::endl;
      }
    }
  }

  std::cout << "Input: " << decoded_dir << std::endl;
  std::cout << "Pairs: " << pairs_file << std::endl;
  std::cout << "Output: " << out_pdf << std::endl;
  std::cout << "Clock (MHz): " << clock_mhz << std::endl;
  std::cout << "Use fine: " << (use_fine ? 1 : 0) << std::endl;
  std::cout << "Min duration (ns): " << min_duration_ns << std::endl;
  std::cout << "Max duration (ns): " << max_duration_ns << std::endl;
  if (!per_channel_min_duration_ns.empty()) {
    std::vector<int> channels_with_min;
    channels_with_min.reserve(per_channel_min_duration_ns.size());
    for (const auto &kv : per_channel_min_duration_ns) {
      channels_with_min.push_back(kv.first);
    }
    std::sort(channels_with_min.begin(), channels_with_min.end());
    std::cout << "Per-channel min duration overrides (ns):";
    for (int ch : channels_with_min) {
      std::cout << " ch" << ch << "=" << per_channel_min_duration_ns[ch];
    }
    std::cout << std::endl;
  }
  if (use_fine && fine_cut > 0) {
    std::cout << "Fine cut (bins): " << fine_cut << std::endl;
  } else {
    std::cout << "Fine cut (bins): 0" << std::endl;
  }
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    std::cout << "Fine calib: " << fine_calib_path << std::endl;
  }

  const int fine_plot_bins = 128;
  const double fine_plot_min = 0.0;
  const double fine_plot_max = static_cast<double>(analysis_time::kFineBins);

  for (auto &pair : pairs) {
    const double hist_window_ns = pair.window_ns * 1.0;
    const double search_window_ns = std::max(pair.window_ns * 15.0, 100.0 + 4.0 * pair.window_ns);

    int bins = 2 * BinsForWindow(hist_window_ns);
    const double bin_width = (bins > 0) ? (2.0 * hist_window_ns / static_cast<double>(bins)) : 1.0;
    std::ostringstream title;
    title << "ch " << pair.channel_a << " vs " << pair.channel_b
          << " (coinc window " << pair.window_ns << " ns);"
          << "#Deltat [ns];pairs";
    std::string name = "h_dt_coinc_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b);
    auto hist = std::make_unique<TH1D>(name.c_str(),
                                       title.str().c_str(),
                                       bins,
                                       -hist_window_ns,
                                       hist_window_ns);
    hist->SetLineWidth(2);
    hist->SetDirectory(nullptr);
    pair.hist_coinc = std::move(hist);
    pair.search_window_ns = search_window_ns;

    int bins_search = static_cast<int>(std::ceil((2.0 * search_window_ns) / bin_width));
    if (bins_search < 10) {
      bins_search = 10;
    }
    std::ostringstream title_search;
    title_search << "ch " << pair.channel_a << " vs " << pair.channel_b
                 << " (search window " << (search_window_ns) << " ns);"
                 << "#Deltat [ns];pairs";
    std::string name_search =
        "h_dt_search_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b);
    auto hist_search = std::make_unique<TH1D>(name_search.c_str(),
                                              title_search.str().c_str(),
                                              bins_search,
                                              -search_window_ns,
                                              search_window_ns);
    hist_search->SetLineWidth(2);
    hist_search->SetDirectory(nullptr);
    pair.hist_search = std::move(hist_search);
    pair.search_window_ns = search_window_ns;

    {
      std::ostringstream title_a;
      title_a << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_a << ", search window " << search_window_ns << " ns);"
              << "#Deltat [ns]; fine (ch " << pair.channel_a << "); pairs";
      std::string name_a =
          "h_dt_fine_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_a";
      auto hist_a = std::make_unique<TH2D>(name_a.c_str(),
                                           title_a.str().c_str(),
                                           bins_search,
                                           -search_window_ns,
                                           search_window_ns,
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max);
      hist_a->SetDirectory(nullptr);
      pair.hist_dt_fine_a = std::move(hist_a);
    }
    {
      std::ostringstream title_b;
      title_b << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_b << ", search window " << search_window_ns << " ns);"
              << "#Deltat [ns]; fine (ch " << pair.channel_b << "); pairs";
      std::string name_b =
          "h_dt_fine_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_b";
      auto hist_b = std::make_unique<TH2D>(name_b.c_str(),
                                           title_b.str().c_str(),
                                           bins_search,
                                           -search_window_ns,
                                           search_window_ns,
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max);
      hist_b->SetDirectory(nullptr);
      pair.hist_dt_fine_b = std::move(hist_b);
    }
    {
      std::ostringstream title_a;
      title_a << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_a << ", coinc window " << pair.window_ns << " ns);"
              << "fine (ch " << pair.channel_a << "); #Deltat [ns]; pairs";
      std::string name_a =
          "h_dt_fine_coinc_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_a";
      auto hist_a = std::make_unique<TH2D>(name_a.c_str(),
                                           title_a.str().c_str(),
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max,
                                           bins,
                                           -hist_window_ns,
                                           hist_window_ns);
      hist_a->SetDirectory(nullptr);
      pair.hist_dt_fine_coinc_a = std::move(hist_a);
    }
    {
      std::ostringstream title_a;
      title_a << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_a << ", TDC0, coinc window " << pair.window_ns << " ns);"
              << "fine (ch " << pair.channel_a << "); #Deltat [ns]; pairs";
      std::string name_a =
          "h_dt_fine_coinc_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_a_tdc0";
      auto hist_a = std::make_unique<TH2D>(name_a.c_str(),
                                           title_a.str().c_str(),
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max,
                                           bins,
                                           -hist_window_ns,
                                           hist_window_ns);
      hist_a->SetDirectory(nullptr);
      pair.hist_dt_fine_coinc_a_tdc0 = std::move(hist_a);
    }
    {
      std::ostringstream title_a;
      title_a << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_a << ", TDC2, coinc window " << pair.window_ns << " ns);"
              << "fine (ch " << pair.channel_a << "); #Deltat [ns]; pairs";
      std::string name_a =
          "h_dt_fine_coinc_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_a_tdc2";
      auto hist_a = std::make_unique<TH2D>(name_a.c_str(),
                                           title_a.str().c_str(),
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max,
                                           bins,
                                           -hist_window_ns,
                                           hist_window_ns);
      hist_a->SetDirectory(nullptr);
      pair.hist_dt_fine_coinc_a_tdc2 = std::move(hist_a);
    }
    {
      std::ostringstream title_b;
      title_b << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_b << ", coinc window " << pair.window_ns << " ns);"
              << "fine (ch " << pair.channel_b << "); #Deltat [ns]; pairs";
      std::string name_b =
          "h_dt_fine_coinc_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_b";
      auto hist_b = std::make_unique<TH2D>(name_b.c_str(),
                                           title_b.str().c_str(),
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max,
                                           bins,
                                           -hist_window_ns,
                                           hist_window_ns);
      hist_b->SetDirectory(nullptr);
      pair.hist_dt_fine_coinc_b = std::move(hist_b);
    }
    {
      std::ostringstream title_b;
      title_b << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_b << ", TDC0, coinc window " << pair.window_ns << " ns);"
              << "fine (ch " << pair.channel_b << "); #Deltat [ns]; pairs";
      std::string name_b =
          "h_dt_fine_coinc_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_b_tdc0";
      auto hist_b = std::make_unique<TH2D>(name_b.c_str(),
                                           title_b.str().c_str(),
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max,
                                           bins,
                                           -hist_window_ns,
                                           hist_window_ns);
      hist_b->SetDirectory(nullptr);
      pair.hist_dt_fine_coinc_b_tdc0 = std::move(hist_b);
    }
    {
      std::ostringstream title_b;
      title_b << "ch " << pair.channel_a << " vs " << pair.channel_b
              << " (#Deltat vs fine ch " << pair.channel_b << ", TDC2, coinc window " << pair.window_ns << " ns);"
              << "fine (ch " << pair.channel_b << "); #Deltat [ns]; pairs";
      std::string name_b =
          "h_dt_fine_coinc_ch" + std::to_string(pair.channel_a) + "_ch" + std::to_string(pair.channel_b) + "_b_tdc2";
      auto hist_b = std::make_unique<TH2D>(name_b.c_str(),
                                           title_b.str().c_str(),
                                           fine_plot_bins,
                                           fine_plot_min,
                                           fine_plot_max,
                                           bins,
                                           -hist_window_ns,
                                           hist_window_ns);
      hist_b->SetDirectory(nullptr);
      pair.hist_dt_fine_coinc_b_tdc2 = std::move(hist_b);
    }

    if (max_duration_ns > 0.0) {
      int tot_bins = static_cast<int>(std::ceil(max_duration_ns * 4.0));
      if (tot_bins < 20) {
        tot_bins = 20;
      }
      if (tot_bins > 400) {
        tot_bins = 400;
      }
      for (int tdc = 0; tdc < 4; ++tdc) {
        {
          std::ostringstream title_a;
          title_a << "ch " << pair.channel_a << " vs " << pair.channel_b
                  << " (ToT vs fine ch " << pair.channel_a << ", TDC" << tdc
                  << ", coinc window " << pair.window_ns << " ns);"
                  << "fine (ch " << pair.channel_a << "); ToT [ns]; pairs";
          std::string name_a = "h_tot_fine_ch" + std::to_string(pair.channel_a) + "_ch" +
                               std::to_string(pair.channel_b) + "_a_tdc" + std::to_string(tdc);
          auto hist_a = std::make_unique<TH2D>(name_a.c_str(),
                                               title_a.str().c_str(),
                                               fine_plot_bins,
                                               fine_plot_min,
                                               fine_plot_max,
                                               tot_bins,
                                               0.0,
                                               max_duration_ns);
          hist_a->SetDirectory(nullptr);
          pair.hist_tot_fine_a[tdc] = std::move(hist_a);
        }
        {
          std::ostringstream title_b;
          title_b << "ch " << pair.channel_a << " vs " << pair.channel_b
                  << " (ToT vs fine ch " << pair.channel_b << ", TDC" << tdc
                  << ", coinc window " << pair.window_ns << " ns);"
                  << "fine (ch " << pair.channel_b << "); ToT [ns]; pairs";
          std::string name_b = "h_tot_fine_ch" + std::to_string(pair.channel_a) + "_ch" +
                               std::to_string(pair.channel_b) + "_b_tdc" + std::to_string(tdc);
          auto hist_b = std::make_unique<TH2D>(name_b.c_str(),
                                               title_b.str().c_str(),
                                               fine_plot_bins,
                                               fine_plot_min,
                                               fine_plot_max,
                                               tot_bins,
                                               0.0,
                                               max_duration_ns);
          hist_b->SetDirectory(nullptr);
          pair.hist_tot_fine_b[tdc] = std::move(hist_b);
        }
      }
    }
  }

  InitGroupHists(groups);

  std::unique_ptr<TH1D> pair_mean_hist;
  double pair_mean_window_a = default_window_ns;
  double pair_mean_window_b = default_window_ns;
  if (channels.count(17) && channels.count(19) && channels.count(22) && channels.count(23)) {
    FindPairWindow(pairs, 17, 19, default_window_ns, pair_mean_window_a);
    FindPairWindow(pairs, 22, 23, default_window_ns, pair_mean_window_b);
    double hist_window_ns = std::max(pair_mean_window_a, pair_mean_window_b) * 2.2;
    int bins = BinsForWindow(hist_window_ns);
    std::ostringstream title;
    title << "mean(t17,t19) - mean(t22,t23) (w17-19=" << pair_mean_window_a
          << " ns, w22-23=" << pair_mean_window_b << " ns);"
          << "#Deltat [ns]; entries";
    std::string name = "h_pair_mean_17_19_22_23";
    auto hist = std::make_unique<TH1D>(name.c_str(),
                                       title.str().c_str(),
                                       bins,
                                       -hist_window_ns,
                                       hist_window_ns);
    hist->SetLineWidth(2);
    hist->SetDirectory(nullptr);
    pair_mean_hist = std::move(hist);
  } else {
    std::cout << "Pair-mean plot skipped (needs channels 17,19,22,23 in config)." << std::endl;
  }

  std::vector<int> channel_list(channels.begin(), channels.end());
  std::sort(channel_list.begin(), channel_list.end());
  const auto timewalk_corrections = LoadTimewalkCorrections(timewalk_calib_path, channel_list);
  if (timewalk_calib_path && timewalk_calib_path[0] != '\0' && timewalk_corrections.empty()) {
    std::cout << "Timewalk calibration requested but no correction will be applied." << std::endl;
  }

  std::unordered_map<int, long long> hit_counts;
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

  analysis_time::ChannelTdcOffsetCalib tdc_offset_calib;
  if (fine_calib_path && fine_calib_path[0] != '\0') {
    tdc_offset_calib.LoadFromFile(fine_calib_path);
  }
  analysis_time::PrintChannelTdcOffsetSummary(tdc_offset_calib);

  if (chan_calib_path && chan_calib_path[0] != '\0') {
    if (chan_calib_loaded) {
      std::cout << "Loaded channel calibration: " << chan_calib_path << std::endl;
    } else {
      std::cout << "Failed to load channel calibration: " << chan_calib_path << " (ignored)" << std::endl;
    }
  }
  analysis_time::PrintChannelCalibSummary(chan_calib);
  if (timewalk_calib_path && timewalk_calib_path[0] != '\0') {
    std::cout << "Timewalk calib: " << timewalk_calib_path << std::endl;
  }
  if (max_duration_ns < 0.0) {
    max_duration_ns = 0.0;
  }
  if (min_duration_ns < 0.0) {
    min_duration_ns = 0.0;
  }
  if (chan_calib.loaded && max_duration_ns <= 0.0) {
    std::cout << "Channel calibration loaded but max_duration_ns <= 0; skipping correction." << std::endl;
  }

  if (preview_hits < 0) {
    preview_hits = 0;
  }

  std::vector<CoincPlotSet> pair_plot_sets = InitStreamingCoincidentPlotSets(pairs, max_duration_ns);
  PreviewStore preview(static_cast<size_t>(preview_hits));

  std::vector<TreeCursor> cursors;
  cursors.reserve(input_spec.files.size());
  for (const auto &file : input_spec.files) {
    TreeCursor cursor;
    if (!OpenTreeCursor(file, input_spec.tree_name, cursor)) {
      return;
    }
    cursors.push_back(std::move(cursor));
    BindTreeCursorBranches(cursors.back());
    AdvanceCursor(cursors.back(), channels, fine_calib, tdc_offset_calib, tick_ns, use_fine, fine_cut);
  }

  long long selected_hits = 0;
  long long processed_spills = 0;
  size_t max_spill_hits = 0;
  long long timewalk_corrected_hits = 0;
  while (true) {
    bool found = false;
    uint64_t min_key = std::numeric_limits<uint64_t>::max();
    for (const auto &cursor : cursors) {
      if (!cursor.has_pending) {
        continue;
      }
      const uint64_t key = HitRunSpillKey(cursor.pending);
      if (!found || key < min_key) {
        min_key = key;
        found = true;
      }
    }
    if (!found) {
      break;
    }

    std::vector<Hit> spill_hits;
    for (auto &cursor : cursors) {
      while (cursor.has_pending && HitRunSpillKey(cursor.pending) == min_key) {
        spill_hits.push_back(cursor.pending);
        AdvanceCursor(cursor, channels, fine_calib, tdc_offset_calib, tick_ns, use_fine, fine_cut);
      }
    }
    selected_hits += static_cast<long long>(spill_hits.size());
    max_spill_hits = std::max(max_spill_hits, spill_hits.size());
    ProcessStreamingSpill(spill_hits,
                          pairs,
                          groups,
                          pair_mean_hist.get(),
                          pair_mean_window_a,
                          pair_mean_window_b,
                          pair_plot_sets,
                          hit_counts,
                          fine_calib,
                          tick_ns,
                          use_fine,
                          min_duration_ns,
                          max_duration_ns,
                          per_channel_min_duration_ns,
                          chan_calib,
                          timewalk_corrections,
                          timewalk_corrected_hits,
                          preview);
    ++processed_spills;
  }
  std::cout << "Processed selected hits: " << selected_hits
            << " across " << processed_spills
            << " run/spill blocks (max selected hits in one block: " << max_spill_hits << ")" << std::endl;
  if (!timewalk_corrections.empty()) {
    std::cout << "Applied timewalk correction to " << timewalk_corrected_hits << " leading hits" << std::endl;
  }
  PrintPreviewStore(channel_list, preview);

  std::cout << "Coincidence summary" << std::endl;
  for (const auto &pair : pairs) {
    std::cout << "  ch " << pair.channel_a << " vs " << pair.channel_b
              << " window=" << pair.window_ns << " ns -> " << pair.count << std::endl;
  }
  for (const auto &group : groups) {
    std::ostringstream label;
    for (size_t i = 0; i < group.channels.size(); ++i) {
      if (i > 0) {
        label << ",";
      }
      label << group.channels[i];
    }
    std::cout << "  group ch " << label.str()
              << " window=" << group.window_ns << " ns -> " << group.count << std::endl;
  }

  std::string out_open = std::string(out_pdf) + "[";
  std::string out_close = std::string(out_pdf) + "]";
  TCanvas c_open("c_open", "c_open", 1600, 900);
  c_open.Print(out_open.c_str());

  std::vector<PairStats> pair_stats;
  pair_stats.reserve(pairs.size());
  for (auto &pair : pairs) {
    PairStats stats;
    TCanvas c("c", "c", 1600, 900);
    c.Divide(2, 1, 0.001, 0.001);
    c.cd(1);
    gPad->SetLogy(1);
    pair.hist_search->Draw("hist");
    c.cd(2);
    gPad->SetLogy(1);
    pair.hist_coinc->Draw("E1");
    stats.entries = static_cast<long long>(pair.hist_coinc->GetEntries());
    if (pair.hist_coinc->GetEntries() > 0) {
      const double bkg_sideband = FitBackgroundFromSidebands(*pair.hist_search, pair.window_ns);
      double x_left = 0.0;
      double x_right = 0.0;
      const double fwhm_hist = ComputeFwhmFromHist(*pair.hist_coinc, bkg_sideband, x_left, x_right);
      stats.bkg = bkg_sideband;
      stats.fwhm = fwhm_hist;
      stats.x_left = x_left;
      stats.x_right = x_right;
      {
        TF1 fit_fn("fit_gaus_bkg", "gaus(0)+pol0(3)", -pair.window_ns, pair.window_ns);
        fit_fn.SetParNames("K", "#mu", "#sigma", "bkg");
        const double max_bin = pair.hist_coinc->GetMaximum();
        const double amp_guess = std::max(1.0, max_bin - bkg_sideband);
        const double sigma_guess = std::max(0.05, pair.hist_coinc->GetRMS());
        fit_fn.SetParameters(amp_guess, 0.0, sigma_guess, bkg_sideband);
        fit_fn.SetParLimits(0, 0.0, amp_guess * 10.0);
        fit_fn.SetParLimits(2, 0.05, std::max(0.05, pair.window_ns));
        auto fit_res = pair.hist_coinc->Fit(&fit_fn, "RQ");
        if (fit_res.Get() && fit_res->IsValid()) {
          stats.gaus_sigma = fit_fn.GetParameter(2);
          stats.gaus_sigma_err = fit_fn.GetParError(2);
        }
      }
      const int boot_trials = 200;
      double fwhm_mean = std::numeric_limits<double>::quiet_NaN();
      double fwhm_sigma = std::numeric_limits<double>::quiet_NaN();
      double bkg_mean = std::numeric_limits<double>::quiet_NaN();
      double bkg_sigma = std::numeric_limits<double>::quiet_NaN();
      if (ComputeFwhmBootstrap(*pair.hist_coinc,
                               *pair.hist_search,
                               pair.window_ns,
                               boot_trials,
                               fwhm_mean,
                               fwhm_sigma,
                               bkg_mean,
                               bkg_sigma)) {
        stats.fwhm_err = fwhm_sigma;
        stats.bkg_err = bkg_sigma;
        stats.boot_trials = boot_trials;
      }
      std::cout << "FWHM ch " << pair.channel_a << " vs " << pair.channel_b
                << " window=" << pair.window_ns << " ns:" << std::endl;
      std::cout << "  bkg=" << bkg_sideband << " (sideband pol0)" << std::endl;
      std::cout.setf(std::ios::fixed);
      std::cout << std::setprecision(2);
      if (std::isfinite(fwhm_hist)) {
        std::cout << "  FWHM_(bkg-sub)=" << fwhm_hist;
        if (std::isfinite(stats.fwhm_err)) {
          std::cout << " ± " << stats.fwhm_err;
        }
        std::cout << " ns" << std::endl;
      }
      if (std::isfinite(stats.gaus_sigma)) {
        std::cout << "  sigma_(gaus)=" << stats.gaus_sigma;
        if (std::isfinite(stats.gaus_sigma_err)) {
          std::cout << " ± " << stats.gaus_sigma_err;
        }
        std::cout << " ns" << std::endl;
      } else {
        std::cout << "  sigma_(gaus)=n/a" << std::endl;
      }

      TLatex *latex = new TLatex();
      latex->SetNDC(true);
      latex->SetTextFont(42);
      latex->SetTextSize(0.04);
      if (std::isfinite(fwhm_hist)) {
        std::ostringstream line;
        line << std::fixed << std::setprecision(2) << "FWHM_{(bkg-sub)} = " << fwhm_hist << " ns";
        latex->DrawLatex(0.12, 0.84, line.str().c_str());
      } else {
        latex->DrawLatex(0.12, 0.84, "FWHM_{(bkg-sub)} = n/a");
      }
      {
        std::ostringstream line;
        if (std::isfinite(stats.gaus_sigma)) {
          line << std::fixed << std::setprecision(2) << "#sigma_{gaus} = " << stats.gaus_sigma << " ns";
        } else {
          line << "#sigma_{gaus} = n/a";
        }
        latex->DrawLatex(0.12, 0.78, line.str().c_str());
      }
    } else {
      std::cout << "FWHM skipped (no entries) for ch " << pair.channel_a << " vs " << pair.channel_b << std::endl;
    }
    pair_stats.push_back(stats);
    c.Print(out_pdf);

    const double fine_draw_min = 20.0;
    const double fine_draw_max = 160.0;
    if (pair.hist_dt_fine_a || pair.hist_dt_fine_b) {
      TCanvas c2("c_dt_fine", "c_dt_fine", 1600, 900);
      c2.Divide(2, 1, 0.001, 0.001);
      c2.cd(1);
      gPad->SetLogz(1);
      gPad->SetRightMargin(0.14);
      if (pair.hist_dt_fine_a) {
        pair.hist_dt_fine_a->GetYaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
        pair.hist_dt_fine_a->Draw("COLZ");
      }
      c2.cd(2);
      gPad->SetLogz(1);
      gPad->SetRightMargin(0.14);
      if (pair.hist_dt_fine_b) {
        pair.hist_dt_fine_b->GetYaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
        pair.hist_dt_fine_b->Draw("COLZ");
      }
      c2.Print(out_pdf);
    }

    if (pair.hist_dt_fine_coinc_a_tdc0 || pair.hist_dt_fine_coinc_b_tdc0 ||
        pair.hist_dt_fine_coinc_a_tdc2 || pair.hist_dt_fine_coinc_b_tdc2) {
      TCanvas c3("c_dt_fine_coinc_tdc", "c_dt_fine_coinc_tdc", 1600, 900);
      c3.Divide(2, 2, 0.001, 0.001);

      c3.cd(1);
      gPad->SetLogz(1);
      gPad->SetRightMargin(0.14);
      if (pair.hist_dt_fine_coinc_a_tdc0) {
        pair.hist_dt_fine_coinc_a_tdc0->GetXaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
        pair.hist_dt_fine_coinc_a_tdc0->Draw("COLZ");
      }

      c3.cd(2);
      gPad->SetLogz(1);
      gPad->SetRightMargin(0.14);
      if (pair.hist_dt_fine_coinc_b_tdc0) {
        pair.hist_dt_fine_coinc_b_tdc0->GetXaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
        pair.hist_dt_fine_coinc_b_tdc0->Draw("COLZ");
      }

      c3.cd(3);
      gPad->SetLogz(1);
      gPad->SetRightMargin(0.14);
      if (pair.hist_dt_fine_coinc_a_tdc2) {
        pair.hist_dt_fine_coinc_a_tdc2->GetXaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
        pair.hist_dt_fine_coinc_a_tdc2->Draw("COLZ");
      }

      c3.cd(4);
      gPad->SetLogz(1);
      gPad->SetRightMargin(0.14);
      if (pair.hist_dt_fine_coinc_b_tdc2) {
        pair.hist_dt_fine_coinc_b_tdc2->GetXaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
        pair.hist_dt_fine_coinc_b_tdc2->Draw("COLZ");
      }

      c3.Print(out_pdf);
    }

    bool has_tot_fine_a = false;
    bool has_tot_fine_b = false;
    for (const auto &h : pair.hist_tot_fine_a) {
      if (h) {
        has_tot_fine_a = true;
        break;
      }
    }
    for (const auto &h : pair.hist_tot_fine_b) {
      if (h) {
        has_tot_fine_b = true;
        break;
      }
    }
    if (has_tot_fine_a) {
      TCanvas c_tot_a("c_tot_fine_a", "c_tot_fine_a", 1600, 900);
      c_tot_a.Divide(2, 2, 0.001, 0.001);
      for (int tdc = 0; tdc < 4; ++tdc) {
        c_tot_a.cd(tdc + 1);
        gPad->SetLogz(1);
        gPad->SetRightMargin(0.14);
        auto &hist = pair.hist_tot_fine_a[tdc];
        if (hist) {
          hist->GetXaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
          hist->Draw("COLZ");
        }
      }
      c_tot_a.Print(out_pdf);
    }
    if (has_tot_fine_b) {
      TCanvas c_tot_b("c_tot_fine_b", "c_tot_fine_b", 1600, 900);
      c_tot_b.Divide(2, 2, 0.001, 0.001);
      for (int tdc = 0; tdc < 4; ++tdc) {
        c_tot_b.cd(tdc + 1);
        gPad->SetLogz(1);
        gPad->SetRightMargin(0.14);
        auto &hist = pair.hist_tot_fine_b[tdc];
        if (hist) {
          hist->GetXaxis()->SetRangeUser(fine_draw_min, fine_draw_max);
          hist->Draw("COLZ");
        }
      }
      c_tot_b.Print(out_pdf);
    }
  }
  if (pair_mean_hist) {
    TCanvas c_mean("c_pair_mean", "c_pair_mean", 1600, 900);
    gPad->SetLogy(1);
    pair_mean_hist->Draw("hist");
    c_mean.Print(out_pdf);
  }

  PlotGroupCoincidencePlots(groups, out_pdf);
  DrawCoincidentStacksPerPair(pair_plot_sets, out_pdf);
  DrawCoincidentStacksPerVariable(pair_plot_sets, out_pdf);

  WriteCoincidenceRoot(out_root,
                       pairs,
                       groups,
                       pair_mean_hist.get(),
                       pair_plot_sets,
                       clock_mhz,
                       use_fine,
                       min_duration_ns,
                       max_duration_ns,
                       fine_cut,
                       pairs_file,
                       per_channel_min_duration_ns);

  WriteCoincidenceTxt(out_txt,
                      pairs,
                      pair_stats,
                      clock_mhz,
                      use_fine,
                      min_duration_ns,
                      max_duration_ns,
                      fine_cut,
                      pairs_file,
                      per_channel_min_duration_ns);

  c_open.Print(out_close.c_str());
}
