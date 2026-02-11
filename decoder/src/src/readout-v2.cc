#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <boost/program_options.hpp>
#include "uhal/uhal.hpp"
#include "alcor/alcor.hh"

#define PARANOID

#define VERSION 0x20220824
#define MAX_FIFOS 25
#define MAX_ALCORS 6

bool running = true;
bool monitor = false;
bool reset = false;

const uhal::Node *occupancy_node[MAX_FIFOS] = {nullptr}, *reset_node[MAX_FIFOS], *data_node[MAX_FIFOS] = {nullptr};//, *pulse_node[6] = {nullptr};
uhal::ValWord<uint32_t> occupancy_register[MAX_FIFOS];
uhal::ValVector<uint32_t> data_register[MAX_FIFOS];
int n_active_fifos = 0, n_active_alcors = 0;
bool active_alcor[MAX_ALCORS] = {false};
int fifo_id[MAX_FIFOS] = {0};
bool fifo_killed[MAX_FIFOS] = {false};
bool any_fifo_killed = false;

struct alcor_hit_t {
  uint32_t fine   : 9;
  uint32_t coarse : 15;
  uint32_t tdc    : 2;
  uint32_t pixel  : 3;
  uint32_t column : 3;
  void print() {
    printf(" hit: %d %d %d %d %d \n", column, pixel, tdc, coarse, fine);
  }
  int get_channel() {
    return pixel + 4 * column;
  }
};

struct program_options_t {
  std::string connection_filename, device_id, output_filename;
  int staging_size, fifo_mask, fifo_occupancy, monitor_period, usleep_period, run_mode, timeout, filter_mode, run_number, fifo_killer;
  bool standalone, send_pulse, reset_fifo, send_reset, quit_on_monitor, one_file, spill_reset;
} opt;

struct ipbus_struct_t {
  const uhal::Node *occupancy_node[MAX_FIFOS] = {nullptr}, *data_node[MAX_FIFOS] = {nullptr}, *pulse_node[MAX_FIFOS] = {nullptr};
};

void process_program_options(int argc, char *argv[], program_options_t &opt);

void
sigint_handler(int signum) {
  std::cout << " --- infinite loop terminate requested" << std::endl;
  running = false;
}

void
sigalrm_handler(int signum) {
  monitor = true;
}


void write_buffer_to_file(std::ofstream &fout, int buffer_source, int buffer_counter, uint8_t *buffer, int buffer_size)
{
  uint32_t header[4];
  header[0] = 0x123caffe;
  header[1] = buffer_source;
  header[2] = buffer_counter;
  header[3] = buffer_size;
  fout.write((char *)&header, 16);
  fout.write((char *)buffer, buffer_size);
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
      ("fifo"             , po::value<int>(&opt.fifo_mask)->default_value(0x1ffffff), "FIFO mask")
      ("occupancy"        , po::value<int>(&opt.fifo_occupancy)->default_value(4096), "FIFO minimum occupancy")
      ("usleep"           , po::value<int>(&opt.usleep_period)->default_value(0), "Microsecond sleep between polling cycles")
      ("staging"          , po::value<int>(&opt.staging_size)->default_value(1048576), "Staging buffer size (bytes)")
      ("monitor-period"   , po::value<int>(&opt.monitor_period)->default_value(1000000), "Monitor period (microseconds)")
      ("run"              , po::value<int>(&opt.run_number)->default_value(0x3), "Run number")
      ("mode"             , po::value<int>(&opt.run_mode)->default_value(0x3), "Run mode")
      ("filter"           , po::value<int>(&opt.filter_mode)->default_value(0x0), "Filter mode")
      ("killer"           , po::value<int>(&opt.fifo_killer)->default_value(8192), "Fifo killer")
      ("timeout"          , po::value<int>(&opt.timeout)->default_value(0), "Readout timeout (microseconds)")
      ("output"           , po::value<std::string>(&opt.output_filename), "Output data filename prefix")
      ("standalone"       , po::bool_switch(&opt.standalone), "Standalone operation mode")
      ("send_pulse"       , po::bool_switch(&opt.send_pulse), "Send a pulse at the beginning")
      ("send_reset"       , po::bool_switch(&opt.send_reset), "Send a reset at the beginning")
      ("spill_reset"      , po::bool_switch(&opt.spill_reset), "Reset ALCOR at the end of each spill")
      ("reset_fifo"       , po::bool_switch(&opt.reset_fifo), "Reset FIFOs at the beginning")
      ("quit_on_monitor"  , po::bool_switch(&opt.quit_on_monitor), "Quit after first monitor")
      ("one_file"         , po::bool_switch(&opt.one_file), "Write data onto a single file")
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

int
ALCOR_reset(uhal::HwInterface &hardware)
{
  /** get current mode **/
  auto mode_node = &hardware.getNode("regfile.mode");
  auto mode_register = mode_node->read();
  hardware.dispatch();
  std::cout << " --- setting run mode: 0x0" << std::endl;
  auto mode = mode_register.value();

  /** reset ALCOR **/
  std::string alcor_reset_system = std::getenv("ALCOR_DIR");
  alcor_reset_system += "/control/alcorInit.sh 666 /tmp";
  std::cout << " calling system: " << alcor_reset_system << std::endl;
  system(alcor_reset_system.c_str());
  
  // set filter mode
  auto filter_command = 0x03300000 | opt.filter_mode;
  std::cout << " --- setting ALCOR filter mode: " << opt.filter_mode << std::endl;
  for (int i = 0; i < MAX_ALCORS; ++i) {
    if (!active_alcor[i]) continue;
    hardware.getNode("alcor_controller_id" + std::to_string(i)).write(filter_command);
  }
  hardware.dispatch();
  for (int i = 0; i < MAX_ALCORS; ++i) {
    if (!active_alcor[i]) continue;
    auto controller_register = hardware.getNode("alcor_controller_id" + std::to_string(i)).read();
    hardware.dispatch();
    auto controller_value = controller_register.value();
    if (controller_value != filter_command) {
      std::cout << " [ERROR] filter command mismatch on ALCOR #" << i << ": " << std::hex << "0x" << controller_value << " != 0x" << filter_command << std::dec << std::endl;
      return 1;
    }
    std::cout << " --- filter command OK on ALCOR #" << i << ": " << std::hex << "0x" << controller_value << " == 0x" << filter_command << std::dec << std::endl;
  }

  /** clean killed FIFOs **/
  any_fifo_killed = false;
  for (int i = 0; i < n_active_fifos; ++i)
    fifo_killed[i] = false;
  
  /** reset fifos **/
  for (int i = 0; i < n_active_fifos; ++i) reset_node[i]->write(0x1);
  hardware.dispatch();
  std::cout << " --- FIFO reset sent " << std::endl;

  /** reset current mode **/
  mode_node->write(mode);
  hardware.dispatch();
  std::cout << " --- setting run mode: " << mode << std::endl;

  return 0;
}

int main(int argc, char *argv[])
{
  std::cout << " --- welcome to ALCOR readout " << std::endl;

  process_program_options(argc, argv, opt);  

  /** initialise and retrieve hardware nodes **/
  uhal::disableLogging();
  uhal::ConnectionManager connection_manager("file://" + opt.connection_filename);
  uhal::HwInterface hardware = connection_manager.getDevice(opt.device_id);
  for (int i = 0; i < MAX_FIFOS; ++i) {
    if ( !(opt.fifo_mask & 1 << i ) ) continue;
    int chip = i / 4;
    int lane = i % 4;
    if (i == 24) {
      std::cout << " --- reading data from trigger FIFO " << std::endl;
      occupancy_node[n_active_fifos] = &hardware.getNode("trigger_info.fifo_occupancy");
      reset_node[n_active_fifos]     = &hardware.getNode("trigger_info.fifo_reset");
      data_node[n_active_fifos]      = &hardware.getNode("trigger_info.fifo_data");
    } else {
      std::cout << " --- reading data from FIFO # " << i << " (chip = " << chip << ", lane = " << lane << ")" << std::endl;
      occupancy_node[n_active_fifos] = &hardware.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_occupancy");
      reset_node[n_active_fifos]     = &hardware.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_reset");
      data_node[n_active_fifos]      = &hardware.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_data");
    }
    fifo_id[n_active_fifos] = i;
    n_active_fifos++;
    active_alcor[chip] = true;
  }

  /** prepare staging buffers and pointers **/
  uint8_t *staging_buffer[MAX_FIFOS] = {nullptr};
  uint8_t *staging_buffer_pointer[MAX_FIFOS] = {nullptr};
  uint32_t staging_buffer_bytes[MAX_FIFOS] = {0};
  uint32_t buffer_counter = 0;
  bool flush_staging_buffers = true; // false;
  for (int i = 0; i < n_active_fifos; ++i) {
    staging_buffer[i] = new uint8_t[opt.staging_size];
    staging_buffer_pointer[i] = staging_buffer[i];
  }
    
  /** open output file **/
  std::ofstream fout[MAX_FIFOS];
  bool write_output = !opt.output_filename.empty();
  if (write_output) {
    for (int i = 0; i < n_active_fifos; ++i) {
      if (opt.one_file && i != 0) continue;
      std::string filename = opt.output_filename + ".fifo_" + std::to_string(fifo_id[i]) + ".dat";
      if (opt.one_file) filename = opt.output_filename + ".dat";
      std::cout << " --- opening output file: " << filename << std::endl;
      fout[i].open(filename, std::ofstream::out | std::ofstream::binary);
      if (!fout[i].is_open()) {
	std::cout << " --- cannot open output file: " << filename << std::endl;
	return 1;
      }
    }
  }

  /** register signal handlers **/
  signal(SIGINT, sigint_handler);
  signal(SIGALRM, sigalrm_handler);

  /** status register to retrive the spill bit **/
  auto status_node = &hardware.getNode("regfile.status");
  uhal::ValWord<uint32_t> status_register;

  /** make sure run mode is off before starting **/
  auto fwrev_node = &hardware.getNode("regfile.fwrev");
  auto fwrev_register = fwrev_node->read();
  auto mode_node = &hardware.getNode("regfile.mode");
  mode_node->write(0);
  hardware.dispatch();
  std::cout << " --- setting run mode: 0x0" << std::endl;
  auto fwrev = fwrev_register.value();

  /** write firmware info and other stuff in file header **/
  auto timestamp = std::time(nullptr);
  if (write_output) {
    uint32_t header[32] = {0x0};
    header[0] = 0x000caffe;
    header[1] = VERSION; // readout version
    header[2] = fwrev; // firmware version
    header[3] = opt.run_number; // run number
    header[4] = timestamp; // timestamp
    header[5] = opt.staging_size; // staging size 
    header[6] = opt.run_mode; // run mode
    header[7] = opt.filter_mode; // filter mode
    for (int i = 0; i < n_active_fifos; ++i) fout[i].write((char *)&header, 64);
  }
  
  /** reset ALCOR **/
  ALCOR_reset(hardware);
  
  /** start infinite loop **/
  std::cout << " --- firmware revision: " << std::hex << fwrev << std::dec << std::endl;
  std::cout << " --- FIFO minimum occupancy: " << opt.fifo_occupancy << std::endl;
  std::cout << " --- staging buffer size: " << opt.staging_size << " bytes" << std::endl;
  std::cout << " --- usleep period: " << opt.usleep_period << " us" << std::endl;
  std::cout << " --- monitor period: " << opt.monitor_period << " us" << std::endl;
  std::cout << " --- fifo killer: " << opt.fifo_killer << std::endl;
  std::cout << " --- starting infinite loop: ctrl+c to interrupt " << std::endl;
  auto start = std::chrono::steady_clock::now();
  auto split = start;
  ualarm(opt.monitor_period, 0);
  int nwords[MAX_FIFOS] = {0}, nbytes[MAX_FIFOS] = {0}, nframes[MAX_FIFOS] = {0}, nhits[MAX_FIFOS] = {0};
  uint32_t max_occupancy[MAX_FIFOS] = {0}, n_polls = 0, n_resets = 0;
  uint32_t occupancy[MAX_FIFOS], bytes[MAX_FIFOS];
  bool fifo_download = false;
  bool fifo_download_this[MAX_FIFOS] = {false};

  // reset fifos
  if (opt.reset_fifo) {
    for (int i = 0; i < n_active_fifos; ++i) {
      reset_node[i]->write(0x1);
    }
    hardware.dispatch();
    std::cout << " --- FIFO reset sent " << std::endl;
  }

  // go into real run mode
  mode_node->write(1);
  hardware.dispatch();
  std::cout << " --- setting run mode: 0x1" << std::endl;
  mode_node->write(opt.run_mode);                                                     
  hardware.dispatch();
  std::cout << " --- setting run mode: 0x" << std::hex << opt.run_mode << std::dec << std::endl;

  // send reset
  if (opt.send_reset) {
    for (int i = 0; i < MAX_ALCORS; ++i) {
      if (!active_alcor[i]) continue;
      hardware.getNode("pulser.reset_id" + std::to_string(i)).write(0x1);
    }
    hardware.dispatch();
    std::cout << " --- reset sent " << std::endl;
  }

  // send pulse
  if (opt.send_pulse) {
    for (int i = 0; i < MAX_ALCORS; ++i) {
      if (!active_alcor[i]) continue;
      hardware.getNode("pulser.testpulse_id" + std::to_string(i)).write(0x1);
    }
    hardware.dispatch();
    std::cout << " --- pulse sent " << std::endl;
  }

  int status;
  bool in_spill = false;
  bool force_flush_staging_buffers = false;
  /** start running **/
  while (running) {

    bool force_flush_staging_buffers = false;
    
    /** read status register to know about the spill status **/
    if (opt.run_mode & 0x4) {
      status_register = status_node->read();
      hardware.dispatch();
      status = status_register.value();
      /** spill still down, keep polling **/
      //      if (status == 0x0 && !in_spill) continue;
      /** spill went up, do something **/
      if (status == 0x1 && !in_spill) in_spill = true;
      /** spill went down, reset all chips **/
      else if (status == 0x0 && in_spill) {
	in_spill = false;
	if (opt.spill_reset || any_fifo_killed) {
	  std::cout << " --- spill down, chip reset via system call " << std::endl;
	  ALCOR_reset(hardware);
	  n_resets++;
	}
	continue;
      } 
    }
    
    // increment poll counter and usleep
    n_polls++;
    usleep(opt.usleep_period);

    // dispatch read fifo occupancy
    for (int i = 0; i < n_active_fifos; ++i) {
      if (fifo_killed[i]) continue;
      occupancy_register[i] = occupancy_node[i]->read();
    }
    hardware.dispatch();
    
    // retrieve fifo occupancy
    // set download flag if at least one fifo is above threshold
    // set flush staging if at least one will overflow
    fifo_download = true; // false;
    flush_staging_buffers = force_flush_staging_buffers;
    for (int i = 0; i < n_active_fifos; ++i) {
      if (fifo_killed[i]) continue;
#ifdef PARANOID
      if (!occupancy_register[i].valid()) continue;
#endif
      occupancy[i] = occupancy_register[i].value() & 0xffff;
      bytes[i] = occupancy[i] * 4;
      if (occupancy[i] > max_occupancy[i]) max_occupancy[i] = occupancy[i];
      //      fifo_download = true; //R+hack
      //      fifo_download_this[i] = true; //R+hack
      if (occupancy[i] >= opt.fifo_occupancy) {
	fifo_download = true;
	fifo_download_this[i] = true;
      }
      if (staging_buffer_bytes[i] + bytes[i] > opt.staging_size) flush_staging_buffers = true;
      // if FIFO saturared kill it and rearrange
      if (occupancy[i] >= opt.fifo_killer) {
	std::cout << " --- FIFO KILLED: " << fifo_id[i] << " (occupancy = " << occupancy[i] << ")"  << std::endl;

	// attempt online post-mortem diagnosics
	std::cout << " --- attempt online post-mortem diagnosics " << std::endl;
	data_register[i] = data_node[i]->readBlock(occupancy[i]);
	hardware.dispatch();
	auto mortem = data_register[i].value();
	auto mortem_data = mortem.back();
	alcor_hit_t *alcor_hit = (alcor_hit_t*)&mortem_data;
	int chip = fifo_id[i] / 4;
	std::cout << " --- 0x" << std::hex << mortem_data << std::dec << " --> chip " << chip << " channel " << alcor_hit->get_channel() << std::endl;

	if (write_output) {
	  write_buffer_to_file(fout[i], fifo_id[i], buffer_counter, staging_buffer[i], staging_buffer_bytes[i]);
	  uint32_t deadfifo = 0x666caffe;
	  fout[i].write((char *)&deadfifo, 4);
	}
	fifo_killed[i] = true;
	any_fifo_killed = true;
      }
    }
    
    // download fifo data
    if (fifo_download || flush_staging_buffers) {
      
      // flush staging buffers if requested
      // write staging buffers to file if requested
      // dispatch block read of fifo data
      for (int i = 0; i < n_active_fifos; ++i) {
	if (fifo_killed[i]) continue;
	if (flush_staging_buffers) {
	  if (write_output) {
	    std::cout << " --- flushing FIFO #" << fifo_id[i] << ": " << staging_buffer_bytes[i] << std::endl;
	    write_buffer_to_file(fout[i], fifo_id[i], buffer_counter, staging_buffer[i], staging_buffer_bytes[i]);
	  }
	  staging_buffer_pointer[i] = staging_buffer[i];
	  staging_buffer_bytes[i] = 0;
	}
	if (occupancy[i] <= 0 || !fifo_download_this[i]) continue;
	data_register[i] = data_node[i]->readBlock(occupancy[i]);
      }
      hardware.dispatch();
      
      if (flush_staging_buffers) {
	buffer_counter++;
	flush_staging_buffers = false;
      }    
      
      for (int i = 0; i < n_active_fifos; ++i) {
	if (fifo_killed[i]) continue;
#ifdef PARANOID      
	if (!data_register[i].valid()) continue;
#endif
	if (!fifo_download_this[i]) continue;
	/** copy data into staging buffer **/
	std::memcpy((char *)staging_buffer_pointer[i], (char *)data_register[i].data(), bytes[i]);
	nwords[i] += occupancy[i];
	nbytes[i] += bytes[i];
	staging_buffer_pointer[i] += bytes[i];
	staging_buffer_bytes[i] += bytes[i];
	fifo_download_this[i] = false;
      }
      
    }
    
    /** monitor **/
    if (monitor) {
      auto end = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed_start = end - start;
      std::chrono::duration<double> elapsed_split = end - split;

      std::cout << std::string(16 * 8, '-') << std::endl;
      //      std::cout << " -- elapsed time: " << elapsed_start.count() << " s" << std::endl;
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "FIFO";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "max occupancy";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "words";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "words/poll";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "words/s";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "bytes/s";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "frames/s";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << "hits/s";
      std::cout << std::endl;
      std::cout << " SOM " << std::string(16 * 8 - 5, '-') << " " << elapsed_start.count() << " " << n_polls / elapsed_split.count() << " " << n_resets << std::endl;

      int nwords_tot = 0, nbytes_tot = 0;

      for (int i = 0; i < n_active_fifos; ++i) {
        
	if (fifo_killed[i]) {
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << (fifo_id[i] == 24 ? "TRG" : std::to_string(fifo_id[i]));
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << "KILLED";
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << 0.;
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << 0.;
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << 0.;
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << 0.;
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << 0.;
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << 0.;
	  std::cout << std::endl;
	} else {
	  std::cout << std::right << std::setw(16) << std::setfill(' ') << (fifo_id[i] == 24 ? "TRG" : std::to_string(fifo_id[i]));
        std::cout << std::right << std::setw(16) << std::setfill(' ') << max_occupancy[i];
	std::cout << std::right << std::setw(16) << std::setfill(' ') << nwords[i];
        std::cout << std::right << std::setw(16) << std::setfill(' ') << nwords[i] / n_polls;
        std::cout << std::right << std::setw(16) << std::setfill(' ') << nwords[i] / elapsed_split.count();
        std::cout << std::right << std::setw(16) << std::setfill(' ') << nbytes[i] / elapsed_split.count();
        std::cout << std::right << std::setw(16) << std::setfill(' ') << nframes[i] / elapsed_split.count();
        std::cout << std::right << std::setw(16) << std::setfill(' ') << nhits[i] / elapsed_split.count();
        std::cout << std::endl;
	}
        
	nwords_tot += nwords[i];
	nbytes_tot += nbytes[i];

	max_occupancy[i] = 0;
	nwords[i] = 0;
	nbytes[i] = 0;
	nframes[i] = 0;
	nhits[i] = 0;
      }
      std::cout << " EOM " << std::string(16 * 8 - 5, '-') << " RUN " << opt.run_number << std::endl;

      std::cout << std::right << std::setw(16) << std::setfill(' ') << " ";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << " ";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << " ";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << " ";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << nwords_tot / elapsed_split.count();
      std::cout << std::right << std::setw(16) << std::setfill(' ') << nbytes_tot / elapsed_split.count();
      std::cout << std::right << std::setw(16) << std::setfill(' ') << " ";
      std::cout << std::right << std::setw(16) << std::setfill(' ') << " ";
      std::cout << std::endl;

      split = std::chrono::steady_clock::now();
      monitor = false;
      n_polls = 0;
      ualarm(opt.monitor_period, 0);

      if (opt.quit_on_monitor) running = false;
      if (opt.timeout > 0 && elapsed_start.count() * 1000000. > opt.timeout) running = false;
      
    }
    
  }

  /** flush and release staging buffers **/
  for (int i = 0; i < n_active_fifos; ++i) {
    /** write staging buffer to file if requested **/
    if (write_output) {
      if (opt.one_file && i !=  0) continue;
      //      std::cout << " --- flushing FIFO #" << fifo_id[i] << ": " << staging_buffer_bytes[i] << std::endl;
      write_buffer_to_file(fout[i], fifo_id[i], buffer_counter, staging_buffer[i], staging_buffer_bytes[i]);
      fout[i].close();
      std::cout << " --- output file closed: " << std::endl;
    }
    staging_buffer_pointer[i] = staging_buffer[i];
    staging_buffer_bytes[i] = 0;
    delete [] staging_buffer[i];
  }
  buffer_counter++;
   
  mode_node->write(0);
  hardware.dispatch();
  std::cout << " --- setting run mode: 0x0" << std::endl;
  
  std::cout << " --- exiting, so long " << std::endl;
  
  return 0;
}
