#define MAXDATA 4096

struct frame_data_t {
  int spill_id;
  int frame_id;
  int fifo;
  int n;
  int column[MAXDATA];
  int pixel[MAXDATA];
  int tdc[MAXDATA];
  int rollover[MAXDATA];
  int coarse[MAXDATA];
  int fine[MAXDATA];
};

int eo2do[32] = {22, 20, 18, 16, 24, 26, 28, 30, 25, 27, 29, 31, 23, 21, 19, 17, 9, 11, 13, 15, 7, 5, 3, 1, 6, 4, 2, 0, 8, 10, 12, 14};
int do2eo[32] = {27, 23, 26, 22, 25, 21, 24, 20, 28, 16, 29, 17, 30, 18, 31, 19, 3, 15, 2, 14, 1, 13, 0, 12, 4, 8, 5, 9, 6, 10, 7, 11};


void
coincidence(const char *fname, int chip, int channel)
{

  int rlane = 24;
  int lane = 4 * chip + channel / 8;
  
  auto fin = TFile::Open(fname);
  TTree *tin[25] = {nullptr};
  frame_data_t frame[25];
  int nev[25] = {0}, nframes = kMaxInt;
  int n_active_fifos = 0;
  int active_fifo[25];

  // loop and link available fifos
  for (int fifo = 0; fifo <= 24; ++fifo) {
    tin[fifo] = (TTree *)fin->Get(Form("miniframe_%d", fifo));
    if (!tin[fifo]) continue;
    nev[fifo] = tin[fifo]->GetEntries();
    std::cout << " --- found fifo " << fifo << ": " << nev[fifo] << " entries " << std::endl;
    active_fifo[n_active_fifos] = fifo;
    n_active_fifos++;
    
    //    frame[fifo] = new frame_data_t;
    tin[fifo]->SetBranchAddress("spill_id", &frame[fifo].spill_id);
    tin[fifo]->SetBranchAddress("frame_id", &frame[fifo].frame_id);
    tin[fifo]->SetBranchAddress("fifo", &frame[fifo].fifo);
    tin[fifo]->SetBranchAddress("n", &frame[fifo].n);
    tin[fifo]->SetBranchAddress("column", &frame[fifo].column);
    tin[fifo]->SetBranchAddress("pixel", &frame[fifo].pixel);
    tin[fifo]->SetBranchAddress("tdc", &frame[fifo].tdc);
    tin[fifo]->SetBranchAddress("rollover", &frame[fifo].rollover);
    tin[fifo]->SetBranchAddress("coarse", &frame[fifo].coarse);
    tin[fifo]->SetBranchAddress("fine", &frame[fifo].fine);

    nframes = std::min(nframes, nev[fifo]);
  }
  if (nev[rlane] != nev[lane]) {
    std::cout << " --- fifos have different events, abort " << std::endl;
    return;
  }
  std::cout << " --- we will loop over " << nframes << " frames " << std::endl;

  TH1 *hDelta = new TH1F("hDelta", "", 1000000, -500000, 500000);
  TH1 *hNTriggers = new TH1F("hNTriggers", "", 1, 0, 1);
  
  int ntriggers = 0;
  
  for (int iframe = 0; iframe < nframes; ++iframe) {

    tin[rlane]->GetEvent(iframe);
    tin[lane]->GetEvent(iframe);

    for (int rhit = 0; rhit < frame[rlane].n; ++rhit) {

      auto reference_coarse = frame[rlane].coarse[rhit];
      auto reference_rollover = frame[rlane].rollover[rhit];
      ntriggers++;
      
      for (int hit = 0; hit < frame[lane].n; ++hit) {
        
        auto ch = frame[lane].pixel[hit] + 4 * frame[lane].column[hit];
        //        if (ch != channel) continue;
        
        auto coarse = frame[lane].coarse[hit];
        auto rollover = frame[lane].rollover[hit];

        auto delta_coarse = coarse - reference_coarse;
        auto delta_rollover = rollover - reference_rollover;
        auto delta = delta_coarse + delta_rollover * 32768;

        hDelta->Fill(delta);

      }
    }
  }

  std::cout << " --- number of triggers: " << ntriggers << std::endl;

  hNTriggers->SetBinContent(1, ntriggers);
  //  hDelta->Sumw2();
  //  hDelta->Scale(1. / ntriggers);
  //  hDelta->Draw();

  TFile fout(Form("coincidence.%s", fname), "RECREATE");
  hDelta->Write();
  hNTriggers->Write();
  fout.Close();
  
}
