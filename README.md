# alcor_ana_INFNGE

ROOT/RDataFrame-based analysis for ALCOR timing data. The public workflow is now intentionally small:

- build/decode raw data
- read/update `config/logbook.json`
- build the standard TDC calibration
- build the standard timewalk/ToT calibration
- run the standard analysis with JSON-defined coincidence groups

## Environment

ROOT must be available in the shell. The scripts can resolve the repository automatically, but setting `ALCOR_ANA_GE` is still useful:

```bash
export ALCOR_ANA_GE=/Users/simone/Work/BNL/EIC/ePIC/ALCOR/directory_apcx12/alcor_ana_INFNGE
```

Raw and decoded data are expected next to this repository:

```text
../raw_data/<run>/...
../data/<run>/kc705-196/decoded/alcdaq.fifo_*.root
```

## Decoder

Build the decoder:

```bash
script/build_decoder.sh
```

Decode one raw run:

```bash
script/decode_raw.sh ../raw_data/20260610-171026 --force
```

Decode runs listed in the logbook:

```bash
script/decode_logbook_runs.sh --mode 1 --force
```

## Logbook

The canonical local logbook used by the scripts is:

```text
config/logbook.json
```

Update it from Google Sheets. The script downloads the CSV copy and regenerates
the JSON logbook used by the software:

```bash
script/get_logbook.sh
```

The downloaded CSV is kept at `config/logbook.csv`; it is treated as an input
artifact. The generated JSON stores normalized run keys, typed values, and
arrays for fields such as `channels` and `operating_mode`.

The parser normalizes run names from `YYYYMMDD_HHMMSS`, `YYYYMMDD-HHMMSS`, and
rows with a trailing `/`.

## Calibrations

### TDC Calibration

The standard output is:

```text
calibration/TDC_calibration.root
```

Run it on one or more decoded inputs:

```bash
script/run_tdc_calibration.sh \
  --input ../data/20260610-171026 \
  --input ../data/20260610-171245
```

Every input run is checked against `config/logbook.json`; all `Operating Mode` values for that run must be `1`.

### Timewalk / ToT Calibration

The standard output is:

```text
calibration/timewalk_correction.root
```

Trigger-based mode keeps the previous three-channel method, for example trigger channel 22 and signal channels 17,19:

```bash
script/run_timewalk_calibration.sh \
  --mode trigger \
  --input ../data/20260610-171026 \
  --trigger 22 \
  --sensors 17,19
```

Laser-intensity mode characterizes `ToT` versus laser intensity from a series of logbook runs:

```bash
script/run_timewalk_calibration.sh \
  --mode intensity \
  --input ../data/20260610-171026 \
  --input ../data/20260610-171245
```

This second mode does not extract an absolute timewalk correction by itself. Without a timing reference, `ToT(intensity)` is observable, but the absolute leading-edge delay versus `ToT` is underconstrained.

Practical options for the future 8-channel setup:

- reserve one channel for a synchronous pulser/laser reference during dedicated calibration runs
- split the calibration into repeated runs with one temporary reference channel if simultaneous trigger readout is impossible
- use pairwise relative timewalk between channels illuminated by the same laser pulse, then solve a relative correction graph
- keep the ToT/intensity scan as a stability and response-linearity diagnostic, not as the sole timing correction

Run both calibration steps with one command:

```bash
script/run_all_calibrations.sh \
  --tdc-input ../data/20260610-171026 \
  --tw-input ../data/20260610-171245 \
  --tw-mode intensity
```

## Analysis

Default analysis config:

```text
config/analysis.json
```

It defines active channels (`"logbook"` by default), timing settings, and coincidence groups. Groups may contain two or more channels:

```json
{
  "channels": "logbook",
  "coincidences": [
    { "name": "ch17_ch19", "channels": [17, 19], "window_ns": 20 },
    { "name": "triple", "channels": [17, 19, 22], "window_ns": 30 }
  ]
}
```

Run the analysis:

```bash
script/run_analysis.sh --input ../data/20260610-171026
```

Run the same analysis without the timewalk correction, while still using the fine-TDC calibration:

```bash
script/run_analysis.sh --input ../data/20260610-171026 --no-timewalk
```

Outputs are written to `output/`:

```text
output/<run>_channels.pdf
output/<run>_coincidence.pdf
output/<run>_coincidence.root
output/<run>_coincidence.txt
```

The analysis always uses `calibration/TDC_calibration.root` for fine timing unless explicitly disabled with `--no-fine`. It also uses `calibration/timewalk_correction.root` when present, unless `--no-timewalk` is passed.

## Public Scripts

- `script/build_decoder.sh`
- `script/decode_raw.sh`
- `script/decode_logbook_runs.sh`
- `script/get_logbook.sh`
- `script/run_tdc_calibration.sh`
- `script/run_timewalk_calibration.sh`
- `script/run_all_calibrations.sh`
- `script/run_analysis.sh`

The remaining ROOT macros are implementation details for these entry points.
