#include <iostream>
#include <fstream>
#include <string>
#include <boost/program_options.hpp>

bool verbose = false;
int integrated_rollover = 0;
int rollover_counter = 0;//-1; // we start from -1 becaue the very first word is a rollover
int frame = 0;

struct main_header_t {
  uint32_t caffe;
  uint32_t readout_version;
  uint32_t firmware_release;
  uint32_t run_number;
  uint32_t timestamp;
  uint32_t staging_size;
  uint32_t run_mode;
  uint32_t filter_mode;
  uint32_t reserved0;
  uint32_t reserved1;
  uint32_t reserved2;
  uint32_t reserved3;
  uint32_t reserved4;
  uint32_t reserved5;
  uint32_t reserved6;
  uint32_t reserved7;
};

struct buffer_header_t {
  uint32_t caffe;
  uint32_t id;
  uint32_t counter;
  uint32_t size;
};

struct spill_t {
  uint32_t coarse   : 15;
  uint32_t rollover : 25;
  uint32_t zero     : 8;
  uint32_t counter  : 12;
  uint32_t id       : 4;
};

struct trigger_t {
  uint32_t coarse   : 15;
  uint32_t rollover : 25;
  uint32_t counter  : 16;
  uint32_t type     : 4;
  uint32_t id       : 4;
};

struct alcor_hit_t {
  uint32_t fine   : 9;
  uint32_t coarse : 15;
  uint32_t tdc    : 2;
  uint32_t pixel  : 3;
  uint32_t column : 3;
  void print() {
    printf(" hit: %d %d %d %d %d \n", column, pixel, tdc, coarse, fine);
  }
};

bool in_spill = false;

void write_data_header(std::ofstream &fout)
{
  fout << "fifo/I:type/I:counter/I:column/I:pixel/I:tdc/I:rollover/I:coarse/I:fine/I" << std::endl;
}

void write_data(std::ofstream &fout, int fifo, int type, int counter, int column, int pixel, int tdc, int rollover, int coarse, int fine)
{
  fout << fifo << " " << type << " " << counter << " " << column << " " << pixel << " " << tdc << " " << rollover << " " << coarse << " " << fine << std::endl;
}

void write_trigger_data(std::ofstream &fout, int fifo, int type, int counter, int rollover, int coarse)
{
  write_data(fout, fifo, type, counter, -1, -1, -1, rollover, coarse, -1);
}

void write_alcor_data(std::ofstream &fout, int fifo, int column, int pixel, int tdc, int rollover, int coarse, int fine)
{
  write_data(fout, fifo, 1, -1, column, pixel, tdc, rollover, coarse, fine);
}
                
void decode_trigger(char *buffer, int fifo, int size, std::ofstream &fout)
{
  if (verbose) printf(" --- decode_trigger: fifo%d, size=%d \n", fifo, size); 

  size /= 4;
  auto word = (uint32_t *)buffer;
  uint32_t pos = 0;

  while (pos < size) {

    /** spill header **/
    if ((*word & 0xf0000000) == 0x70000000) {
      uint32_t counter = (*word & 0x0fff0000) >> 16;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- spill header (counter=%d)\n", *word, counter);
      trigger_time = (uint64_t)(*word & 0xff) << 32;
      ++word; ++pos;
      if (verbose) printf(" 0x%08x -- spill header continued \n", *word);
      trigger_time |= *word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      write_trigger_data(fout, fifo, 7, counter, rollover, coarse);
      ++word; ++pos;
    }
    
    /** spill trailer **/
    else if ((*word & 0xf0000000) == 0xf0000000) {
      spill_t *spill = (spill_t *)word;
      uint32_t counter = (*word & 0x0fff0000) >> 16;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- spill trailer (counter=%d)\n", *word, counter);
      trigger_time = (uint64_t)(*word & 0xff) << 32;
      ++word; ++pos;
      if (verbose) printf(" 0x%08x -- spill trailer continued \n", *word);
      trigger_time |= *word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      write_trigger_data(fout, fifo, 15, counter, rollover, coarse);
      ++word; ++pos;
    }
    
    /** trigger **/
    else if ((*word & 0xf0000000) == 0x90000000) {
      trigger_t *trigger = (trigger_t *)word;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- trigger header\n", *word);
      trigger_time = (uint64_t)(*word & 0xff) << 32;
      uint32_t counter = (*word & 0xffff00) >> 16;
      ++word; ++pos;
      if (verbose) printf(" 0x%08x -- trigger header continued \n", *word);
      trigger_time |= *word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      write_trigger_data(fout, fifo, 9, counter, rollover, coarse);
      ++word; ++pos;
    }

    /** else **/
    else {
      printf(" 0x%08x -- unexpected word \n", *word);
      ++word; ++pos;
    }
    
  }

}

void decode(char *buffer, int fifo, int size, std::ofstream &fout, bool is_filtered)
{
  size /= 4;
  auto word = (uint32_t *)buffer;
  alcor_hit_t *hit;
  uint32_t pos = 0;

  // loop over buffer data
  while (pos < size) {

    // find spill header if not in spill already
    while (!in_spill && pos < size) {
      
      /** spill header **/
      if ((*word & 0xf0000000) == 0x70000000) {
        uint32_t counter = (*word & 0x0fff0000) >> 16;
        uint64_t trigger_time = 0x0;
        if (verbose) printf(" 0x%08x -- spill header (counter=%d)\n", *word, counter);
        trigger_time = (uint64_t)(*word & 0xff) << 32;
        ++word; ++pos;
        if (verbose) printf(" 0x%08x -- spill header continued \n", *word);
        trigger_time |= *word;
        uint32_t coarse = trigger_time & 0x7fff;
        uint32_t rollover = trigger_time >> 15;
        write_trigger_data(fout, fifo, 7, counter, rollover, coarse);
        ++word; ++pos;
        in_spill = true;

	// [R+HACK] check first word in spill, if rollover skip it
	//	if (*word == 0x5c5c5c5c) {
	//	  if (verbose) printf(" 0x%08x -- rollover to skip (counter=%d) \n", *word, rollover_counter);
	//	  ++word; ++pos;
	//	}
	
	// [R+HACK] check first word in spill, if not rollover pretend there was one
	//	if (*word != 0x5c5c5c5c) {
	//	  if (verbose) printf(" 0x%08x -- not a rollover (counter=%d) \n", *word, rollover_counter);
	//	  rollover_counter++;
	//	}
	
	break;
      }

      /** something else **/
      if (verbose) printf(" 0x%08x -- \n", *word);
      ++word; ++pos;
    }
    
    // find spill trailer
    while (pos < size) {

      /** killed fifo **/
      if (*word == 0x666caffe) {
        if (verbose) printf(" 0x%08x -- killed fifo \n", *word);
        write_trigger_data(fout, fifo, 15, -1, -1, -1);
        ++word; ++pos;
        in_spill = false;
	rollover_counter = 0;
        break;	
      }
      
      /** spill trailer **/
      if ((*word & 0xf0000000) == 0xf0000000) {
        spill_t *spill = (spill_t *)word;
        uint32_t counter = (*word & 0x0fff0000) >> 16;
        uint64_t trigger_time = 0x0;
        if (verbose) printf(" 0x%08x -- spill trailer (counter=%d)\n", *word, counter);
        trigger_time = (uint64_t)(*word & 0xff) << 32;
        ++word; ++pos;
        if (verbose) printf(" 0x%08x -- spill trailer continued \n", *word);
        trigger_time |= *word;
        uint32_t coarse = trigger_time & 0x7fff;
        uint32_t rollover = trigger_time >> 15;
        write_trigger_data(fout, fifo, 15, counter, rollover, coarse);
        ++word; ++pos;
        in_spill = false;
	rollover_counter = 0;
        break;
      }

      /** rollover **/
      if (*word == 0x5c5c5c5c) {
        if (verbose) printf(" 0x%08x -- rollover (counter=%d) \n", *word, rollover_counter);
        ++rollover_counter;
	++integrated_rollover;
        ++word; ++pos;
        continue;
      }

      /** hit **/
      hit = (alcor_hit_t *)word;
      if (verbose) printf(" 0x%08x -- hit (coarse=%d, fine=%d, column=%d, pixel=%d --> channel=%d)\n", *word, hit->coarse, hit->fine, hit->column, hit->pixel, hit->column * 4 + hit->pixel);
      write_alcor_data(fout, fifo, hit->column, hit->pixel, hit->tdc, rollover_counter, hit->coarse, hit->fine);
      ++word; ++pos;
      
    }
  }

#if 0

  
  // find spill trailer
  while (pos < size) {
    if (*word & 0xf0000000 == 0x90000000) {
      ++word; ++pos;
      continue;
    }
    
  }
  
  while (pos < size) {

    // find next rollover
    while (pos < size) {
      if (*word != 0x5c5c5c5c) {
        ++word; ++pos;
        continue;
      }
      if (verbose) printf(" 0x%08x -- rollover \n", *word);
      break;
    }
    ++rollover;
    ++integrated_rollover;
    ++word; ++pos;
      
    // find frame header
    while (pos < size && !is_filtered) {
      if (*word == 0x1c1c1c1c) break;
      if (verbose) printf(" 0x%08x -- \n", *word);
      ++word; ++pos;
    }
    if (verbose) printf(" 0x%08x -- frame header \n", *word);
    ++word; ++pos;
    if (verbose) printf(" 0x%08x -- frame counter \n", *word);
    auto frame = *word;
    ++word; ++pos;
    
    // find next rollover
    while (pos < size) {
      if (*word == 0x5c5c5c5c) break;
      if (verbose) printf(" 0x%08x -- hit \n", *word);
      hit = (alcor_hit_t *)word;
      fout << 1 << " " << -1 << " " << hit->column << " " << hit->pixel << " " << hit->tdc << " " << frame << " " << rollover << " " << hit->coarse << " " << hit->fine << std::endl;
      //      fout << rollover << " " << hit->coarse << std::endl;
      ++word; ++pos;
    }
    
  }
  
  return;

#if 0
  
  word++; pos++;


  
  // skip till the frame header
  for (; pos < size; pos++) {
    if (word == 0x1c1c1c1c) break;
    word++;
  }

  // this is the frame counter
  word++; pos++

  // till the end these are hits
  for (; pos < size; pos++) {
    if (word == 0x1c1c1c1c) break;
    word++;
  }

  for (int i = 0; i < size; ++i) {

    
    word++;
  }

#endif
#endif
  
}

int main(int argc, char *argv[])
{
  std::cout << " --- welcome to ALCOR decoder " << std::endl;

  std::string input_filename, output_filename;
  
  /** process arguments **/
  namespace po = boost::program_options;
  po::options_description desc("Options");
  try {
    desc.add_options()
      ("help"    , "Print help messages")
      ("input"   , po::value<std::string>(&input_filename), "Input data file")
      ("output"  , po::value<std::string>(&output_filename), "Output data file")
      ("verbose" , po::bool_switch(&verbose)->default_value(false), "Verbose mode flag")
      ;
    
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    
    if (vm.count("help")) {
      std::cout << desc << std::endl;
      return 1;
    }
  }
  catch(std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    std::cout << desc << std::endl;
    return 1;
  }

  /** open input file **/
  std::cout << " --- opening input file: " << input_filename << std::endl;
  std::ifstream fin;
  fin.open(input_filename, std::ofstream::in | std::ofstream::binary);
  
  /** read main header **/ 
  main_header_t main_header;
  fin.read((char *)&main_header, sizeof(main_header_t));
  if (main_header.caffe != 0x000caffe) {
    printf(" --- [ERROR] caffe header mismatch in main header: 0x%08x \n", main_header.caffe);
    return 1;
  }
  if (verbose) {
    printf(" --- [main header] caffe header detected: 0x%08x \n", main_header.caffe);
    printf(" --- [main header] readout version: 0x%08x \n", main_header.readout_version);
    printf(" --- [main header] firmware release: 0x%08x \n", main_header.firmware_release);
    printf(" --- [main header] run number: %d \n", main_header.run_number);
    printf(" --- [main header] timestamp: %d \n", main_header.timestamp);
    printf(" --- [main header] staging buffer size: %d \n", main_header.staging_size);
    printf(" --- [main header] run mode: 0x%1x \n", main_header.run_mode);
    printf(" --- [main header] filter mode: 0x%1x \n", main_header.filter_mode);
    printf(" --- [main header] timestamp: %d \n", main_header.timestamp);
  }

  // check that we know how to decode it
  bool is_filtered;
  if (main_header.filter_mode == 0x0)
    is_filtered = false;
  else if (main_header.filter_mode == 0xf)
    is_filtered = true;
  else {
    printf(" --- [ERROR] filter mode not supported: 0x%01x \n", main_header.filter_mode);
    return 1;
  }
  
  // create reading buffer
  auto staging_size = main_header.staging_size;
  char *buffer = new char[staging_size];
  
  /** open output file **/
  std::cout << " --- opening output file: " << output_filename << std::endl;
  std::ofstream fout;
  fout.open(output_filename, std::ofstream::out);
  write_data_header(fout);

  /** loop over data **/
  buffer_header_t buffer_header;
  uint32_t word;
  while (true) {
    fin.read((char *)(&buffer_header), sizeof(buffer_header_t));
    if (fin.eof()) break;
    if (buffer_header.caffe != 0x123caffe) {
      printf(" --- [ERROR] caffe header mismatch in buffer header: %08x \n", buffer_header.caffe);
      break;
    }
    if (verbose) {
      printf(" --- [buffer header] caffe header detected: 0x%08x \n", buffer_header.caffe);
      printf(" --- [buffer header] buffer id: %d \n", buffer_header.id);
      printf(" --- [buffer header] buffer counter: %d \n", buffer_header.counter);
      printf(" --- [buffer header] buffer size: %d \n", buffer_header.size);
    }
    fin.read(buffer, buffer_header.size);

    if (buffer_header.id < 24) {
      if (verbose) printf(" --- decoding ALCOR FIFO \n");
      decode(buffer, buffer_header.id, buffer_header.size, fout, is_filtered);
    }
    else if (buffer_header.id == 24) {
      if (verbose) printf(" --- decoding TRIGGER FIFO \n");
      decode_trigger(buffer, buffer_header.id, buffer_header.size, fout);
    }
  }
  
  double integrated = (double)integrated_rollover * 0.0001024;
  std::cout << " --- integrated seconds: " << integrated << std::endl;

  /** close input file **/
  fin.close();
  fout.close();
  std::cout << " --- all done, so long " << std::endl;

  return 0;
}
