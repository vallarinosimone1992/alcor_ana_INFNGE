#include "style.C"

int eo2do[32] = {22, 20, 18, 16, 24, 26, 28, 30, 25, 27, 29, 31, 23, 21, 19, 17, 9, 11, 13, 15, 7, 5, 3, 1, 6, 4, 2, 0, 8, 10, 12, 14};


int channel[4] = {27, 23, 26, 22};
int threshold2[4] = {9, 12, 8, 14};
int threshold5[4] = {12, 15, 11, 17};
int threshold10[4] = {17, 20, 16, 22};
int color[4] = {kBlack, kAzure-3, kGreen+2, kRed+1};

TF1 *Vbd(TGraphErrors *gin, double vbdi = 48.);
TGraphErrors *getRate(TTree *tin, int threshold, bool isdelta = true);

void
draw_hama(const char *tag, int chip, int threshold = 3, bool hide_outliers = false)
{
  style();

  TCanvas *c[5];
  for (int ic = 0; ic < 5; ++ic) {
    c[ic] = new TCanvas(Form("c%d", ic), Form("c%d", ic), 1600, 800);
    c[ic]->Divide(4, 2, 0, 0);
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 2; ++j) {
        c[ic]->cd(i + j * 4 + 1)->DrawFrame(47., 5.e-1, 57., 2.e7, ";V_{bias} (V);DCR (Hz)");
        c[ic]->cd(i + j * 4 + 1)->SetLogy();
        c[ic]->cd(i + j * 4 + 1)->SetGridx();
        c[ic]->cd(i + j * 4 + 1)->SetGridy();
      }
    }
  }
    
  TProfile p1[2] = { TProfile("p1_50um", "", 4096, 0, 4096, "S"), TProfile("p1_25um", "", 4096, 0, 4096, "S") };
  for (int i = 0; i < 32; ++i) {
    auto doch = eo2do[i];
    if (hide_outliers && doch == 17) continue;
    auto crow = doch / 4;
    auto ccol = doch % 4;
    c[0]->cd(ccol + (crow % 2) * 4 + 1);
    auto t = new TTree;
    t->ReadFile(Form("%s.%d.%d.tree.dat", tag, chip, i));
    auto g = getRate(t, threshold);
    g->SetName(Form("grate_%d", doch));
    g->SetMarkerStyle(20);
    g->SetMarkerSize(0.8);
    int shift[4] = {0, 1, 2, 3};
    g->SetMarkerColor(kAzure + shift[crow / 2]);
    g->SetLineColor(kAzure + shift[crow / 2]);

    // fill profile for average behaviour
    for (int ipt = 0; ipt < g->GetN(); ++ipt) {
      if (hide_outliers && doch == 17) continue;
      p1[crow % 2].Fill(g->GetX()[ipt], g->GetY()[ipt]);
    }

    // before drawing we convert dac->vbias
    for (int ipt = 0; ipt < g->GetN(); ++ipt)
      g->GetX()[ipt] = -0.0237928 + 0.0191619 * g->GetX()[ipt];

    g->Draw("samep");
  }
  TGraphErrors g1[2] = { TGraphErrors(), TGraphErrors() };
  for (int i = 0; i < 2; ++i) {
    int npt = 0;
    for (int j = 0; j < 4096; ++j) {
      if (p1[i].GetBinContent(j + 1) == 0.) continue;
      auto dac = p1[i].GetBinLowEdge(j + 1);
      auto vbias = -0.0237928 + 0.0191619 * dac;
      g1[i].SetPoint(npt, vbias, p1[i].GetBinContent(j + 1));
      g1[i].SetPointError(npt, 0., p1[i].GetBinError(j + 1));
      npt++;
    }
  }

  std::cout << "g1[0] = " << g1[0].Eval(51) << std::endl;
  std::cout << "g1[1] = " << g1[0].Eval(54) << std::endl;
  
  g1[0].SetFillColor(kAzure-3);
  g1[0].SetFillStyle(3004);
  g1[1].SetFillColor(kAzure-3);
  g1[1].SetFillStyle(3002);
  for (int i = 0; i < 4; ++i) {
    c[1]->cd(i + 1);
    g1[0].SetFillStyle(3001);
    g1[0].DrawClone("same,e3");
    g1[0].SetFillStyle(0);
    g1[0].DrawClone("same,e3");

    c[1]->cd(i + 5);
    g1[1].SetFillStyle(3001);
    g1[1].DrawClone("same,e3");
    g1[1].SetFillStyle(0);
    g1[1].DrawClone("same,e3");

    c[4]->cd(i + 1);
    g1[0].SetFillStyle(3001);
    g1[0].DrawClone("same,e3");
    g1[0].SetFillStyle(0);
    g1[0].DrawClone("same,e3");

    c[4]->cd(i + 5);
    g1[1].SetFillStyle(3001);
    g1[1].DrawClone("same,e3");
    g1[1].SetFillStyle(0);
    g1[1].DrawClone("same,e3");
}
  
  for (int ic = 0; ic < 5; ++ic)
    c[ic]->SaveAs(Form("draw_hama_%d.png", ic));
  
}

#if 0
/********************************************************************/

TGraphErrors *
getHealthy(TGraphErrors *gin) {
  auto gout = new TGraphErrors;
  int npt = 0;
  for (int i = 1; i < gin->GetN() - 1; ++i) {
    auto xi = gin->GetX()[i - 1];
    auto yi = gin->GetY()[i - 1];
    auto xf = gin->GetX()[i + 1];
    auto yf = gin->GetY()[i + 1];
    auto xt = gin->GetX()[i];
    auto yt = yi + (yi - yf) / (xf - xi) * (xt - xi);

    
    
  }
}
#endif

/********************************************************************/

TGraphErrors *
getRate(TTree *tin, int threshold, bool isdelta)
{
  TH1F hcounts("hcounts", "", 4096, 0, 4096);
  TH1F hperiod("hperiod", "", 4096, 0, 4096);
  TH1F hrate("hrate", "", 4096, 0, 4096);
  auto nev = tin->GetEntries();
  for (int iev = 0; iev < nev; ++iev) {
    tin->GetEntry(iev);
    if (isdelta) {
      if (tin->GetLeaf("delta_threshold")->GetValue() != threshold) continue;
    } else {
      if (tin->GetLeaf("threshold")->GetValue() != threshold) continue;
    }
    //    if (tin->GetLeaf("period")->GetValue() < 0.1) continue;
    auto value = tin->GetLeaf("value")->GetValue();
    auto ibin = hcounts.FindBin(value);
    hcounts.AddBinContent(ibin, tin->GetLeaf("counts")->GetValue());
    hperiod.AddBinContent(ibin, tin->GetLeaf("period")->GetValue());
    hrate.SetBinContent(ibin, tin->GetLeaf("rate")->GetValue());
    hrate.SetBinError(ibin, tin->GetLeaf("ratee")->GetValue());
  }
  
  auto gout = new TGraphErrors;
  int npt = 0;
  for (int i = 0; i < 4096; ++i) {
    auto period = hperiod.GetBinContent(i + 1);
    if (period <= 0.) continue;
    auto dac = hcounts.GetBinLowEdge(i + 1);
    auto vbias = -0.0237928 + 0.0191619 * dac;
    auto val = hcounts.GetBinContent(i + 1);
    auto vale = std::sqrt(val);
    val /= period;
    vale /= period;
    val = hrate.GetBinContent(i + 1);
    vale = hrate.GetBinError(i + 1);
    //    gout->SetPoint(npt, vbias, val);
    gout->SetPoint(npt, dac, val);
    gout->SetPointError(npt, 0., vale);
    npt++;
  }
  return gout;
}

/********************************************************************/

TF1 *
Vbd(TGraphErrors *gin, double vbdi = 48.)
{
  auto f = (TF1 *)gROOT->GetFunction("pol2");
  double vbd = vbdi;
  do {
    vbdi = vbd;
    gin->Fit(f, "0q", "", vbdi + 3., 55.);
    vbd = f->GetX(0, 45., 55.);
    std::cout << "vbdi=" << vbdi << " vbd=" << vbd << std::endl;
  } while ( fabs(vbd - vbdi) > 0.1);
  f->SetRange(vbd, 60.);
  return f;
}

