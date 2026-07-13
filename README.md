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

The TDC calibration macro also studies relative channel/TDC offsets using the
newly calibrated TDC times. By default the reference channel is ch22 and the
channels to study are inferred from `config/logbook.json`:

```bash
script/run_tdc_calibration.sh \
  --input ../data/<offset-calibration-run> \
  --offset-reference 22 \
  --offset-window 100
```

Use `--offset-channels 17,19,22` to override the logbook selection, or
`--offset-channels all` to restore the all-channel study.

The ROOT output includes `hChannelTdcOffset`, `hChannelTdcOffsetEntries`,
`hChannelLeadingOffset`, and the `channel_tdc_offsets` tree. The offset value is
the median `t_channel,TDC - t_reference,TDC`, so it can be subtracted from that
channel/TDC time to align it to the reference. Use `--offset-reference event-median`
only when you explicitly want an event-median reference instead of ch22. The
offset study builds events independently for each TDC; `--offset-event-window`
can override the event-building window, otherwise `--offset-window` is used.

### Timewalk / ToT Calibration

The standard output is:

```text
calibration/timewalk_correction.root
```

By default, trigger mode extracts timewalk versus the trigger/reference channel,
normally ch22. The input times already include the fine-TDC calibration and the
static channel/TDC offset from
`calibration/TDC_calibration.root`. Events are built around the cleaned ch22
trigger; each sensor channel contributes at most one hit per event, chosen as
the closest hit inside `--event-window` (default: same as `--window`).
If a diagonal `dt`/`ToT` selection is used, `--dt-tot-cut-direction below`
keeps `dt <= DT0 + SLOPE*ToT` and is the default; use `above` only to keep the
opposite side of the line.

The default timewalk model is now `inverse-power`. The macro fills the raw
`TH2D(dt vs ToT)`, builds a `TProfile` along ToT, and fits the profile with:

```text
f(ToT) = p0 + p1 / pow(ToT - p2, p3)
```

The applied correction is `t_corr = t_raw - f(ToT)`. The default fit range is
`17:10:30,19:10:30`, so that the effective threshold parameter `p2` stays below
the fit domain. If the free-exponent fit fails or has poor covariance quality,
the macro retries with `p3 = 1`. The older models remain selectable with
`--timewalk-fit-model pol1`, `pol1-plateau`, or `lin-exp-plateau`.

```bash
script/run_timewalk_calibration.sh \
  --mode trigger \
  --input ../data/20260610-171026 \
  --trigger 22 \
  --sensors 17,19
```

The event-median reference is still available explicitly for diagnostics or
global-offset studies:

```bash
script/run_timewalk_calibration.sh \
  --mode trigger \
  --reference-mode event-median \
  --input ../data/20260610-171026 \
  --sensors 17,19,22
```

Laser-intensity mode characterizes `ToT` versus laser intensity from a series of logbook runs:

```bash
script/run_timewalk_calibration.sh \
  --mode intensity \
  --input ../data/20260610-171026 \
  --input ../data/20260610-171245
```

This second mode does not extract an absolute timewalk correction by itself. Without a timing reference, `ToT(intensity)` is observable, but the absolute leading-edge delay versus `ToT` is underconstrained.

The timewalk ROOT output stores the raw `TH2D`, the `TProfile`, the fitted
`TF1`, corrected `TH2D` maps, and before/after `dt` projections. The PDF puts
the main results first and the cut/trigger diagnostics at the end.

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

The analysis always reads `calibration/TDC_calibration.root` for fine timing and
for the static channel/TDC offsets. `--no-fine` disables the fine-time
interpolation, while the static offsets from the calibration file are still
available. It also uses `calibration/timewalk_correction.root` when present,
unless `--no-timewalk` is passed.

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
