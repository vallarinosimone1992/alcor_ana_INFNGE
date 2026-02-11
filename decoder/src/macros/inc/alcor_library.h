#define MAXDATA 1024

Bool_t  kVerbose = true;

struct uData_Structure {
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


Bool_t
IsTimingChip
 ( Int_t fCurrent_Chip = 0, std::initializer_list<Int_t> fTimingChips = {4} )   {
    for ( auto kTimingChip : fTimingChips ) {
        if ( kTimingChip == fCurrent_Chip ) return true;
    }
    return false;
}

TH2F*
ReverseX
( TH2F* hTarget ) {
    auto    fResult =   (TH2F*)(hTarget->Clone());
    for ( Int_t xBin = 0; xBin < hTarget->GetNbinsX(); xBin++ )    {
        for ( Int_t yBin = 0; yBin < hTarget->GetNbinsY(); yBin++ )    {
            fResult->SetBinContent( hTarget->GetNbinsX() - xBin, yBin+1, hTarget->GetBinContent( xBin+1, yBin+1 ) );
        }
    }
    return  fResult;
}

TH2F*
ReverseY
( TH2F* hTarget ) {
    auto    fResult =   (TH2F*)(hTarget->Clone());
    for ( Int_t xBin = 0; xBin < hTarget->GetNbinsX(); xBin++ )    {
        for ( Int_t yBin = 0; yBin < hTarget->GetNbinsY(); yBin++ )    {
            fResult->SetBinContent( xBin+1, hTarget->GetNbinsY() - yBin, hTarget->GetBinContent( xBin+1, yBin+1 ) );
        }
    }
    return  fResult;
}

TH2F*
ReverseXY
 ( TH2F* hTarget ) {
    return  ReverseX(ReverseY(hTarget));
}

std::pair<Int_t,std::vector<std::tuple<Int_t,TTree*,uData_Structure*>>>
load_fifos
( TString fInputFilePath ) {
    std::vector<std::tuple<Int_t,TTree*,uData_Structure*>>  fResults_TreeTuple;
    //
    //  Open Data File
    auto  fInputFile  =    TFile::Open( fInputFilePath );
    //
    //  Utility variables
    Int_t   nFrames     =   kMaxInt;
    Int_t   iTer        =   0;
    //
    //  Loop to find full FIFOs
    for ( Int_t iFIFO = 0; iFIFO < 24; iFIFO++ )    {
        auto    fCurrent_tree           =   ( TTree* )( fInputFile->Get( Form("miniframe_%i", iFIFO) ) );
        auto    fCurrent_DataStructure  =   new uData_Structure;
        //
        //  Skip non-available FIFOs
        if ( !fCurrent_tree ) continue;
        //
        //  Save available FIFO
        fResults_TreeTuple.push_back(std::tuple<Int_t,TTree*,uData_Structure*>(iFIFO,fCurrent_tree,fCurrent_DataStructure));
        auto    fCurrent_nEvents    =   fCurrent_tree->GetEntries();
        //
        //  linking Data Tree
        fCurrent_tree->SetBranchAddress("spill_id",  &fCurrent_DataStructure->spill_id);  //  Spill number
        fCurrent_tree->SetBranchAddress("frame_id",  &fCurrent_DataStructure->frame_id);  //  Frame number
        fCurrent_tree->SetBranchAddress("fifo",      &fCurrent_DataStructure->fifo);      //  FIFO  number
        fCurrent_tree->SetBranchAddress("n",         &fCurrent_DataStructure->n);         //  # Events
        fCurrent_tree->SetBranchAddress("column",    &fCurrent_DataStructure->column);    //  Hit Column
        fCurrent_tree->SetBranchAddress("pixel",     &fCurrent_DataStructure->pixel);     //  Hit Pixel
        fCurrent_tree->SetBranchAddress("tdc",       &fCurrent_DataStructure->tdc);       //  TDC
        fCurrent_tree->SetBranchAddress("rollover",  &fCurrent_DataStructure->rollover);  //  Rollover
        fCurrent_tree->SetBranchAddress("coarse",    &fCurrent_DataStructure->coarse);    //  Coarse
        fCurrent_tree->SetBranchAddress("fine",      &fCurrent_DataStructure->fine);      //  ETA/Clock Phase
        nFrames     =   std::min( (int)nFrames, (int)fCurrent_nEvents );
        iTer++;
        //
        if ( kVerbose ) cout << "[INFO] Found FIFO " << iFIFO << " w/ " << fCurrent_nEvents << " entries " << endl;
    }
    //
    return std::pair<Int_t,std::vector<std::tuple<Int_t,TTree*,uData_Structure*>>>(nFrames,fResults_TreeTuple);
}

