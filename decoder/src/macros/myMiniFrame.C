/**
   PS spill lenght is at most 1 second
   with a frame length = 8 rollover
   and maximum number of frames = 1024
   we reach 838 ms, which is ok
 **/

#define FRAME_SIZE 8
#define MAXFRAMES 256 //1024
#define MAXDATA 1024*1024

void
myMiniFrame(const char *fnamein, const char *fnameout, bool verbose = false)
{
  auto fin = TFile::Open(fnamein);
  auto tin = (TTree *)fin->Get("alcor");

  struct data_t {
    int fifo;
    int type;
    int counter;
    int column;
    int pixel;
    int tdc;
    int rollover;
    int coarse;
    int fine;
  } data;
  tin->SetBranchAddress("fifo", &data.fifo);
  tin->SetBranchAddress("type", &data.type);
  tin->SetBranchAddress("counter", &data.counter);
  tin->SetBranchAddress("column", &data.column);
  tin->SetBranchAddress("pixel", &data.pixel);
  tin->SetBranchAddress("tdc", &data.tdc);
  tin->SetBranchAddress("rollover", &data.rollover);
  tin->SetBranchAddress("coarse", &data.coarse);
  tin->SetBranchAddress("fine", &data.fine);
  auto nev = tin->GetEntries();
  tin->GetEntry(0);
  
  auto fout = TFile::Open(fnameout, "RECREATE");
  auto tout = new TTree(Form("miniframe_%d", data.fifo), "miniframe");
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

  frame_data_t *frame = new frame_data_t[MAXFRAMES];
  
  tout->Branch("spill_id", &frame[0].spill_id, "spill_id/I");
  tout->Branch("frame_id", &frame[0].frame_id, "frame_id/I");
  tout->Branch("fifo", &frame[0].fifo, "fifo/I");
  tout->Branch("n", &frame[0].n, "n/I");
  tout->Branch("column", &frame[0].column, "column[n]/I");
  tout->Branch("pixel", &frame[0].pixel, "pixel[n]/I");
  tout->Branch("tdc", &frame[0].tdc, "tdc[n]/I");
  tout->Branch("rollover", &frame[0].rollover, "rollover[n]/I");
  tout->Branch("coarse", &frame[0].coarse, "coarse[n]/I");
  tout->Branch("fine", &frame[0].fine, "fine[n]/I");

  int spill_counter = 0;
  for (int iev = 0; iev < nev; ++iev) {
    tin->GetEntry(iev);

    // spill header
    if (data.type == 7) {
      std::cout << " spill header: " << spill_counter << std::endl;
      // reset all frames
      for (int iframe = 0; iframe < MAXFRAMES; ++iframe) {
        frame[iframe].frame_id = iframe;
        frame[iframe].spill_id = spill_counter; // data.counter;
        frame[iframe].fifo = data.fifo;
        frame[iframe].n = 0;
      }
      spill_counter++;
      continue;
    }

    // alcor hit / trigger
    if (data.type == 1 || data.type == 9) {
      int frame_id = data.rollover / FRAME_SIZE;
      if (frame_id >= MAXFRAMES) continue;
      auto n = frame[frame_id].n;
      if (n == MAXDATA) {
        std::cout << " [WARNING] buffer overflow " << std::endl;
        continue;
      }
      frame[frame_id].column[n] = data.column;
      frame[frame_id].pixel[n] = data.pixel;
      frame[frame_id].tdc[n] = data.tdc;
      frame[frame_id].rollover[n] = data.rollover;
      frame[frame_id].coarse[n] = data.coarse;
      frame[frame_id].fine[n] = data.fine;
      frame[frame_id].n++;
      continue;
    }

    if (data.type == 15 || data.type == 666) { // spill trailer
      std::cout << " spill trailer" << std::endl;
      // write all frames
      for (int iframe = 0; iframe < MAXFRAMES; ++iframe) {
        if (verbose) std::cout << " --- filling tree with frame #" << iframe << ": " << frame[iframe].n << " entries" << std::endl;
        tout->SetBranchAddress("spill_id", &frame[iframe].spill_id);
        tout->SetBranchAddress("frame_id", &frame[iframe].frame_id);
        tout->SetBranchAddress("fifo", &frame[iframe].fifo);
        tout->SetBranchAddress("n", &frame[iframe].n);
        tout->SetBranchAddress("column", &frame[iframe].column);
        tout->SetBranchAddress("pixel", &frame[iframe].pixel);
        tout->SetBranchAddress("tdc", &frame[iframe].tdc);
        tout->SetBranchAddress("rollover", &frame[iframe].rollover);
        tout->SetBranchAddress("coarse", &frame[iframe].coarse);
        tout->SetBranchAddress("fine", &frame[iframe].fine);
        tout->Fill();
      }
    }
  }

  tout->Write();

}
