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
  std::string connection_filename, device_id, node_name, write_value;
  int repeat, usleep;
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
      ("write"            , po::value<std::string>(&opt.write_value), "Write a value before reading")
      ("repeat"           , po::value<int>(&opt.repeat)->default_value(1), "Number of write repetitions")
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

  if (!opt.write_value.empty()) {
    unsigned int value;   
    std::stringstream ss;
    ss << std::hex << opt.write_value;
    ss >> value;
    std::cout << " --- writing " << std::hex << value << std::dec << " to " << opt.node_name << std::endl;
    for (int irepeat = 0; irepeat < opt.repeat; ++irepeat) {
      node.write(value);
      hardware.dispatch();
      usleep(opt.usleep);
    }
    return 0;
  }

  uhal::ValWord<uint32_t> reg = node.read();
  hardware.dispatch();
  std::cout << "     " << opt.node_name << " = 0x" << std::hex << reg.value() << std::endl;

  return 0;
}
