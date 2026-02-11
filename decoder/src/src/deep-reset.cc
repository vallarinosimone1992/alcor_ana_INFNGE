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

namespace po = boost::program_options;

struct program_options_t {
  po::variables_map vm;
  std::string connection, device;
  int chip, lane, usleep;
};

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
      ("chip"             , po::value<int>(&opt.chip)->required(), "ALCOR chip")
      ("usleep"           , po::value<int>(&opt.usleep)->default_value(10), "Microsecond sleep")
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

int main(int argc, char *argv[])
{

  /** process program options **/
  program_options_t opt;
  process_program_options(argc, argv, opt);

  /** initialise ipbus **/
  uhal::disableLogging();
  uhal::ConnectionManager connection_manager("file://" + opt.connection);
  uhal::HwInterface hardware = connection_manager.getDevice(opt.device);

  /** initialise DAQ interface **/
  alcor::daq_t daq;
  auto &alcor = daq.alcor[opt.chip];
  daq.init(hardware);

  auto start = std::chrono::high_resolution_clock::now();
  
  /** deep reset all fifos **/
  uhal::ValWord<uint32_t> fifo_occupancy[4];
  int fifo_occupancy_value[4];
  int ntry = 0;
  while (ntry < 100) {
    for (int ilane = 0; ilane < 4; ++ilane)
      alcor.fifo[ilane].reset->write(0x1);
    hardware.dispatch();
    usleep(opt.usleep);
    bool need_reset = false;
    for (int ilane = 0; ilane < 4; ++ilane)
      fifo_occupancy[ilane] = alcor.fifo[ilane].occupancy->read();
    hardware.dispatch();
    for (int ilane = 0; ilane < 4; ++ilane) {
      fifo_occupancy_value[ilane] = fifo_occupancy[ilane].value() & 0xffff;
      if (fifo_occupancy_value[ilane] > 1000) {
	//	std::cout << " lane " << ilane << " was not empty: " << fifo_occupancy_value[ilane] << std::endl;
	need_reset = true;
      }
    }
    if (!need_reset) break;
    ++ntry;
  }

  /** download any residual crap if something is left **/
  for (int ilane = 0; ilane < 4; ++ilane) {
    if (fifo_occupancy_value[ilane] > 0)
      auto fifo_data = alcor.fifo[ilane].data->readBlock(fifo_occupancy_value[ilane]);
    hardware.dispatch();
  }
  
  /** reset all fifos once more **/
  for (int ilane = 0; ilane < 4; ++ilane)
    alcor.fifo[ilane].reset->write(0x1);
  hardware.dispatch();

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  //  std::cout << "full reset done in " << ntry << " cycles: " << duration << " microseconds." << std::endl;

  

  return 0;
}
  
