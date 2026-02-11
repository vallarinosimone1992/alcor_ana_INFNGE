#include <iostream>
#include <fstream>
#include <boost/program_options.hpp>
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <signal.h>
#include "uhal/uhal.hpp"

#define VERSION 0x20220824
#define MAX_FIFOS 25
#define MAX_ALCORS 6

struct shared_t {
  bool start_of_run;
  bool end_of_run;
  bool start_of_spill;
  bool end_of_spill;
  uint32_t mode;
  uint32_t status;
} *shared_data = nullptr;

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

struct program_options_t {
  std::string connection_filename, device_id, output_filename;
  int usleep_period, run_number, run_mode, filter_mode, fifo_mask;
  int staging_size, min_occupancy;
} opt;

//int *shared_memory;

bool running = true;
bool inspill = false;
bool inrun = false;
bool monitor = false;
bool overflow = false;

char *staging_buffer = nullptr;
char *staging_buffer_end = nullptr;

void
sigint_handler(int signum) {
  std::cout << " --- infinite loop terminate requested" << std::endl;
  running = false;
}

void
sigalrm_handler(int signum) {
  std::cout << " --- waiting for spill " << std::endl;
  alarm(1);
}

void
decode(int chip, int lane, char *buffer, int size, bool verbose = false)
{
  std::cout << " --- start decoding spill " << std::endl;

  auto word = (uint32_t *)buffer;
  auto nwords = size /= 4;
  auto last = word + nwords;

  /** first word must be spill header **/
  if ( *word & 0xf0000000 != 0x7000000) {
    printf (" 0x%08x -- spill header mismatch ", *word);
    return;
  }
  
  /** last word must be spill trailer **/
  if ( *(word + nwords - 2) & 0xf0000000 != 0xf0000000) {
    printf (" 0x%08x -- spill trailer mismatch ", *word);
    return;
  }

  /** buffer is safe, decode it **/
  word += 2;
  int rollover_counter = 0, hit_counter = 0;
  int channel_hit_counter[8] = {0};
  while (word < last) {

    /** spill trailer **/
    if ( *word & 0xf0000000 != 0xf0000000) {
      printf (" 0x%08x -- spill trailer ", *word);
      break;
    }
    
    /** rollover **/
    if (*word == 0x5c5c5c5c) {
      if (verbose) printf(" 0x%08x -- rollover (counter=%d) \n", *word, rollover_counter);
      ++rollover_counter;
      ++word;
      continue;
    }

    /** hit **/
    auto hit = (alcor_hit_t *)word;
    auto pixel = hit->pixel;
    auto column = hit->column;
    auto channel = pixel + 4 * column - 8 * lane;
    hit_counter++;
    if (channel >= 0 && channel < 8) channel_hit_counter[channel]++;
    ++word;
    
  }
  
  std::cout << " --- spill decode succesfull: "
	    << rollover_counter << " rollovers"
	    << " | "
	    << hit_counter << " hits"
	    << " | "
	    << (float)hit_counter / (float)rollover_counter * 9765.625 << " hits/s"
	    << std::endl;
  for (int ich = 0; ich < 8; ++ich)
    std::cout << " ---- channel " << ich << ": " << (float)channel_hit_counter[ich] / (float)rollover_counter * 9765.625 << " hits/s"
	      << std::endl;

  /** post ALCOR rates **/
  std::string alcor_post_system = std::getenv("ALCOR_DIR");
  alcor_post_system += "/measure/readout-box/post_rates.sh " + std::to_string(chip) + " " + std::to_string(lane);
  for (int ich = 0; ich < 8; ++ich) alcor_post_system += std::string(" ") + std::to_string((float)channel_hit_counter[ich] / (float)rollover_counter * 9765.625);
  alcor_post_system += std::string(" &");
  std::cout << " --- calling system: " << alcor_post_system << std::endl;
  system(alcor_post_system.c_str());
  
}

void
process_program_options(int argc, char *argv[], program_options_t &opt)
{
  /** process arguments **/
  namespace po = boost::program_options;
  po::options_description desc("Options");
  try {
    desc.add_options()
      ("help"             , "Print help messages")
      ("connection"       , po::value<std::string>(&opt.connection_filename)->required(), "IPbus XML connection file")
      ("device"           , po::value<std::string>(&opt.device_id)->default_value("kc705"), "Device ID")
      ("usleep"           , po::value<int>(&opt.usleep_period)->default_value(1000), "Microsecond sleep between polling cycles")
      ("fifo"             , po::value<int>(&opt.fifo_mask)->default_value(0x1ffffff), "FIFO mask")
      ("run"              , po::value<int>(&opt.run_number)->default_value(666), "Run number")
      ("mode"             , po::value<int>(&opt.run_mode)->default_value(0x3), "Run mode")
      ("filter"           , po::value<int>(&opt.filter_mode)->default_value(0x0), "Filter mode")
      ("staging"          , po::value<int>(&opt.staging_size)->default_value(1048576), "Staging buffer size (bytes)")
      ("occupancy"        , po::value<int>(&opt.min_occupancy)->default_value(4096), "FIFO minimum occupancy")
      ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
      std::cout << desc << std::endl;
      exit(1);
    }
  } catch (std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    std::cout << desc << std::endl;
    exit(1);
  }
}

int main(int argc, char *argv[])
{
  std::cout << " --- welcome to ALCOR pico-readout " << std::endl;
  process_program_options(argc, argv, opt);

  /** create an anonymous shared memory segment **/
  using namespace boost::interprocess;
  //  try{
  mapped_region region(anonymous_shared_memory(sizeof(shared_t)));
  void *shared_address = region.get_address();
  shared_data = new (shared_address) shared_t;
  //The segment is unmapped when "region" goes out of scope
  //   }
  //   catch(interprocess_exception &ex){
  //      std::cout << ex.what() << std::endl;
  //      return 1;
  //   }  

  /** fork a child for each requested FIFO **/
  int chip, lane;
  bool parent;
  for (int ififo = 0; ififo < MAX_FIFOS; ++ififo) {
    if ( !(opt.fifo_mask & 1 << ififo ) ) continue;
    chip = ififo / 4;
    lane = ififo % 4;
    child = fork() == 0;
    if (child) break; /** this is the child **/
  }
    
  /** initialise and retrieve hardware nodes **/
  uhal::disableLogging();
  uhal::ConnectionManager connection_manager("file://" + opt.connection_filename);
  uhal::HwInterface hardware = connection_manager.getDevice(opt.device_id);

  /** child wait for parent
  
  /** make sure run mode is off before starting **/
  //  auto status_node = &hardware.getNode("regfile.status");
  //  auto mode_node = &hardware.getNode("regfile.mode");
  //  auto status_register = status_node->read();
  //  auto mode_register = mode_node->read();
  //  hardware.dispatch();
  //  auto mode_value = mode_register.value();
  auto mode_value = shared_memory[0];
  if (mode_value != 0x0) {
    std::cout << " --- mode is not zero at startup: 0x"
	      << std::hex << mode_value << std::dec
	      << std::endl;
    exit(1);
  }

  /** data stuff **/
  auto occupancy_node = &hardware.getNode("alcor_readout_id" + std::to_string(opt.chip) + "_lane" + std::to_string(opt.lane) + ".fifo_occupancy");
  auto data_node = &hardware.getNode("alcor_readout_id" + std::to_string(opt.chip) + "_lane" + std::to_string(opt.lane) + ".fifo_data");
  
  /** open output file **/
  std::string filename = opt.output_filename + ".fifo_" + std::to_string(4 * opt.chip + opt.lane) + ".dat";
  std::cout << " --- opening output file: " << filename << std::endl;
  std::ofstream fout(filename, std::ofstream::out | std::ofstream::binary);
  if (!fout.is_open()) {
    std::cout << " --- cannot open output file: " << filename << std::endl;
    exit(1);
  }
  
  /** write firmware info and other stuff in file header **/
  auto timestamp = std::time(nullptr);
  uint32_t header[32] = {0x0};
  header[0] = 0x000caffe;
  //    header[1] = VERSION; // readout version
  //    header[2] = fwrev; // firmware version
  //    header[3] = opt.run_number; // run number
  //    header[4] = timestamp; // timestamp
  //    header[5] = opt.staging_size; // staging size 
  //    header[6] = opt.run_mode; // run mode
  //    header[7] = opt.filter_mode; // filter mode
  fout.write((char *)&header, 64);
  
  /** prepare staging buffer and pointer **/
  staging_buffer = new char[opt.staging_size];
  staging_buffer_end = staging_buffer + opt.staging_size;
  auto staging_buffer_pointer = staging_buffer;
  int buffer_counter = 0;
  
  /** register signal handlers **/
  signal(SIGINT, sigint_handler);
  signal(SIGALRM, sigalrm_handler);

  /** start infinite loop **/
  int n_polls = 0, max_occupancy = 0, integrated_words = 0;
  while (running) {

    /** usleep a bit **/
    usleep(opt.usleep_period);

    auto mode_value = shared_memory[0];
    auto status_value = shared_memory[1];
    
    /** check run status **/
    if (!inrun && (mode_value & 0x1)) {
      std::cout << " --- start of run " << std::endl;
      inrun = true;
    }
    if (inrun && !(mode_value & 0x1)) {
      std::cout << " --- end of run " << std::endl;
      inrun = false;
      running = false;
    }
    
    /** check detect start/end of spill **/
    bool status = mode_value & 0x4 ? status_value == 1 : mode_value & 0x2;
    if (inspill != status) {
      if (status) {
	alarm(0); // cancel alarm when spill goes up
	if (shared_memory[2] == 0) overflow = false; // cancel overflow only here
      }
      else monitor = true; // monitor when spill goes down
      inspill = status;
    }

    if (overflow || !inspill) continue;
    n_polls++;

    /** read fifo occupancy **/
    auto occupancy_register = occupancy_node->read();
    hardware.dispatch();
    auto occupancy_value = (occupancy_register.value() & 0xffff);
    if (occupancy_value <= opt.min_occupancy && !monitor) continue;
    if (occupancy_value > max_occupancy) max_occupancy = occupancy_value;

    /** read fifo data **/
    auto bytes = occupancy_value * 4;
    auto data_register = data_node->readBlock(occupancy_value);
    hardware.dispatch();
    integrated_words += occupancy_value;

    /** check space left on buffer **/
    if ( (int)(staging_buffer_end - staging_buffer_pointer) < bytes) {
      std::cout << " --- staging buffer overflow " << std::endl;
      overflow = true;
      continue;
    }
    
    /** stage data on buffer **/
    std::memcpy(staging_buffer_pointer, data_register.data(), bytes);
    staging_buffer_pointer += bytes;
    
    /** monitor **/
    if (monitor) {

      /** request ALCOR reset if overflow or 
	  too large occupancy detected **/
      if (overflow) {
	shared_memory[2] = 1;
	//	staging_buffer_pointer = staging_buffer; // write empty event
      }
      
      /** write staging buffer **/
      int buffer_size = (int)(staging_buffer_pointer - staging_buffer);
      uint32_t header[4];
      header[0] = 0x123caffe;
      header[1] = 4 * opt.chip + opt.lane;
      header[2] = buffer_counter++;
      header[3] = buffer_size;
      fout.write((char *)&header, 16);
      fout.write(staging_buffer, buffer_size);  

      /** printout monitor **/
      std::cout << " n_polls: " << n_polls
		<< " max_occupancy: " << max_occupancy
		<< " integrated_bytes: " << 4 * integrated_words
		<< std::endl;
      n_polls = max_occupancy = integrated_words = 0;
      monitor = overflow = false;

      /** decode **/
      decode(opt.chip, opt.lane, staging_buffer, buffer_size);
      
      staging_buffer_pointer = staging_buffer;
      alarm(1);
    }

  }

  /** close output file **/
  fout.close();
  std::cout << " --- output file closed: " << filename << std::endl;

  
  return 0;
}

