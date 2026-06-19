#include "TDC_calibration_rdf.cxx"

#include <exception>
#include <iostream>
#include <string>

namespace {
void Usage(const char *program)
{
  std::cerr << "usage: " << program
            << " INPUT_LIST OUT_ROOT Q_LOW Q_HIGH MIN_ENTRIES OUT_PDF"
            << " MAX_DURATION_NS MATCH_COINCIDENCE CLOCK_MHZ OFFSET_STUDY"
            << " OFFSET_REFERENCE_CHANNEL OFFSET_WINDOW_NS OFFSET_MIN_CHANNELS"
            << " OFFSET_CHANNELS_CSV OFFSET_EVENT_WINDOW_NS\n";
}

bool ToBool(const char *value)
{
  return std::stoi(value ? value : "0") != 0;
}
}  // namespace

int main(int argc, char **argv)
{
  if (argc != 16) {
    Usage(argv[0]);
    return 2;
  }

  try {
    TDC_calibration_rdf(argv[1],
                        argv[2],
                        std::stod(argv[3]),
                        std::stod(argv[4]),
                        std::stoi(argv[5]),
                        argv[6],
                        std::stod(argv[7]),
                        ToBool(argv[8]),
                        std::stod(argv[9]),
                        ToBool(argv[10]),
                        std::stoi(argv[11]),
                        std::stod(argv[12]),
                        std::stoi(argv[13]),
                        argv[14],
                        std::stod(argv[15]));
  } catch (const std::exception &error) {
    std::cerr << "run_tdc_calibration_main: " << error.what() << std::endl;
    return 1;
  }

  return 0;
}
