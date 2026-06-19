#include "tot_intensity_scan_rdf.cxx"

#include <exception>
#include <iostream>
#include <string>

namespace {
void Usage(const char *program)
{
  std::cerr << "usage: " << program
            << " RUNLIST TDC_CALIB OUT_PDF OUT_ROOT OUT_TXT MAX_DURATION_NS"
            << " CLOCK_MHZ USE_FINE USE_LUT FINE_CUT\n";
}

bool ToBool(const char *value)
{
  return std::stoi(value ? value : "0") != 0;
}
}  // namespace

int main(int argc, char **argv)
{
  if (argc != 11) {
    Usage(argv[0]);
    return 2;
  }

  try {
    tot_intensity_scan_rdf(argv[1],
                           argv[2],
                           argv[3],
                           argv[4],
                           argv[5],
                           std::stod(argv[6]),
                           std::stod(argv[7]),
                           ToBool(argv[8]),
                           ToBool(argv[9]),
                           std::stoi(argv[10]));
  } catch (const std::exception &error) {
    std::cerr << "run_tot_intensity_scan_main: " << error.what() << std::endl;
    return 1;
  }

  return 0;
}
