#include <iostream>
#include <fstream>
#include <string>
#include <boost/program_options.hpp>
#include "TFile.h"
#include "TH1F.h"
#include "TGraph.h"
#include "TTree.h"

bool verbose = false;
int integrated_rollover = 0;
int integrated_spill = 0;
int integrated_hits = 0;
int rollover_counter = 0;//-1; // we start from -1 becaue the very first word is a rollover
int frame = 0;

TGraph *gRollover = nullptr;
TGraph *gSpill = nullptr;

struct main_header_t {
  uint32_t caffe;
  uint32_t readout_version;
  uint32_t firmware_release;
  uint32_t run_number;
  uint32_t timestamp;
  uint32_t staging_size;
  uint32_t run_mode;
  uint32_t filter_mode;
  uint32_t device;
  uint32_t reserved1;
  uint32_t reserved2;
  uint32_t reserved3;
  uint32_t reserved4;
  uint32_t reserved5;
  uint32_t reserved6;
  uint32_t reserved7;
};

struct buffer_header_t {
  uint32_t caffe;
  uint32_t id;
  uint32_t counter;
  uint32_t size;
};

struct spill_t {
  uint32_t coarse   : 15;
  uint32_t rollover : 25;
  uint32_t zero     : 8;
  uint32_t counter  : 12;
  uint32_t id       : 4;
};

struct trigger_t {
  uint32_t coarse   : 15;
  uint32_t rollover : 25;
  uint32_t counter  : 16;
  uint32_t type     : 4;
  uint32_t id       : 4;
};

struct alcor_hit_t {
  uint32_t fine   : 9;
  uint32_t coarse : 15;
  uint32_t tdc    : 2;
  uint32_t pixel  : 3;
  uint32_t column : 3;
  void print() {
    printf(" hit: %d %d %d %d %d \n", column, pixel, tdc, coarse, fine);
  }
};

struct data_t {
  int device;
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

bool in_spill = false;

void write_data(TTree *tout, int device, int fifo, int type, int counter, int column, int pixel, int tdc, int rollover, int coarse, int fine)
{
  data.device = device;
  data.fifo = fifo;
  data.type = type;
  data.counter = counter;
  data.column = column;
  data.pixel = pixel;
  data.tdc = tdc;
  data.rollover = rollover;
  data.coarse = coarse;
  data.fine = fine;
  tout->Fill();
}

void write_trigger_data(TTree *tout, int device, int fifo, int type, int counter, int rollover, int coarse)
{
  write_data(tout, device, fifo, type, counter, -1, -1, -1, rollover, coarse, -1);
}

void write_alcor_data(TTree *tout, int device, int fifo, int column, int pixel, int tdc, int rollover, int coarse, int fine)
{
  write_data(tout, device, fifo, 1, -1, column, pixel, tdc, rollover, coarse, fine);
}
                
void decode_trigger(char *buffer, int device, int fifo, int size, TTree *tout)
{
  if (verbose) printf(" --- decode_trigger: device-%d fifo-%d, size=%d \n", device, fifo, size); 

  size /= 4;
  auto word = (uint32_t *)buffer;
  uint32_t pos = 0;

  while (pos < size) {

    /** spill header **/
    if ((*word & 0xf0000000) == 0x70000000) {
      uint32_t counter = (*word & 0x0fff0000) >> 16;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- spill header (counter=%d)\n", *word, counter);
      trigger_time = (uint64_t)(*word & 0xff) << 32;
      ++word; ++pos;
      if (verbose) printf(" 0x%08x -- spill header continued \n", *word);
      trigger_time |= *word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      write_trigger_data(tout, device, fifo, 7, counter, rollover, coarse);
      ++word; ++pos;
    }
    
    /** spill trailer **/
    else if ((*word & 0xf0000000) == 0xf0000000) {
      spill_t *spill = (spill_t *)word;
      uint32_t counter = (*word & 0x0fff0000) >> 16;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- spill trailer (counter=%d)\n", *word, counter);
      trigger_time = (uint64_t)(*word & 0xff) << 32;
      ++word; ++pos;
      if (verbose) printf(" 0x%08x -- spill trailer continued \n", *word);
      trigger_time |= *word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      gSpill->AddPoint(integrated_spill, trigger_time);
      integrated_spill++;
      write_trigger_data(tout, device, fifo, 15, counter, rollover, coarse);
      ++word; ++pos;
    }
    
    /** trigger **/
    else if ((*word & 0xf0000000) == 0x90000000) {
      trigger_t *trigger = (trigger_t *)word;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- trigger header\n", *word);
      trigger_time = (uint64_t)(*word & 0xff) << 32;
      uint32_t counter = (*word & 0xffff00) >> 16;
      ++word; ++pos;
      if (verbose) printf(" 0x%08x -- trigger header continued \n", *word);
      trigger_time |= *word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      write_trigger_data(tout, device, fifo, 9, counter, rollover, coarse);
      ++word; ++pos;
    }

    /** else **/
    else {
      printf(" 0x%08x -- unexpected word \n", *word);
      ++word; ++pos;
    }
    
  }

}

void decode(char *buffer, int device, int fifo, int size, TTree *tout, bool is_filtered)
{
  size /= 4;
  auto word = (uint32_t *)buffer;
  alcor_hit_t *hit;
  uint32_t pos = 0;

  // loop over buffer data
  while (pos < size) {

    // find spill header if not in spill already
    while (!in_spill && pos < size) {
      
      /** spill header **/
      if ((*word & 0xf0000000) == 0x70000000) {
        uint32_t counter = (*word & 0x0fff0000) >> 16;
        uint64_t trigger_time = 0x0;
        if (verbose) printf(" 0x%08x -- spill header (counter=%d)\n", *word, counter);
        trigger_time = (uint64_t)(*word & 0xff) << 32;
        ++word; ++pos;
        if (verbose) printf(" 0x%08x -- spill header continued \n", *word);
        trigger_time |= *word;
        uint32_t coarse = trigger_time & 0x7fff;
        uint32_t rollover = trigger_time >> 15;
        write_trigger_data(tout, device, fifo, 7, counter, rollover, coarse);
        ++word; ++pos;
        in_spill = true;

	break;
      }

      /** something else **/
      if (verbose) {
	//	if (!in_spill)
	//	  printf(" 0x%08x -- filler (pos=%d)\n", *word, pos % 16);
	//	else 
	  printf(" 0x%08x -- \n", *word);
      }
      ++word; ++pos;
    }
    
    // find spill trailer
    while (pos < size) {

      /** killed fifo **/
      if (*word == 0x666caffe) {
        if (verbose) printf(" 0x%08x -- killed fifo \n", *word);
        write_trigger_data(tout, device, fifo, 15, -1, -1, -1);
        ++word; ++pos;
        in_spill = false;
	rollover_counter = 0;
        break;	
      }
      
      /** spill trailer **/
      if ((*word & 0xf0000000) == 0xf0000000) {
        spill_t *spill = (spill_t *)word;
        uint32_t counter = (*word & 0x0fff0000) >> 16;
        uint64_t trigger_time = 0x0;
        if (verbose) printf(" 0x%08x -- spill trailer (counter=%d)\n", *word, counter);
        trigger_time = (uint64_t)(*word & 0xff) << 32;
        ++word; ++pos;
        if (verbose) printf(" 0x%08x -- spill trailer continued \n", *word);
        trigger_time |= *word;
        uint32_t coarse = trigger_time & 0x7fff;
        uint32_t rollover = trigger_time >> 15;
        write_trigger_data(tout, device, fifo, 15, counter, rollover, coarse);
        ++word; ++pos;
        in_spill = false;
	gRollover->AddPoint(integrated_spill, rollover_counter);
	integrated_spill++;
	rollover_counter = 0;
	break;
      }

      /** rollover **/
      if (*word == 0x5c5c5c5c) {
        if (verbose) printf(" 0x%08x -- rollover (counter=%d) \n", *word, rollover_counter);
        ++rollover_counter;
	++integrated_rollover;
        ++word; ++pos;
        continue;
      }

      /** hit **/
      hit = (alcor_hit_t *)word;
      if (verbose) printf(" 0x%08x -- hit (coarse=%d, fine=%d, column=%d, pixel=%d --> channel=%d)\n", *word, hit->coarse, hit->fine, hit->column, hit->pixel, hit->column * 4 + hit->pixel);
      write_alcor_data(tout, device, fifo, hit->column, hit->pixel, hit->tdc, rollover_counter, hit->coarse, hit->fine);
      integrated_hits++;
      ++word; ++pos;
      
    }
  }

}

int main(int argc, char *argv[])
{
  std::cout << " --- welcome to ALCOR decoder " << std::endl;

  std::string input_filename, output_filename;
  
  /** process arguments **/
  namespace po = boost::program_options;
  po::options_description desc("Options");
  try {
    desc.add_options()
      ("help"    , "Print help messages")
      ("input"   , po::value<std::string>(&input_filename)->required(), "Input data file")
      ("output"  , po::value<std::string>(&output_filename)->required(), "Output data file")
      ("verbose" , po::bool_switch(&verbose)->default_value(false), "Verbose mode flag")
      ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    if (vm.count("help")) {
      std::cout << desc << std::endl;
      return 1;
    }
  }
  catch(std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    std::cout << desc << std::endl;
    return 1;
  }

  /** open input file **/
  std::cout << " --- opening input file: " << input_filename << std::endl;
  std::ifstream fin;
  fin.open(input_filename, std::ofstream::in | std::ofstream::binary);
  
  /** read main header **/ 
  main_header_t main_header;
  fin.read((char *)&main_header, sizeof(main_header_t));
  if (main_header.caffe != 0x000caffe) {
    printf(" --- [ERROR] caffe header mismatch in main header: 0x%08x \n", main_header.caffe);
    return 1;
  }
  if (verbose) {
    printf(" --- [main header] caffe header detected: 0x%08x \n", main_header.caffe);
    printf(" --- [main header] readout version: 0x%08x \n", main_header.readout_version);
    printf(" --- [main header] firmware release: 0x%08x \n", main_header.firmware_release);
    printf(" --- [main header] run number: %d \n", main_header.run_number);
    printf(" --- [main header] timestamp: %d \n", main_header.timestamp);
    printf(" --- [main header] staging buffer size: %d \n", main_header.staging_size);
    printf(" --- [main header] run mode: 0x%1x \n", main_header.run_mode);
    printf(" --- [main header] filter mode: 0x%1x \n", main_header.filter_mode);
    printf(" --- [main header] device: %d \n", main_header.device);
    printf(" --- [main header] timestamp: %d \n", main_header.timestamp);
  }

  // check that we know how to decode it
  bool is_filtered;
  if (main_header.filter_mode == 0x0)
    is_filtered = false;
  else if (main_header.filter_mode == 0xf)
    is_filtered = true;
  else {
    printf(" --- [ERROR] filter mode not supported: 0x%01x \n", main_header.filter_mode);
    return 1;
  }
  
  // create reading buffer
  auto staging_size = main_header.staging_size;
  char *buffer = new char[staging_size];
  
  /** open output file **/
  std::cout << " --- opening output file: " << output_filename << std::endl;
  auto fout = TFile::Open(output_filename.c_str(), "RECREATE");
  auto tout = new TTree("alcor", "ALCOR");
  tout->Branch("device", &data.device, "device/I");
  tout->Branch("fifo", &data.fifo, "fifo/I");
  tout->Branch("type", &data.type, "type/I");
  tout->Branch("counter", &data.counter, "counter/I");
  tout->Branch("column", &data.column, "column/I");
  tout->Branch("pixel", &data.pixel, "pixel/I");
  tout->Branch("tdc", &data.tdc, "tdc/I");
  tout->Branch("rollover", &data.rollover, "rollover/I");
  tout->Branch("coarse", &data.coarse, "coarse/I");
  tout->Branch("fine", &data.fine, "fine/I");

  /** output histograms **/
  auto hCounters = new TH1F("hCounters", "", 3, 0, 3);
  gRollover = new TGraph;
  gSpill = new TGraph;
  
  /** loop over data **/
  buffer_header_t buffer_header;
  uint32_t word;
  while (true) {
    fin.read((char *)(&buffer_header), sizeof(buffer_header_t));
    if (fin.eof()) break;
    if (buffer_header.caffe != 0x123caffe) {
      printf(" --- [ERROR] caffe header mismatch in buffer header: %08x \n", buffer_header.caffe);
      break;
    }
    if (verbose) {
      printf(" --- [buffer header] caffe header detected: 0x%08x \n", buffer_header.caffe);
      printf(" --- [buffer header] buffer id: %d \n", buffer_header.id);
      printf(" --- [buffer header] buffer counter: %d \n", buffer_header.counter);
      printf(" --- [buffer header] buffer size: %d \n", buffer_header.size);
    }
    fin.read(buffer, buffer_header.size);

    if (buffer_header.id < 24) {
      if (verbose) printf(" --- decoding ALCOR FIFO \n");
      decode(buffer, main_header.device, buffer_header.id, buffer_header.size, tout, is_filtered);
    }
    else if (buffer_header.id == 24) {
      if (verbose) printf(" --- decoding TRIGGER FIFO \n");
      decode_trigger(buffer, main_header.device, buffer_header.id, buffer_header.size, tout);
    }
  }
  
  double integrated = (double)integrated_rollover * 0.0001024;
  std::cout << " --- integrated seconds: " << integrated << std::endl;

  /** write tree and close output */
  tout->Write();
  std::cout << " --- integrated spill: " << integrated_spill << std::endl;
  hCounters->SetBinContent(1, integrated_spill);
  hCounters->SetBinContent(2, integrated_rollover);
  hCounters->SetBinContent(3, integrated_hits);
  hCounters->Write();
  gRollover->Write("gRollover");
  gSpill->Write("gSpill");
  fout->Close();
  
  /** close input file **/
  fin.close();
  std::cout << " --- all done, so long " << std::endl;

  return 0;
}
