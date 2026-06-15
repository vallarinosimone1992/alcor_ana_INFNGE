#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <boost/program_options.hpp>
#include "TObject.h"
#include "TFile.h"
#include "TTree.h"

bool verbose = false;
int integrated_rollover = 0;
int integrated_spill = 0;
long long integrated_hits = 0;
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

class output_tree_t {
public:
  bool open(const std::string &output_filename)
  {
    output_filename_ = output_filename;
    tmp_output_filename_ = output_filename + ".tmp";
    std::remove(tmp_output_filename_.c_str());

    file_.reset(TFile::Open(tmp_output_filename_.c_str(), "RECREATE"));
    if (!file_ || file_->IsZombie()) {
      std::cerr << " --- [ERROR] cannot open output file: " << tmp_output_filename_ << std::endl;
      return false;
    }

    file_->cd();
    tree_ = std::make_unique<TTree>("alcor", "ALCOR", 99, nullptr);
    tree_->SetDirectory(file_.get());
    tree_->SetAutoFlush(-64LL * 1024LL * 1024LL);
    tree_->SetAutoSave(-256LL * 1024LL * 1024LL);
    tree_->Branch("device", &data_.device, "device/I");
    tree_->Branch("fifo", &data_.fifo, "fifo/I");
    tree_->Branch("type", &data_.type, "type/I");
    tree_->Branch("counter", &data_.counter, "counter/I");
    tree_->Branch("spill", &data_.spill, "spill/I");
    tree_->Branch("column", &data_.column, "column/I");
    tree_->Branch("pixel", &data_.pixel, "pixel/I");
    tree_->Branch("tdc", &data_.tdc, "tdc/I");
    tree_->Branch("rollover", &data_.rollover, "rollover/I");
    tree_->Branch("coarse", &data_.coarse, "coarse/I");
    tree_->Branch("fine", &data_.fine, "fine/I");
    return true;
  }

  bool fill(const data_t &data)
  {
    data_ = data;
    if (tree_->Fill() < 0) {
      std::cerr << " --- [ERROR] failed to fill output tree" << std::endl;
      return false;
    }
    ++entries_;
    return true;
  }

  size_t entries() const { return entries_; }

  bool finalize()
  {
    file_->cd();
    if (tree_->Write("", TObject::kOverwrite) <= 0) {
      std::cerr << " --- [ERROR] failed to write output tree: " << output_filename_ << std::endl;
      abort();
      return false;
    }

    tree_->SetDirectory(nullptr);
    tree_.reset();
    file_->Close();
    file_.reset();

    std::remove(output_filename_.c_str());
    if (std::rename(tmp_output_filename_.c_str(), output_filename_.c_str()) != 0) {
      std::cerr << " --- [ERROR] cannot rename temporary output file "
                << tmp_output_filename_ << " to " << output_filename_
                << ": " << std::strerror(errno) << std::endl;
      std::remove(tmp_output_filename_.c_str());
      return false;
    }
    finalized_ = true;
    return true;
  }

  void abort()
  {
    if (tree_) {
      tree_->SetDirectory(nullptr);
      tree_.reset();
    }
    if (file_) {
      file_->Close();
      file_.reset();
    }
    if (!tmp_output_filename_.empty()) {
      std::remove(tmp_output_filename_.c_str());
    }
  }

  ~output_tree_t()
  {
    if (!finalized_) {
      abort();
    }
  }

private:
  std::string output_filename_;
  std::string tmp_output_filename_;
  std::unique_ptr<TFile> file_;
  std::unique_ptr<TTree> tree_;
  data_t data_{};
  size_t entries_ = 0;
  bool finalized_ = false;
};

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

bool write_data(output_tree_t &output,
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
  return output.fill({device, fifo, type, counter, spill, column, pixel, tdc, rollover, coarse, fine});
}

bool write_trigger_data(output_tree_t &output,
                        int device,
                        int fifo,
                        int type,
                        int counter,
                        int spill,
                        int rollover,
                        int coarse)
{
  return write_data(output, device, fifo, type, counter, spill, -1, -1, -1, rollover, coarse, -1);
}

bool write_alcor_data(output_tree_t &output,
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
  return write_data(output, device, fifo, 1, -1, spill, column, pixel, tdc, rollover, coarse, fine);
}

bool has_words(uint32_t pos, uint32_t size, uint32_t needed)
{
  return pos <= size && needed <= (size - pos);
}
                
bool decode_trigger(const char *buffer, int device, int fifo, int size, output_tree_t &output)
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
      if (!write_trigger_data(output, device, fifo, 7, counter, spill_counter[fifo], rollover, coarse)) {
        return false;
      }
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
      if (!write_trigger_data(output, device, fifo, 15, counter, spill_counter[fifo], rollover, coarse)) {
        return false;
      }
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
      if (!write_trigger_data(output, device, fifo, 9, counter, spill_counter[fifo], rollover, coarse)) {
        return false;
      }
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

bool decode(const char *buffer, int device, int fifo, int size, output_tree_t &output, bool is_filtered)
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
        if (!write_trigger_data(output, device, fifo, 7, counter, spill_counter[fifo], rollover, coarse)) {
          return false;
        }
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
        if (!write_trigger_data(output, device, fifo, 15, -1, spill_counter[fifo], -1, -1)) {
          return false;
        }
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
        if (!write_trigger_data(output, device, fifo, 15, counter, spill_counter[fifo], rollover, coarse)) {
          return false;
        }
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
      if (!write_alcor_data(output,
                            device,
                            fifo,
                            spill_counter[fifo],
                            hit_column(word),
                            hit_pixel(word),
                            hit_tdc(word),
                            rollover_counter,
                            hit_coarse(word),
                            hit_fine(word))) {
        return false;
      }
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

  /** open output file before decoding so ROOT can flush baskets incrementally **/
  std::cout << " --- opening output file: " << output_filename << std::endl;
  output_tree_t output;
  if (!output.open(output_filename)) {
    return 1;
  }
  
  /** loop over data **/
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
      if (!decode(buffer.data(), main_header.device, buffer_header.id, buffer_header.size, output, is_filtered)) {
        input_error = true;
        break;
      }
    }
    else if (buffer_header.id == 24) {
      if (verbose) printf(" --- decoding TRIGGER FIFO \n");
      if (!decode_trigger(buffer.data(), main_header.device, buffer_header.id, buffer_header.size, output)) {
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
    output.abort();
    return 1;
  }
  
  double integrated = (double)integrated_rollover * 0.0001024;
  double integrated_rate = integrated > 0.0 ? (double)integrated_hits / integrated : 0.0;
  std::cout << " --- integrated seconds: " << integrated << std::endl;
  std::cout << " --- integrated hits: " <<integrated_hits << std::endl;
  std::cout << " --- integrated rate: " << integrated_rate << std::endl;
  std::cout << " --- entries: " << output.entries() << std::endl;
  if (unexpected_word_count > 0) {
    std::cout << " --- unexpected words: " << unexpected_word_count;
    if (unexpected_word_count > kUnexpectedPrintLimit) {
      std::cout << " (suppressed " << (unexpected_word_count - kUnexpectedPrintLimit) << ")";
    }
    std::cout << std::endl;
  }

  if (!output.finalize()) {
    return 1;
  }
  std::cout << " --- integrated spill: " << integrated_spill << std::endl;
  
  /** close input file **/
  fin.close();
  std::cout << " --- all done, so long " << std::endl;

  std::_Exit(EXIT_SUCCESS);
}
