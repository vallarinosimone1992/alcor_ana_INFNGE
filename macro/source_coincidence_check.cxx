// Standalone diagnostic: apply pre-fit sensor timewalk corrections (derived
// from the SR-mode laser scan, via run_timewalk_calibration.sh) directly to
// two-channel radioactive-source runs (ch17/ch19, no trigger channel), and
// check how well the resulting ch17-ch19 coincidence centers at zero, across
// the full source LE2DAC scan.
//
// Reuses the raw-hit reconstruction machinery (Hit, OpenTreeCursor,
// AdvanceCursor, ComputeTot, analysis_time helpers) from
// laser_intensity_scan_rdf.cxx by including it directly.
#include "laser_intensity_scan_rdf.cxx"

struct SimpleCorrection {
  double p0 = 0.0;
  double p1 = 0.0;
  double x0 = 0.0;
  bool enabled = false;

  double CorrectionNs(double tot) const
  {
    if (!enabled || !std::isfinite(tot)) {
      return 0.0;
    }
    return p0 + p1 * std::min(tot, x0);
  }
};

std::vector<Hit> LoadHits(const std::string &run_dir, const std::vector<int> &channels,
                           const analysis_time::FineCalib &fine_calib,
                           const analysis_time::ChannelTdcOffsetCalib &tdc_offset_calib,
                           double tick_ns, bool use_fine, int fine_cut)
{
  auto input = analysis_io::ResolveInputSpec(run_dir);
  std::vector<Hit> hits;
  if (input.files.empty()) {
    std::cerr << "No decoded ROOT input found for " << run_dir << std::endl;
    return hits;
  }

  std::unordered_set<int> selected_channels(channels.begin(), channels.end());
  std::unordered_set<int> no_edge_channels;
  TdcSelection tdc_selection;
  SpillRange spill_range;

  for (const auto &file : input.files) {
    TreeCursor cursor;
    if (!OpenTreeCursor(file, input.tree_name, cursor)) {
      continue;
    }
    while (AdvanceCursor(cursor, selected_channels, no_edge_channels, tdc_selection, fine_calib, tdc_offset_calib,
                          tick_ns, use_fine, fine_cut, spill_range)) {
      hits.push_back(cursor.pending);
    }
  }
  ComputeTot(hits, 30.0);
  return hits;
}

struct PeakResult {
  double centroid = 0.0;
  double centroid_err = 0.0;
  double sigma = 0.0;
  double total_excess = 0.0;
  double pedestal_per_bin = 0.0;
};

PeakResult AnalyzePeak(const TH1D &h)
{
  PeakResult r;
  const int nbins = h.GetNbinsX();
  const double wing_lo = h.Integral(1, nbins / 4);
  const double wing_hi = h.Integral(3 * nbins / 4 + 1, nbins);
  r.pedestal_per_bin = (wing_lo + wing_hi) / (nbins / 2);
  double excess_sum = 0.0, excess_weighted = 0.0, excess_weighted_sq = 0.0;
  for (int i = 1; i <= nbins; ++i) {
    const double excess = h.GetBinContent(i) - r.pedestal_per_bin;
    if (excess > 0) {
      const double x = h.GetBinCenter(i);
      excess_sum += excess;
      excess_weighted += excess * x;
      excess_weighted_sq += excess * x * x;
    }
  }
  r.total_excess = excess_sum;
  if (excess_sum > 0) {
    r.centroid = excess_weighted / excess_sum;
    const double variance = excess_weighted_sq / excess_sum - r.centroid * r.centroid;
    r.sigma = std::sqrt(std::max(variance, 0.0));
    r.centroid_err = r.sigma / std::sqrt(excess_sum);
  }
  return r;
}

void source_coincidence_check()
{
  const std::string fine_calib_path = "calibration/TDC_calibration.root";
  const double tick_ns = 1000.0 / 320.0;
  const bool use_fine = true;
  const int fine_cut = 0;
  const double match_window_ns = 30.0;

  analysis_time::FineCalib fine_calib;
  if (!fine_calib.LoadFromFile(fine_calib_path.c_str())) {
    std::cerr << "Warning: fine calibration not loaded: " << fine_calib_path << std::endl;
  }
  fine_calib.use_lut = true;
  analysis_time::ChannelTdcOffsetCalib tdc_offset_calib;
  tdc_offset_calib.LoadFromFile(fine_calib_path.c_str());

  // none = no correction; signed = full-population signed-dt baseline
  // (output/sr_baseline_signed_dt.txt); signed_g1/g2/g3 = signed-dt fits
  // restricted to "good" (non-echo) trigger-ToT clusters identified from the
  // window scan: 13.5:14.5, 17:18, 20:21 (output/sr_signed_good_*.txt)
  SimpleCorrection corr17_none, corr19_none;
  SimpleCorrection corr17_signed{6.80184, -0.170113, 13.7268, true};
  SimpleCorrection corr19_signed{5.08192, 1.24037, 1.72489, true};
  SimpleCorrection corr17_g1{4.54679, 0.0528011, 13.7268, true};
  SimpleCorrection corr19_g1{4.04957, 0.342464, 13.7268, true};
  SimpleCorrection corr17_g2{4.28466, 0.29025, 13.7268, true};
  SimpleCorrection corr19_g2{6.63739, 1.86503, 1.12493, true};
  SimpleCorrection corr17_g3{7.96745, -0.152126, 13.7268, true};
  SimpleCorrection corr19_g3{8.42175, 0.0864365, 13.7268, true};

  struct Run {
    std::string dir;
    std::string label;
  };
  std::vector<Run> runs = {
      {"../data/20260623-101039", "LE2DAC 10"},
      {"../data/20260623-102647", "LE2DAC 25"},
      {"../data/20260623-104452", "LE2DAC 40"},
      {"../data/20260623-110204", "LE2DAC 55"},
      {"../data/20260623-111846", "LE2DAC 63"},
      {"../data/20260623-163850", "LE2DAC 25 long"},
  };

  TCanvas c("c_source_coinc", "c_source_coinc", 2400, 900);
  c.Print("output/source_coincidence_check.pdf[");

  std::cout << std::left << std::setw(12) << "LE2DAC" << std::setw(22) << "corr" << std::setw(12) << "centroid_ns"
            << std::setw(10) << "err_ns" << std::setw(10) << "sigma_ns" << std::setw(14) << "excess"
            << std::setw(12) << "mean_tot17" << std::setw(12) << "mean_tot19" << std::endl;

  for (const auto &run : runs) {
    std::vector<Hit> hits = LoadHits(run.dir, {17, 19}, fine_calib, tdc_offset_calib, tick_ns, use_fine, fine_cut);

    std::vector<Hit> ch17, ch19;
    for (const auto &h : hits) {
      if (!h.leading || h.tot_ns < 0.0) {
        continue;
      }
      if (h.channel == 17) {
        ch17.push_back(h);
      } else if (h.channel == 19) {
        ch19.push_back(h);
      }
    }
    std::sort(ch17.begin(), ch17.end(), [](const Hit &a, const Hit &b) { return a.time_ns < b.time_ns; });
    std::sort(ch19.begin(), ch19.end(), [](const Hit &a, const Hit &b) { return a.time_ns < b.time_ns; });

    double sum17 = 0.0, sum19 = 0.0;
    for (const auto &h : ch17) sum17 += h.tot_ns;
    for (const auto &h : ch19) sum19 += h.tot_ns;
    const double mean_tot17 = ch17.empty() ? 0.0 : sum17 / ch17.size();
    const double mean_tot19 = ch19.empty() ? 0.0 : sum19 / ch19.size();

    // TAC-style: fill ALL ch19 hits within the window for every ch17 hit (not
    // just the nearest one), so a real narrow coincidence peak can be told
    // apart from the flat accidental pedestal.
    auto buildHist = [&](const char *name, const SimpleCorrection &c17, const SimpleCorrection &c19) {
      auto h = std::make_unique<TH1D>(name, name, 400, -match_window_ns, match_window_ns);
      size_t j_start = 0;
      for (const auto &h17 : ch17) {
        const double t17 = h17.time_ns - c17.CorrectionNs(h17.tot_ns);
        while (j_start < ch19.size() && ch19[j_start].time_ns < h17.time_ns - match_window_ns) {
          ++j_start;
        }
        for (size_t j = j_start; j < ch19.size(); ++j) {
          const auto &h19 = ch19[j];
          if (h19.time_ns > h17.time_ns + match_window_ns) {
            break;
          }
          const double t19 = h19.time_ns - c19.CorrectionNs(h19.tot_ns);
          h->Fill(t19 - t17);
        }
      }
      return h;
    };

    auto h_none = buildHist((run.label + "_none").c_str(), corr17_none, corr19_none);
    auto h_signed = buildHist((run.label + "_signed").c_str(), corr17_signed, corr19_signed);
    auto h_g1 = buildHist((run.label + "_g1").c_str(), corr17_g1, corr19_g1);
    auto h_g2 = buildHist((run.label + "_g2").c_str(), corr17_g2, corr19_g2);
    auto h_g3 = buildHist((run.label + "_g3").c_str(), corr17_g3, corr19_g3);

    c.Clear();
    c.Divide(5, 1);
    int pad = 1;
    std::vector<std::pair<TH1D *, const char *>> entries = {
        {h_none.get(), "none"},      {h_signed.get(), "signed-full"}, {h_g1.get(), "signed-good-13.5_14.5"},
        {h_g2.get(), "signed-good-17_18"}, {h_g3.get(), "signed-good-20_21"},
    };
    for (auto &[hp, tag] : entries) {
      const auto result = AnalyzePeak(*hp);
      std::cout << std::left << std::setw(12) << run.label << std::setw(22) << tag << std::setw(12)
                << result.centroid << std::setw(10) << result.centroid_err << std::setw(10) << result.sigma
                << std::setw(14) << result.total_excess << std::setw(12) << mean_tot17 << std::setw(12)
                << mean_tot19 << std::endl;
      c.cd(pad++);
      hp->SetTitle((run.label + " [" + std::string(tag) + "];dt [ns];entries").c_str());
      hp->Draw("hist");
    }
    c.Print("output/source_coincidence_check.pdf");
  }

  c.Print("output/source_coincidence_check.pdf]");
  std::cout << "wrote output/source_coincidence_check.pdf" << std::endl;
}
