# ALCOR Timing Calibration Notes

## Scope

This document describes the timing model, calibration chain, validation logic, and main interpretation caveats for the ALCOR timing analysis.

It is intentionally focused on the analysis method and on how to read the produced calibration and coincidence outputs. Software usage, scripts, and repository layout are summarized in `README.md`.

## Objective

The goal of the calibration chain is to improve timing resolution by correcting:

- fine-time non-linearity
- time-walk through a ToT-dependent correction
- residual channel-to-channel offsets

The target is sub-nanosecond timing, with an aspirational scale of a few hundred picoseconds if the detector response and dataset quality support it.

## Dataset Roles

Two dataset roles are assumed:

- calibration run: used to build the fine and channel calibration constants
- golden run: used as an independent validation sample

Recommended rule:

- build the calibrations on the calibration run
- validate them on a distinct golden run

Using the same dataset both to build and validate the calibration can hide overfitting or dataset-specific biases.

## Analysis Chain Overview

The intended flow is:

1. decode raw data into ROOT files
2. build the fine-time calibration
3. build the channel-level ToT and offset calibration
4. run coincidence analysis with and without calibration
5. compare calibration-run and golden-run behavior

The analysis works on decoded ROOT files under:

```text
data/<run>/kc705-196/decoded/alcdaq.fifo_*.root
```

## Timing Representation

The raw timestamp is represented as:

```text
time_tick = rollover * 32768 + coarse
tick_ns = 1000 / clock_mhz
```

The calibrated time is then derived from:

```text
time_ns = (time_tick - fine_fraction) * tick_ns
```

when fine correction is enabled, or:

```text
time_ns = time_tick * tick_ns
```

when it is disabled.

## Global TDC Index

Fine calibration is performed per physical TDC, not only per local `tdc=0..3`.

The code uses the global TDC index:

```text
tdc_index = tdc + 4*pixel + 16*column + 128*fifo
```

This matters because:

- each FIFO contains 128 physical TDCs
- different columns and pixels must not be merged into the same calibration entry
- any change in the TDC indexing convention requires the fine and channel calibrations to be regenerated

## Fine-Time Calibration

### Motivation

The raw fine counter is not linearly proportional to time. The measured fine-code distribution reflects the phase relation between an asynchronous discriminator crossing and the TDC clock, and the conversion can be distorted across the 512 fine bins.

### CDF LUT Method

For each physical TDC:

- build a 512-bin histogram of `fine_raw`
- compute the cumulative distribution
- convert the cumulative distribution into a fine-time lookup table

For bin `b` with count `c_b` and total entries `N`:

```text
cum_b = sum_{k<=b} c_k
frac_b = clamp((cum_b - 0.5*c_b) / N, 0, 1)
fine_fraction = frac_b - 0.5
```

The LUT is stored in `hFineLut` with:

- X: global TDC index
- Y: raw fine code
- Z: calibrated fine fraction

### Min/Max and Linear Fallback

The same calibration step also extracts `min` and `max` values from configurable quantiles. If the LUT is disabled or unavailable, the code falls back to a linear mapping:

```text
fine_fraction = (fine_raw - min) / (max - min)
if fine_raw > cut: fine_fraction -= 1
```

with:

```text
cut = (min + max) / 2
```

### When the LUT Assumption Is Valid

The CDF LUT approach is appropriate when the arrival phase relative to the clock is effectively random. Typical good cases are:

- dark counts
- asynchronous detector signals
- calibration pulses not phase-locked to the TDC clock

In that case the fine histogram should look roughly like a plateau with edge roll-off. If the fine code instead collapses into narrow peaks, the LUT can become biased and a different calibration strategy may be needed.

## Channel Calibration

The channel calibration has two parts:

- a ToT-dependent correction
- a residual static offset per channel

### Time-Walk Correction

Leading-edge timing depends on pulse amplitude. Time-over-Threshold is used as a proxy for amplitude, so the calibration builds a correction as a function of ToT.

Procedure:

1. choose a reference channel
2. build `dt vs ToT` for each channel
3. estimate the correction using the median in each ToT bin
4. apply the correction once and recompute
5. use the refined curve as the final channel correction

Important details:

- the correction is applied only to leading edges
- a valid trailing edge must exist so that ToT is defined
- if `max_duration <= 0`, the ToT correction is skipped

Application convention:

```text
t_lead -= CorrectionNs(channel, ToT)
```

### Channel Offsets

After the ToT-dependent part is applied, a residual fixed offset may remain. The channel calibration stores a static offset for each channel to absorb this remaining shift.

The current ROOT output stores both:

- `hChanCalib_chN`: ToT-dependent correction histogram
- `hChanOffset`: global per-channel offset summary

### Symmetric Reference Mode

In symmetric mode, the correction is split between the calibrated channel and the reference channel, rather than keeping the reference artificially fixed.

This is often useful for two-channel datasets, but it should be interpreted carefully once more channels contribute to the same reference scale.

## Coincidence Analysis

Coincidence studies are configured through `config/*.txt`.

Supported line formats:

- pair line: `chA chB [window_ns]`
- group line: `group ch1 ch2 ch3 [window=ns]`

Typical outputs include:

- search histograms
- coincidence histograms
- FWHM estimates
- `dt vs fine` 2D plots
- ROOT and TXT summaries

The coincidence TXT files are particularly useful for compact comparisons. For a simple pair configuration, the main summary is usually on the line that reports:

```text
fwhm_bkg_sub=...
```

## Important Implementation Caveat

The current `coincidence_rdf.cxx` implementation does not enforce unique or nearest-neighbour matching for pair coincidences. Instead, it fills all hit pairs found inside the coincidence window.

Implications:

- coincidence counts can be inflated when multiple hits are present in the same window
- the width of the coincidence peak can be biased by combinatorial matches
- dataset-to-dataset comparisons must be interpreted with this in mind

This behavior is especially important when pair multiplicity is not negligible.

## Fine Cut

An optional fine cut rejects hits near the wrap point:

```text
abs(fine_raw - cut) <= fine_cut
```

where `cut` is derived from the calibrated `min` and `max` for the corresponding TDC.

## Validation Metrics for Fine Calibration

`fine_validation_rdf` reports several metrics to assess how well the calibrated fine fraction approaches a uniform distribution on `[-0.5, 0.5]`.

Main metrics:

- KS `D`: Kolmogorov-Smirnov distance to uniform
- KS p-value: larger is better
- AD (`A^2`): Anderson-Darling statistic
- mean: should be close to 0
- std: should be close to `0.288675` for a uniform distribution on `[-0.5, 0.5]`

Reported categories:

- `raw`: no LUT correction
- `intrinsic`: LUT evaluated on the same sample used to build it
- `cross`: LUT evaluated on a held-out split

Typical supporting plots:

- CDF residuals
- DNL
- INL

## Reading the Output Files

### Fine Calibration Output

`fine_calibration.root` typically contains:

- `hFineMin`
- `hFineMax`
- `hFineEntries`
- `hFineLut`
- `fine_calib` tree

Useful checks:

- number of valid TDCs
- `min` and `max` values for the active TDCs
- whether the active TDC set matches the channels actually used in the run

### Channel Calibration Output

`channel_calibration.root` typically contains:

- `hChanOffset`
- `hChanCalib_ch0..31`
- metadata such as reference channel, window, ToT range, and symmetric-reference mode

Useful checks:

- per-channel offset magnitudes
- number of nonzero ToT bins per active channel
- whether most of the correction is a static offset or a real ToT-dependent shape

### Coincidence Output

The coincidence TXT output is the fastest place to compare runs.

For a simple pair analysis, check:

- pair definition
- coincidence count
- `fwhm_bkg_sub`
- background estimate
- left/right half-maximum crossings

The PDF is then used to inspect whether the peak shape and `dt vs fine` behavior are physically sensible.

## Practical Validation Strategy

Recommended comparison sequence:

1. build fine and channel calibration on the calibration run
2. compare calibration run without calibration vs with calibration
3. compare golden run without calibration vs with calibration
4. inspect `dt vs fine` and, if relevant, `dt vs ToT`
5. confirm that the calibrated result improves, or at least does not degrade, the independent golden run

If the calibration run improves but the golden run worsens, likely explanations include:

- overfitting to the calibration sample
- unstable ToT or offset extraction
- coincidence-window combinatorics
- a mismatch between the calibration sample and the golden sample

## Documentation Policy

Performance statements should be tied to specific generated outputs and run identifiers, not kept as timeless claims in this document.

In particular:

- do not assume a fixed ToT-correction amplitude across datasets
- do not assume a fixed resolution improvement
- always quote the relevant output artifact or regenerate the result on the current calibration and golden runs
