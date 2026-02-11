#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <chrono>
#include <boost/program_options.hpp>
#include "uhal/uhal.hpp"
#include "alcor/alcor.hh"

#define VERSION 0x30220131

#define ERROR "\033[1;31m"
#define RESET "\033[0m"

namespace po = boost::program_options;

bool running = true;

struct program_options_t {
  po::variables_map vm;
  std::string connection, device, output;
  int mode, chip, lane, filter, usleep, staging;
  int opmode, threshold, vth, range, offset1, delta_threshold, gain1;
  float integrated;
  bool skip_user_settings, verbose;
};

void sigint_handler(int signum);
void process_program_options(int argc, char *argv[], program_options_t &opt);
void write_buffer_to_file(std::ofstream &fout, int buffer_source, int buffer_counter, char *buffer, int buffer_size);

void
sigint_handler(int signum) {
  std::cout << " --- infinite loop terminate requested" << std::endl;
  running = false;
}

void
process_program_options(int argc, char *argv[], program_options_t &opt)
{
  /** process arguments **/
  po::options_description desc("Options");
  try {
    desc.add_options()
      ("help"             , "Print help messages")
      ("connection"       , po::value<std::string>(&opt.connection)->required(), "IPbus XML connection file")
      ("device"           , po::value<std::string>(&opt.device)->default_value("kc705"), "Device ID")
      ("output"           , po::value<std::string>(&opt.output), "Output data filename prefix")
      ("mode"             , po::value<int>(&opt.mode)->default_value(3), "Run mode")
      ("chip"             , po::value<int>(&opt.chip)->required(), "ALCOR chip")
      ("lane"             , po::value<int>(&opt.lane)->required(), "ALCOR lane number")
      ("filter"           , po::value<int>(&opt.filter)->default_value(0xf), "Filter mode")
      ("usleep"           , po::value<int>(&opt.usleep)->default_value(10), "Microsecond sleep")
      ("integrated"       , po::value<float>(&opt.integrated)->default_value(1.), "Quit after integrated seconds")
      ("staging"          , po::value<int>(&opt.staging)->default_value(1048576), "Staging buffer size (bytes)")      
      ;
    
    po::store(po::parse_command_line(argc, argv, desc), opt.vm);
    po::notify(opt.vm);
    
    if (opt.vm.count("help")) {
      std::cout << desc << std::endl;
      exit(1);
    }
  }
  catch(std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    std::cout << desc << std::endl;
    exit(1);
  }

}

void
write_buffer_to_file(std::ofstream &fout, int buffer_source, int buffer_counter, char *buffer, int buffer_size)
{
  uint32_t header[4];
  header[0] = 0x123caffe;
  header[1] = buffer_source;
  header[2] = buffer_counter;
  header[3] = buffer_size;
  fout.write((char *)&header, 16);
  fout.write((char *)buffer, buffer_size);
}

int main(int argc, char *argv[])
{

  /** register signal handlers **/
  signal(SIGINT, sigint_handler);
  //  signal(SIGALRM, sigalrm_handler);
  

  /** process program options **/
  program_options_t opt;
  process_program_options(argc, argv, opt);

  auto device_id_str = opt.device;
  int device_id = 0;
  if (device_id_str.rfind("kc705-", 0) == 0) {
    device_id_str = device_id_str.substr(6);
    device_id = std::atoi(device_id_str.c_str());
  }      
  
  /** initialise ipbus **/
  uhal::disableLogging();
  uhal::ConnectionManager connection_manager("file://" + opt.connection);
  uhal::HwInterface hardware = connection_manager.getDevice(opt.device);

  /** initialise DAQ interface **/
  alcor::daq_t daq;
  auto &alcor = daq.alcor[opt.chip];
  daq.init(hardware);
  int lane = opt.lane;
  int fifo_id = 4 * opt.chip + lane;

  /** make sure run mode is off **/
  daq.regfile.mode->write(0);
  hardware.dispatch();
  
  /** retrieve firmware version **/
  auto fwrev_register = daq.regfile.fwrev->read();
  hardware.dispatch();
  auto fwrev = fwrev_register.value();

  /** set filter mode **/
  auto filter_command = 0x03300000 | opt.filter;
  std::cout << " --- setting ALCOR filter mode: " << opt.filter << std::endl;
  alcor.service.controller->write(filter_command);
  hardware.dispatch();
  auto controller_register = alcor.service.controller->read();
  hardware.dispatch();
  auto controller_value = controller_register.value();
  if (controller_value != filter_command) {
    std::cout << " [ERROR] filter command mismatch: " << std::hex << "0x" << controller_value << " != 0x" << filter_command << std::dec << std::endl;
    return 1;
  }
  
  /** prepare staging buffers and pointers **/
  char *staging_buffer = nullptr;
  char *staging_buffer_pointer = nullptr;
  int staging_buffer_bytes = 0;
  staging_buffer = new char[opt.staging];
  staging_buffer_pointer = staging_buffer;

  std::ofstream fout;
  bool write_output = !opt.output.empty();
  if (write_output) {
    /** open alcor output file **/
    std::string filename = opt.output + ".chip_" + std::to_string(opt.chip) + ".lane_" + std::to_string(opt.lane) + ".alcor.dat";
    std::cout << " --- opening output file: " << filename << std::endl;
    fout.open(filename, std::ofstream::out | std::ofstream::binary);
    if (!fout.is_open()) {
      std::cout << " --- cannot open output file: " << filename << std::endl;
      return 1;
    }
    /** write firmware info and other stuff in file header **/
    auto timestamp = std::time(nullptr);
    uint32_t header[32] = {0x0};
    header[0] = 0x000caffe;
    header[1] = VERSION; // readout version
    header[2] = fwrev; // firmware version
    header[3] = 0; // run number
    header[4] = timestamp; // timestamp
    header[5] = opt.staging; // staging size
    header[6] = 0; // run mode
    header[7] = opt.filter; // filter mode
    header[8] = device_id; // device id
    fout.write((char *)&header, 64);
  }
  
  /** ALCOR init and reset **/
  //  std::string alcor_init_system = std::string(std::getenv("ALCOR_DIR")) + "/control/alcorInit.sh 666 /tmp > /dev/null 2>&1";
  //  std::string alcor_reset_system = std::string(std::getenv("ALCOR_DIR")) + "/control/alcorInit.sh 0 /tmp > /dev/null 2>&1";
  std::string alcor_init_system = "/au/pdu/measure/alcorInit.sh " + opt.device + " 666 /tmp > /dev/null 2>&1";
  std::string alcor_reset_system = alcor_init_system;
  //  std::cout << " --- ALCOR init call: " << alcor_init_system << std::endl;
  //  system(alcor_init_system.c_str());
  
  /** run mode on **/
  daq.regfile.mode->write(1);
  hardware.dispatch();

  /** DAQ loop **/
  long long int buffer_counter = 0;
  long long int integrated_resets = 0, integrated_polls = 0, integrated_downloads = 0, integrated_occupancy = 0, integrated_bytes = 0, integrated_timer = 0, integrated_rollover = 0;
  bool need_reset = true, fifo_overflow = false, staging_overflow = false;
  
  long long int n_polls = 0, n_downloads = 0;

#if 0
  /** reset fifo **/
  while (true) {
    usleep(opt.usleep);
    auto fifo_occupancy = alcor.fifo[lane].occupancy->read();
    hardware.dispatch();
    auto fifo_occupancy_value = fifo_occupancy.value() & 0xffff;
    if (fifo_occupancy_value == 0) break;    
    std::cout << "occupancy = " << fifo_occupancy_value << ": need reset " << std::endl;
    alcor.fifo[lane].reset->write(0x1);
    hardware.dispatch();
  }
#endif

  /** reset fifo **/
  alcor.fifo[lane].reset->write(0x1);
  hardware.dispatch();
   
  /** set run mode running **/
  daq.regfile.mode->write(opt.mode);
  hardware.dispatch();

  /** sleep until we reach integrated time **/
  std::cout << " --- sleep until we reach integrated time: " << opt.integrated << std::endl;
  while (true) {

    /** sleep at least a few useconds **/
    usleep(opt.usleep);

    /** read timer **/
    auto fifo_timer = alcor.fifo[lane].timer->read();
    hardware.dispatch();
    auto fifo_timer_value = fifo_timer.value();

    /** stop if we are beyond timer **/
    if (fifo_timer_value > opt.integrated * 31250000.) {
      integrated_timer = fifo_timer_value;
      break;
    }
    
  }


  /** set run mode idle **/
  daq.regfile.mode->write(1);
  hardware.dispatch();
    
  /** reset staging buffers **/
  staging_buffer_pointer = staging_buffer;
  staging_buffer_bytes = 0;
    
  /** loop till FIFO is empty **/
  std::cout << " --- download data" << std::endl;
  while (true) {
    
    /** sleep at least a few useconds **/
    usleep(opt.usleep);
    
    /** read occupancy **/
    auto fifo_occupancy = alcor.fifo[lane].occupancy->read();
    hardware.dispatch();
    auto fifo_occupancy_value = fifo_occupancy.value() & 0xffff;
    integrated_polls++;
    n_polls++;
    
    /** check if there is data in the FIFO **/
    std::cout << " --- fifo occupancy = " << fifo_occupancy_value << std::endl;
    if (fifo_occupancy_value == 0) break;
    
    /** download data **/
    auto fifo_data = alcor.fifo[lane].data->readBlock(fifo_occupancy_value);
    auto fifo_bytes = fifo_occupancy_value * 4;
    hardware.dispatch();
    integrated_downloads++;
    n_downloads++;
    
    /** count rollover **/
    for (int i = 0; i < fifo_data.value().size(); ++i)
      if (fifo_data.value()[i] == 0x5c5c5c5c) integrated_rollover++;
    
    /** check if chip is broken **/
    if (false && fifo_occupancy_value > 0) {
      auto last_data = fifo_data.value()[fifo_occupancy_value - 1];
      if ((last_data & 0x000000ff) == 0) {
	std::cout << ERROR << " --- chip is broken, need reset: " << std::hex << "0x" << last_data << std::dec << RESET << std::endl;
	integrated_resets++;
	need_reset = true;
	break;
      }
    }
    
    /** check if staging buffer overflow **/
    if (staging_buffer_bytes + fifo_bytes > opt.staging) {
      std::cout << ERROR << " --- staging buffer overflow " << RESET << std::endl;
      staging_overflow = true;
      break;
    }
    
    /** copy data into staging buffer **/
    std::memcpy((char *)staging_buffer_pointer, (char *)fifo_data.data(), fifo_bytes);
    staging_buffer_pointer += fifo_bytes;
    staging_buffer_bytes += fifo_bytes;
    
  }

  /** flush staging buffer **/
  std::cout << " --- flushing staging buffer: " << staging_buffer_bytes << std::endl;
  write_buffer_to_file(fout, fifo_id, buffer_counter, staging_buffer, staging_buffer_bytes);
  buffer_counter++;

  /** set run mode off **/
  hardware.getNode("regfile.mode").write(0);
  hardware.dispatch();

  /** monitor **/
  auto integrated_seconds = (float)integrated_timer / 31250000.;
  std::cout << std::endl;
  std::cout << " integrated: " << integrated_polls << " polls " << std::endl
	    << "             " << integrated_downloads << " downloads " << std::endl
	    << "             " << integrated_resets << " resets " << std::endl
	    << "             " << integrated_occupancy << " words " << std::endl
	    << "             " << integrated_bytes << " bytes " << std::endl
	    << "             " << integrated_rollover << " rollover " << std::endl
	    << "             " << integrated_seconds << " seconds " << std::endl;
  std::cout << std::endl;
  std::cout << "      rates: " << (float)integrated_polls / integrated_seconds << " polls/s" << std::endl
	    << "             " << (float)integrated_downloads / integrated_seconds << " downloads/s" << std::endl
	    << "             " << (float)integrated_resets / integrated_seconds << " resets/s" << std::endl
	    << "             " << (float)integrated_occupancy / integrated_seconds << " words/s" << std::endl
	    << "             " << (float)integrated_bytes / integrated_seconds << " bytes/s" << std::endl
	    << "             " << (float)integrated_rollover / integrated_seconds << " rollover/s" << std::endl;
  std::cout << std::endl;

  return 0;
}
  
