/*

  Main program to monitor SEU during irradiation at Trento

  The program works with three concurrent threads:

  a)  - readout of output FIFOs: monitor of control words and roll-over frequency
      - check configuration bits every given seconds
  b) consolle (main): allows to add entries to log + give commands
  c) logger 

  TODO:
  - opzioni da aggiungere
  
*/  
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::system_clock;
using namespace std;

#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include "uhal/uhal.hpp"

#include "alcor/alcor.hh"

#define BCRregs  8
#define ECCRregs 4
#define PCRregs  4
#define ALCORch  32
#define LANES    4

#define START_OF_SPILL 0x70000000
#define END_OF_SPILL   0xF0000000
#define ROLL_OVER      0x5C5C5C5C
#define STATUS_HEADER  0x7C7C7C7C
#define CRC_HEADER     0x9C9C9C9C
#define FRAME_HEADER   0x1C1C1C1C

#define HWD       hardware.dispatch()

#define     BUFLEN    (1024*1024)    // 1 MWord = 4 MBytes  -- 
struct laneBuf_t {
  uint32_t write;
  uint32_t buf[BUFLEN];
};

struct alcorConf_t {
  int eccr[ECCRregs];
  int bcr[BCRregs];
  int pcr[PCRregs*ALCORch];
};

struct counters_t {
  long long int rl[LANES];
  long long int ff[LANES];
  long long int ro[LANES];
  long long int fh[LANES];
  long long int sw[LANES];
  long long int crc[LANES];
  long long int seu[LANES];
  long long int sos[LANES];
  long long int eos[LANES];
};

laneBuf_t alcd[LANES];

struct program_options_t {
  std::string connection_filename, device_id;
  int chip;
  int filter;
  bool dump;
};


// common variables
program_options_t opt;
counters_t cnt;
int fifo_occupancy_status[LANES]={4*0};
int rc = 0, eu = 0, pu = 0, bu =0;
long long int start_of_run = 0, uptime = 0;
fstream LogFile;
std::mutex bus;
atomic_bool stop_monitor_seu = false;
atomic_bool stop_logger = false;

int lsw[LANES]={4*0};


void printConf(alcorConf_t *);
void readConf(alcor::alcor_t alcor, alcorConf_t *conf);
void writeConf(alcor::alcor_t alcor, alcorConf_t *conf);
void process_program_options(int argc, char *argv[], program_options_t &opt);

void process_program_options(int argc, char *argv[], program_options_t &opt)
{
  /** process arguments **/
  namespace po = boost::program_options;
  po::options_description desc("Options");
  try {
    desc.add_options()
      ("help"             , "Print help messages")
      ("connection"       , po::value<std::string>(&opt.connection_filename)->required(), "IPbus XML connection file")
      ("device"           , po::value<std::string>(&opt.device_id)->default_value("kc705"), "Device ID")
      ("chip"             , po::value<int>(&opt.chip)->required(), "ALCOR chip number")
      ("filter"           , po::value<int>(&opt.filter)->default_value(20), "ALCOR FIFO filter bits")
      ("dump"             , po::value<bool>(&opt.dump)->default_value(false), "dump lane data (debug only)")
      ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    if (vm.count("help")) {
      std::cout << desc << std::endl;
      exit(EXIT_SUCCESS);
    }
  }
  catch(std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    std::cout << desc << std::endl;
    exit(EXIT_FAILURE);
  }
}


int openLog(string lf)
{
  int stat=EXIT_SUCCESS;
  LogFile.open(lf,ios::out);
  if (!LogFile) {
    cerr << "FATAL: Error while creating logFile"<<endl;
    stat=EXIT_FAILURE;
  } else {
    cout << "LogFile: "<<lf<<endl;
  }
  return stat;
}

void logFlush()
{
  LogFile.flush();
}
void log(string msg)
{
  time_t curr_time;
  tm * curr_tm;
  char tstring[40];
  time(&curr_time);
  curr_tm = localtime(&curr_time);
  strftime(tstring,40,"%d/%m/%Y-%T",curr_tm);
  LogFile<<tstring<<" "<<msg<<endl;
  //  cout<<tstring<<msg<<endl;
}  

void logCounters()
{
  std::stringstream ms;
  for (int i = 0; i<LANES; i++) {
    ms<<"Lane # "<<i<< " (r/o: "<<cnt.rl[i]<<" full: "<<cnt.ff[i]<<"): RO: "<<cnt.ro[i]<<" SOS: "<<cnt.sos[i]<<" EOS: "<<cnt.eos[i]<<" FH: "<<cnt.fh[i]<<" SW: "<<cnt.sw[i]<<" CRC: "<<cnt.crc[i]<<" SEU: "<<cnt.seu[i];
      log(ms.str()); ms.str(""); ms.clear();
  }
  double upt=(double)uptime;
  auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  ms << "Uptime reading data: " << uptime << " (ms) [Run duration: "<< (now-start_of_run)<< "(ms) Eff.: "<<upt*100./(double)(now-start_of_run)<<"%]";
  log(ms.str()); ms.str(""); ms.clear();
  ms << "Clock frequency from Roll Over (MHz): ";
  for (int i=0;i<LANES;i++) ms << (double)(cnt.ro[i]*(0x7FFF))/(upt*1000.)<<" ";
  log(ms.str()); ms.str(""); ms.clear();
  ms<<"CONF upsets summary so-far (# "<<rc<<" r/o): ECCR: "<<eu<<" BCR: "<<bu<<" PCR: "<<pu;
  log(ms.str()); ms.str(""); ms.clear();
  logFlush();
}

void logSession(program_options_t opt) {
  log("Session started");
  while (!stop_logger ) {
    sleep(20);
    logCounters();
  }
  log("logger exiting");
}

void checkConf(alcor::alcor_t alcor,alcorConf_t ref)
{
  alcorConf_t current;
  rc++;
  readConf(alcor,&current);
  int diff=bcmp((void *)&ref,(void*)&current,sizeof(alcorConf_t));
  if (diff != 0) {
      char msg[100];
      for (int i=0;i<ECCRregs;i++) {
	if (ref.eccr[i] != current.eccr[i]) {
	  eu++;
	  sprintf(msg,"!-SEU detected ECCR %d REF: 0x%x FOUND: 0x%x",i,ref.eccr[i],current.eccr[i]);
	  log(msg);
	}
      }
      for (int i=0;i<BCRregs;i++) {
	if (ref.bcr[i] != current.bcr[i]) {
	  bu++;
	  sprintf(msg,"!-SEU detected BCR %d REF: 0x%x FOUND: 0x%x",i,ref.bcr[i],current.bcr[i]);
	  log(msg);
	}
      }
      for (int i=0;i<PCRregs*ALCORch;i++) {
	if (ref.pcr[i] != current.pcr[i]) {
	  pu++;
	  sprintf(msg,"!-SEU! detected PCR %d REF: 0x%x FOUND: 0x%x",i,ref.pcr[i],current.pcr[i]);
	  log(msg);
	}
      }
      writeConf(alcor,&ref);
  }
}

int readFIFO(uhal::HwInterface hardware,alcor::alcor_t alcor)
{
  int nwr = 0;
  bool fifo_full = false;
  for (int lane=0;lane<LANES;lane++) {
      auto fifo_occupancy = alcor.fifo[lane].occupancy->read();
      HWD;
      auto fifo_occupancy_value = fifo_occupancy.value() & 0xFFFF;
      fifo_occupancy_status[lane]=fifo_occupancy_value;

      if (fifo_occupancy_value == 8193) {
	cnt.ff[lane]++;
	fifo_full = true ;
      }

      if (fifo_occupancy_value > 0) {
	auto fifo_data = alcor.fifo[lane].data->readBlock(fifo_occupancy_value);
	HWD;
	cnt.rl[lane]++;
	nwr+=fifo_occupancy_value;
	if ( (alcd[lane].write + fifo_occupancy_value ) > BUFLEN ) {
	  alcd[lane].write = 0;
	  cout<< "Resetting buf "<<lane;
	}
	auto ptr = &alcd[lane].buf[alcd[lane].write];
	std::memcpy(ptr,fifo_data.data(),fifo_occupancy_value*4);
	alcd[lane].write+=fifo_occupancy_value;
      }
  }
  
  /*
  if (fifo_full) return -1;
  else return nwr;
  */
  return nwr;
}  

void decodeFIFO()
{
  for (int lane = 0; lane < LANES; ++lane) {
    bool eosFound = false;
    bool sosFound = false;
    int spillCount = -1;
    int last = alcd[lane].write;
    uint32_t sos = alcd[lane].buf[0];
    if (opt.dump && lane == 0) {
      printf(" --------------- DUMP BUFFER \n");
      for (int i = 0; i<last ; ++i) {
	printf(" %d 0x%08X\n",i,alcd[lane].buf[i]);
      }
      printf(" --------------- END OF BUFFER \n");
    }
    if ( (sos & 0xF0000000) == 0x70000000 ) {
      spillCount = (sos & 0xFFF0000 ) >> 16;
      sosFound = true;
      cnt.sos[lane]++;
    }
    if (!sosFound) {
      cout<<"Missing SOS on lane "<<lane<<endl;
    }
    uint32_t expEos = 0xF0000000 | (sos&0xFFF0000);
    //    cout<<boost::format("Lane: %d SOS 0x%08X (lenght: %d)") % lane % sos % last;
    for (int i = last; i>1; --i) { 
      uint32_t eos = alcd[lane].buf[i];
      //      cout<<boost::format("     Lane: %d %d EOS 0x%08X \n") % lane %i %eos;
      if (eos == expEos) {last = i; eosFound = true; break; } 
    }
    if (!eosFound) {
      cout<<"Missing EOS on lane "<<lane<<"(Spill # "<<spillCount<<endl;
    } else {
      //      cout<<"EOS found at "<<last<<endl;
      cnt.eos[lane]++;
    }
    for (int i = 1;i<last;++i) {
      int sw = 0;
      uint32_t w = alcd[lane].buf[i];
      switch (w) {
	  case ROLL_OVER:
	    cnt.ro[lane]++;
	    sw=0;
	    break;
	  case FRAME_HEADER:
	    cnt.fh[lane]++;
	    sw=0;
	    break;
	  case CRC_HEADER:
	    cnt.crc[lane]++;
	    sw=0;
	    break;
	  case STATUS_HEADER:
	    cnt.sw[lane]++;
	    sw=1;
	    break;
	  default:
	    if ( (sw > 0) && ( sw < 9 ) ) {  // if we have pending Status Words
	      if ( (w & 0xF) != 0 ) {
		char msg[100];
		int col = ( w &0xE000  ) >> 29;
		int pix = ( w &0x1C00  ) >> 26;
		sprintf(msg,"!-SEU detected on Column %d Pixel %d [lane # %d cnt# %d cntSW %d] [SW: 0x%08X]",col,pix,lane,i,lsw[lane],w); log(msg);
		cnt.seu[lane]++;
	      }
	      sw++;
	      if (sw == 9) sw=0;
	    }
	    break;
      }
    }
    alcd[lane].write=0;
  }
}


void monitorSEU(uhal::HwInterface hardware,alcor::daq_t daq,alcor::alcor_t alcor, alcorConf_t ref)
{
  int nr;
  bool fifo_full = false;
  daq.regfile.mode->write(1); HWD;
  for (int i=0;i<LANES;i++) {
    alcor.fifo[i].reset->write(0x1);
    cnt.rl[i]=0; cnt.ff[i]=0;cnt.ro[i]=0;cnt.fh[i]=0;cnt.crc[i]=0;cnt.sw[i]=0;cnt.seu[i]=0;cnt.sos[i]=0;cnt.eos[i]=0;
    alcd[i].write=0;
  }
  HWD;
  log("SEU monitor started, now getting data");

  daq.regfile.mode->write(3); HWD;
  auto reference = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  start_of_run = reference;

  while (!stop_monitor_seu) {
    
    nr=readFIFO(hardware,alcor);

    if (nr == -1) {                    
      daq.regfile.mode->write(1); HWD;
      for (int i=0;i<LANES;i++) { alcor.fifo[i].reset->write(0x1); }
      HWD;
      daq.regfile.mode->write(3); HWD;
      fifo_full = true;
      log("FIFO FULL resetting FIFOs");
    }
    
    auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    if ( now - reference >= 1000) {     // time to check config
      daq.regfile.mode->write(1); HWD;  // we stop spill
      uptime+=(now - reference);
      checkConf(alcor,ref);
      int i = 0;
      while ( (readFIFO(hardware,alcor) > 0)  && ( i != 10) ) ++i;                 // read all remaining data
      if (i==10) for (int i=0;i<LANES;i++) alcor.fifo[i].reset->write(0x1); HWD;   // we reset FIFO if not empty
      if (opt.dump) cout<<" END OF SPILL "<<endl;
      if (fifo_full) { for (int i=0;i<LANES;i++) alcd[i].write=0; fifo_full = false; } // we discard the spill 
      else decodeFIFO();                // we decode all data from the spill
      daq.regfile.mode->write(3); HWD;  // we restart reading fifos
      reference = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
  }

  daq.regfile.mode->write(1); HWD;
  int i = 0;

  while ( (readFIFO(hardware,alcor) > 0)  && ( i != 10) ) ++i;
  if (!fifo_full) decodeFIFO();
  for (int i=0;i<LANES;i++) alcor.fifo[i].reset->write(0x1);
  HWD;
  log("SEU monitor exiting");
}


void printConf(alcorConf_t *conf)
{
  printf("ECCR  : "); for (int i = 0;i< ECCRregs; i++) printf("0x%04X ",conf->eccr[i]); printf("\n");
  printf("BCR   : "); for (int i = 0;i< BCRregs; i++) printf("0x%04X ",conf->bcr[i]); printf("\n");
  for (int ch = 0; ch < ALCORch; ch++) {
    printf("Ch. %02d: ",ch); for (int i = 0;i< PCRregs; i++) printf("0x%04X ",conf->pcr[i]);  printf("\n");
  }  
}

void readConf(alcor::alcor_t alcor, alcorConf_t *conf)
{
  alcor.spi.read_block(ECCR(0),ECCRregs,(int *)conf->eccr);
  alcor.spi.read_block(BCR(0),BCRregs,(int *)conf->bcr);
  alcor.spi.read_block(PCR(0,0,0),PCRregs*ALCORch,(int *)conf->pcr);
  //  printConf(conf);
}

void writeConf(alcor::alcor_t alcor, alcorConf_t *conf)
{
  alcor.spi.write_block(ECCR(0),ECCRregs,(int *)conf->eccr);
  alcor.spi.write_block(BCR(0),BCRregs,(int *)conf->bcr);
  alcor.spi.write_block(PCR(0,0,0),PCRregs*ALCORch,(int *)conf->pcr);
}


int main(int argc, char *argv[])
{
  string lnam(100,0);
  time_t t = time(nullptr);
  lnam.resize(strftime(&lnam[0],lnam.size(),"/home/daq/DATA/TIFPA/%Y%m%d.%H%M%S.log",localtime(&t)));
  cout<<"SEU Monitor:  Session log: "<<lnam<<endl;
  openLog("/tmp/test.log");     // substitute with lnam...

  process_program_options(argc, argv, opt);


  thread logger(logSession,opt);
  logger.detach();

  // open IPBUS connection with FPGA
  uhal::disableLogging();
  uhal::ConnectionManager connection_manager("file://" + opt.connection_filename);
  uhal::HwInterface hardware = connection_manager.getDevice(opt.device_id);

  alcor::daq_t daq;
  auto &alcor = daq.alcor[opt.chip];
  alcorConf_t conf;
  
  daq.init(hardware);
  daq.regfile.mode->write(0); //run mode off
  hardware.dispatch();

  auto filter_command = 0x03300000 | opt.filter;
  alcor.service.controller->write(filter_command);
  hardware.dispatch();

  auto controller_register = alcor.service.controller->read();
  hardware.dispatch();
  auto controller_value = controller_register.value();
  if (controller_value != filter_command) {
    std::cout << " [ERROR] filter command mismatch: " << std::hex << "0x" << controller_value << " != 0x" << filter_command << std::dec << std::endl;
    exit (EXIT_FAILURE);
  }


  readConf(alcor,&conf);
  cout<<"Target ALCOR (chip # "<<opt.chip<<") found with following configuration"<<endl;
  printConf(&conf);
  
  thread seuMonitor(monitorSEU,hardware,daq,alcor,conf);
  seuMonitor.detach();


  sleep(1);
  log("All required monitor threads started ");

  bool run_the_program = true;
  while (run_the_program) {
    cout<<">";
    string st,cmd;
    getline(cin,st);
    istringstream ss(st);
    ss>>cmd;
    boost::to_lower(cmd);
    if (cmd == "quit") {
      cout<<"quit command received"<<endl;
      log("quit command received");
      stop_monitor_seu=true;
      sleep(2);
      run_the_program = false;
      stop_logger=true;
    } else if (cmd == "conf") {
      log("conf command received");
      printConf(&conf);
    } else if (cmd == "fifo") {
      log("fifo command received");
      cout<<"Reading loop # ";
      for (int i=0;i<LANES;++i) cout<<cnt.rl[i]<<" ";
      cout<<"Occupancy: ";
      for (int i=0;i<LANES;++i) cout<<fifo_occupancy_status[i]<<" ";
      cout<<endl;
    } else if (cmd == "uptime") {
      log("uptime command received");
      double upt=(double)uptime*1000.;
      cout<<"Uptime reading data: "<<uptime<<"(ms) (Clock frequency from Roll Over (MHz): ";
      for (int i=0;i<LANES;i++) {
	double f = (double)(cnt.ro[i]*(0x7FFF))/upt;
	cout<<f<<" ";
      } cout<<")"<<endl;
    } else {
      log(st);
    }
  }
  logCounters();
  log("All done");
  LogFile.close();
  return 0;
}
