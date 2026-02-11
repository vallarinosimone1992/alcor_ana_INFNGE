
#include "./inc/style.h"
#include "./inc/alcor_library.h"




void
find_coincindeces
 ( TString sInputFile = "../alcdaq.miniframe.root", TString sOutputFile = "../alcdaq.miniframe.coincidences.root", std::initializer_list<Int_t> fTimingChips = {4}, Double_t kHighTimeWindow = 15, Double_t kLowTimeWindow = -10 )   {
    //
    if ( kVerbose ) cout << "[INFO] Loading available FIFOs" << endl;
    style();
    //
    //  Laoding available FIFOs
    auto                                                    fAvailable_FIFOs_DatStructure   =   load_fifos( sInputFile );
    //
    Int_t                                                   nFrames                         =   fAvailable_FIFOs_DatStructure.first;
    std::vector<std::tuple<Int_t,TTree*,uData_Structure*>>  kAvailableFIFOs                 =   fAvailable_FIFOs_DatStructure.second;
    //
    if ( kVerbose ) cout << "[INFO] Total frames " << nFrames << endl;
    //
    //  Output File
    TFile*  fOutputFile =   new TFile( sOutputFile, "RECREATE" );
    // 3.125 #ns        Coarse
    // 102396.875 #ns   Rollover
    //
    //  Define Final Histograms
    std::vector<TH2F*>              hInclusiveHitMap;
    std::vector<TH2F*>              hDeltaTimeHitMap;
    std::vector<TH2F*>              hTimeResoltutionMap;
    std::vector<TH2F*>              hTimeOffSetMap;
    std::vector<std::vector<TH1F*>> hChannelDeltaTiming;
    for ( Int_t iTer = 0; iTer < 8; iTer++ ) {
        hInclusiveHitMap.push_back( new TH2F(Form("hInclusiveHitMap_Chip%i",iTer),"",4,0.,4.,8,0.,8.) );
        hDeltaTimeHitMap.push_back( new TH2F(Form("hTimingHitMap_Chip%i",iTer),"",4,0.,4.,8,0.,8.) );
        hTimeResoltutionMap.push_back( new TH2F(Form("hTimeResoltutionMap_Chip%i",iTer),"",4,0.,4.,8,0.,8.) );
        hTimeOffSetMap.push_back( new TH2F(Form("hTimeOffSetMap_Chip%i",iTer),"",4,0.,4.,8,0.,8.) );
        std::vector<TH1F*>  fChannelTimingHistogram;
        for ( Int_t jTer = 0; jTer < 32; jTer++ ) {
            fChannelTimingHistogram.push_back( new TH1F(Form("hChannelDeltaTiming_Chip_%i_Channel_%i",iTer,jTer),"",320,-500.,500) );
        }
        hChannelDeltaTiming.push_back( fChannelTimingHistogram );
    }
    //
    gROOT       ->      SetBatch(kTRUE);
    //  Looping over all frames
    for ( Int_t iFrm = 0; iFrm < nFrames; iFrm++ )    {
        //
        //  Recover Earliest Time Reference
        std::pair<Int_t,Int_t>  kTiming_Hit (kMaxInt,kMaxInt);
        for ( auto &fCurrent_FIFO   :   kAvailableFIFOs )   {
            Int_t                   nCurrentFIFO        =   get<0>(fCurrent_FIFO);
            Int_t                   nCurrentChip        =   nCurrentFIFO/4;
            //
            TTree*                  fCurrent_Tree       =   get<1>(fCurrent_FIFO);
            //
            uData_Structure*        fCurrent_DataStruct =   get<2>(fCurrent_FIFO);
            //
            Bool_t                  kIsTimingChip       =   IsTimingChip( nCurrentChip, fTimingChips );
            if ( !kIsTimingChip ) continue;
            //
            //  Load event
            fCurrent_Tree           ->  GetEvent(iFrm);
            //  Hit Loop
            for ( Int_t iHit = 0; iHit < fCurrent_DataStruct->n; iHit++ )    {
                auto    fCurrent_Coarse     =   fCurrent_DataStruct->coarse[iHit];
                auto    fCurrent_Rollover   =   fCurrent_DataStruct->rollover[iHit];
                auto    fCurrent_Pixel      =   fCurrent_DataStruct->pixel[iHit];
                auto    fCurrent_Column     =   fCurrent_DataStruct->column[iHit];
                kTiming_Hit =   std::pair<Int_t,Int_t>( min( kTiming_Hit.first, fCurrent_Rollover ), min( kTiming_Hit.second, fCurrent_Coarse ) );
                //
                //  All Chips Histograms
                hInclusiveHitMap.at(nCurrentChip)->Fill(fCurrent_Pixel,fCurrent_Column);
            }
        }
        for ( auto &fCurrent_FIFO   :   kAvailableFIFOs )   {
            Int_t                   nCurrentFIFO        =   get<0>(fCurrent_FIFO);
            Int_t                   nCurrentChip        =   nCurrentFIFO/4;
            //
            TTree*                  fCurrent_Tree       =   get<1>(fCurrent_FIFO);
            //
            uData_Structure*        fCurrent_DataStruct =   get<2>(fCurrent_FIFO);
            //
            Bool_t                  kIsTimingChip       =   IsTimingChip( nCurrentChip, fTimingChips );
            if ( kIsTimingChip ) continue;
            //
            //  Load event
            fCurrent_Tree           ->  GetEvent(iFrm);
            //  Hit Loop
            for ( Int_t iHit = 0; iHit < fCurrent_DataStruct->n; iHit++ )    {
                auto    fCurrent_Rollover   =   fCurrent_DataStruct->rollover[iHit];
                auto    fCurrent_Coarse     =   fCurrent_DataStruct->coarse[iHit];
                auto    fDelta_Rollover     =   fCurrent_Rollover   -   kTiming_Hit.first;
                auto    fDelta_Coarse       =   fCurrent_Coarse     -   kTiming_Hit.second;
                auto    fCurrent_Pixel      =   fCurrent_DataStruct->pixel[iHit];
                auto    fCurrent_Column     =   fCurrent_DataStruct->column[iHit];
                auto    fCurrent_DeltaTime  =   fDelta_Coarse*3.125 + fDelta_Rollover*102396.875;
                //
                //  All Chips Histograms
                hInclusiveHitMap.at(nCurrentChip)->Fill(fCurrent_Pixel,fCurrent_Column);
                //
                //  Non-Timing Chips Histograms
                if ( kTiming_Hit.first == kMaxInt ) continue;
                hChannelDeltaTiming.at(nCurrentChip).at(fCurrent_Column*4+fCurrent_Pixel)->Fill(fCurrent_DeltaTime);
                if ( fCurrent_DeltaTime < kHighTimeWindow && fCurrent_DeltaTime > kLowTimeWindow ) hDeltaTimeHitMap.at(nCurrentChip)->Fill(fCurrent_Pixel,fCurrent_Column);
            }
        }
        //
    }
    //
    //  Draw the Results
    //
    gROOT       ->      ProcessLine(".! mkdir -p ./Plots/");
    gROOT       ->      ProcessLine(".! mkdir -p ./Plots/All_Chips");
    for ( Int_t iChip = 0; iChip < 8; iChip++ ) gROOT       ->      ProcessLine(Form(".! mkdir -p ./Plots/Chip_%i",iChip));
    TCanvas*    cDrawInclusiveHitMap    =   new TCanvas("","",1600,800);
    cDrawInclusiveHitMap->Divide(4,2);
    TCanvas*    cDrawTimingHitMap       =   new TCanvas("","",1600,800);
    cDrawTimingHitMap->Divide(4,2);
    TCanvas*    cDrawTimeResolutionMap  =   new TCanvas("","",1600,800);
    cDrawTimeResolutionMap->Divide(4,2);
    TCanvas*    cDrawTimeOffsetMap      =   new TCanvas("","",1600,800);
    cDrawTimeOffsetMap->Divide(4,2);
    auto    iTer    =   0;
    for ( auto &fCurrent_InclusiveHitMap : hInclusiveHitMap )   {
        iTer++;
        auto    jTer    =   0;
        for ( auto &fCurrent_ChannelTiming : hChannelDeltaTiming.at(iTer-1) )  {
            jTer++;
            //
            //  Draw Timing
            TCanvas*    cDrawChannelTiming =   new TCanvas("","",800,800);
            fCurrent_ChannelTiming->Draw();
            fCurrent_ChannelTiming->Write();
            cDrawChannelTiming->SaveAs(Form("./Plots/Chip_%i/ChannelTiming_Chip_%i_Channel_%i.pdf",iTer-1,iTer-1,jTer-1));
            cDrawChannelTiming->SaveAs(Form("./Plots/Chip_%i/ChannelTiming_Chip_%i_Channel_%i.png",iTer-1,iTer-1,jTer-1));
            
            //
            //  Fit for Resolution and Time Offset
            fCurrent_ChannelTiming->Fit("gaus","IMEQ");
            auto    fFitResults =   fCurrent_ChannelTiming->GetFunction("gaus");
            if ( !fFitResults ) continue;
            hTimeResoltutionMap.at(iTer-1)  ->  SetBinContent((jTer-1)%4+1,(jTer-1)/4+1,fFitResults->GetParameter(2));
            hTimeOffSetMap.at(iTer-1)       ->  SetBinContent((jTer-1)%4+1,(jTer-1)/4+1,fFitResults->GetParameter(1));
        }
        cDrawInclusiveHitMap->cd(iTer);
        gStyle->SetOptStat(0);
        fCurrent_InclusiveHitMap->Draw("COLZ TEXT");
        fCurrent_InclusiveHitMap->Write();
        fCurrent_InclusiveHitMap->SetTitle(Form("INC HIT MAP CHIP %i",iTer-1));
        cDrawTimingHitMap->cd(iTer);
        gStyle->SetOptStat(0);
        hDeltaTimeHitMap.at(iTer-1)->Draw("COLZ TEXT");
        hDeltaTimeHitMap.at(iTer-1)->Write();
        hDeltaTimeHitMap.at(iTer-1)->SetTitle(Form("INC HIT MAP CHIP %i",iTer-1));
        cDrawTimeResolutionMap->cd(iTer);
        gStyle->SetOptStat(0);
        hTimeResoltutionMap.at(iTer-1)->Draw("COLZ TEXT");
        hTimeResoltutionMap.at(iTer-1)->Write();
        hTimeResoltutionMap.at(iTer-1)->SetTitle(Form("INC HIT MAP CHIP %i",iTer-1));
        cDrawTimeOffsetMap->cd(iTer);
        gStyle->SetOptStat(0);
        hTimeOffSetMap.at(iTer-1)->Draw("COLZ TEXT");
        hTimeOffSetMap.at(iTer-1)->Write();
        hTimeOffSetMap.at(iTer-1)->SetTitle(Form("INC HIT MAP CHIP %i",iTer-1));
    }
    cDrawInclusiveHitMap    ->SaveAs("./Plots/All_Chips/InclusiveHitMap.pdf");
    cDrawInclusiveHitMap    ->SaveAs("./Plots/All_Chips/InclusiveHitMap.png");
    cDrawTimingHitMap       ->SaveAs("./Plots/All_Chips/TimingHitMap.pdf");
    cDrawTimingHitMap       ->SaveAs("./Plots/All_Chips/TimingHitMap.png");
    cDrawTimeResolutionMap  ->SaveAs("./Plots/All_Chips/TimeResolutionMap.pdf");
    cDrawTimeResolutionMap  ->SaveAs("./Plots/All_Chips/TimeResolutionMap.png");
    cDrawTimeOffsetMap      ->SaveAs("./Plots/All_Chips/TimeOffsetMap.pdf");
    cDrawTimeOffsetMap      ->SaveAs("./Plots/All_Chips/TimeOffsetMap.png");
    gROOT       ->      SetBatch(kFALSE);
}
