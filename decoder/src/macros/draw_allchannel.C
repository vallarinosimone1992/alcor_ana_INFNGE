#include "style.C"

int eo2do[32] = {22, 20, 18, 16, 24, 26, 28, 30, 25, 27, 29, 31, 23, 21, 19, 17, 9, 11, 13, 15, 7, 5, 3, 1, 6, 4, 2, 0, 8, 10, 12, 14};

int col[4] = {kRed+1, kGreen+2, kAzure-3, kYellow+2};
int sty[4] = {kSolid, kSolid, kDashed, kDashed};

TGraphErrors *
draw(const char *dirname, const char *tagname, int chip, int channel, int vth, int range, int offset, int color, int width, int style, const char *title, const char *opt = "l,same") {  
  TTree t;
  t.ReadFile(Form("%s/%s.scanthr.lanechannel_%d.vth_%d.range_%d.offset_%d.txt", dirname, tagname, channel % 8, vth, range, offset));

  TProfile p("hprof", "", 63, 0., 63.);
  t.Project("hprof", "rate : threshold", Form("chip == %d && channel == %d", chip, channel), "profile");

  //  auto g = (TProfile*)gPad->GetPrimitive("htemp");
  //  htemp->SetLineWidth

  auto g = new TGraphErrors();
  int n = 0;
  for (int i = 0; i < 63; ++i) {
    //    if (p.GetBinError(i + 1) == 0.) continue;
    //    if (p.GetBinEffectiveEntries(i + 1) <= 0.) continue;
    if (p.GetBinEntries(i + 1) <= 0.) continue;
    auto x = p.GetBinLowEdge(i + 1);
    auto y = p.GetBinContent(i + 1);
    auto ey = p.GetBinError(i + 1);
    g->SetPoint(n, x, y);
    g->SetPointError(n, 0., ey);    
    n++;
  }
  //  auto g = (TGraph*)gPad->GetListOfPrimitives()->Last();
  g->SetTitle(title);
  g->SetLineWidth(width);
  g->SetLineColor(color);
  g->SetLineStyle(style);
  g->SetMarkerColor(color);
  g->SetMarkerStyle(20);
  g->SetMarkerSize(0.5);
  g->SetFillStyle(0);
  g->SetFillColor(0);

  g->Draw(opt);

  return g;
}

void
draw_onechannel(const char *dirname, int chip, int channel, std::vector<std::string> tags, std::vector<int> colors = {kBlack})
{
  style();

  gStyle->SetOptTitle(false);

  TCanvas* c = new TCanvas("c", "c", 800, 800);
  auto hframe = c->DrawFrame(0., 2.e-1, 63., 5.e7, ";threshold (au);rate (Hz)");
  hframe->GetXaxis()->SetNdivisions(507);
  c->SetLogy();
  c->SetGridx();
  c->SetGridy();
  
  // loop over tags
  for (int tag = 0; tag < tags.size(); ++tag) {
    if (tags[tag].empty()) continue;
    int icolor = tag % colors.size();
    auto g = draw(dirname, tags[tag].c_str(), chip, channel, -1, -1, -1, colors[icolor], 3, kSolid, "", "l,same");
    //    g->SetMarkerSize(1.5);
  }

  c->SaveAs(Form("%s/scanthr.chip_%d.channel_%d.png", dirname, chip, channel));
}

void
draw_allchannel(const char *dirname, int chip, std::vector<std::string> tags, std::vector<int> colors = {kBlack}, bool eo = true)
{
  style();

  gStyle->SetOptTitle(false);

  TCanvas* c = new TCanvas("c", "c", 1600, 800);
  c->Divide(8, 4, 0., 0.);

  // loop over channels
  for (int channel = 0; channel < 32; ++channel) {
    auto icol = eo2do[channel] / 4;
    auto irow = 3 - eo2do[channel] % 4;
    if (eo) {
      icol = channel % 8 ;
      irow = 3 - channel / 8;
    }
    auto icanvas = irow * 8 + icol + 1;
    c->cd(icanvas);
    auto hframe = c->cd(icanvas)->DrawFrame(0., 2.e-1, 63., 5.e7, ";threshold (au);rate (Hz)");
    hframe->GetXaxis()->SetNdivisions(507);
    c->cd(icanvas)->SetLogy();
    c->cd(icanvas)->SetGridx();
    c->cd(icanvas)->SetGridy();
    
    // loop over tags
    for (int tag = 0; tag < tags.size(); ++tag) {
      
        if (tags[tag].empty()) continue;
        
        // loop over vth values
	//        for (int vth = 0; vth < 4; ++vth) {
	int icolor = tag % colors.size();
	auto g = draw(dirname, tags[tag].c_str(), chip, channel, -1, -1, -1, colors[icolor], 3, kSolid, "", "l,same");

	//        }
        
      }
  }

  if (eo)
    c->SaveAs(Form("%s/scanthr.chip_%d.eo.png", dirname, chip));
  else
    c->SaveAs(Form("%s/scanthr.chip_%d.png", dirname, chip));

}
