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
  std::string connection_filename, device_id, output;
  int chip_mask, channel, vth, range, offset1, min_counts, max_timer, min_timer, usleep, udelay;
  bool skip_user_settings, verbose;
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
      ("chip_mask"        , po::value<int>(&opt.chip_mask)->required(), "ALCOR chip mask")
      ("channel"          , po::value<int>(&opt.channel)->required(), "ALCOR channel number")
      ("vth"              , po::value<int>(&opt.vth)->default_value(0), "ALCOR threshold offset")
      ("range"            , po::value<int>(&opt.range)->default_value(0), "ALCOR threshold range")
      ("offset1"          , po::value<int>(&opt.offset1)->default_value(-1), "ALCOR baseline offset")
      ("min_counts"       , po::value<int>(&opt.min_counts)->default_value(100), "Minimum number of counts")
      ("min_timer"        , po::value<int>(&opt.min_timer)->default_value(32000), "Minimum number of timer")
      ("max_timer"        , po::value<int>(&opt.max_timer)->default_value(32000000), "Maximum number of timer")
      ("output"           , po::value<std::string>(&opt.output)->default_value("scan_thr.txt"), "Output filename")
      ("usleep"           , po::value<int>(&opt.usleep)->default_value(1000000), "Microsecond sleep")
      ("udelay"           , po::value<int>(&opt.udelay)->default_value(0), "Microsecond delay between measurements")
      ("skip_user_settings"          , po::bool_switch(&opt.skip_user_settings), "Skip user settings")
      ("verbose"          , po::bool_switch(&opt.verbose), "Verbose mode")
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

  alcor::alcor_t alcor[6];
  bool chip_active[6] = {false};
  int n_active_chip_lanes = 0;

  printf(" --- received chip mask %08x \n", opt.chip_mask);
  for (int chip = 0; chip < 6; ++chip) {
    if ((1 << chip) & opt.chip_mask) {
      std::cout << " --- chip " << chip << " is active" << std::endl;
      chip_active[chip] = true;
      n_active_chip_lanes += 4;
    }
  }

  for (int chip = 0; chip < 6; ++chip) {
    if (!chip_active[chip]) continue;
    alcor[chip].init(hardware, chip);
  }

  alcor::bcr0246_union_t bcr0246;
  alcor::bcr1357_union_t bcr1357;
  alcor::pcr2_union_t pcr2[6][4], pcr2_save[6][4];
  alcor::pcr3_union_t pcr3_temp, pcr3[6][4], pcr3_save[6][4];

  // save the coordinates
  int channel[4], pix[4], col[4], add2[4], add3[4]; 
  for (int chip = 0; chip < 6; ++chip) {
    if (!chip_active[chip]) continue;
    for (int lane = 0; lane < 4; ++lane) {
      channel[lane] = opt.channel + 8 * lane;
      pix[lane] = channel[lane] % 4;
      col[lane] = channel[lane] / 4;
      add2[lane] = PCR(2, pix[lane], col[lane]);
      add3[lane] = PCR(3, pix[lane], col[lane]);
    }
  }

  // save pcr2 / pcr3 at start 
  for (int chip = 0; chip < 6; ++chip) {
    if (!chip_active[chip]) continue;
    for (int lane = 0; lane < 4; ++lane) {
      pcr2_save[chip][lane].val = alcor[chip].spi.read(add2[lane]);
      pcr3_save[chip][lane].val = alcor[chip].spi.read(add3[lane]);
    }  
  }

  // switch off all channels
  for (int chip = 0; chip < 6; ++chip) {
    if (!chip_active[chip]) continue;
    for (int ich = 0; ich < 32; ++ich) {
      int ipix = ich % 4;
      int icol = ich / 4;
      pcr3_temp.val = alcor[chip].spi.read(PCR(3, ipix, icol));
      pcr3_temp.reg.OpMode = 0x0;
      alcor[chip].spi.write(PCR(3, ipix, icol), pcr3_temp.val);
    }
  }

  // restore initial settings and apply user setting on top of initial values, if not requested to skip them
  for (int chip = 0; chip < 6; ++chip) {
    if (!chip_active[chip]) continue;
    for (int lane = 0; lane < 4; ++lane) {
      pcr2[chip][lane].val = pcr2_save[chip][lane].val;
      pcr3[chip][lane].val = pcr3_save[chip][lane].val;
      if (!opt.skip_user_settings) {
	pcr2[chip][lane].reg.LEDACVth = opt.vth;
	pcr2[chip][lane].reg.LEDACrange = opt.range;
	pcr3[chip][lane].reg.Offset1 = opt.offset1;
      }
    }
  }

  uhal::ValWord<uint32_t> fifo_occupancy[6][4], fifo_timer[6][4];
  uhal::ValVector<uint32_t> fifo_data[6][4];

  std::ofstream fout(opt.output);
  fout << "chip/I:channel/I:threshold/I:range/I:vth/I:offset/I:rate/F:ratee/F" << std::endl;  

  hardware.getNode("regfile.mode").write(3);
  hardware.dispatch();

  int sum_occupancy[6][4] = {0};
  int sum_timer[6][4] = {0};
  bool lane_continue[6][4] = {false};
  bool lane_done[6][4] = {false};
  bool lane_broken[6][4] = {false};
  int nbroken = 0;

  // loop over threshold
  for (int threshold = 0x3f; threshold >- 0; --threshold) {
    
    if (opt.verbose)
      std::cout << "--- new threshold setup ---" << std::endl;
    
    // switch off
    for (int chip = 0; chip < 6; ++chip) {
    if (!chip_active[chip]) continue;
      for (int lane = 0; lane < 4; ++lane) {
	pcr3[chip][lane].reg.OpMode = 0;
	alcor[chip].spi.write(add3[lane], pcr3[chip][lane].val);
      }
    }

    // set threshold
    for (int chip = 0; chip < 6; ++chip) {
      if (!chip_active[chip]) continue;
      for (int lane = 0; lane < 4; ++lane) {
	pcr2[chip][lane].reg.LE1DAC = threshold;
	alcor[chip].spi.write(add2[lane], pcr2[chip][lane].val);
	if (opt.verbose) {
	  std::cout << " --- setting PCR2 on chip " << chip << ", lane " << lane << std::endl;
	  pcr2[chip][lane].reg.print();
	}
      }
    }

    // switch on
    for (int chip = 0; chip < 6; ++chip) {
      if (!chip_active[chip]) continue;
      for (int lane = 0; lane < 4; ++lane) {
	pcr3[chip][lane].reg.OpMode = 1;
	alcor[chip].spi.write(add3[lane], pcr3[chip][lane].val);
	if (opt.verbose) {
	  std::cout << " --- setting PCR3 on chip " << chip << ", lane " << lane << std::endl;
	  pcr3[chip][lane].reg.print();
	}
      }
    }

    // reset counters
    for (int chip = 0; chip < 6; ++chip) {
      if (!chip_active[chip]) continue;
      for (int lane = 0; lane < 4; ++lane) {
	sum_occupancy[chip][lane] = 0;
	sum_timer[chip][lane] = 0;
	lane_continue[chip][lane] = false;
      }
    }

    usleep(opt.udelay);
    
    // reset/read loop until minimum counts/timer achieved
    while (true) {

      // reset fifo lanes
      for (int chip = 0; chip < 6; ++chip) {
	if (!chip_active[chip]) continue;
	for (int lane = 0; lane < 4; ++lane) {
	  if (lane_broken[chip][lane]) continue;
	  alcor[chip].fifo[lane].reset->write(0x1);
	  if (opt.verbose) {
	    std::cout << " --- resetting fifo on chip " << chip << ", lane " << lane << std::endl;
	  }
	}
      }
      hardware.dispatch();

      // sleep at least a few useconds
      usleep(opt.usleep);

      // read fifo lane occupancy
      for (int chip = 0; chip < 6; ++chip) {
	if (!chip_active[chip]) continue;
	for (int lane = 0; lane < 4; ++lane) {
	  if (lane_broken[chip][lane]) continue;
	  fifo_occupancy[chip][lane] = alcor[chip].fifo[lane].occupancy->read();
	  fifo_timer[chip][lane] = alcor[chip].fifo[lane].timer->read();
	  if (opt.verbose) {
	    std::cout << " --- reading fifo occupancy and timer on chip " << chip << ", lane " << lane << std::endl;
	  }
	}
      }
      hardware.dispatch();

      // read data from the lane
      for (int chip = 0; chip < 6; ++chip) {
	if (!chip_active[chip]) continue;
	for (int lane = 0; lane < 4; ++lane) {
	  if (lane_broken[chip][lane]) continue;
	  if (fifo_occupancy[chip][lane].value() & 0xffff > 0) {
	    fifo_data[chip][lane] = alcor[chip].fifo[lane].data->readBlock(fifo_occupancy[chip][lane]);
	    if (opt.verbose) {
	      std::cout << " --- reading fifo data on chip " << chip << ", lane " << lane << " (" << fifo_occupancy[chip][lane] << ")" <<  std::endl;
	    }
	  }
	}
      }
      hardware.dispatch();
      
      // check if lane is broken
      for (int chip = 0; chip < 6; ++chip) {
	if (!chip_active[chip]) continue;
	for (int lane = 0; lane < 4; ++lane) {
	  if (lane_broken[chip][lane]) continue;
	  if (!fifo_data[chip][lane].valid()) continue;
	  auto last = fifo_data[chip][lane].value()[fifo_data[chip][lane].size() - 1];
	  if ((last & 0x000000ff) == 0) {
	    printf(" --- chip %d lane %d is broken: 0x%08x \n", chip, lane, last);
	    lane_broken[chip][lane] = true;
	    nbroken++;
	    break;
	  }
	}
      }

      // increment counters
      for (int chip = 0; chip < 6; ++chip) {
	if (!chip_active[chip]) continue;
	for (int lane = 0; lane < 4; ++lane) {
	  if (lane_broken[chip][lane]) continue;
	  sum_occupancy[chip][lane] += (fifo_occupancy[chip][lane].value() & 0xffff);
	  sum_timer[chip][lane] += fifo_timer[chip][lane].value();
	  
	  if (sum_timer[chip][lane] < opt.min_timer) {
	    //	  std::cout << lane << " below min_timer " << std::endl;
	    lane_continue[chip][lane] = true;
	  }
	  if (sum_timer[chip][lane] >= opt.max_timer) {
	    //	  std::cout << lane << " above max_timer " << std::endl;
	    lane_continue[chip][lane] = false;
	  }
	  if (sum_occupancy[chip][lane] >= opt.min_counts) {
	    //	  std::cout << lane << " above min_counts " << std::endl;
	    lane_continue[chip][lane] = false;
	  }
	  
	  //	  std::cout << chip << " " << channel[lane] << " " << sum_timer[lane] << " " << sum_occupancy[lane] << std::endl;
	}
      }
      
      // check number of broken lanes
      if (nbroken >= n_active_chip_lanes) {
	std::cout << " --- all chip lanes are broken, quit " << std::endl;
	break;
      }
      
      // check if all lanes are done
      bool all_lane_done = true;
      for (int chip = 0; chip < 6; ++chip) {
	if (!chip_active[chip]) continue;
	for (int lane = 0; lane < 4; ++lane) {
	  if (lane_broken[chip][lane]) continue;
	  if (lane_continue[chip][lane]) {
	    all_lane_done = false;
	    break;
	  }
	}    
      }  
      if (all_lane_done) break;
      
    }

    // check number of broken lanes
    if (nbroken >= n_active_chip_lanes) {
      //      std::cout << " --- all lanes are broken, quit " << std::endl;
      break;
    }
    
    // write output to file
    for (int chip = 0; chip < 6; ++chip) {
      if (!chip_active[chip]) continue;
      for (int lane = 0; lane < 4; ++lane) {
	if (lane_broken[chip][lane]) continue;
	double counts = sum_occupancy[chip][lane];
	double period = sum_timer[chip][lane] / 32.e6;
	
	if (opt.verbose) {
	  std::cout << chip << " "
		    << channel[lane] << " "
		    << pcr2[chip][lane].reg.LE1DAC << " "
		    << pcr2[chip][lane].reg.LEDACrange << " "
		    << pcr2[chip][lane].reg.LEDACVth << " " 
		    << pcr3[chip][lane].reg.Offset1 << " " 
		    << counts << " " << period << std::endl;
	}	

	fout << chip << " "
	     << channel[lane] << " "
	     << pcr2[chip][lane].reg.LE1DAC << " "
	     << pcr2[chip][lane].reg.LEDACrange << " "
	     << pcr2[chip][lane].reg.LEDACVth << " " 
	     << pcr3[chip][lane].reg.Offset1 << " " 
	     << counts / period << " " 
	     << std::sqrt(counts) / period << std::endl;
	
      }
    }
    
  }
  
  hardware.getNode("regfile.mode").write(0);
  hardware.dispatch();
  
  fout.close();
  
  return 0;
}
  
