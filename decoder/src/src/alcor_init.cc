#define BCRregs  8
#define ECCRregs 4
#define PCRregs  4
#define ALCORch  32
#define LANES    4

#define encodeBCR0(cal,iTDC,iblatchTDC) ( (iblatchTDC<<0) | (iTDC << 2) | (cal<<7) )
#define encodeBCR1(ib_2,ib_3,ib_sF,S0,Bit_boost,Bit_cg) (  (Bit_cg << 0) | (Bit_boost << 5) | (S0 << 10) | (ib_sF << 11) | (ib_3 << 12) | (ib_2 << 14) )
#define encodePCR2(le2DAC,offset,rangeThr,threshold) ( (le2DAC&0x3F) << 10 | (offset&0x3) << 8 | (rangeThr&0x3) << 6 | (threshold&0x3F) << 0 )
#define encodePCR3(offset1,opMode,offset2,gain1,gain2,polarity) ( ((offset1&0x7)<<13) | ((opMode&0xF)<<9) | ((offset2&0x7) << 6) | ((gain1&0x3) << 4) | ((gain2&0x3) << 2) | ((polarity&0x1) << 1)) 

#define ThrMax        0x3F 
#define ThrOffsetMax  0x3
#define ThrRangeMax   0x3
#define ThrDefault    ThrMax
//                      LE1DAC             RANGE       OFFSET      LE2DAC
#define PCR2default  ( (ThrDefault << 0) | (3 << 6) | (0 << 8) | (ThrMax << 10) )
#define POLARITY_SELECT 0x2
uint16_t PCRdefault[4]={0x7777,0x8888,PCR2default,POLARITY_SELECT};

int loadPCRSetup(int chip, string PCRfile)
{
  fstream f;
  uint16_t pcrblock[PCRregs*ALCORch];
  cout<<"Loading PCR file: "<<PCRfile<<" for chip # "<<chip<<endl;
  f.open(PCRfile,ios::in);
  if (!f) {
    cout<<"File "<<PCRfile<<" not found - FATAL"<<endl;
    exit(EXIT_FAILURE);
  }
  for (string str;getline(f,str);) {
      if (str.at(0) != '#') {
	int ch,l2dac,offset,rangeThr,threshold,Offset1,OpMode,Offset2,Gain1,Gain2,Polarity;
	cout<<str<<endl;
	istringstream ss(str);
	ss>>ch;
	pcrblock[ch*4]=PCRdefault[0];
	pcrblock[ch*4+1]=PCRdefault[1];
	ss>>l2dac>>offset>>rangeThr>>threshold>>Offset1>>OpMode>>Offset2>>Gain1>>Gain2>>Polarity;
	// qui discutere con Roberto se vogliamo tenere la mask e il single channel...
	pcrblock[ch*4+2]=encodePCR2(l2dac,offset,rangeThr,threshold);
	pcrblock[ch*4+3]=encodePCR3(Offset1,OpMode,Offset2,Gain1,Gain2,Polarity);
      }
  }
  f.close();
  for (int i = 0; i<ALCORch ; i++) {
    cout<< "PCR "<<i<<":";
    int k=0;
    for (int j=i*4; k<PCRregs;k++,j++) cout<<" 0x"<<hex<<pcrblock[j];
    cout<<dec<<endl;
  }
  return EXIT_SUCCESS;
}

int loadBCRSetup(int chip, string BCRfile)
{
  fstream f;
  uint16_t bcrblock[BCRregs];
  cout<<"Loading BCR file: "<<BCRfile<<" for chip # "<<chip<<endl;
  f.open(BCRfile,ios::in);
  if (!f) {
    cout<<"File "<<BCRfile<<" not found - FATAL"<<endl;
    exit(EXIT_FAILURE);
  }
  for (string str;getline(f,str);) {
      if (str.at(0) != '#') {
	cout<<str<<endl;
	istringstream ss(str);
	int reg,ib_2, ib_3,ib_sF,S0,Bit_boost,Bit_cg;
	int cal,iTDC,iblatchTDC;
	ss>>reg;
	if (reg & 0x1) {
	  ss >> ib_2 >> ib_3 >> ib_sF >> S0 >> Bit_boost >> Bit_cg;
	  bcrblock[reg]=encodeBCR1(ib_2,ib_3,ib_sF,S0,Bit_boost,Bit_cg);
	} else {
	  ss >> cal >> iTDC >> iblatchTDC;
	  bcrblock[reg]=encodeBCR0(cal,iTDC,iblatchTDC);
	}
    }
  }
  f.close();
  for (int i = 0; i<BCRregs ; i++) cout<< "BCR "<<i<<": 0x"<< hex << bcrblock[i]<<endl;
  return 0;
}
