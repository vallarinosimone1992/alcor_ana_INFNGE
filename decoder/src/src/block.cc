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

struct program_options_t {
  std::string connection_filename, device_id, node_name;
  int size, usleep;
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
      ("node"             , po::value<std::string>(&opt.node_name)->required(), "Name of the node")
      ("size"             , po::value<int>(&opt.size)->required(), "Block size to read")
      ("usleep"           , po::value<int>(&opt.usleep)->default_value(1000), "Sleep between write repetition")
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
  const uhal::Node &node = hardware.getNode(opt.node_name);
  
  auto data = node.readBlock(opt.size);
  hardware.dispatch();
  for (int i = 0; i < data.value().size(); ++i)
    std::cout << std::setfill('0') << std::setw(8) << std::right << std::hex << data.value()[i] << std::endl;
  
  return 0;
}
