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

struct program_options_t {
  std::string connection_filename, device_id;
  std::vector<std::string> write_value;
  int eccr, bcr, pcr, pixel, column;
  int chip;
};

void process_program_options(int argc, char *argv[], program_options_t &opt);

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
      ("chip"             , po::value<int>(&opt.chip)->required(), "ALCOR chip number")
      ("eccr"             , po::value<int>(&opt.eccr)->default_value(-1), "ALCOR ECCR register")
      ("bcr"              , po::value<int>(&opt.bcr)->default_value(-1), "ALCOR BCR register")
      ("pcr"              , po::value<int>(&opt.pcr)->default_value(-1), "ALCOR PCR register")
      ("pixel"            , po::value<int>(&opt.pixel)->default_value(0), "ALCOR PCR pixel register")
      ("column"           , po::value<int>(&opt.column)->default_value(0), "ALCOR PCR column register")
      ("write"            , po::value<std::vector<std::string>>(&opt.write_value)->multitoken(), "Write a value before reading")
      ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    if (vm.count("help")) {
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

  program_options_t opt;
  process_program_options(argc, argv, opt);

  uhal::disableLogging();
  uhal::ConnectionManager connection_manager("file://" + opt.connection_filename);
  uhal::HwInterface hardware = connection_manager.getDevice(opt.device_id);

  alcor::alcor_t alcor;
  alcor.init(hardware, opt.chip);

  if (!opt.write_value.empty()) {
    if (opt.eccr != -1) {      
      int data[4];
      for (int i = 0; i < 4; ++i) {
	std::cout << opt.write_value[i] << std::endl;
	std::stringstream ss;
	ss << std::hex << opt.write_value[i];
	ss >> data[i];
      }
      alcor.spi.write_block(ECCR(0), 4, (int *)data);
    }
#if 0
    if (opt.bcr != -1) alcor.spi.write(BCR(opt.bcr), value);
    if (opt.pcr != -1) alcor.spi.write(PCR(opt.pcr, opt.pixel, opt.column), value);
#endif
  }
  if (opt.eccr != -1) {
    int data[4];
    alcor.spi.read_block(ECCR(0), 4, (int *)data);
    for (int i = 0; i < 4; ++i)
      printf("ECCR(%d) = 0x%04x \n", i, data[i]);
  }
  if (opt.bcr != -1) printf("BCR(%d) = 0x%04x \n", opt.bcr, alcor.spi.read(BCR(opt.bcr)));
  if (opt.pcr != -1) printf("PCR(%d,%d,%d) = 0x%04x \n", opt.pcr, opt.pixel, opt.column, alcor.spi.read(PCR(opt.pcr, opt.pixel, opt.column)));

  return 0;

}
