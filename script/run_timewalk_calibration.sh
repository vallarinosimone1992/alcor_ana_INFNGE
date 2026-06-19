#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_timewalk_calibration.sh --input PATH [--input PATH ...] [options]

Create/update calibration/timewalk_correction.root.

Modes:
  --mode trigger       timewalk extraction from a timing reference in the same run (default)
  --mode intensity     ToT versus laser-intensity characterization without
                       extracting an absolute timewalk correction

Common options:
  -i, --input PATH         decoded dir, run dir, or ROOT file (repeatable)
  -l, --logbook FILE      JSON logbook (default: config/logbook.json)
  -k, --tdc-calib FILE    TDC calibration ROOT file (default: calibration/TDC_calibration.root)
      --calib FILE        alias for --tdc-calib
  -o, --output FILE       output ROOT file (default: calibration/timewalk_correction.root)
  -P, --pdf FILE          output PDF file (default: output/timewalk_correction.pdf)
  -T, --txt FILE          output text summary (default: output/timewalk_correction.txt)
      --channels LIST     channels for --mode intensity, or metadata override
  -d, --duration NS       max leading-trailing duration / ToT range (default: 30)
  -m, --clock MHz         clock frequency (default: 320)
  -f, --use-fine [0|1]    enable fine timing (default: 1)
      --no-fine           disable fine timing
      --no-lut            disable fine LUT
      --fine-cut N        exclude hits close to fine wrap cut (default: 0)
      --dry-run           print commands without executing them
  -h, --help              show this help

Trigger-mode options:
      --reference-mode trigger|event-median
                           trigger uses --trigger as laser reference (default: ch22);
                           event-median uses selected SiPMs as laser reference
      --reference-min-channels N
                           minimum channels per event-median cluster (default: 3)
      --trigger CH        laser trigger/reference channel (default: 22)
      --sensors CSV       SiPM channels to correct (default: logbook; the trigger
                           channel is removed in --reference-mode trigger)
  -w, --window NS         trigger matching window (default: 100)
      --event-window NS   event-building window (default: same as --window)
      --trigger-veto NS   veto after an accepted trigger, alias --trigger-deadtime
                           (default: 0; e.g. 20 rejects 12-15 ns rebounds)
      --trigger-period NS expected valid trigger period (default: 1000000)
      --trigger-period-tol NS
                           tolerance for trigger-period cleanup (default: 50000)
      --signed-dt         match nearest trigger and keep signed time differences
      --timewalk-fit-ranges CSV
                           fit ranges per channel (default: 17:10:30,19:10:30)
      --timewalk-fit-model MODEL
                           inverse-power, pol1, pol1-plateau, or lin-exp-plateau
                           (default: inverse-power)
      --dt-tot-cut CSV    diagonal cut CH:DT0:SLOPE[:TOT_MIN:TOT_MAX]
      --dt-tot-cut-direction below|above
                           below keeps dt <= line; above keeps dt >= line (default: below)
      --trigger-tot-window CSV
                           trigger ToT window MIN:MAX
      --spill-range CSV   inclusive spill selection range MIN:MAX
      --channel-tot-window CSV
                           per-channel ToT window CH:MIN:MAX[,CH:MIN:MAX]
      --tdc-selection CSV  leading TDC selection, TDC or CH:TDC CSV
                           examples: 0 or 17:0,19:2,22:0; trailing partner is kept for ToT
      --edge-spill N      spill for edge maps (default: first selected spill)
      --edge-channels CSV all, analysis, or CSV (default: all)
      --edge-phase-period NS
                           period used to fold edge maps
      --edge-spill-fraction X
                           initial spill fraction used by edge maps (default: 0.01)
      --no-require-tot    also match leading hits without valid ToT
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
source "${script_dir}/lib/logbook.sh"
source "${script_dir}/lib/root_tools.sh"

inputs=()
mode="trigger"
logbook="$(logbook_default_path)"
tdc_calib="${qa_dir}/calibration/TDC_calibration.root"
out_root="${qa_dir}/calibration/timewalk_correction.root"
out_pdf="${qa_dir}/output/timewalk_correction.pdf"
out_txt="${qa_dir}/output/timewalk_correction.txt"
channels_override=""
duration_ns=30
clock_mhz=320
use_fine=1
use_lut=1
fine_cut=0
dry_run=0

reference_mode="trigger"
reference_min_channels=3
trigger_channel=22
sensor_channels="logbook"
match_window_ns=100
event_window_ns=0
trigger_deadtime_ns=0
trigger_period_ns=1000000
trigger_period_tolerance_ns=50000
trigger_period_set=0
trigger_period_tolerance_set=0
signed_dt=0
timewalk_fit_ranges="17:10:30,19:10:30"
timewalk_fit_model="inverse-power"
dt_tot_cut=""
dt_tot_cut_direction="below"
trigger_tot_window=""
spill_range=""
channel_tot_windows=""
tdc_selection=""
edge_spill=-1
edge_channels="all"
edge_phase_period_ns=0
edge_spill_fraction=0.01
require_tot=1

need_arg() {
  if [ "$#" -lt 2 ] || [ -z "${2-}" ]; then
    echo "Missing value for $1" >&2
    usage >&2
    exit 1
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --mode)
      need_arg "$@"
      mode=${2:-}
      shift 2
      ;;
    --mode=*)
      mode=${1#*=}
      shift
      ;;
    -i|--input)
      need_arg "$@"
      inputs+=("${2:-}")
      shift 2
      ;;
    --input=*)
      inputs+=("${1#*=}")
      shift
      ;;
    -l|--logbook)
      need_arg "$@"
      logbook=${2:-}
      shift 2
      ;;
    --logbook=*)
      logbook=${1#*=}
      shift
      ;;
    -k|--tdc-calib|--calib)
      need_arg "$@"
      tdc_calib=${2:-}
      shift 2
      ;;
    --tdc-calib=*|--calib=*)
      tdc_calib=${1#*=}
      shift
      ;;
    -o|--output)
      need_arg "$@"
      out_root=${2:-}
      shift 2
      ;;
    --output=*)
      out_root=${1#*=}
      shift
      ;;
    -P|--pdf)
      need_arg "$@"
      out_pdf=${2:-}
      shift 2
      ;;
    --pdf=*)
      out_pdf=${1#*=}
      shift
      ;;
    -T|--txt)
      need_arg "$@"
      out_txt=${2:-}
      shift 2
      ;;
    --txt=*)
      out_txt=${1#*=}
      shift
      ;;
    --channels)
      need_arg "$@"
      channels_override=${2:-}
      shift 2
      ;;
    --channels=*)
      channels_override=${1#*=}
      shift
      ;;
    -d|--duration)
      need_arg "$@"
      duration_ns=${2:-}
      shift 2
      ;;
    --duration=*)
      duration_ns=${1#*=}
      shift
      ;;
    -m|--clock)
      need_arg "$@"
      clock_mhz=${2:-}
      shift 2
      ;;
    --clock=*)
      clock_mhz=${1#*=}
      shift
      ;;
    -f|--use-fine)
      if [ "${2:-}" = "0" ] || [ "${2:-}" = "1" ]; then
        use_fine=$2
        shift 2
      else
        use_fine=1
        shift
      fi
      ;;
    --use-fine=*)
      use_fine=${1#*=}
      shift
      ;;
    --no-fine)
      use_fine=0
      shift
      ;;
    --no-lut)
      use_lut=0
      shift
      ;;
    --fine-cut)
      need_arg "$@"
      fine_cut=${2:-}
      shift 2
      ;;
    --fine-cut=*)
      fine_cut=${1#*=}
      shift
      ;;
    --trigger)
      need_arg "$@"
      trigger_channel=${2:-}
      shift 2
      ;;
    --trigger=*)
      trigger_channel=${1#*=}
      shift
      ;;
    --sensors)
      need_arg "$@"
      sensor_channels=${2:-}
      shift 2
      ;;
    --sensors=*)
      sensor_channels=${1#*=}
      shift
      ;;
    --reference-mode|--time-reference|--timewalk-reference)
      need_arg "$@"
      reference_mode=${2:-}
      shift 2
      ;;
    --reference-mode=*|--time-reference=*|--timewalk-reference=*)
      reference_mode=${1#*=}
      shift
      ;;
    --reference-min-channels|--event-reference-min-channels)
      need_arg "$@"
      reference_min_channels=${2:-}
      shift 2
      ;;
    --reference-min-channels=*|--event-reference-min-channels=*)
      reference_min_channels=${1#*=}
      shift
      ;;
    -w|--window)
      need_arg "$@"
      match_window_ns=${2:-}
      shift 2
      ;;
    --window=*)
      match_window_ns=${1#*=}
      shift
      ;;
    --event-window|--event-window-ns)
      need_arg "$@"
      event_window_ns=${2:-}
      shift 2
      ;;
    --event-window=*|--event-window-ns=*)
      event_window_ns=${1#*=}
      shift
      ;;
    --trigger-deadtime|--trigger-veto|--trigger-veto-ns)
      need_arg "$@"
      trigger_deadtime_ns=${2:-}
      shift 2
      ;;
    --trigger-deadtime=*|--trigger-veto=*|--trigger-veto-ns=*)
      trigger_deadtime_ns=${1#*=}
      shift
      ;;
    --trigger-period)
      need_arg "$@"
      trigger_period_ns=${2:-}
      trigger_period_set=1
      shift 2
      ;;
    --trigger-period=*)
      trigger_period_ns=${1#*=}
      trigger_period_set=1
      shift
      ;;
    --trigger-period-tol|--trigger-period-tolerance)
      need_arg "$@"
      trigger_period_tolerance_ns=${2:-}
      trigger_period_tolerance_set=1
      shift 2
      ;;
    --trigger-period-tol=*|--trigger-period-tolerance=*)
      trigger_period_tolerance_ns=${1#*=}
      trigger_period_tolerance_set=1
      shift
      ;;
    --signed-dt)
      signed_dt=1
      shift
      ;;
    --timewalk-fit-ranges)
      need_arg "$@"
      timewalk_fit_ranges=${2:-}
      shift 2
      ;;
    --timewalk-fit-ranges=*)
      timewalk_fit_ranges=${1#*=}
      shift
      ;;
    --timewalk-fit-model)
      need_arg "$@"
      timewalk_fit_model=${2:-}
      shift 2
      ;;
    --timewalk-fit-model=*)
      timewalk_fit_model=${1#*=}
      shift
      ;;
    --dt-tot-cut|--timewalk-dt-tot-cut|--timewalk-selection-cut)
      need_arg "$@"
      dt_tot_cut=${2:-}
      shift 2
      ;;
    --dt-tot-cut=*|--timewalk-dt-tot-cut=*|--timewalk-selection-cut=*)
      dt_tot_cut=${1#*=}
      shift
      ;;
    --dt-tot-cut-direction|--dt-tot-direction|--timewalk-selection-direction)
      need_arg "$@"
      dt_tot_cut_direction=${2:-}
      shift 2
      ;;
    --dt-tot-cut-direction=*|--dt-tot-direction=*|--timewalk-selection-direction=*)
      dt_tot_cut_direction=${1#*=}
      shift
      ;;
    --trigger-tot-window|--trigger-tot-cut|--reference-tot-window|--ref-tot-window)
      need_arg "$@"
      trigger_tot_window=${2:-}
      shift 2
      ;;
    --trigger-tot-window=*|--trigger-tot-cut=*|--reference-tot-window=*|--ref-tot-window=*)
      trigger_tot_window=${1#*=}
      shift
      ;;
    --spill-range|--spills|--spill-window)
      need_arg "$@"
      spill_range=${2:-}
      shift 2
      ;;
    --spill-range=*|--spills=*|--spill-window=*)
      spill_range=${1#*=}
      shift
      ;;
    --channel-tot-window|--channel-tot-range|--tot-window-by-channel|--duration-window)
      need_arg "$@"
      channel_tot_windows=${2:-}
      shift 2
      ;;
    --channel-tot-window=*|--channel-tot-range=*|--tot-window-by-channel=*|--duration-window=*)
      channel_tot_windows=${1#*=}
      shift
      ;;
    --tdc-selection|--leading-tdc|--tdc-filter|--timewalk-tdc-selection)
      need_arg "$@"
      tdc_selection=${2:-}
      shift 2
      ;;
    --tdc-selection=*|--leading-tdc=*|--tdc-filter=*|--timewalk-tdc-selection=*)
      tdc_selection=${1#*=}
      shift
      ;;
    --edge-spill|--edge-plot-spill)
      need_arg "$@"
      edge_spill=${2:-}
      shift 2
      ;;
    --edge-spill=*|--edge-plot-spill=*)
      edge_spill=${1#*=}
      shift
      ;;
    --edge-channels|--edge-plot-channels)
      need_arg "$@"
      edge_channels=${2:-}
      shift 2
      ;;
    --edge-channels=*|--edge-plot-channels=*)
      edge_channels=${1#*=}
      shift
      ;;
    --edge-phase-period|--edge-period)
      need_arg "$@"
      edge_phase_period_ns=${2:-}
      shift 2
      ;;
    --edge-phase-period=*|--edge-period=*)
      edge_phase_period_ns=${1#*=}
      shift
      ;;
    --edge-spill-fraction|--edge-fraction)
      need_arg "$@"
      edge_spill_fraction=${2:-}
      shift 2
      ;;
    --edge-spill-fraction=*|--edge-fraction=*)
      edge_spill_fraction=${1#*=}
      shift
      ;;
    --no-require-tot)
      require_tot=0
      shift
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ "${#inputs[@]}" -eq 0 ]; then
  usage >&2
  exit 1
fi
case "${mode}" in
  trigger|intensity) ;;
  *)
    echo "Unknown mode: ${mode}" >&2
    usage >&2
    exit 1
    ;;
esac
case "${reference_mode}" in
  event-median|event_median|median|event)
    reference_mode="event-median"
    ;;
  trigger|ch22|reference-channel|reference_channel)
    reference_mode="trigger"
    ;;
  *)
    echo "Unknown reference mode: ${reference_mode}" >&2
    usage >&2
    exit 1
    ;;
esac
case "${dt_tot_cut_direction}" in
  below|keep-below|keep_below|upper|max|"<="|lt)
    dt_tot_cut_direction="below"
    ;;
  above|keep-above|keep_above|lower|min|">="|gt)
    dt_tot_cut_direction="above"
    ;;
  *)
    echo "Unknown dt/ToT cut direction: ${dt_tot_cut_direction}" >&2
    usage >&2
    exit 1
    ;;
esac
if ! [[ "${reference_min_channels}" =~ ^[0-9]+$ ]] || [ "${reference_min_channels}" -lt 1 ]; then
  echo "Invalid --reference-min-channels: ${reference_min_channels}" >&2
  exit 1
fi

mkdir -p "$(dirname "${out_root}")" "$(dirname "${out_pdf}")" "$(dirname "${out_txt}")" "${qa_dir}/output"
if [ "${dry_run}" -eq 0 ] && [ ! -s "${tdc_calib}" ]; then
  echo "TDC calibration not found: ${tdc_calib}" >&2
  echo "Create it with script/run_tdc_calibration.sh first." >&2
  exit 1
fi

logbook_available=0
if [ -f "${logbook}" ]; then
  logbook_available=1
fi

channels_to_csv() {
  local value="$1"
  python3 - "${value}" <<'PY'
import re
import sys

value = sys.argv[1]
out = []
seen = set()
for token in re.split(r"[,\s_]+", value.strip()):
    if not token:
        continue
    try:
        channel = int(token)
    except ValueError:
        raise SystemExit(f"invalid channel token: {token}")
    if channel < 0 or channel >= 32:
        raise SystemExit(f"channel out of range: {channel}")
    if channel in seen:
        continue
    seen.add(channel)
    out.append(str(channel))
print(",".join(out))
PY
}

remove_channel_from_csv() {
  local value="$1"
  local channel="$2"
  value="${value//,/ }"
  awk -v value="${value}" -v channel="${channel}" 'BEGIN {
    n = split(value, a, /[[:space:]]+/)
    out = ""
    for (i = 1; i <= n; ++i) {
      if (a[i] == "" || a[i] == channel) continue
      if (out != "") out = out ","
      out = out a[i]
    }
    print out
  }'
}

runlist="${qa_dir}/output/timewalk_correction_runlist.tsv"
if [ "${dry_run}" -eq 1 ]; then
  printf ': > %q\n' "${runlist}"
else
  : > "${runlist}"
fi

auto_laser_rate_khz=""
sensor_channels_from_inputs=""
for input in "${inputs[@]}"; do
  run="$(logbook_run_from_path "${input}")"
  label="${run}"
  intensity="0"
  laser_rate=""
  channels="${channels_override}"
  thresholds=""
  vbias=""
  if [ "${logbook_available}" -eq 1 ] && logbook_has_run "${logbook}" "${run}"; then
    intensity="$(logbook_field "${logbook}" "${run}" intensity)"
    laser_rate="$(logbook_field "${logbook}" "${run}" rate || true)"
    if [ -n "${laser_rate}" ]; then
      if [ -z "${auto_laser_rate_khz}" ]; then
        auto_laser_rate_khz="${laser_rate}"
      elif [ "${auto_laser_rate_khz}" != "${laser_rate}" ]; then
        echo "Warning: multiple laser rates in inputs (${auto_laser_rate_khz}, ${laser_rate}); using ${auto_laser_rate_khz} kHz for trigger cleanup." >&2
      fi
    fi
    if [ -z "${channels}" ] || [ "${channels}" = "logbook" ]; then
      channels="$(logbook_channels_csv "${logbook}" "${run}")"
    fi
    thresholds="$(logbook_field "${logbook}" "${run}" threshold || true)"
    vbias="$(logbook_field "${logbook}" "${run}" vbias || true)"
  fi
  if [ "${channels}" = "logbook" ]; then
    channels=""
  fi
  if [ -z "${channels}" ]; then
    if [ "${mode}" = "trigger" ] && [ "${sensor_channels}" != "logbook" ]; then
      channels="${sensor_channels},${trigger_channel}"
    else
      echo "Cannot infer channels for ${input}; pass --channels or add the run to ${logbook}" >&2
      exit 1
    fi
  fi
  channels="$(channels_to_csv "${channels}")"
  if [ "${sensor_channels}" = "logbook" ] && [ -n "${channels}" ]; then
    sensor_channels_from_inputs="${sensor_channels_from_inputs} ${channels//,/ }"
  fi
  if [ "${dry_run}" -eq 1 ]; then
    printf 'printf %q >> %q\n' "${label}\t${input}\t${intensity}\t${channels}\t${thresholds}\t\t${vbias}\t${mode}" "${runlist}"
  else
    printf '%s\t%s\t%s\t%s\t%s\t\t%s\t%s\n' "${label}" "${input}" "${intensity}" "${channels}" "${thresholds}" "${vbias}" "${mode}" >> "${runlist}"
  fi
done

if [ "${sensor_channels}" = "logbook" ]; then
  sensor_channels="$(channels_to_csv "${sensor_channels_from_inputs}")"
  if [ -z "${sensor_channels}" ]; then
    echo "Cannot infer --sensors from logbook; pass --sensors explicitly." >&2
    exit 1
  fi
fi
if [ "${mode}" = "trigger" ] && [ "${reference_mode}" = "trigger" ]; then
  sensor_channels="$(remove_channel_from_csv "${sensor_channels}" "${trigger_channel}")"
  if [ -z "${sensor_channels}" ]; then
    echo "No sensor channels left after removing trigger/reference channel ${trigger_channel}." >&2
    exit 1
  fi
fi

if [ "${mode}" = "trigger" ] && [ "${reference_mode}" = "trigger" ] &&
   [ "${trigger_period_set}" -eq 0 ] && [ -n "${auto_laser_rate_khz}" ]; then
  trigger_period_ns="$(awk -v rate="${auto_laser_rate_khz}" 'BEGIN {
    if (rate > 0) {
      printf "%.9g", 1000000.0 / rate
    }
  }')"
  if [ -n "${trigger_period_ns}" ] && [ "${trigger_period_tolerance_set}" -eq 0 ]; then
    trigger_period_tolerance_ns="$(awk -v period="${trigger_period_ns}" 'BEGIN {
      if (period > 0) {
        printf "%.9g", period * 0.05
      }
    }')"
  fi
fi

if [ "${mode}" = "trigger" ]; then
  log_path="${qa_dir}/output/log_timewalk_correction_macro.txt"
  tool_source="${qa_dir}/macro/run_timewalk_calibration_main.cxx"
  if [ "${dry_run}" -eq 1 ]; then
    tool_exe="$(root_tool_path "${qa_dir}" "run_timewalk_calibration")"
  else
    tool_exe="$(root_tool_build "${qa_dir}" "run_timewalk_calibration" "${tool_source}")"
  fi
  cmd=("${tool_exe}" "${runlist}" "${tdc_calib}" "" "${out_pdf}" "${out_root}" "${out_txt}" "${trigger_channel}" "${sensor_channels}" "${match_window_ns}" "${duration_ns}" "${clock_mhz}" "${use_fine}" "${use_lut}" "${fine_cut}" "${require_tot}" "${trigger_deadtime_ns}" "${trigger_period_ns}" "${trigger_period_tolerance_ns}" "${signed_dt}" "${timewalk_fit_ranges}" "${timewalk_fit_model}" "${dt_tot_cut}" "${trigger_tot_window}" "${spill_range}" "${channel_tot_windows}" "${edge_spill}" "${edge_channels}" "${edge_phase_period_ns}" "${edge_spill_fraction}" "${tdc_selection}" "${reference_mode}" "${reference_min_channels}" "${event_window_ns}" "${dt_tot_cut_direction}")
else
  log_path="${qa_dir}/output/log_tot_intensity_scan_macro.txt"
  tool_source="${qa_dir}/macro/run_tot_intensity_scan_main.cxx"
  if [ "${dry_run}" -eq 1 ]; then
    tool_exe="$(root_tool_path "${qa_dir}" "run_tot_intensity_scan")"
  else
    tool_exe="$(root_tool_build "${qa_dir}" "run_tot_intensity_scan" "${tool_source}")"
  fi
  cmd=("${tool_exe}" "${runlist}" "${tdc_calib}" "${out_pdf}" "${out_root}" "${out_txt}" "${duration_ns}" "${clock_mhz}" "${use_fine}" "${use_lut}" "${fine_cut}")
fi

echo "== Timewalk/ToT mode: ${mode}"
echo "== Run list: ${runlist}"
echo "== TDC calibration: ${tdc_calib}"
echo "== Output calibration: ${out_root}"
echo "== Output PDF: ${out_pdf}"
echo "== Output TXT: ${out_txt}"
if [ "${mode}" = "trigger" ]; then
  echo "== Reference mode: ${reference_mode}"
  if [ "${reference_mode}" = "event-median" ]; then
    echo "== Event reference min channels: ${reference_min_channels}"
  fi
  echo "== Laser trigger/reference channel: ${trigger_channel}"
  echo "== Sensor channels: ${sensor_channels}"
  if [ "${event_window_ns}" = "0" ] || [ "${event_window_ns}" = "0.0" ]; then
    echo "== Event-building window: same as match window"
  else
    echo "== Event-building window: ${event_window_ns} ns"
  fi
  if [ "${reference_mode}" = "event-median" ]; then
    echo "== Timewalk observable: dt(sensor - event median excluding sensor) vs sensor ToT"
  else
    echo "== Timewalk observable: dt(sensor - laser ch${trigger_channel}) vs sensor ToT"
  fi
  echo "== Trigger veto/dead-time after accepted trigger: ${trigger_deadtime_ns} ns"
  echo "== Trigger expected period: ${trigger_period_ns} ns (tolerance ${trigger_period_tolerance_ns} ns)"
  echo "== Leading TDC selection: ${tdc_selection:-all}"
  echo "== dt/ToT cut direction: ${dt_tot_cut_direction}"
  echo "== Fit ranges: ${timewalk_fit_ranges}"
  echo "== Fit model: ${timewalk_fit_model}"
else
  echo "== Note: intensity mode writes ToT/intensity characterization, not absolute timewalk corrections."
fi

if [ "${dry_run}" -eq 1 ]; then
  printf '%q ' "${cmd[@]}"
  printf '\n'
  exit 0
fi

"${cmd[@]}" 2>&1 | tee "${log_path}"

if [ ! -s "${out_root}" ]; then
  echo "Timewalk/ToT output was not produced: ${out_root}" >&2
  exit 1
fi
