#include "laser_intensity_scan_rdf.cxx"

#include <exception>
#include <iostream>
#include <string>

namespace {
void Usage(const char *program)
{
  std::cerr << "usage: " << program
            << " RUNLIST TDC_CALIB CHANNEL_CALIB OUT_PDF OUT_ROOT OUT_TXT"
            << " TRIGGER_CHANNEL SENSOR_CHANNELS MATCH_WINDOW_NS MAX_DURATION_NS"
            << " CLOCK_MHZ USE_FINE USE_LUT FINE_CUT REQUIRE_VALID_TOT"
            << " TRIGGER_DEADTIME_NS TRIGGER_PERIOD_NS TRIGGER_PERIOD_TOLERANCE_NS"
            << " SIGNED_DT TIMEWALK_FIT_RANGES TIMEWALK_FIT_MODEL DT_TOT_CUTS"
            << " TRIGGER_TOT_WINDOW SPILL_RANGE CHANNEL_TOT_WINDOWS EDGE_SPILL"
            << " EDGE_CHANNELS EDGE_PHASE_PERIOD_NS EDGE_SPILL_FRACTION"
            << " TDC_SELECTION REFERENCE_MODE REFERENCE_MIN_CHANNELS"
            << " EVENT_WINDOW_NS DT_TOT_CUT_DIRECTION OPMODE SENSOR_DURATION_NS\n";
}

bool ToBool(const char *value)
{
  return std::stoi(value ? value : "0") != 0;
}
}  // namespace

int main(int argc, char **argv)
{
  if (argc != 37) {
    Usage(argv[0]);
    return 2;
  }

  try {
    laser_intensity_scan_rdf(argv[1],
                             argv[2],
                             argv[3],
                             argv[4],
                             argv[5],
                             argv[6],
                             std::stoi(argv[7]),
                             argv[8],
                             std::stod(argv[9]),
                             std::stod(argv[10]),
                             std::stod(argv[11]),
                             ToBool(argv[12]),
                             ToBool(argv[13]),
                             std::stoi(argv[14]),
                             ToBool(argv[15]),
                             std::stod(argv[16]),
                             std::stod(argv[17]),
                             std::stod(argv[18]),
                             ToBool(argv[19]),
                             argv[20],
                             argv[21],
                             argv[22],
                             argv[23],
                             argv[24],
                             argv[25],
                             std::stoi(argv[26]),
                             argv[27],
                             std::stod(argv[28]),
                             std::stod(argv[29]),
                             argv[30],
                             argv[31],
                             std::stoi(argv[32]),
                             std::stod(argv[33]),
                             argv[34],
                             argv[35],
                             std::stod(argv[36]));
  } catch (const std::exception &error) {
    std::cerr << "run_timewalk_calibration_main: " << error.what() << std::endl;
    return 1;
  }

  return 0;
}
