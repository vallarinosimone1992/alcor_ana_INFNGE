#ifndef ANALYSIS_TIME_H
#define ANALYSIS_TIME_H

#include <Rtypes.h>
#include <TFile.h>
#include <TH1.h>
#include <TH2.h>

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace analysis_time {
constexpr int kRolloverToCoarse = 32768;
constexpr int kFineBins = 512;
constexpr int kColumnsPerChip = 8;
constexpr int kPixelsPerColumn = 4;
constexpr int kTdcPerPixel = 4;
constexpr int kTdcPerFifo = kColumnsPerChip * kPixelsPerColumn * kTdcPerPixel;
constexpr int kFineCalibSize = 24 * kTdcPerFifo;
constexpr double kDefaultFineMin = 37.0;
constexpr double kDefaultFineMax = 101.0;
constexpr int kDefaultFineCut = 0;

inline double TickNs(double clock_mhz)
{
  return 1000.0 / clock_mhz;
}

inline Long64_t TimeTick(int rollover, int coarse)
{
  return static_cast<Long64_t>(rollover) * kRolloverToCoarse + static_cast<Long64_t>(coarse);
}

struct FineCalib {
  bool loaded = false;
  bool lut_loaded = false;
  bool use_lut = true;
  std::array<double, kFineCalibSize> cut{};
  std::array<double, kFineCalibSize> min{};
  std::array<double, kFineCalibSize> inv_range{};
  std::array<float, kFineCalibSize * kFineBins> lut{};

  FineCalib()
  {
    FillDefault();
  }

  void FillDefault()
  {
    const double cut_val = 0.5 * (kDefaultFineMin + kDefaultFineMax);
    for (int i = 0; i < kFineCalibSize; ++i) {
      cut[i] = cut_val;
      min[i] = kDefaultFineMin;
      inv_range[i] = 0.0;
    }
    lut_loaded = false;
    use_lut = true;
    loaded = false;
  }

  bool LoadFromFile(const std::string &path)
  {
    if (path.empty()) {
      return false;
    }
    FillDefault();
    std::unique_ptr<TFile> file(TFile::Open(path.c_str(), "READ"));
    if (!file || file->IsZombie()) {
      return false;
    }
    auto *hmin = dynamic_cast<TH1 *>(file->Get("hFineMin"));
    auto *hmax = dynamic_cast<TH1 *>(file->Get("hFineMax"));
    auto *hlut = dynamic_cast<TH2 *>(file->Get("hFineLut"));
    if (!hmin || !hmax) {
      return false;
    }

    bool any_valid = false;
    bool any_lut = false;
    for (int i = 0; i < kFineCalibSize; ++i) {
      double min_val = hmin->GetBinContent(i + 1);
      double max_val = hmax->GetBinContent(i + 1);
      if (min_val <= 0.0 || max_val <= 0.0 || max_val <= min_val) {
        inv_range[i] = 0.0;
        continue;
      }
      cut[i] = 0.5 * (min_val + max_val);
      min[i] = min_val;
      inv_range[i] = 1.0 / (max_val - min_val);
      any_valid = true;
      if (hlut) {
        for (int b = 0; b < kFineBins; ++b) {
          lut[i * kFineBins + b] = static_cast<float>(hlut->GetBinContent(i + 1, b + 1));
        }
        any_lut = true;
      }
    }
    loaded = any_valid;
    lut_loaded = any_lut;
    return loaded;
  }
};

struct ChannelCalib {
  struct ChannelHist {
    int bins = 0;
    double xmin = 0.0;
    double xmax = 0.0;
    double binw = 0.0;
    std::vector<double> values;
    bool valid = false;
  };

  bool loaded = false;
  std::array<ChannelHist, 32> channels{};
  std::array<double, 32> offsets{};

  bool LoadFromFile(const std::string &path)
  {
    if (path.empty()) {
      return false;
    }
    loaded = false;
    for (auto &ch : channels) {
      ch = ChannelHist{};
    }
    for (auto &off : offsets) {
      off = 0.0;
    }
    std::unique_ptr<TFile> file(TFile::Open(path.c_str(), "READ"));
    if (!file || file->IsZombie()) {
      return false;
    }
    auto *hoff = dynamic_cast<TH1 *>(file->Get("hChanOffset"));
    if (hoff) {
      const int bins = hoff->GetNbinsX();
      for (int ch = 0; ch < static_cast<int>(offsets.size()) && ch < bins; ++ch) {
        offsets[ch] = hoff->GetBinContent(ch + 1);
      }
    }
    for (int ch = 0; ch < static_cast<int>(channels.size()); ++ch) {
      std::string name = "hChanCalib_ch" + std::to_string(ch);
      auto *hist = dynamic_cast<TH1 *>(file->Get(name.c_str()));
      if (!hist) {
        continue;
      }
      const int bins = hist->GetNbinsX();
      if (bins < 1) {
        continue;
      }
      ChannelHist &dst = channels[ch];
      dst.bins = bins;
      dst.xmin = hist->GetXaxis()->GetXmin();
      dst.xmax = hist->GetXaxis()->GetXmax();
      dst.binw = (dst.xmax - dst.xmin) / static_cast<double>(bins);
      dst.values.resize(bins);
      for (int b = 1; b <= bins; ++b) {
        dst.values[b - 1] = hist->GetBinContent(b);
      }
      dst.valid = true;
      loaded = true;
    }
    return loaded;
  }

  double CorrectionNs(int channel, double tot) const
  {
    if (!loaded || channel < 0 || channel >= static_cast<int>(channels.size())) {
      return 0.0;
    }
    double offset = offsets[channel];
    const ChannelHist &ch = channels[channel];
    if (!ch.valid || ch.bins < 1) {
      return offset;
    }
    if (tot <= ch.xmin) {
      return ch.values.front() + offset;
    }
    if (tot >= ch.xmax) {
      return ch.values.back() + offset;
    }
    int bin = static_cast<int>((tot - ch.xmin) / ch.binw);
    if (bin < 0) {
      bin = 0;
    }
    if (bin >= ch.bins) {
      bin = ch.bins - 1;
    }
    return ch.values[bin] + offset;
  }
};

inline void PrintFineCalibConstants(const FineCalib &calib)
{
  if (!calib.loaded) {
    const double cut = 0.5 * (kDefaultFineMin + kDefaultFineMax);
    std::cout << "Fine calibration constants (default): min=" << kDefaultFineMin << " max=" << kDefaultFineMax
              << " cut=" << cut << std::endl;
    return;
  }
  int valid = 0;
  for (int i = 0; i < kFineCalibSize; ++i) {
    if (calib.inv_range[i] <= 0.0) {
      continue;
    }
    const double min_val = calib.min[i];
    const double max_val = min_val + 1.0 / calib.inv_range[i];
    std::cout << "Fine calib idx " << i << " min=" << min_val << " max=" << max_val << " cut=" << calib.cut[i]
              << std::endl;
    ++valid;
  }
  std::cout << "Fine calibration constants printed for " << valid << " / " << kFineCalibSize
            << " TDCs (others use default)" << std::endl;
  if (calib.lut_loaded && calib.use_lut) {
    std::cout << "Fine LUT loaded: using CDF-based mapping" << std::endl;
  } else if (calib.lut_loaded && !calib.use_lut) {
    std::cout << "Fine LUT loaded but disabled: using linear min/max mapping" << std::endl;
  } else {
    std::cout << "Fine LUT not found: using linear min/max mapping" << std::endl;
  }
}

inline void PrintChannelCalibSummary(const ChannelCalib &calib)
{
  if (!calib.loaded) {
    std::cout << "Channel calibration: (none)" << std::endl;
    return;
  }
  for (int ch = 0; ch < static_cast<int>(calib.channels.size()); ++ch) {
    const auto &entry = calib.channels[ch];
    if (!entry.valid || entry.bins < 1) {
      continue;
    }
    double first = entry.values.empty() ? 0.0 : entry.values.front();
    double last = entry.values.empty() ? 0.0 : entry.values.back();
    std::cout << "Channel calib ch " << ch << " bins=" << entry.bins << " range=[" << entry.xmin << ", "
              << entry.xmax << "] first=" << first << " last=" << last << " offset=" << calib.offsets[ch]
              << std::endl;
  }
}

inline int TdcIndex(int fifo, int column, int pixel, int tdc)
{
  if (fifo < 0 || fifo >= 24) {
    return -1;
  }
  if (column < 0 || pixel < 0 || tdc < 0) {
    return -1;
  }
  if (column >= kColumnsPerChip || pixel >= kPixelsPerColumn || tdc >= kTdcPerPixel) {
    return -1;
  }
  int idx = tdc + kTdcPerPixel * pixel + (kTdcPerPixel * kPixelsPerColumn) * column + kTdcPerFifo * fifo;
  if (idx < 0 || idx >= kFineCalibSize) {
    return -1;
  }
  return idx;
}

inline double FineFractionDefault(int fine_raw)
{
  if (fine_raw < 0) {
    return 0.0;
  }
  const double cut_val = 0.5 * (kDefaultFineMin + kDefaultFineMax);
  const double range = kDefaultFineMax - kDefaultFineMin;
  if (range <= 0.0) {
    return 0.0;
  }
  double fine = (static_cast<double>(fine_raw) - kDefaultFineMin) / range;
  if (static_cast<double>(fine_raw) > cut_val) {
    fine -= 1.0;
  }
  return fine;
}

inline double FineFraction(const FineCalib &calib, int fine_raw, int tdc_index)
{
  if (fine_raw < 0) {
    return 0.0;
  }
  if (tdc_index < 0 || tdc_index >= kFineCalibSize) {
    return FineFractionDefault(fine_raw);
  }
  if (calib.use_lut && calib.lut_loaded && calib.inv_range[tdc_index] > 0.0 && fine_raw < kFineBins) {
    return static_cast<double>(calib.lut[tdc_index * kFineBins + fine_raw]);
  }
  const double inv = calib.inv_range[tdc_index];
  if (inv <= 0.0) {
    return FineFractionDefault(fine_raw);
  }
  double fine = (static_cast<double>(fine_raw) - calib.min[tdc_index]) * inv;
  if (static_cast<double>(fine_raw) > calib.cut[tdc_index]) {
    fine -= 1.0;
  }
  return fine;
}

inline double FineCutValue(const FineCalib &calib, int tdc_index)
{
  if (tdc_index >= 0 && tdc_index < kFineCalibSize) {
    if (calib.inv_range[tdc_index] > 0.0) {
      return calib.cut[tdc_index];
    }
  }
  return 0.5 * (kDefaultFineMin + kDefaultFineMax);
}

inline bool PassFineCut(const FineCalib &calib, int fine_raw, int tdc_index, int fine_cut)
{
  if (fine_cut <= 0) {
    return true;
  }
  if (fine_raw < 0) {
    return true;
  }
  const double cut = FineCutValue(calib, tdc_index);
  return std::abs(static_cast<double>(fine_raw) - cut) > static_cast<double>(fine_cut);
}

inline double TimeNsFromTick(Long64_t time_tick, int fine, double tick_ns, bool use_fine)
{
  double time = static_cast<double>(time_tick);
  if (use_fine) {
    time -= FineFractionDefault(fine);
  }
  return time * tick_ns;
}

inline double TimeNsFromFields(int rollover, int coarse, int fine, double tick_ns, bool use_fine)
{
  return TimeNsFromTick(TimeTick(rollover, coarse), fine, tick_ns, use_fine);
}

inline double TimeNsFromTick(const FineCalib &calib,
                             Long64_t time_tick,
                             int fine_raw,
                             int tdc_index,
                             double tick_ns,
                             bool use_fine)
{
  double time = static_cast<double>(time_tick);
  if (use_fine) {
    time -= FineFraction(calib, fine_raw, tdc_index);
  }
  return time * tick_ns;
}

inline double TimeNsFromFields(const FineCalib &calib,
                               int rollover,
                               int coarse,
                               int fine_raw,
                               int fifo,
                               int column,
                               int pixel,
                               int tdc,
                               double tick_ns,
                               bool use_fine)
{
  const Long64_t time_tick = TimeTick(rollover, coarse);
  const int tdc_index = TdcIndex(fifo, column, pixel, tdc);
  return TimeNsFromTick(calib, time_tick, fine_raw, tdc_index, tick_ns, use_fine);
}

inline auto TimeTickLambda()
{
  return [](int rollover, int coarse) -> Long64_t {
    return static_cast<Long64_t>(rollover) * kRolloverToCoarse + static_cast<Long64_t>(coarse);
  };
}

inline auto TimeNsFromTickLambda(double tick_ns, bool use_fine)
{
  return [tick_ns, use_fine](Long64_t time_tick, int fine) -> double {
    double time = static_cast<double>(time_tick);
    if (use_fine) {
      time -= FineFractionDefault(fine);
    }
    return time * tick_ns;
  };
}

inline auto TimeNsFromFieldsLambda(double tick_ns, bool use_fine)
{
  return [tick_ns, use_fine](int rollover, int coarse, int fine) -> double {
    Long64_t time_tick = static_cast<Long64_t>(rollover) * kRolloverToCoarse + static_cast<Long64_t>(coarse);
    double time = static_cast<double>(time_tick);
    if (use_fine) {
      time -= FineFractionDefault(fine);
    }
    return time * tick_ns;
  };
}

inline auto TimeNsFromFieldsLambda(const FineCalib &calib, double tick_ns, bool use_fine)
{
  return [calib, tick_ns, use_fine](int rollover,
                                    int coarse,
                                    int fine_raw,
                                    int fifo,
                                    int column,
                                    int pixel,
                                    int tdc) -> double {
    const Long64_t time_tick = static_cast<Long64_t>(rollover) * kRolloverToCoarse + static_cast<Long64_t>(coarse);
    const int tdc_index = TdcIndex(fifo, column, pixel, tdc);
    double time = static_cast<double>(time_tick);
    if (use_fine) {
      time -= FineFraction(calib, fine_raw, tdc_index);
    }
    return time * tick_ns;
  };
}
}  // namespace analysis_time

#endif
