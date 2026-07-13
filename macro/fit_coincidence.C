#include <cstdio>
#include "TF1.h"
#include "TH1.h"
#include "TFile.h"
#include "TCanvas.h"
#include "TLatex.h"
#include "TStyle.h"
#include "TMath.h"

// Fit a coincidence-dt histogram with a single Gaussian (core, iterative +-2 sigma).
// Returns the fitted TF1; optionally reports the fitted sigma and its error.
TF1* fit(TH1* h, int color = kRed, double* sigma = nullptr, double* sigma_err = nullptr){
    if(!h){ printf("fit: null histogram\n"); return nullptr; }

    const double pk  = h->GetBinCenter(h->GetMaximumBin());
    const double rms = h->GetRMS();

    TF1* g = new TF1(Form("g_%s", h->GetName()), "gaus", pk-1.5*rms, pk+1.5*rms);
    g->SetParameters(h->GetMaximum(), pk, rms);
    g->SetLineColor(color);
    g->SetNpx(1000);

    h->Fit(g, "RQ");                                  // first pass
    double m = g->GetParameter(1), s = std::fabs(g->GetParameter(2));
    g->SetRange(m-2*s, m+2*s);
    h->Fit(g, "RQ");                                  // refit within +-2 sigma (core, shoulders cut off)
    m = g->GetParameter(1); s = std::fabs(g->GetParameter(2));
    const double sErr = g->GetParError(2);

    if(sigma)     *sigma     = s;
    if(sigma_err) *sigma_err = sErr;

    printf("%-40s  mu=%+.4f  sigma=%.4f +/- %.4f ns  (per-ch ~ %.4f)\n",
           h->GetName(), m, s, sErr, s/TMath::Sqrt2());
    return g;
}

// Sum the per-run coincidence histograms (whole dataset) for a given prefix.
TH1* sum_runs(TFile* f, const char* prefix){
    const char* runs[] = {"20260616_103238","20260616_110415","20260616_122729","20260616_140420"};
    TH1* total = nullptr;
    for(const char* run : runs){
        TH1* h = (TH1*)f->Get(Form("%s%s", prefix, run));
        if(!h){ printf("sum_runs: missing %s%s\n", prefix, run); continue; }
        if(!total){ total = (TH1*)h->Clone(Form("%sall", prefix)); total->SetDirectory(nullptr); }
        else      { total->Add(h); }
    }
    return total;
}

// Overlay + single-Gaussian fit of the before (uncorrected) and after (corrected)
// coincidence peaks in the current pad. Reports both sigmas.
void draw_before_after(TH1* hb_in, TH1* ha_in, const char* title,
                       double& sig_before, double& sig_after, int rebin = 1){
    // work on clones so the originals (and the summed histogram) are never
    // mutated/double-rebinned; rebin merges the 0.025 ns comb into 0.05 ns bins
    TH1* h_before = (TH1*)hb_in->Clone(Form("%s_disp", hb_in->GetName()));
    TH1* h_after  = (TH1*)ha_in->Clone(Form("%s_disp", ha_in->GetName()));
    h_before->SetDirectory(nullptr);
    h_after->SetDirectory(nullptr);
    if(rebin > 1){ h_before->Rebin(rebin); h_after->Rebin(rebin); }

    // distributions both in black; the fits carry the color coding
    h_before->SetLineColor(kBlack);  h_before->SetLineWidth(1);
    h_after->SetLineColor(kBlack);   h_after->SetLineWidth(1);
    h_before->SetTitle(title);

    const double ymax = 1.20 * TMath::Max(h_before->GetMaximum(), h_after->GetMaximum());
    h_before->SetMaximum(ymax);
    h_before->SetMinimum(0.0);

    h_before->Draw("hist");
    h_after->Draw("hist same");

    TF1* g_before = fit(h_before, kBlue,  &sig_before);
    TF1* g_after  = fit(h_after,  kRed+1, &sig_after);
    if(g_before) g_before->Draw("same");
    if(g_after)  g_after->Draw("same");
}

void fit_coincidence(){
    gStyle->SetOptStat(0);

    TFile* f = TFile::Open("~/Documents/INFN/DTagger/alcor_ana_INFNGE/calibration/timewalk_correction_signed_dt_7p5_to_13p5_veto200_totwnd.root");
    if(!f || f->IsZombie()){ printf("could not open input file\n"); return; }

    const char*  pre_before = "h_dt_uncorr_ch17_ch19_";   // before timewalk
    const char*  pre_after  = "h_dt_corr_ch17_ch19_";     // after  timewalk
    const char*  runs[4] = {"20260616_103238","20260616_110415","20260616_122729","20260616_140420"};
    const double I[4]    = {7.5, 9.5, 11.5, 13.5};
    const int    rebin   = 2;   // 0.025 ns -> 0.05 ns bins (removes the comb)

    TCanvas* c = new TCanvas("c", "coincidence ch17-ch19 before/after", 1000, 1200);
    c->Divide(2, 3);   // 2 wide: 4 run pads + 1 combined pad (+1 empty)

    TLatex tx;
    tx.SetNDC();
    tx.SetTextSize(0.055);

    // --- each run independently: before vs after ---
    for(int i = 0; i < 4; ++i){
        c->cd(i+1);
        TH1* hb = (TH1*)f->Get(Form("%s%s", pre_before, runs[i]));
        TH1* ha = (TH1*)f->Get(Form("%s%s", pre_after,  runs[i]));
        if(!hb || !ha){ printf("missing histogram(s) for run %s\n", runs[i]); continue; }
        double sb = 0, sa = 0;
        draw_before_after(hb, ha, Form("ch17-ch19, I=%.1f", I[i]), sb, sa, rebin);
        tx.SetTextColor(kBlue);  tx.DrawLatex(0.58, 0.84, Form("before #sigma=%.3f", sb));
        tx.SetTextColor(kRed+1); tx.DrawLatex(0.58, 0.77, Form("after  #sigma=%.3f", sa));
    }

    // --- combined (summed over all runs): before vs after ---
    c->cd(5);
    TH1* hb = sum_runs(f, pre_before);
    TH1* ha = sum_runs(f, pre_after);
    if(hb && ha){
        double sb = 0, sa = 0;
        draw_before_after(hb, ha, "ch17-ch19, summed 7.5-13.5", sb, sa, rebin);
        tx.SetTextColor(kBlue);  tx.DrawLatex(0.58, 0.84, Form("before #sigma=%.3f", sb));
        tx.SetTextColor(kRed+1); tx.DrawLatex(0.58, 0.77, Form("after  #sigma=%.3f", sa));
    }

    c->SaveAs("output/coincidence_fit_ch17_ch19_beforeafter.pdf");
}
