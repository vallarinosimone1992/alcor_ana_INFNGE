#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <boost/program_options.hpp>
#include "TFile.h"
#include "TTree.h"

bool verbose = false;
int integrated_rollover = 0;
int integrated_spill = 0;
int integrated_hits = 0;
int rollover_counter = 0;//-1; // we start from -1 becaue the very first word is a rollover
int frame = 0;
int spill_counter[25];

int unexpected_word_count = 0;
int unexpected_word_printed = 0;
const int kUnexpectedPrintLimit = 10;

struct main_header_t {
  uint32_t caffe;
  uint32_t readout_version;
  uint32_t firmware_release;
  uint32_t run_number;
  uint32_t timestamp;
  uint32_t staging_size;
  uint32_t run_mode;
  uint32_t filter_mode;
  uint32_t device;
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

struct data_t {
  int device;
  int fifo;
  int type;
  int counter;
  int spill;
  int column;
  int pixel;
  int tdc;
  int rollover;
  int coarse;
  int fine;
};

using records_t = std::vector<data_t>;

bool in_spill = false;

uint32_t read_word(const char *buffer, uint32_t pos)
{
  uint32_t word = 0;
  std::memcpy(&word, buffer + static_cast<size_t>(pos) * sizeof(uint32_t), sizeof(word));
  return word;
}

int hit_fine(uint32_t word) { return word & 0x1ff; }
int hit_coarse(uint32_t word) { return (word >> 9) & 0x7fff; }
int hit_tdc(uint32_t word) { return (word >> 24) & 0x3; }
int hit_pixel(uint32_t word) { return (word >> 26) & 0x7; }
int hit_column(uint32_t word) { return (word >> 29) & 0x7; }

void write_data(records_t &records,
                int device,
                int fifo,
                int type,
                int counter,
                int spill,
                int column,
                int pixel,
                int tdc,
                int rollover,
                int coarse,
                int fine)
{
  records.push_back({device, fifo, type, counter, spill, column, pixel, tdc, rollover, coarse, fine});
}

void write_trigger_data(records_t &records,
                        int device,
                        int fifo,
                        int type,
                        int counter,
                        int spill,
                        int rollover,
                        int coarse)
{
  write_data(records, device, fifo, type, counter, spill, -1, -1, -1, rollover, coarse, -1);
}

void write_alcor_data(records_t &records,
                      int device,
                      int fifo,
                      int spill,
                      int column,
                      int pixel,
                      int tdc,
                      int rollover,
                      int coarse,
                      int fine)
{
  write_data(records, device, fifo, 1, -1, spill, column, pixel, tdc, rollover, coarse, fine);
}

bool has_words(uint32_t pos, uint32_t size, uint32_t needed)
{
  return pos <= size && needed <= (size - pos);
}
                
bool decode_trigger(const char *buffer, int device, int fifo, int size, records_t &records)
{
  if (verbose) printf(" --- decode_trigger: device-%d fifo-%d, size=%d \n", device, fifo, size); 

  size /= 4;
  uint32_t pos = 0;

  while (pos < size) {
    uint32_t word = read_word(buffer, pos);

    /** spill header **/
    if ((word & 0xf0000000) == 0x70000000) {
      if (!has_words(pos, size, 2)) {
        std::cerr << " --- [ERROR] truncated trigger spill header in fifo " << fifo << std::endl;
        return false;
      }
      ++spill_counter[fifo];
      uint32_t counter = (word & 0x0fff0000) >> 16;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- spill header (counter=%d)\n", word, counter);
      trigger_time = (uint64_t)(word & 0xff) << 32;
      ++pos;
      word = read_word(buffer, pos);
      if (verbose) printf(" 0x%08x -- spill header continued \n", word);
      trigger_time |= word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      write_trigger_data(records, device, fifo, 7, counter, spill_counter[fifo], rollover, coarse);
      ++pos;
    }
    
    /** spill trailer **/
    else if ((word & 0xf0000000) == 0xf0000000) {
      if (!has_words(pos, size, 2)) {
        std::cerr << " --- [ERROR] truncated trigger spill trailer in fifo " << fifo << std::endl;
        return false;
      }
      uint32_t counter = (word & 0x0fff0000) >> 16;
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- spill trailer (counter=%d)\n", word, counter);
      trigger_time = (uint64_t)(word & 0xff) << 32;
      ++pos;
      word = read_word(buffer, pos);
      if (verbose) printf(" 0x%08x -- spill trailer continued \n", word);
      trigger_time |= word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      integrated_spill++;
      write_trigger_data(records, device, fifo, 15, counter, spill_counter[fifo], rollover, coarse);
      ++pos;
    }
    
    /** trigger **/
    else if ((word & 0xf0000000) == 0x90000000) {
      if (!has_words(pos, size, 2)) {
        std::cerr << " --- [ERROR] truncated trigger word in fifo " << fifo << std::endl;
        return false;
      }
      uint64_t trigger_time = 0x0;
      if (verbose) printf(" 0x%08x -- trigger header\n", word);
      trigger_time = (uint64_t)(word & 0xff) << 32;
      uint32_t counter = (word & 0xffff00) >> 16;
      ++pos;
      word = read_word(buffer, pos);
      if (verbose) printf(" 0x%08x -- trigger header continued \n", word);
      trigger_time |= word;
      uint32_t coarse = trigger_time & 0x7fff;
      uint32_t rollover = trigger_time >> 15;
      write_trigger_data(records, device, fifo, 9, counter, spill_counter[fifo], rollover, coarse);
      ++pos;
    }

    /** else **/
    else {
      ++unexpected_word_count;
      if (unexpected_word_printed < kUnexpectedPrintLimit) {
        printf(" 0x%08x -- unexpected word \n", word);
        ++unexpected_word_printed;
        if (unexpected_word_printed == kUnexpectedPrintLimit) {
          printf(" --- further unexpected words suppressed (showing first %d) \n",
                 kUnexpectedPrintLimit);
        }
      }
      ++pos;
    }
    
  }

  return true;
}

bool decode(const char *buffer, int device, int fifo, int size, records_t &records, bool is_filtered)
{
  (void)is_filtered;
  size /= 4;
  uint32_t pos = 0;

  // loop over buffer data
  while (pos < size) {

    // find spill header if not in spill already
    while (!in_spill && pos < size) {
      uint32_t word = read_word(buffer, pos);
      
      /** spill header **/
      if ((word & 0xf0000000) == 0x70000000) {
        if (!has_words(pos, size, 2)) {
          std::cerr << " --- [ERROR] truncated spill header in fifo " << fifo << std::endl;
          return false;
        }
        ++spill_counter[fifo];
        uint32_t counter = (word & 0x0fff0000) >> 16;
        uint64_t trigger_time = 0x0;
        if (verbose) printf(" 0x%08x -- spill header (counter=%d)\n", word, counter);
        trigger_time = (uint64_t)(word & 0xff) << 32;
        ++pos;
        word = read_word(buffer, pos);
        if (verbose) printf(" 0x%08x -- spill header continued \n", word);
        trigger_time |= word;
        uint32_t coarse = trigger_time & 0x7fff;
        uint32_t rollover = trigger_time >> 15;
        write_trigger_data(records, device, fifo, 7, counter, spill_counter[fifo], rollover, coarse);
        ++pos;
        in_spill = true;

	break;
      }

      /** something else **/
      if (verbose) {
	//	if (!in_spill)
	//	  printf(" 0x%08x -- filler (pos=%d)\n", word, pos % 16);
	//	else 
	  printf(" 0x%08x -- \n", word);
      }
      ++pos;
    }
    
    // find spill trailer
    while (pos < size) {
      uint32_t word = read_word(buffer, pos);

      /** killed fifo **/
      if (word == 0x666caffe) {
        if (verbose) printf(" 0x%08x -- killed fifo \n", word);
        write_trigger_data(records, device, fifo, 15, -1, spill_counter[fifo], -1, -1);
        ++pos;
        in_spill = false;
	rollover_counter = 0;
        break;	
      }
      
      /** spill trailer **/
      if ((word & 0xf0000000) == 0xf0000000) {
        if (!has_words(pos, size, 2)) {
          std::cerr << " --- [ERROR] truncated spill trailer in fifo " << fifo << std::endl;
          return false;
        }
        uint32_t counter = (word & 0x0fff0000) >> 16;
        uint64_t trigger_time = 0x0;
        if (verbose) printf(" 0x%08x -- spill trailer (counter=%d)\n", word, counter);
        trigger_time = (uint64_t)(word & 0xff) << 32;
        ++pos;
        word = read_word(buffer, pos);
        if (verbose) printf(" 0x%08x -- spill trailer continued \n", word);
        trigger_time |= word;
        uint32_t coarse = trigger_time & 0x7fff;
        uint32_t rollover = trigger_time >> 15;
        write_trigger_data(records, device, fifo, 15, counter, spill_counter[fifo], rollover, coarse);
        ++pos;
        in_spill = false;
	integrated_spill++;
	rollover_counter = 0;
	break;
      }

      /** rollover **/
      if (word == 0x5c5c5c5c) {
        if (verbose) printf(" 0x%08x -- rollover (counter=%d) \n", word, rollover_counter);
        ++rollover_counter;
	++integrated_rollover;
        ++pos;
        continue;
      }

      /** hit **/
      if (verbose) printf(" 0x%08x -- hit (coarse=%d, fine=%d, column=%d, pixel=%d --> channel=%d)\n",
                          word,
                          hit_coarse(word),
                          hit_fine(word),
                          hit_column(word),
                          hit_pixel(word),
                          hit_column(word) * 4 + hit_pixel(word));
      write_alcor_data(records,
                       device,
                       fifo,
                       spill_counter[fifo],
                       hit_column(word),
                       hit_pixel(word),
                       hit_tdc(word),
                       rollover_counter,
                       hit_coarse(word),
                       hit_fine(word));
      integrated_hits++;
      ++pos;
      
    }
  }

  return true;
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
      ("input"   , po::value<std::string>(&input_filename)->required(), "Input data file")
      ("output"  , po::value<std::string>(&output_filename)->required(), "Output data file")
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
  if (!fin) {
    std::cerr << " --- [ERROR] cannot open input file: " << input_filename << std::endl;
    return 1;
  }
  
  /** read main header **/ 
  main_header_t main_header;
  fin.read((char *)&main_header, sizeof(main_header_t));
  if (fin.gcount() != static_cast<std::streamsize>(sizeof(main_header_t))) {
    std::cerr << " --- [ERROR] input file is shorter than the main header" << std::endl;
    return 1;
  }
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
    printf(" --- [main header] device: %d \n", main_header.device);
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
  if (staging_size == 0 || staging_size > 256u * 1024u * 1024u) {
    std::cerr << " --- [ERROR] invalid staging buffer size: " << staging_size << std::endl;
    return 1;
  }
  std::vector<char> buffer(staging_size);
  
  /** loop over data **/
  records_t records;
  for (int i = 0; i < 25; ++i) {
    spill_counter[i] = -1;
  }
  buffer_header_t buffer_header;
  bool input_error = false;
  while (true) {
    fin.read((char *)(&buffer_header), sizeof(buffer_header_t));
    const std::streamsize header_bytes = fin.gcount();
    if (header_bytes == 0 && fin.eof()) {
      break;
    }
    if (header_bytes != static_cast<std::streamsize>(sizeof(buffer_header_t))) {
      std::cerr << " --- [ERROR] truncated buffer header after " << header_bytes << " bytes" << std::endl;
      input_error = true;
      break;
    }
    if (buffer_header.caffe != 0x123caffe) {
      printf(" --- [ERROR] caffe header mismatch in buffer header: %08x \n", buffer_header.caffe);
      input_error = true;
      break;
    }
    if (verbose) {
      printf(" --- [buffer header] caffe header detected: 0x%08x \n", buffer_header.caffe);
      printf(" --- [buffer header] buffer id: %d \n", buffer_header.id);
      printf(" --- [buffer header] buffer counter: %d \n", buffer_header.counter);
      printf(" --- [buffer header] buffer size: %d \n", buffer_header.size);
    }
    if (buffer_header.size > staging_size) {
      std::cerr << " --- [ERROR] buffer size " << buffer_header.size
                << " exceeds staging buffer size " << staging_size << std::endl;
      input_error = true;
      break;
    }
    if ((buffer_header.size % 4) != 0) {
      std::cerr << " --- [ERROR] buffer size is not 32-bit aligned: " << buffer_header.size << std::endl;
      input_error = true;
      break;
    }
    fin.read(buffer.data(), buffer_header.size);
    if (fin.gcount() != static_cast<std::streamsize>(buffer_header.size)) {
      std::cerr << " --- [ERROR] truncated payload for buffer counter " << buffer_header.counter
                << ": expected " << buffer_header.size << " bytes, got " << fin.gcount() << std::endl;
      input_error = true;
      break;
    }

    if (buffer_header.id < 24) {
      if (verbose) printf(" --- decoding ALCOR FIFO \n");
      if (!decode(buffer.data(), main_header.device, buffer_header.id, buffer_header.size, records, is_filtered)) {
        input_error = true;
        break;
      }
    }
    else if (buffer_header.id == 24) {
      if (verbose) printf(" --- decoding TRIGGER FIFO \n");
      if (!decode_trigger(buffer.data(), main_header.device, buffer_header.id, buffer_header.size, records)) {
        input_error = true;
        break;
      }
    }
    else {
      std::cerr << " --- [WARNING] skipping unsupported buffer id: " << buffer_header.id << std::endl;
    }
  }

  if (input_error) {
    std::cerr << " --- [ERROR] input decode failed; output file will not be written" << std::endl;
    return 1;
  }
  
  double integrated = (double)integrated_rollover * 0.0001024;
  double integrated_rate = integrated > 0.0 ? (double)integrated_hits / integrated : 0.0;
  std::cout << " --- integrated seconds: " << integrated << std::endl;
  std::cout << " --- integrated hits: " <<integrated_hits << std::endl;
  std::cout << " --- integrated rate: " << integrated_rate << std::endl;
  std::cout << " --- entries: " << records.size() << std::endl;
  if (unexpected_word_count > 0) {
    std::cout << " --- unexpected words: " << unexpected_word_count;
    if (unexpected_word_count > kUnexpectedPrintLimit) {
      std::cout << " (suppressed " << (unexpected_word_count - kUnexpectedPrintLimit) << ")";
    }
    std::cout << std::endl;
  }

  /** open output file and write tree **/
  std::cout << " --- opening output file: " << output_filename << std::endl;
  std::unique_ptr<TFile> fout(TFile::Open(output_filename.c_str(), "RECREATE"));
  if (!fout || fout->IsZombie()) {
    std::cerr << " --- [ERROR] cannot open output file: " << output_filename << std::endl;
    return 1;
  }
  fout->cd();
  TTree tout("alcor", "ALCOR");
  tout.SetDirectory(fout.get());
  data_t data;
  tout.Branch("device", &data.device, "device/I");
  tout.Branch("fifo", &data.fifo, "fifo/I");
  tout.Branch("type", &data.type, "type/I");
  tout.Branch("counter", &data.counter, "counter/I");
  tout.Branch("spill", &data.spill, "spill/I");
  tout.Branch("column", &data.column, "column/I");
  tout.Branch("pixel", &data.pixel, "pixel/I");
  tout.Branch("tdc", &data.tdc, "tdc/I");
  tout.Branch("rollover", &data.rollover, "rollover/I");
  tout.Branch("coarse", &data.coarse, "coarse/I");
  tout.Branch("fine", &data.fine, "fine/I");

  for (const auto &record : records) {
    data = record;
    tout.Fill();
  }

  if (tout.Write() <= 0) {
    std::cerr << " --- [ERROR] failed to write output tree: " << output_filename << std::endl;
    fout->Close();
    return 1;
  }
  std::cout << " --- integrated spill: " << integrated_spill << std::endl;
  fout->Close();
  fout.reset();
  
  /** close input file **/
  fin.close();
  std::cout << " --- all done, so long " << std::endl;

  return 0;
}
