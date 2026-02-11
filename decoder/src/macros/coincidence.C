#define MAXDATA 1024

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
coincidence()
{

  auto fin = TFile::Open("alcdaq.miniframe.root");
  TTree *tin[25] = {nullptr};
  frame_data_t frame[25];
  int nev[25] = {0}, nframes = kMaxInt;
  int n_active_fifos = 0;
  int active_fifo[25];

  // loop and link available fifos
  for (int fifo = 0; fifo <= 24; ++fifo) {
    if (fifo != 0 && fifo != 24) continue;
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
  std::cout << " --- we will loop over " << nframes << " frames " << std::endl;

  TH1 *hDelta = new TH1F("hDelta", "", 100000, -5000000., 5000000.);

  int nhits[6] = {0}, fastest_fine[6];
  float rollover[6] = {0.}, coarse[6] = {0.}, fastest[6] = {99999999.}, fastest_rollover[6], fastest_coarse[6];
  for (int iframe = 0; iframe < nframes; ++iframe) {

    std::cout << iframe << std::endl;

    // for each trigger in frame
    tin[24]->GetEvent(iframe);
    tin[1]->GetEvent(iframe);
    for (int trigger = 0; trigger < frame[24].n; ++trigger) {
      float reference_coarse = frame[24].coarse[trigger];
      float reference_rollover = frame[24].rollover[trigger];
      
      for (int hit = 0; hit < frame[0].n && hit < MAXDATA; ++hit) {
	auto delta_coarse = frame[0].coarse[hit] - reference_coarse;
	auto delta_rollover = frame[0].rollover[hit] - reference_rollover;
	auto delta = delta_coarse + 32768. * delta_rollover;
	hDelta->Fill(delta);      
      }
    }
  }

  hDelta->Draw();
  
}
