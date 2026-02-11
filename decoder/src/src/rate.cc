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
  std::string connection_filename, device_id, tag;
  int chip, channel, max_timer, min_timer, min_counts, usleep, udelay, threshold, vth, range, offset1, delta_threshold, mode;
  bool skip_user_settings, verbose, dump;
};

struct alcor_hit_t {
  uint32_t fine   : 9;
  uint32_t coarse : 15;
  uint32_t tdc    : 2;
  uint32_t pixel  : 3;
  uint32_t column : 3;
  void print() {
    printf(" hit: col=%d pix=%d tdc=%d coarse=%d fine=%d \n", column, pixel, tdc, coarse, fine);
  }
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
      ("chip"             , po::value<int>(&opt.chip)->required(), "ALCOR chip")
      ("channel"          , po::value<int>(&opt.channel)->required(), "ALCOR channel number")
      ("min_timer"        , po::value<int>(&opt.min_timer)->default_value(3200000), "Minimum number of timer")
      ("min_counts"       , po::value<int>(&opt.min_counts)->default_value(0), "Minimum number of counts")
      ("max_timer"        , po::value<int>(&opt.max_timer)->default_value(32000000), "Maximum number of timer")
      ("usleep"           , po::value<int>(&opt.usleep)->default_value(1000), "Microsecond sleep")
      ("udelay"           , po::value<int>(&opt.udelay)->default_value(10000), "Microdelay sleep")
      ("threshold"        , po::value<int>(&opt.threshold)->default_value(-1), "ALCOR threshold value")
      ("delta_threshold"  , po::value<int>(&opt.delta_threshold)->default_value(0), "ALCOR threshold delta")
      ("vth"              , po::value<int>(&opt.vth)->default_value(-1), "ALCOR threshold offset")
      ("range"            , po::value<int>(&opt.range)->default_value(-1), "ALCOR threshold range")
      ("offset1"          , po::value<int>(&opt.offset1)->default_value(-1), "ALCOR baseline offset")
      ("tag"              , po::value<std::string>(&opt.tag), "Output print tag")
      ("mode"              , po::value<int>(&opt.mode)->default_value(3), "Readout mode")
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

  int lane = opt.channel / 8;
  
  alcor::pcr2_union_t pcr2, pcr2_init;
  alcor::pcr3_union_t pcr3, pcr3_init;  
  int pixel = opt.channel % 4;
  int column = opt.channel / 4;
  
  // construct channel mask and tag
  int chmask = 0xfc000000;
  int chtag = (pixel << 26) | (column << 29);
  
  // read PCR2 and PCR3
  pcr2.val = alcor.spi.read(PCR(2, pixel, column));
  pcr3.val = alcor.spi.read(PCR(3, pixel, column));

  // save them
  pcr2_init.val = pcr2.val;
  pcr3_init.val = pcr3.val;
  
  // set threshold values
  if (opt.threshold != -1) pcr2.reg.LE1DAC = opt.threshold;
  else {
    if (pcr2.reg.LE1DAC + opt.delta_threshold > 63)
      pcr2.reg.LE1DAC = 63;
    else
      pcr2.reg.LE1DAC += opt.delta_threshold;
  }
  if (opt.vth != -1) pcr2.reg.LEDACVth = opt.vth;
  if (opt.range != -1) pcr2.reg.LEDACrange = opt.range;
  if (opt.offset1 != -1) pcr3.reg.Offset1 = opt.offset1;

  // switch on channel
  pcr3.reg.OpMode = 0x1;

  // write PCR2 and PCR3
  alcor.spi.write(PCR(2, pixel, column), pcr2.val);
  alcor.spi.write(PCR(3, pixel, column), pcr3.val);
  
  // sleep at least a few useconds
  usleep(opt.udelay);
  
  // set run mode
  hardware.getNode("regfile.mode").write(1);
  hardware.dispatch();
  //  hardware.getNode("regfile.mode").write(0x20b);
  hardware.getNode("regfile.mode").write(opt.mode);
  hardware.dispatch();

  // reset
  alcor.fifo[lane].reset->write(0x1);
  hardware.dispatch();

  // reset/read loop until minimum counts/timer achieved
  int sum_occupancy = 0, sum_timer = 0, sum_loops = 0;
  bool broken = false, corrupted = false;
  while (sum_timer < opt.max_timer) {
    // reset
    //    alcor.fifo[lane].reset->write(0x1);
    //    hardware.dispatch();
    
    // sleep at least a few useconds
    usleep(opt.usleep);
    
    // read occupancy and times
    auto fifo_occupancy = alcor.fifo[lane].occupancy->read();
    auto fifo_timer = alcor.fifo[lane].timer->read();
    hardware.dispatch();
    auto fifo_occupancy_value = fifo_occupancy.value() & 0xffff;
    auto fifo_timer_value = fifo_timer.value();

    // check if broken and/or corrupted
    if (fifo_occupancy_value > 0) {
      auto fifo_data = alcor.fifo[lane].data->readBlock(fifo_occupancy_value);
      hardware.dispatch();

      for (int i = 0; i < fifo_occupancy_value; ++i) {
	//	printf(" %08x \n", fifo_data.value()[i]); // to be commented
	// check if corrupted
	if ( (fifo_data.value()[i] & chmask) != chtag ) {
	  //	  std::cout << " corrupted " << std::endl; // to be commented
	  //	  printf(" %08x != %08x \n", fifo_data.value()[i] & chmask, chtag); // to be commented
	  corrupted = true;
	  break;
	}
	// check if broken
	if ( (fifo_data.value()[i] & 0x000000ff) == 0 ) {
	  //	  std::cout << " broken " << std::endl; // to be commented
	  broken = true;
	  break;
	}
      }

      // check if broken
      if (broken || corrupted)
	break;

    }
    
    // increment counters if not broken
    sum_occupancy += fifo_occupancy_value;
    //    sum_timer += fifo_timer_value;
    sum_timer = fifo_timer_value;
    sum_loops++;

    //    std::cout << sum_timer << " " << sum_occupancy << std::endl;
    if (sum_occupancy >= opt.min_counts && sum_timer >= opt.min_timer) break;
    //    if (sum_timer >= opt.max_timer) break;
    

  }
  
  // set run mode
  hardware.getNode("regfile.mode").write(1);
  hardware.dispatch();
  hardware.getNode("regfile.mode").write(0);
  hardware.dispatch();

  // restore PCR2 and PCR3
  alcor.spi.write(PCR(2, pixel, column), pcr2_init.val);
  alcor.spi.write(PCR(3, pixel, column), pcr3_init.val);

  if (corrupted || sum_timer == 0) return broken;
  
  // print results
  auto counts = sum_occupancy;
  auto period = (float)sum_timer / 31.25e6;
  auto rate = (float)counts / period;
  auto ratee = (float)std::sqrt(counts) / period;
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::string tag = "";
  if (!opt.tag.empty()) tag = "_" + opt.tag;
  std::cout << " threshold" << tag << " = " << pcr2.reg.LE1DAC
	    << " timestamp" << tag << " = " << timestamp
	    << " counts" << tag << " = " << counts
	    << " period" << tag << " = " << period
	    << " rate" << tag << " = " << rate
	    << " ratee" << tag << " = " << ratee
    //	    << " loops" << tag << " = " << sum_loops
    //	    << " broken" << tag << " = " << broken
	    << std::endl;
  
  return broken;
}
  
