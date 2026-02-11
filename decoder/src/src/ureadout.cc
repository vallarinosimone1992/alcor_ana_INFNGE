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
  int chip, channel, filter, usleep, occupancy, timer, staging;
  int opmode, threshold, vth, range, offset1, delta_threshold, gain1;
  int max_resets;
  float integrated;
  bool skip_user_settings, verbose, noinit;
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
      ("chip"             , po::value<int>(&opt.chip)->required(), "ALCOR chip")
      ("channel"          , po::value<int>(&opt.channel)->required(), "ALCOR channel number")
      ("filter"           , po::value<int>(&opt.filter)->default_value(0xf), "Filter mode")
      ("usleep"           , po::value<int>(&opt.usleep)->default_value(10), "Microsecond sleep")
      ("occupancy"        , po::value<int>(&opt.occupancy)->default_value(4096), "FIFO occupancy to download")
      ("timer"            , po::value<int>(&opt.timer)->default_value(31250000), "Spill length")
      ("max_resets"       , po::value<int>(&opt.max_resets)->default_value(100), "Maximum number of allowed resets")
      ("integrated"       , po::value<float>(&opt.integrated)->default_value(1.), "Quit after integrated seconds")
      ("staging"          , po::value<int>(&opt.staging)->default_value(4194304), "Staging buffer size (bytes)")      
      ("opmode"           , po::value<int>(&opt.opmode)->default_value(0x1), "ALCOR channel operation mode")
      ("threshold"        , po::value<int>(&opt.threshold), "ALCOR threshold value")
      ("delta_threshold"  , po::value<int>(&opt.delta_threshold), "ALCOR threshold delta")
      ("vth"              , po::value<int>(&opt.vth), "ALCOR threshold offset")
      ("range"            , po::value<int>(&opt.range), "ALCOR threshold range")
      ("offset1"          , po::value<int>(&opt.offset1), "ALCOR baseline offset")
      ("gain1"            , po::value<int>(&opt.gain1), "ALCOR TIA gain")
      ("noinit"           , po::bool_switch(&opt.noinit), "Do not run ALCOR init")
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
  int lane = opt.channel / 8;
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
  char *staging_buffer = nullptr, *staging_buffer_trg = nullptr;
  char *staging_buffer_pointer = nullptr, *staging_buffer_trg_pointer = nullptr;
  int staging_buffer_bytes = 0, staging_buffer_trg_bytes = 0;
  staging_buffer = new char[opt.staging];
  staging_buffer_pointer = staging_buffer;
  staging_buffer_trg = new char[opt.staging];
  staging_buffer_trg_pointer = staging_buffer_trg;

  std::ofstream fout, fout_trg;
  bool write_output = !opt.output.empty();
  if (write_output) {
    /** open alcor output file **/
    std::string filename = opt.output + ".chip_" + std::to_string(opt.chip) + ".channel_" + std::to_string(opt.channel) + ".alcor.dat";
    std::cout << " --- opening output file: " << filename << std::endl;
    fout.open(filename, std::ofstream::out | std::ofstream::binary);
    if (!fout.is_open()) {
      std::cout << " --- cannot open output file: " << filename << std::endl;
      return 1;
    }
    /** open trigger output file **/
    std::string filename_trg = opt.output + ".chip_" + std::to_string(opt.chip) + ".channel_" + std::to_string(opt.channel) + ".trigger.dat";
    std::cout << " --- opening output file: " << filename_trg << std::endl;
    fout_trg.open(filename_trg, std::ofstream::out | std::ofstream::binary);
    if (!fout_trg.is_open()) {
      std::cout << " --- cannot open output file: " << filename_trg << std::endl;
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
    fout_trg.write((char *)&header, 64);
  }
  
  /** ALCOR init and reset **/
  //  std::string alcor_init_system = std::string(std::getenv("ALCOR_DIR")) + "/control/alcorInit.sh 666 /tmp > /dev/null 2>&1";
  //  std::string alcor_reset_system = std::string(std::getenv("ALCOR_DIR")) + "/control/alcorInit.sh 0 /tmp > /dev/null 2>&1";
  std::string alcor_init_system = std::string(std::getenv("ALCOR_DIR")) +
    "/measure/alcor_fast_init_readout.sh " + std::to_string(opt.chip) + " " + std::to_string(opt.channel) + " > /dev/null 2>&1";

  alcor_init_system = "/au/pdu/control/alcor_fast_init_readout.sh " +
    opt.device + " " +
    std::to_string(opt.chip) + " " +
    std::to_string(opt.channel) +
    " > /dev/null 2>&1";

  /** run ALCOR init unless hinibited **/
  std::string alcor_reset_system = alcor_init_system;
  if (!opt.noinit) {
    std::cout << " --- ALCOR init call: " << alcor_init_system << std::endl;
    system(alcor_init_system.c_str());
  }

  /** read PCR2 and PCR3 register **/
  alcor::pcr2_union_t pcr2, pcr2_init;
  alcor::pcr3_union_t pcr3, pcr3_init;
  int pixel = opt.channel % 4;
  int column = opt.channel / 4;
  pcr2.val = pcr2_init.val = alcor.spi.read(PCR(2, pixel, column));
  pcr3.val = pcr3_init.val = alcor.spi.read(PCR(3, pixel, column));

  /** set threshold values **/
  if (opt.vm.count("threshold")) pcr2.reg.LE1DAC = opt.threshold;
  if (opt.vm.count("vth")) pcr2.reg.LEDACVth = opt.vth;
  if (opt.vm.count("range")) pcr2.reg.LEDACrange = opt.range;
  if (opt.vm.count("offset1")) pcr3.reg.Offset1 = opt.offset1;
  if (opt.vm.count("gain1")) pcr3.reg.Gain1 = opt.gain1;
  pcr3.reg.OpMode = opt.opmode;

  /** increment with delta_threshold **/
  if (opt.vm.count("delta_threshold")) {
    if (pcr2.reg.LE1DAC + opt.delta_threshold > 63) {
      std::cout << " --- LE1DAC value overflow: " << pcr2.reg.LE1DAC + opt.delta_threshold << std::endl;
      return 1; 
    }
    pcr2.reg.LE1DAC += opt.delta_threshold;
  }
  
  /** run mode on **/
  daq.regfile.mode->write(1);
  hardware.dispatch();
  
  /** switch on channel and write PCR2 and PCR3 **/
  pcr3.reg.OpMode = opt.opmode;
  alcor.spi.write(PCR(2, pixel, column), pcr2.val);
  alcor.spi.write(PCR(3, pixel, column), pcr3.val);
    
  /** print PCR2 and PCR3 registers **/
  std::cout << " --- status of PCR2 and PRC3 registers before running " << std::endl;
  pcr2.reg.print();
  pcr3.reg.print();

  /** DAQ loop **/
  long long int buffer_counter = 0;
  long long int integrated_resets = 0, integrated_polls = 0, integrated_downloads = 0, integrated_occupancy = 0, integrated_bytes = 0, integrated_timer = 0, integrated_rollover = 0;
  bool need_reset = false, fifo_overflow = false, staging_overflow = false;
  
  long long int n_polls = 0, n_downloads = 0;
  while (running) {

    if (integrated_timer > opt.integrated * 31250000. ||
	integrated_resets > opt.max_resets) break;
    
    /** reset ALCOR if needed **/
    if (need_reset)
      system(alcor_reset_system.c_str());

    /** reset flags **/
    need_reset = false;
    fifo_overflow = false;
    staging_overflow = false;

    n_polls = 0, n_downloads = 0;

    /** reset staging buffers **/
    staging_buffer_pointer = staging_buffer;
    staging_buffer_bytes = 0;
    staging_buffer_trg_pointer = staging_buffer_trg;
    staging_buffer_trg_bytes = 0;

    /** reset fifo **/
    alcor.fifo[lane].reset->write(0x1);
    daq.trigger.reset->write(0x1);
    hardware.dispatch();

    /** set run mode running **/
    std::cout << " >>> start of spill " << std::endl;
    daq.regfile.mode->write(3);
    hardware.dispatch();
    
    /** loop till min occupancy / timer reached **/
    while (true) {
      
      /** sleep at least a few useconds **/
      usleep(opt.usleep);
      
      /** read occupancy and timer **/
      auto fifo_occupancy = alcor.fifo[lane].occupancy->read();
      auto fifo_occupancy_trg = daq.trigger.occupancy->read();
      auto fifo_timer = alcor.fifo[lane].timer->read();
      hardware.dispatch();
      auto fifo_occupancy_value = fifo_occupancy.value() & 0xffff;
      auto fifo_occupancy_trg_value = fifo_occupancy_trg.value() & 0xffff;
      auto fifo_timer_value = fifo_timer.value();
      integrated_polls++;
      n_polls++;

      /** stop if we are beyond timer **/
      if (fifo_timer_value > opt.timer) break;
      
      /** check if we need to download data right now **/
      if (fifo_occupancy_value < opt.occupancy &&
	  fifo_occupancy_trg_value < opt.occupancy) continue;

      //      std::cout << " fifo_time_value: " << fifo_timer_value << std::endl;
      
      /** download data **/
      auto fifo_data = alcor.fifo[lane].data->readBlock(fifo_occupancy_value);
      auto fifo_bytes = fifo_occupancy_value * 4;
      auto fifo_trg_data = daq.trigger.data->readBlock(fifo_occupancy_trg_value);
      auto fifo_trg_bytes = fifo_occupancy_trg_value * 4;
      hardware.dispatch();
      integrated_downloads++;
      n_downloads++;
      
      /** check if chip is broken **/
      if (fifo_occupancy_value > 0) {
	auto last_data = fifo_data.value()[fifo_occupancy_value - 1];
	if ((last_data & 0x000000ff) == 0) {
	  std::cout << ERROR << " --- chip is broken, need reset: " << std::hex << "0x" << last_data << std::dec << RESET << std::endl;
	  integrated_resets++;
	  need_reset = true;
	  break;
	}
      }

      /** check if FIFO overflow **/
      if (fifo_occupancy_value >= 8191 ||
	  fifo_occupancy_trg_value >= 8191 ) {
	std::cout << ERROR << " --- fifo buffer overflow " << RESET << std::endl;
	fifo_overflow = true;
	break;
      }

      /** check if staging buffer overflow **/
      if (staging_buffer_bytes + fifo_bytes > opt.staging ||
	  staging_buffer_trg_bytes + fifo_trg_bytes > opt.staging) {
	std::cout << ERROR << " --- staging buffer overflow " << RESET << std::endl;
	staging_overflow = true;
	break;
      }
      
      /** copy data into staging buffer **/
      std::memcpy((char *)staging_buffer_pointer, (char *)fifo_data.data(), fifo_bytes);
      staging_buffer_pointer += fifo_bytes;
      staging_buffer_bytes += fifo_bytes;
      std::memcpy((char *)staging_buffer_trg_pointer, (char *)fifo_trg_data.data(), fifo_trg_bytes);
      staging_buffer_trg_pointer += fifo_trg_bytes;
      staging_buffer_trg_bytes += fifo_trg_bytes;

      /** count rollover **/
      for (int i = 0; i < fifo_data.value().size(); ++i)
	if (fifo_data.value()[i] == 0x5c5c5c5c) integrated_rollover++;
    
      /** check if we integrated sufficient time in this spill **/
      if (fifo_timer_value >= opt.timer) break;
      
    } /** end of loop till min occupancy / timer reached **/


    /** set run mode idle **/
    std::cout << " >>> end of spill " << std::endl;
    daq.regfile.mode->write(1);
    hardware.dispatch();
    
    /** stop here if data is corrupted already **/
    if (need_reset || fifo_overflow || staging_overflow) continue;

    /** read timer **/
    auto fifo_timer = alcor.fifo[lane].timer->read();
    hardware.dispatch();
    auto fifo_timer_value = fifo_timer.value();
    integrated_polls++;
    n_polls++;
    
    /** download all leftover data **/
    std::cout << " --- downloading leftover data " << std::endl;
    while (true) {
    
      /** read occupancy **/
      auto fifo_occupancy = alcor.fifo[lane].occupancy->read();
      auto fifo_occupancy_trg = daq.trigger.occupancy->read();
      auto fifo_timer = alcor.fifo[lane].timer->read();
      hardware.dispatch();
      auto fifo_occupancy_value = fifo_occupancy.value() & 0xffff;
      auto fifo_occupancy_trg_value = fifo_occupancy_trg.value() & 0xffff;
      auto fifo_timer_value = fifo_timer.value();
      std::cout << " --- fifo occupancy: " << fifo_occupancy_value << std::endl;
      
      if (fifo_occupancy_value == 0) break;
    
      /** download data **/
      auto fifo_data = alcor.fifo[lane].data->readBlock(fifo_occupancy_value);
      auto fifo_bytes = fifo_occupancy_value * 4;
      auto fifo_trg_data = daq.trigger.data->readBlock(fifo_occupancy_trg_value);
      auto fifo_trg_bytes = fifo_occupancy_trg_value * 4;
      hardware.dispatch();
      integrated_downloads++;
      n_downloads++;

#if 0 // R+FIXME
      /** check that we got the spill trailer **/
      if (fifo_occupancy_value > 2) {
	auto data = fifo_data.value()[fifo_occupancy_value - 2]; // last two is spill trailer, must go back
	if ( !(data & 0xf0000000) ) {
	  std::cout << ERROR << " --- there is no spill trailer in the buffer " << RESET << std::endl;
	  continue;
	}
      }
#endif // R+FIXME

#if 0 // R+FIXME
      /** check if chip is broken **/
      if (fifo_occupancy_value > 2) {
	auto last_data = fifo_data.value()[fifo_occupancy_value - 3]; // last two is spill trailer, must go back
	if ((last_data & 0x000000ff) == 0) {
	  std::cout << ERROR << " --- chip is broken, need reset: " << std::hex << "0x" << last_data << std::dec << RESET << std::endl;
	  integrated_resets++;
	  need_reset = true;
	  continue;
	}
      }
#endif // R+FIXME
      
      /** check if FIFO overflow **/
      if (fifo_occupancy_value >= 8191 ||
	  fifo_occupancy_trg_value >= 8191 ) {
	std::cout << ERROR << " --- fifo buffer overflow " << RESET << std::endl;
	continue;
      }
      
      /** check if staging buffer overflow **/
      if (staging_buffer_bytes + fifo_bytes > opt.staging ||
	  staging_buffer_trg_bytes + fifo_trg_bytes > opt.staging) {
	std::cout << ERROR << " --- staging buffer overflow " << RESET << std::endl;
	continue;
      }
      
      /** copy data into staging buffer **/
      std::memcpy((char *)staging_buffer_pointer, (char *)fifo_data.data(), fifo_bytes);
      staging_buffer_pointer += fifo_bytes;
      staging_buffer_bytes += fifo_bytes;
      std::memcpy((char *)staging_buffer_trg_pointer, (char *)fifo_trg_data.data(), fifo_trg_bytes);
      staging_buffer_trg_pointer += fifo_trg_bytes;
      staging_buffer_trg_bytes += fifo_trg_bytes;

      /** count rollover **/
      for (int i = 0; i < fifo_data.value().size(); ++i)
	if (fifo_data.value()[i] == 0x5c5c5c5c) integrated_rollover++;
    
    } /** end of download all leftover data **/

    /** flush staging buffer **/
    std::cout << " --- flushing staging buffer: " << staging_buffer_bytes << std::endl;
    write_buffer_to_file(fout, fifo_id, buffer_counter, staging_buffer, staging_buffer_bytes);
    std::cout << " --- flushing staging buffer TRG: " << staging_buffer_trg_bytes << std::endl;
    write_buffer_to_file(fout_trg, 24, buffer_counter, staging_buffer_trg, staging_buffer_trg_bytes);
    buffer_counter++;

    /** increment counters **/
    integrated_occupancy += staging_buffer_bytes / 4;
    integrated_bytes += staging_buffer_bytes;
    integrated_timer += fifo_timer_value;

    std::cout << " --- timer counter was: " << fifo_timer_value << std::endl;
    //    auto trg_data0 = fifo_trg_data.value()[fifo_occupancy_trg_value - 2];
    //    printf(" 0x%08x \n", trg_data0);
    //    auto trg_data1 = fifo_trg_data.value()[fifo_occupancy_trg_value - 1];
    //    printf(" 0x%08x \n", trg_data1);
    std::cout << " --- n_polls: " << n_polls << " | n_downloads: " << n_downloads << std::endl;

  }

  /** switch off channel **/
  pcr3.reg.OpMode = 0x0;
  alcor.spi.write(PCR(3, pixel, column), pcr3.val);
  
  /** set run mode off **/
  hardware.getNode("regfile.mode").write(0);
  hardware.dispatch();

  /** restore PCR2 and PCR3 **/
  alcor.spi.write(PCR(2, pixel, column), pcr2_init.val);
  alcor.spi.write(PCR(3, pixel, column), pcr3_init.val);
    
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

#if 0
  if (write_output) {
    /** flush staging buffer before closing **/
    std::cout << " --- flushing staging buffer: " << staging_buffer_bytes << std::endl;
    write_buffer_to_file(fout, fifo_id, buffer_counter, staging_buffer, staging_buffer_bytes);
    staging_buffer_pointer = staging_buffer;
    staging_buffer_bytes = 0;
    std::cout << " --- flushing staging buffer TRG: " << staging_buffer_trg_bytes << std::endl;
    write_buffer_to_file(fout_trg, 24, buffer_counter, staging_buffer_trg, staging_buffer_trg_bytes);
    staging_buffer_trg_pointer = staging_buffer_trg;
    staging_buffer_trg_bytes = 0;
    buffer_counter++;       
    /** close output file **/
    fout.close();
  }
#endif
  
  return 0;
}
  
