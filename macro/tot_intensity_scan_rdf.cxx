#include <TCanvas.h>
#include <TChain.h>
#include <TFile.h>
#include <TGraphErrors.h>
#include <TH1D.h>
#include <TH2D.h>
#include <TLegend.h>
#include <TNamed.h>
#include <TParameter.h>
#include <TStyle.h>

#include "analysis_io.h"
#include "analysis_time.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
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

void PrintHelp()
{
  std::cout << "tot_intensity_scan_rdf usage:\n";
  std::cout << "  tot_intensity_scan_rdf(\"runlist.tsv\", \"TDC_calibration.root\", \"out.pdf\", \"timewalk_correction.root\", \"out.txt\", 30, 320, true, true, 0)\n";
  std::cout << "Runlist TSV columns: label input_path laser_intensity channels\n";
  std::cout << "This macro characterizes ToT versus laser intensity. It does not extract an absolute timewalk correction without a timing reference.\n";
}

std::vector<std::string> SplitTabs(const std::string &line)
{
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, '\t')) {
    out.push_back(analysis_io::Trim(item));
  }
  return out;
}

std::vector<int> ParseChannels(std::string value)
{
  for (char &ch : value) {
    if (ch == ',' || ch == ';' || ch == '_') {
      ch = ' ';
    }
  }
  std::stringstream ss(value);
  std::vector<int> channels;
  int ch = -1;
  while (ss >> ch) {
    if (ch >= 0 && ch < 32) {
      channels.push_back(ch);
    }
  }
  std::sort(channels.begin(), channels.end());
  channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
  return channels;
}

std::string Sanitize(std::string value)
{
  for (char &ch : value) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
      ch = '_';
    }
  }
  return value;
}

struct RunEntry {
  std::string label;
  std::string input;
  double intensity = 0.0;
  std::vector<int> channels;
};

std::vector<RunEntry> ReadRunlist(const std::string &path)
{
  std::vector<RunEntry> entries;
  std::ifstream fin(path);
  if (!fin) {
    std::cout << "Cannot open runlist: " << path << std::endl;
    return entries;
  }
  std::string line;
  while (std::getline(fin, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    line = analysis_io::Trim(line);
    if (line.empty()) {
      continue;
    }
    auto fields = SplitTabs(line);
    if (fields.size() < 4) {
      std::cout << "Skipping malformed runlist line: " << line << std::endl;
      continue;
    }
    RunEntry entry;
    entry.label = fields[0];
    entry.input = fields[1];
    entry.intensity = std::atof(fields[2].c_str());
    entry.channels = ParseChannels(fields[3]);
    if (entry.label.empty() || entry.input.empty() || entry.channels.empty()) {
      std::cout << "Skipping incomplete runlist line: " << line << std::endl;
      continue;
    }
    entries.push_back(entry);
  }
  return entries;
}

struct Hit {
  int channel = -1;
  int tdc = -1;
  int spill = 0;
  double time_ns = 0.0;
};

bool IsLeading(int tdc)
{
  return (tdc & 1) == 0;
}

bool IsTrailing(int tdc)
{
  return (tdc & 1) == 1;
}

std::vector<double> ExtractTot(const RunEntry &entry,
                               int channel,
                               const analysis_time::FineCalib &fine_calib,
                               double max_duration_ns,
                               double clock_mhz,
                               bool use_fine,
                               bool use_lut,
                               int fine_cut)
{
  analysis_io::InputSpec spec = analysis_io::ResolveInputSpec(entry.input);
  if (spec.files.empty()) {
    std::cout << "No decoded ROOT files found for " << entry.input << std::endl;
    return {};
  }

  TChain chain(spec.tree_name.c_str());
  for (const auto &file : spec.files) {
    chain.Add(file.c_str());
  }

  int type = 0;
  int fifo = 0;
  int column = 0;
  int pixel = 0;
  int tdc = 0;
  int rollover = 0;
  int coarse = 0;
  int fine = 0;
  int spill = 0;
  chain.SetBranchAddress("type", &type);
  chain.SetBranchAddress("fifo", &fifo);
  chain.SetBranchAddress("column", &column);
  chain.SetBranchAddress("pixel", &pixel);
  chain.SetBranchAddress("tdc", &tdc);
  chain.SetBranchAddress("rollover", &rollover);
  chain.SetBranchAddress("coarse", &coarse);
  chain.SetBranchAddress("fine", &fine);
  const bool has_spill = chain.GetBranch("spill") != nullptr;
  if (has_spill) {
    chain.SetBranchAddress("spill", &spill);
  }

  std::vector<Hit> hits;
  const double tick_ns = analysis_time::TickNs(clock_mhz);
  for (Long64_t i = 0, n = chain.GetEntries(); i < n; ++i) {
    chain.GetEntry(i);
    if (type != 1) {
      continue;
    }
    const int ch = column * 4 + pixel;
    if (ch != channel || tdc < 0 || tdc > 3) {
      continue;
    }
    const int tdc_index = analysis_time::TdcIndex(fifo, column, pixel, tdc);
    if (!analysis_time::PassFineCut(fine_calib, fine, tdc_index, fine_cut)) {
      continue;
    }
    Hit hit;
    hit.channel = ch;
    hit.tdc = tdc;
    hit.spill = has_spill ? spill : 0;
    hit.time_ns = analysis_time::TimeNsFromFields(fine_calib,
                                                  rollover,
                                                  coarse,
                                                  fine,
                                                  fifo,
                                                  column,
                                                  pixel,
                                                  tdc,
                                                  tick_ns,
                                                  use_fine);
    hits.push_back(hit);
  }

  std::sort(hits.begin(), hits.end(), [](const Hit &lhs, const Hit &rhs) {
    if (lhs.spill != rhs.spill) {
      return lhs.spill < rhs.spill;
    }
    return lhs.time_ns < rhs.time_ns;
  });

  std::vector<double> tots;
  std::vector<char> used(hits.size(), 0);
  for (size_t i = 0; i < hits.size(); ++i) {
    if (!IsLeading(hits[i].tdc)) {
      continue;
    }
    const int trailing_tdc = hits[i].tdc ^ 1;
    for (size_t j = i + 1; j < hits.size(); ++j) {
      if (hits[j].spill != hits[i].spill) {
        break;
      }
      const double dt = hits[j].time_ns - hits[i].time_ns;
      if (dt <= 0.0) {
        continue;
      }
      if (max_duration_ns > 0.0 && dt > max_duration_ns) {
        break;
      }
      if (used[j] || !IsTrailing(hits[j].tdc) || hits[j].tdc != trailing_tdc) {
        continue;
      }
      used[j] = 1;
      tots.push_back(dt);
      break;
    }
  }
  return tots;
}
}  // namespace

void tot_intensity_scan_rdf(const char *runlist_path = "runlist.tsv",
                            const char *tdc_calib_path = "TDC_calibration.root",
                            const char *out_pdf = "tot_intensity_scan.pdf",
                            const char *out_root = "timewalk_correction.root",
                            const char *out_txt = "tot_intensity_scan.txt",
                            double max_duration_ns = 30.0,
                            double clock_mhz = 320.0,
                            bool use_fine = true,
                            bool use_lut = true,
                            int fine_cut = 0)
{
  if (WantsHelp(runlist_path)) {
    PrintHelp();
    return;
  }

  auto entries = ReadRunlist(runlist_path ? runlist_path : "");
  if (entries.empty()) {
    std::cout << "No valid runlist entries." << std::endl;
    return;
  }

  analysis_time::FineCalib fine_calib;
  fine_calib.use_lut = use_lut;
  if (tdc_calib_path && tdc_calib_path[0] != '\0') {
    fine_calib.LoadFromFile(tdc_calib_path);
    fine_calib.use_lut = use_lut;
  }

  std::set<int> all_channels;
  for (const auto &entry : entries) {
    all_channels.insert(entry.channels.begin(), entry.channels.end());
  }

  std::unique_ptr<TFile> fout;
  if (out_root && out_root[0] != '\0') {
    fout.reset(TFile::Open(out_root, "RECREATE"));
  }
  std::ofstream txt;
  if (out_txt && out_txt[0] != '\0') {
    txt.open(out_txt);
  }

  if (txt) {
    txt << "# mode\tchannel\tlabel\tintensity\tentries\tmean_tot_ns\trms_tot_ns\n";
  }

  std::map<int, std::vector<double>> x_by_ch;
  std::map<int, std::vector<double>> y_by_ch;
  std::map<int, std::vector<double>> ey_by_ch;
  std::map<int, std::unique_ptr<TH2D>> h2_by_ch;
  std::map<std::pair<int, std::string>, std::unique_ptr<TH1D>> h1_by_ch_run;

  for (const auto &entry : entries) {
    for (int ch : entry.channels) {
      const auto tots = ExtractTot(entry, ch, fine_calib, max_duration_ns, clock_mhz, use_fine, use_lut, fine_cut);
      const std::string tag = Sanitize(entry.label);
      const std::string hname = "h_tot_" + tag + "_ch" + std::to_string(ch);
      const double hist_hi = max_duration_ns > 0.0 ? max_duration_ns : 100.0;
      auto hist = std::make_unique<TH1D>(hname.c_str(),
                                         ("ToT " + entry.label + " ch " + std::to_string(ch) + ";ToT [ns];Entries").c_str(),
                                         200,
                                         0.0,
                                         hist_hi);
      for (double tot : tots) {
        hist->Fill(tot);
      }
      const double mean = hist->GetEntries() > 0 ? hist->GetMean() : 0.0;
      const double rms = hist->GetEntries() > 0 ? hist->GetRMS() : 0.0;
      const double err = hist->GetEntries() > 1 ? rms / std::sqrt(hist->GetEntries()) : 0.0;
      x_by_ch[ch].push_back(entry.intensity);
      y_by_ch[ch].push_back(mean);
      ey_by_ch[ch].push_back(err);
      if (!h2_by_ch[ch]) {
        h2_by_ch[ch] = std::make_unique<TH2D>(("h_tot_vs_intensity_ch" + std::to_string(ch)).c_str(),
                                              ("ToT vs laser intensity ch " + std::to_string(ch) +
                                               ";Laser intensity;ToT [ns]")
                                                  .c_str(),
                                              100,
                                              0.0,
                                              20.0,
                                              200,
                                              0.0,
                                              hist_hi);
      }
      for (double tot : tots) {
        h2_by_ch[ch]->Fill(entry.intensity, tot);
      }
      if (txt) {
        txt << "tot_intensity\t" << ch << "\t" << entry.label << "\t" << entry.intensity << "\t"
            << hist->GetEntries() << "\t" << mean << "\t" << rms << "\n";
      }
      h1_by_ch_run[{ch, entry.label}] = std::move(hist);
    }
  }

  if (fout) {
    fout->cd();
    TNamed mode("calibration_mode", "tot_intensity_characterization");
    mode.Write();
    TParameter<double> p_duration("max_duration_ns", max_duration_ns);
    TParameter<double> p_clock("clock_mhz", clock_mhz);
    p_duration.Write();
    p_clock.Write();
    for (int ch : all_channels) {
      TParameter<int> valid(("timewalk_corr_valid_ch" + std::to_string(ch)).c_str(), 0);
      valid.Write();
    }
    for (auto &kv : h1_by_ch_run) {
      kv.second->Write();
    }
    for (auto &kv : h2_by_ch) {
      kv.second->Write();
    }
  }

  std::vector<std::unique_ptr<TGraphErrors>> graphs;
  for (int ch : all_channels) {
    auto &xs = x_by_ch[ch];
    auto &ys = y_by_ch[ch];
    auto &eys = ey_by_ch[ch];
    auto graph = std::make_unique<TGraphErrors>(static_cast<int>(xs.size()));
    graph->SetName(("g_tot_mean_ch" + std::to_string(ch)).c_str());
    graph->SetTitle(("Mean ToT vs laser intensity ch " + std::to_string(ch) + ";Laser intensity;Mean ToT [ns]").c_str());
    graph->SetMarkerStyle(20);
    graph->SetMarkerSize(1.0);
    for (size_t i = 0; i < xs.size(); ++i) {
      graph->SetPoint(static_cast<int>(i), xs[i], ys[i]);
      graph->SetPointError(static_cast<int>(i), 0.0, eys[i]);
    }
    if (fout) {
      fout->cd();
      graph->Write();
    }
    graphs.push_back(std::move(graph));
  }

  if (out_pdf && out_pdf[0] != '\0') {
    gStyle->SetOptStat(1110);
    TCanvas c_open("c_open", "c_open", 1600, 900);
    std::string open = std::string(out_pdf) + "[";
    std::string close = std::string(out_pdf) + "]";
    c_open.Print(open.c_str());

    for (auto &kv : h2_by_ch) {
      TCanvas c(("c_tot_vs_intensity_ch" + std::to_string(kv.first)).c_str(), "tot_vs_intensity", 1600, 900);
      kv.second->Draw("COLZ");
      c.Print(out_pdf);
    }

    for (auto &graph : graphs) {
      TCanvas c((std::string("c_") + graph->GetName()).c_str(), graph->GetName(), 1600, 900);
      graph->Draw("AP");
      c.Print(out_pdf);
    }

    c_open.Print(close.c_str());
  }

  if (fout) {
    fout->Close();
  }
}
