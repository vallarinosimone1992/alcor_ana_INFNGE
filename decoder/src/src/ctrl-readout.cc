#include <iostream>
#include <fstream>
#include <ctime>
#include <boost/program_options.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <signal.h>
#include <chrono>
#include "uhal/uhal.hpp"

struct shared_t {
  bool in_run; // in run flag
  bool in_spill; // in spill flag
  uint32_t mode; // current run mode
  uint32_t status; // current status
  uint32_t fwrev; // firmware version
  uint32_t run_number; // run number
  uint32_t timestamp; // timestamp
  uint32_t run_mode; // run mode
  uint32_t filter_mode; // filter mode
  uint32_t reset[6][4]; // chip-lane reset request
} *shared_data = nullptr;

struct program_options_t {
  std::string connection_filename, device_id;
  int usleep_period, run_number, run_mode, filter_mode, fifo_mask, max_spills;
  bool reset;
} opt;

#define VERSION 0x20220824
#define MAX_FIFOS 25
#define MAX_ALCORS 6

bool running = true;
//bool inspill = false;

int n_active_fifos = 0, n_active_alcors = 0;
int active_alcor[MAX_ALCORS], active_fifo[MAX_FIFOS];
  
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
      ("reset"            , po::bool_switch(&opt.reset), "Force reset at end of spill")
      ("run"              , po::value<int>(&opt.run_number)->default_value(666), "Run number")
      ("mode"             , po::value<int>(&opt.run_mode)->default_value(0x3), "Run mode")
      ("filter"           , po::value<int>(&opt.filter_mode)->default_value(0x0), "Filter mode")
      ("usleep"           , po::value<int>(&opt.usleep_period)->default_value(1000), "Microsecond sleep between polling cycles")
      ("nspill"           , po::value<int>(&opt.max_spills)->default_value(100), "Maximum number of spills")
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
ALCOR_reset(uhal::HwInterface &hardware, bool really_doit = true)
{
  std::cout << " --- ALCOR reset process " << std::endl;

  /** reset mode **/
  std::cout << " --- setting run mode: 0x0" << std::endl;
  hardware.getNode("regfile.mode").write(0x0);
  hardware.dispatch();

  /** reset ALCOR **/
  std::string alcor_reset_system = std::getenv("ALCOR_DIR");
  alcor_reset_system += "/control/alcorInit.sh 666 /tmp > /dev/null";
  if (really_doit) {
    std::cout << " --- calling system: " << alcor_reset_system << std::endl;
    system(alcor_reset_system.c_str());
  } else {
    usleep(500000);
  }

  /** set filter mode **/
  auto filter_command = 0x03300000 | opt.filter_mode;
  std::cout << " --- setting ALCOR filter mode: 0x" << std::hex << opt.filter_mode << std::dec << std::endl;
  for (int ichip = 0; ichip < n_active_alcors; ++ichip)
    hardware.getNode("alcor_controller_id" + std::to_string(ichip)).write(filter_command);
  hardware.dispatch();

#if 0
  /** reset fifos **/
  /** is this really needed? **/
  std::cout << " --- sending FIFO reset " << std::endl;
  for (int ififo = 0; ififo < n_active_fifos; ++ififo) {
    int fifo = active_fifo[ififo];
    int chip = fifo / 4;
    int lane = fifo % 4;
    hardware.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_reset").write(0x1);
  }
  hardware.dispatch();
#endif

  /** setting mode **/
  std::cout << " --- setting run mode: 0x" << std::hex << shared_data->mode << std::dec << std::endl;
  hardware.getNode("regfile.mode").write(shared_data->mode);
  hardware.dispatch();

  return 0;
}


int main(int argc, char *argv[])
{
  std::cout << " --- welcome to ALCOR ctrl-readout " << std::endl;
  process_program_options(argc, argv, opt);

  /** initialise local data **/
  bool is_alcor_active[MAX_ALCORS] = {false};
  for (int ififo = 0; ififo < MAX_FIFOS; ++ififo) {
    if ( !(opt.fifo_mask & 1 << ififo ) ) continue;
    int ichip = ififo / 4;
    is_alcor_active[ichip] = true;
    active_fifo[n_active_fifos] = ififo;
    n_active_fifos++;
  }
  for (int ichip = 0; ichip < MAX_ALCORS; ++ichip) {
    if (!is_alcor_active[ichip]) continue;
    active_alcor[n_active_alcors++];
  }

  using namespace boost::interprocess;
  std::string shm_name = "shm_" + opt.device_id;
  /** remove shared memory on construction and destruction **/
  struct shm_remove {
    shm_remove(std::string name) : _name(name) { shared_memory_object::remove(_name.c_str()); }
    ~shm_remove(){ shared_memory_object::remove(_name.c_str()); }
    std::string _name;
  } remover(shm_name);
  
  /** create a shared memory object **/
  shared_memory_object shm (create_only, shm_name.c_str(), read_write);
  /** set size **/
  shm.truncate(sizeof(shared_t));
  /** map the whole shared memory in this process **/
  mapped_region region(shm, read_write);
  /** get shared memory pointer **/
  shared_data = (shared_t *)region.get_address();
  /** set shared memory to zero **/
  std::memset(shared_data, 0, sizeof(shared_t));
  //  void *shared_address = region.get_address();
  //  shared_data = new (shared_address) shared_t;

  /** initialise and retrieve hardware top node **/
  uhal::disableLogging();
  uhal::ConnectionManager connection_manager("file://" + opt.connection_filename);
  uhal::HwInterface hardware = connection_manager.getDevice(opt.device_id);

  /** write information on shared data **/
  auto fwrev_register = hardware.getNode("regfile.fwrev").read();
  hardware.dispatch();
  shared_data->fwrev = fwrev_register.value();
  shared_data->run_number = opt.run_number;
  shared_data->timestamp = std::time(nullptr);
  shared_data-> run_mode = opt.run_mode;
  shared_data->filter_mode = opt.filter_mode;
  
  /** make sure run mode is off before starting **/
  auto mode_register = hardware.getNode("regfile.mode").read();
  hardware.dispatch();
  shared_data->mode = mode_register.value();
  if (shared_data->mode != 0x0) {
    std::cout << " --- mode is not zero at startup: 0x" << std::hex << shared_data->mode << std::dec << std::endl;
    exit(1);
  }

  /** register signal handlers **/
  signal(SIGINT, sigint_handler);
  signal(SIGALRM, sigalrm_handler);

  /** set filter mode **/
  auto filter_command = 0x03300000 | opt.filter_mode;
  std::cout << " --- setting ALCOR filter mode: 0x" << std::hex << opt.filter_mode << std::dec << std::endl;
  for (int ichip = 0; ichip < n_active_alcors; ++ichip)
    hardware.getNode("alcor_controller_id" + std::to_string(ichip)).write(filter_command);
  hardware.dispatch();

  /** endless loop till start of run (or interrupted) **/
  std::cout << " --- waiting for start of run " << std::endl;
  system(std::string("/home/daq/bin/influx_write.sh readout-processes,process=ctrl-readout,name=status,device=" + opt.device_id + " value=1").c_str());
  while (running) {

    /** usleep a bit **/
    usleep(opt.usleep_period);

    /** read mode register **/
    auto mode_register = hardware.getNode("regfile.mode").read();
    hardware.dispatch();

    /** mode updated **/
    if (mode_register.value() != shared_data->mode) {
      std::cout << " --- setting run mode: 0x" << std::hex << shared_data->mode << std::dec << std::endl;
      hardware.getNode("regfile.mode").write(shared_data->mode);
      hardware.dispatch();
    }
    
    /** detect start of run **/
    if ((shared_data->mode & 0x1) == 1) {
      std::cout << " --- start of run detected " << std::endl;
      shared_data->in_run = true;
      break;
    }
    
  } /** end of endless loop till start of run (or interrupted) **/

  /** reset ALCOR **/
  //  ALCOR_reset(hardware, true);
  
  /** endless loop till end of run (or interrupted) **/
  std::cout << " --- serving till end of run " << std::endl;
  int nspills = 0;
  system(std::string("/home/daq/bin/influx_write.sh readout-processes,process=ctrl-readout,name=nspills,device=" + opt.device_id + " value=" + std::to_string(nspills)).c_str());

  while (running) {

    /** usleep a bit **/
    usleep(opt.usleep_period);

    /** read mode and status registers **/
    auto mode_register = hardware.getNode("regfile.mode").read();
    auto status_register = hardware.getNode("regfile.status").read();
    hardware.dispatch();

    /** update shared data **/
    shared_data->status = status_register.value();

    /** detect mode update **/
    if (mode_register.value() != shared_data->mode) {
      std::cout << " --- setting run mode: 0x" << std::hex << shared_data->mode << std::dec << std::endl;
      hardware.getNode("regfile.mode").write(shared_data->mode);
      hardware.dispatch();
    }
    
    /** detect end of run **/
    if ((shared_data->mode & 0x1) == 0) {
      std::cout << " --- end of run detected " << std::endl;
      shared_data->in_run = false;
      break;
    }

    /** detect start/end of spill **/
    bool do_reset = false;
    bool in_spill = (shared_data->mode & 0x4) ? shared_data->status == 1 : (shared_data->mode & 0x2);
    if (shared_data->in_spill != in_spill) {
      shared_data->in_spill = in_spill;
      if (in_spill) std::cout << " --- start of spill detected " << std::endl;
      else {
	std::cout << " --- end of spill detected " << std::endl;
	nspills++;
	system(std::string("/home/daq/bin/influx_write.sh readout-processes,process=ctrl-readout,name=nspills,device=" + opt.device_id + " value=" + std::to_string(nspills)).c_str());
	if (opt.reset) {
	  std::cout << " --- force reset at end of spill " << std::endl;
	  do_reset = true;
	}
	usleep(1000); // sleep 1 ms to allow nano-readout processes to communicate reset requests 
      }
    }

#if 0
    /** check ALCOR reset requests **/
    if (!shared_data->in_spill) {
      for (int chip = 0; chip < 6; ++chip) {
	for (int lane = 0; lane < 4; ++lane) {
	  if (shared_data->reset[chip][lane] != 0) {
	    printf(" --- reset request detected: chip %d, lane %d (0x%08x) \n", chip, lane, shared_data->reset[chip][lane]);
	    shared_data->reset[chip][lane] = 0;
	    do_reset = true;
	  }
	}
      }
      if (do_reset) {
	auto start = std::chrono::high_resolution_clock::now();
	ALCOR_reset(hardware);
	auto stop = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
	std::cout << " --- reset executed: " << duration.count() << " ms" << std::endl;
	do_reset = false;
      }
    }
#endif

    /** force end of run after exceeding max number of spills **/
    if (nspills >= opt.max_spills) {
      std::cout << " --- max number of spills exceeded: forcing end of run " << std::endl;	  
      shared_data->mode = 0;
      usleep(1000); // sleep 1 ms to allow nano-readout processes to realise
    }

  } /** end of endless loop till end of run (or interrupted) **/

  std::cout << " --- it has been fun, so long " << std::endl;
  system(std::string("/home/daq/bin/influx_write.sh readout-processes,process=ctrl-readout,name=status,device=" + opt.device_id + " value=0").c_str());
  return 0;
}

