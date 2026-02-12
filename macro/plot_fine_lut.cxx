#include <TCanvas.h>
#include <TFile.h>
#include <TH1.h>
#include <TH2.h>
#include <TLatex.h>
#include <TStyle.h>
#include <TSystem.h>

#include "analysis_time.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
std::vector<int> ParseIndices(const std::string &input)
{
  std::vector<int> indices;
  if (input.empty()) {
    return indices;
  }
  std::string cleaned = input;
  for (char &ch : cleaned) {
    if (ch == ',') {
      ch = ' ';
    }
  }
  std::stringstream ss(cleaned);
  int val = 0;
  while (ss >> val) {
    indices.push_back(val);
  }
  return indices;
}

std::vector<int> PickTopIndices(TH1 *entries, int max_count, int idx_min, int idx_max)
{
  std::vector<int> out;
  if (!entries) {
    for (int i = idx_min; i <= idx_max && static_cast<int>(out.size()) < max_count; ++i) {
      out.push_back(i);
    }
    return out;
  }
  std::vector<std::pair<double, int>> vals;
  vals.reserve(static_cast<size_t>(idx_max - idx_min + 1));
  for (int idx = idx_min; idx <= idx_max; ++idx) {
    const double count = entries->GetBinContent(idx + 1);
    if (count <= 0.0) {
      continue;
    }
    vals.emplace_back(count, idx);
  }
  std::sort(vals.begin(), vals.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  for (const auto &pair : vals) {
    out.push_back(pair.second);
    if (static_cast<int>(out.size()) >= max_count) {
      break;
    }
  }
  if (out.empty()) {
    for (int i = idx_min; i <= idx_max && static_cast<int>(out.size()) < max_count; ++i) {
      out.push_back(i);
    }
  }
  return out;
}
}

void plot_fine_lut(const std::string &calib_root,
                   const std::string &out_pdf,
                   int fifo = -1,
                   const std::string &indices = "")
{
  const auto t0 = std::chrono::steady_clock::now();

  if (calib_root.empty() || out_pdf.empty()) {
    std::cout << "calib_root and out_pdf must be provided" << std::endl;
    return;
  }

  std::unique_ptr<TFile> file(TFile::Open(calib_root.c_str(), "READ"));
  if (!file || file->IsZombie()) {
    std::cout << "Failed to open calibration file: " << calib_root << std::endl;
    return;
  }

  auto *hlut = dynamic_cast<TH2 *>(file->Get("hFineLut"));
  if (!hlut) {
    std::cout << "hFineLut not found in " << calib_root << std::endl;
    return;
  }

  auto *hentries = dynamic_cast<TH1 *>(file->Get("hFineEntries"));

  int idx_min = 0;
  int idx_max = hlut->GetNbinsX() - 1;
  if (fifo >= 0) {
    idx_min = fifo * analysis_time::kTdcPerFifo;
    idx_max = idx_min + analysis_time::kTdcPerFifo - 1;
    if (idx_min < 0) {
      idx_min = 0;
    }
    if (idx_max >= hlut->GetNbinsX()) {
      idx_max = hlut->GetNbinsX() - 1;
    }
  }

  std::vector<int> idx_list = ParseIndices(indices);
  if (idx_list.empty()) {
    idx_list = PickTopIndices(hentries, 4, idx_min, idx_max);
  }
  std::vector<int> filtered;
  filtered.reserve(idx_list.size());
  for (int idx : idx_list) {
    if (idx < 0 || idx > idx_max || idx < idx_min) {
      continue;
    }
    filtered.push_back(idx);
  }
  idx_list.swap(filtered);
  if (idx_list.empty()) {
    std::cout << "No valid TDC indices to plot." << std::endl;
    return;
  }

  gStyle->SetOptStat(0);
  gStyle->SetNumberContours(50);

  TCanvas copen("copen", "copen", 900, 700);
  copen.Print((out_pdf + "[").c_str());

  {
    TCanvas c2d("c2d", "Fine LUT", 1100, 800);
    if (fifo >= 0) {
      hlut->GetXaxis()->SetRangeUser(idx_min + 0.5, idx_max + 0.5);
    }
    hlut->SetTitle("Fine LUT (CDF);TDC index;fine raw;fraction");
    hlut->Draw("COLZ");
    c2d.Print(out_pdf.c_str());
  }

  if (hentries) {
    TCanvas c2d_entries("c2d_entries", "Fine LUT entries", 1100, 800);
    const int nbx = hlut->GetNbinsX();
    const int nby = hlut->GetNbinsY();
    const double xlow = hlut->GetXaxis()->GetXmin();
    const double xhigh = hlut->GetXaxis()->GetXmax();
    const double ylow = hlut->GetYaxis()->GetXmin();
    const double yhigh = hlut->GetYaxis()->GetXmax();
    auto entries_map = std::make_unique<TH2D>("hFineLutEntriesMap",
                                             "Fine LUT entries;TDC index;fine raw;entries",
                                             nbx, xlow, xhigh, nby, ylow, yhigh);
    entries_map->SetDirectory(nullptr);
    for (int x = 1; x <= nbx; ++x) {
      const double val = hentries->GetBinContent(x);
      for (int y = 1; y <= nby; ++y) {
        entries_map->SetBinContent(x, y, val);
      }
    }
    if (fifo >= 0) {
      entries_map->GetXaxis()->SetRangeUser(idx_min + 0.5, idx_max + 0.5);
    }
    entries_map->Draw("COLZ");
    c2d_entries.Print(out_pdf.c_str());
  }

  const int per_page = 4;
  for (size_t start = 0; start < idx_list.size(); start += per_page) {
    TCanvas c1d("c1d", "LUT slices", 1000, 800);
    c1d.Divide(2, 2);
    for (int pad = 0; pad < per_page; ++pad) {
      size_t idx_pos = start + static_cast<size_t>(pad);
      if (idx_pos >= idx_list.size()) {
        break;
      }
      const int idx = idx_list[idx_pos];
      c1d.cd(pad + 1);
      std::string name = "lut_idx_" + std::to_string(idx) + "_" + std::to_string(start);
      std::unique_ptr<TH1D> proj(hlut->ProjectionY(name.c_str(), idx + 1, idx + 1));
      proj->SetDirectory(nullptr);
      proj->SetTitle(("LUT slice (TDC index " + std::to_string(idx) + ");fine raw;fraction").c_str());
      proj->GetYaxis()->SetRangeUser(-0.55, 0.55);
      proj->Draw("HIST");
    }
    c1d.Print(out_pdf.c_str());
  }

  TCanvas cclose("cclose", "cclose", 900, 700);
  cclose.Print((out_pdf + "]").c_str());

  const auto t1 = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "LUT plot saved to " << out_pdf << std::endl;
  std::cout << "Elapsed time (plot_fine_lut): " << seconds << " s" << std::endl;
}
