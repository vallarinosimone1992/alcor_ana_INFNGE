#include "fine_calibration_rdf.cxx"

void TDC_calibration_rdf(const char *input = "../data/TDC_calibration",
                         const char *out_root = "TDC_calibration.root",
                         double q_low = 0.01,
                         double q_high = 0.99,
                         int min_entries = 200,
                         const char *out_pdf = "TDC_calibration.pdf",
                         double max_duration_ns = 0.0,
                         bool match_coincidence = false,
                         double clock_mhz = 320.0,
                         bool study_channel_offsets = true,
                         int offset_reference_channel = 22,
                         double offset_match_window_ns = 100.0,
                         int offset_min_channels = 3,
                         const char *offset_channels_csv = "",
                         double offset_event_window_ns = 0.0)
{
  std::string input_value = input ? input : "";
  std::string output_value = out_root ? out_root : "";
  if (input_value == "-h" || input_value == "--help" || input_value == "help" ||
      output_value == "-h" || output_value == "--help" || output_value == "help") {
    std::cout << "TDC_calibration_rdf usage:\n";
    std::cout << "  TDC_calibration_rdf(\"/path/to/decoded_or_list\", \"TDC_calibration.root\","
              << " 0.01, 0.99, 200, \"TDC_calibration.pdf\", 0.0, false, 320.0, true, 22, 100.0, 3, \"17,19,22\", 0.0)\n";
    std::cout << "  output histograms: hFineMin, hFineMax, hFineLut\n";
    std::cout << "  optional channel/TDC offset study uses calibrated TDC times and writes hChannelTdcOffset\n";
    std::cout << "  offset_reference_channel defaults to reference time on ch22; -1 uses an event-median reference\n";
    std::cout << "  offset_event_window_ns defines event building; 0 uses offset_match_window_ns\n";
    std::cout << "  offset_channels_csv limits the offset study to selected channels; empty means all channels\n";
    return;
  }
  fine_calibration_rdf(input,
                       out_root,
                       q_low,
                       q_high,
                       min_entries,
                       out_pdf,
                       max_duration_ns,
                       match_coincidence,
                       clock_mhz,
                       study_channel_offsets,
                       offset_reference_channel,
                       offset_match_window_ns,
                       offset_min_channels,
                       offset_channels_csv,
                       offset_event_window_ns);
}
