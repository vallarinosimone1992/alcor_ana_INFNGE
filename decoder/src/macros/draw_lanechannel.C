#include "style.C"

int col[4] = {kRed+1, kGreen+2, kAzure-3, kYellow+2};
int sty[4] = {kSolid, kSolid, kDashed, kDashed};

std::vector<std::pair<int, std::pair<int, int>>> thresholds[4];

void draw(const char *dirname, const char *tagname, int chip, int channel, int vth, int range, int offset, int color, int width, int style, const char *title, const char *opt = "l,same") {  
  TTree t;
  t.ReadFile(Form("%s/%s.scanthr.lanechannel_%d.vth_%d.range_%d.offset_%d.txt", dirname, tagname, channel % 8, vth, range, offset));
  t.Draw("rate : threshold", Form("chip == %d && channel == %d", chip, channel), opt);
  
  auto g = (TGraph*)gPad->GetPrimitive("Graph");//ListOfPrimitives()->Last();
  if (!g) return;

  // find when we cross 1 kHz going downwards
  int threshold = 63;
  for (int i = 0; i < g->GetN(); ++i) {
    if (g->GetY()[i] > 1.e3) { 
      threshold = g->GetX()[i];
      break;
    }
  }
  
  //  auto f = (TF1 *)gROOT->GetFunction("expo");
  //  g->Fit(f, "0", "same");
  //  auto node = (std::log(0.01) - f->GetParameter(0)) / f->GetParameter(1);

  //  std::cout << threshold << std::endl;
  
  if (threshold >= 3. && threshold < 50.) thresholds[range].push_back( {threshold , {vth, offset} } );
  //  if (threshold >= 32. && threshold < 52.) thresholds[range].push_back( {threshold , {vth, offset} } ); // R+TEMP
  
  g->SetName("Old");
  g->SetTitle(title);
  g->SetLineWidth(width);
  g->SetLineColor(color);
  g->SetLineStyle(style);
  g->SetMarkerColor(color);
  g->SetMarkerStyle(20);
  g->SetMarkerSize(0.6);
  g->SetFillStyle(0);
  g->SetFillColor(0);
  
}

void
draw_lanechannel(const char *dirname, int chip, int channel, std::vector<std::string> tags, bool finalise = false)
{
  style();

  int lanechannel = channel % 8;
  
  gStyle->SetOptTitle(false);

  if (!finalise) {
  
  auto c = new TCanvas("c", "c", 1600, 800);
  c->Divide(8, 4, 0., 0.);
  
  // loop over offset/range values
  for (int range = 0; range < 4; ++range) {      
    for (int offset = 0; offset < 8; ++offset) {

      //      std::cout << " ---- "  << std::endl;

      int icanvas = range * 8 + offset + 1;
      c->cd(icanvas);
      auto hframe = c->cd(icanvas)->DrawFrame(0., 2.e-1, 63., 5.e6, ";threshold;rate (Hz)");
      hframe->GetXaxis()->SetNdivisions(507);
      hframe->SetTitle(Form("offset=%d", offset));
      c->cd(icanvas)->SetLogy();
      c->cd(icanvas)->SetGridx();
      c->cd(icanvas)->SetGridy();

      // loop over tags
      for (int tag = 0; tag < tags.size(); ++tag) {

        if (tags[tag].empty()) continue;
        
        // loop over vth values
        for (int vth = 0; vth < 4; ++vth) {
          draw(dirname, tags[tag].c_str(), chip, channel, vth, range, offset, col[vth], 3, sty[tag], Form("vth=%d", vth));
        }

      }

    }}

  // draw legend
  c->cd(8);
  auto legend = new TLegend(0.1, 0.1, 0.9, 0.9);
  for (int vth = 0; vth < 4; ++vth) {
    auto g = new TGraph;
    g->SetLineColor(col[vth]);
    g->SetLineWidth(3);
    legend->AddEntry(g, Form("vth=%d", vth), "l");
  }
  legend->Draw("same");
  
          
  c->SaveAs(Form("%s/scanthr.chip_%d.channel_%d.png", dirname, chip, channel));

  // print thresholds
  std::ofstream outs(Form("%s/scanthr.chip_%d.channel_%d.threshold", dirname, chip, channel), std::ofstream::out);
  std::cout << "# chip channel vth range offset thr" << std::endl;
  outs << "# chip channel vth range offset thr" << std::endl;
  for (int range = 0; range < 4; ++range) {
    std::sort(thresholds[range].begin(), thresholds[range].end());
    //    auto vth =  thresholds[range].size() > 0 ? thresholds[range][0].second.first : 3;
    //    auto offset = thresholds[range].size() > 0 ? thresholds[range][0].second.second : 0;
    //    auto thr = thresholds[range].size() > 0 ? thresholds[range][0].first : 63;
    auto vth =  thresholds[range].size() > 0 ? thresholds[range][0].second.first : 0;
    auto offset = thresholds[range].size() > 0 ? thresholds[range][0].second.second : 7;
    auto thr = thresholds[range].size() > 0 ? thresholds[range][0].first : 0;
    std::cout << chip << " " << channel << " " << vth << " " << range << " " << offset << " " << thr << std::endl;
    outs << chip << " " << channel << " " << vth << " " << range << " " << offset << " " << thr << std::endl;
  }
  outs.close();
  
  } else {
  
  auto c1 = new TCanvas("c1", "c1", 800, 800);
  auto hframe = c1->cd()->DrawFrame(0., 2.e-1, 63., 5.e6, ";threshold;rate (Hz)");
  hframe->GetXaxis()->SetNdivisions(507);
  c1->cd()->SetLogy();
  c1->cd()->SetGridx();
  c1->cd()->SetGridy();
  
  // loop over tags
  for (int tag = 0; tag < tags.size(); ++tag) {
    if (tags[tag].empty()) continue;
    draw(dirname, tags[tag].c_str(), chip, channel, -1, -1, -1, col[tag], 3, kSolid, "");
  }
  
  c1->SaveAs(Form("%s/scanthr.finalise.chip_%d.channel_%d.png", dirname, chip, channel));

  }
}
