# alcor_ana_INFNGE

ROOT/RDataFrame-based analysis for ALCOR timing studies. This package works on decoded ROOT files (`alcdaq.fifo_*.root`) and provides:

- fine-time calibration
- channel-level ToT and offset calibration
- coincidence analysis
- validation scripts for fine calibration and time-walk calibration

Algorithmic details and calibration notes are documented in `REPORT_CALIBRATION.md`. This README is intentionally limited to software structure and usage.

## Requirements

- ROOT available in the environment
- decoder built and available at `decoder/bin/decoder`, or provided via `DECODER_BIN`

Optional but recommended:

- set `ALCOR_ANA_GE` to this directory so the scripts resolve paths consistently

```bash
export ALCOR_ANA_GE=/Users/simone/Work/BNL/EIC/ePIC/ALCOR/directory_apcx12/alcor_ana_INFNGE
```

If `ALCOR_ANA_GE` is not set, scripts fall back to paths relative to their own location.

## Directory Layout

- `script/`: entry-point shell scripts
- `macro/`: ROOT C++ macros
- `config/`: coincidence pair and group configuration files
- `calibration/`: generated calibration ROOT files
- `output/`: generated PDF, ROOT, TXT, and log outputs
- `decoder/`: decoder sources, build tree, and binary

## Data Layout

The analysis distinguishes raw data and decoded data:

- `../raw_data/<run>/...`: raw `.dat` files and DAQ-side run content
- `../data/<run>/kc705-196/decoded/*.root`: decoded ROOT files used by the analysis

Common symlinks:

- `../raw_data/calibration`
- `../raw_data/golden_run`
- `../data/calibration`
- `../data/golden_run`

Important:

- pipeline scripts can depend on both `raw_data/*` and `data/*`
- if you retarget the calibration or golden dataset, either update both symlink pairs or pass explicit input paths
- calibration files should be built from the calibration run and then validated on an independent golden run

## Build the Decoder

The analysis scripts expect a decoder binary at `decoder/bin/decoder` unless `DECODER_BIN` is set explicitly.

Standard build:

```bash
script/build_decoder.sh
```

Useful options:

- `script/build_decoder.sh --clean`: remove the build directory before configuring
- `script/build_decoder.sh -j 8`: build with a fixed number of parallel jobs
- `script/build_decoder.sh -h`: show all supported options

The build script configures CMake in `decoder/build`, installs into `decoder/local`, and creates or refreshes the `decoder/bin/decoder` symlink automatically.

After the build, you should have:

```text
decoder/bin/decoder
```

## Quick Start

### 1. Decode raw data

```bash
script/decode_raw.sh /path/to/raw_or_run [--force]
```

Decoded files are written to:

```text
../data/<run>/kc705-196/decoded/alcdaq.fifo_*.root
```

By default the decoder is taken from `decoder/bin/decoder`. Override with `DECODER_BIN=...` if needed.

### 2. Build the fine calibration

```bash
script/run_fine_calibration.sh -i ../data/calibration
```

Outputs:

- `calibration/fine_calibration.root`
- `output/fine_calibration.pdf`

### 3. Build the channel calibration

```bash
script/run_channel_calibration.sh -i ../data/calibration -k calibration/fine_calibration.root
```

Output:

- `calibration/channel_calibration.root`

### 4. Run coincidence analysis

```bash
script/run_coincidence.sh \
  -i ../data/<run> \
  -p config/coincidence_17_19.txt \
  -k calibration/fine_calibration.root \
  -K calibration/channel_calibration.root
```

Outputs are written to `output/` as PDF, ROOT, and TXT files.

### 5. Optional validation

Fine-calibration validation:

```bash
script/run_fine_validation.sh -i ../data/calibration
```

Time-walk validation:

```bash
script/run_tw_validation.sh \
  --calib-input ../data/calibration \
  --golden-input ../data/golden_run
```

## Main Scripts

- `decode_raw.sh`: decode raw `.dat` files into ROOT files
- `build_decoder.sh`: build the decoder from `decoder/src/`
- `run_fine_calibration.sh`: build the fine calibration ROOT file
- `run_fine_validation.sh`: validate the fine calibration
- `run_channel_calibration.sh`: build per-channel ToT and offset calibration
- `run_coincidence.sh`: run coincidence analysis and write PDF/ROOT/TXT outputs
- `run_plot.sh`: channel and spill plots
- `run_plot_lut.sh`: visualize the fine LUT
- `run_tw_validation.sh`: validate time-walk corrections
- `run_golden_pipeline.sh`: compact decode -> calibrate -> coincidence workflow
- `run_test_calib_pipeline.sh`: extended calibration/golden comparison workflow

## Typical Outputs

Calibration products:

- `calibration/fine_calibration.root`
- `calibration/channel_calibration.root`

Run outputs:

- `output/<label>.pdf`
- `output/<label>.root`
- `output/<label>.txt`
- `output/log_<label>_macro.txt`

## Practical Notes

- `run_golden_pipeline.sh` and `run_test_calib_pipeline.sh` expect the standard calibration and golden symlinks unless you edit the script or invoke lower-level commands directly.
- `run_coincidence.sh` can be pointed to explicit decoded paths if you do not want to rely on symlinks.
- If you change the calibration dataset, regenerate both fine and channel calibration files before comparing runs.

## Further Documentation

See `REPORT_CALIBRATION.md` for:

- timing model details
- fine LUT construction
- ToT and offset calibration method
- validation metrics
- important caveats when interpreting the outputs
