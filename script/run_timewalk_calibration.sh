#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_timewalk_calibration.sh [options]

Decode the raw run associated with raw_data/TW_correction, extract absolute
per-channel timewalk corrections versus a trigger/reference channel, and update
the stable calibration symlink calibration/timewalk_correction.root.

The input run must contain the trigger/reference channel and the SiPM channels.
The default correction model is pol1; when applied, negative extrapolated
corrections are clipped to zero.

Options:
      --source-run RUN        update raw_data/TW_correction to point to RUN
                              (default run can be written as 20260521-110703)
  -R, --raw-input PATH        raw run or symlink to decode (default: raw_data/TW_correction)
  -D, --data-root DIR         decoded data root passed as DATA_ROOT (default: ../data)
  -k, --calib FILE            fine TDC calibration ROOT file (default: calibration/fine_calibration.root)
  -o, --output NAME           output calibration ROOT name (default: timewalk_correction_<label>.root)
      --calib-link FILE       stable symlink to update (default: calibration/timewalk_correction.root)
      --no-link               do not update the stable calibration symlink
      --skip-decode           use already decoded data under DATA_ROOT
      --force-decode          pass --force to decode_raw.sh (default)
      --no-force-decode       keep existing decoded ROOT files when present
      --trigger CH            trigger/reference channel (default: 22)
      --sensors CSV           SiPM channels to correct (default: 17,19)
  -w, --window NS             trigger matching window (default: 100)
      --trigger-deadtime NS   minimum separation for trigger cleanup (default: 50000)
      --trigger-period NS     expected valid trigger period (default: 1000000)
      --trigger-period-tol NS tolerance for trigger-period cleanup (default: 50000)
      --signed-dt             match to nearest trigger and keep signed time differences
      --timewalk-fit-ranges CSV
                              ToT fit ranges per channel (default: 17:7:14,19:3:13)
      --timewalk-fit-model MODEL
                              timewalk fit model: pol1 or lin-exp-plateau (default: pol1)
      --dt-tot-cut CSV        lower diagonal cut in the dt/ToT plane, per channel:
                              CH:DT0:SLOPE[:TOT_MIN:TOT_MAX], keeps dt >= DT0 + SLOPE*ToT
      --trigger-tot-window CSV
                              trigger/reference ToT selection window MIN:MAX, e.g. 1:3
      --spill-range CSV       inclusive spill selection range MIN:MAX
      --channel-tot-window CSV
                              per-channel ToT selection window CH:MIN:MAX[,CH:MIN:MAX]
      --edge-spill N          spill used for leading/trailing edge time maps
                              (default: first selected spill)
      --edge-channels CSV     channels for edge maps: all, analysis, or CSV (default: all)
      --edge-phase-period NS  period used to fold edge maps (default: trigger period)
      --edge-spill-fraction X initial spill fraction used by edge maps (default: 0.01)
  -d, --duration NS           max leading-trailing duration / ToT range (default: 30)
  -m, --clock MHz             clock frequency (default: 320)
  -f, --use-fine [0|1]        enable fine timing (default: 1)
      --no-fine               disable fine timing
      --no-lut                disable fine LUT
      --fine-cut N            exclude hits close to fine wrap cut (default: 0)
      --no-require-tot        also match leading hits without valid ToT
      --dry-run               print commands without executing them
  -h, --help                  show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
base_dir="$(cd "${qa_dir}/.." && pwd)"

raw_root="${base_dir}/raw_data"
data_root="${base_dir}/data"
raw_link_name="TW_correction"
raw_input=""
source_run=""
fine_calib="${qa_dir}/calibration/fine_calibration.root"
output_name=""
calib_link="${qa_dir}/calibration/timewalk_correction.root"
update_link=1
do_decode=1
force_decode=1
dry_run=0

trigger_channel=22
sensor_channels="17,19"
match_window_ns=100
trigger_deadtime_ns=50000
trigger_period_ns=1000000
trigger_period_tolerance_ns=50000
signed_dt=0
timewalk_fit_ranges="17:7:14,19:3:13"
timewalk_fit_model="pol1"
dt_tot_cut=""
trigger_tot_window=""
spill_range=""
channel_tot_windows=""
edge_spill=-1
edge_channels="all"
edge_phase_period_ns=0
edge_spill_fraction=0.01
duration_ns=30
clock_mhz=320
use_fine=1
use_lut=1
fine_cut=0
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
    --source-run)
      need_arg "$@"
      source_run=${2:-}
      shift 2
      ;;
    --source-run=*)
      source_run=${1#*=}
      shift
      ;;
    -R|--raw-input)
      need_arg "$@"
      raw_input=${2:-}
      shift 2
      ;;
    --raw-input=*)
      raw_input=${1#*=}
      shift
      ;;
    -D|--data-root)
      need_arg "$@"
      data_root=${2:-}
      shift 2
      ;;
    --data-root=*)
      data_root=${1#*=}
      shift
      ;;
    -k|--calib)
      need_arg "$@"
      fine_calib=${2:-}
      shift 2
      ;;
    --calib=*)
      fine_calib=${1#*=}
      shift
      ;;
    -o|--output)
      need_arg "$@"
      output_name=${2:-}
      shift 2
      ;;
    --output=*)
      output_name=${1#*=}
      shift
      ;;
    --calib-link)
      need_arg "$@"
      calib_link=${2:-}
      shift 2
      ;;
    --calib-link=*)
      calib_link=${1#*=}
      shift
      ;;
    --no-link)
      update_link=0
      shift
      ;;
    --skip-decode)
      do_decode=0
      shift
      ;;
    --force-decode)
      force_decode=1
      shift
      ;;
    --no-force-decode)
      force_decode=0
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
    -w|--window)
      need_arg "$@"
      match_window_ns=${2:-}
      shift 2
      ;;
    --window=*)
      match_window_ns=${1#*=}
      shift
      ;;
    --trigger-deadtime)
      need_arg "$@"
      trigger_deadtime_ns=${2:-}
      shift 2
      ;;
    --trigger-deadtime=*)
      trigger_deadtime_ns=${1#*=}
      shift
      ;;
    --trigger-period)
      need_arg "$@"
      trigger_period_ns=${2:-}
      shift 2
      ;;
    --trigger-period=*)
      trigger_period_ns=${1#*=}
      shift
      ;;
    --trigger-period-tol|--trigger-period-tolerance)
      need_arg "$@"
      trigger_period_tolerance_ns=${2:-}
      shift 2
      ;;
    --trigger-period-tol=*|--trigger-period-tolerance=*)
      trigger_period_tolerance_ns=${1#*=}
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

raw_link="${raw_root}/${raw_link_name}"
if [ -n "${source_run}" ]; then
  source_run="${source_run//_/-}"
  source_path="${raw_root}/${source_run}"
  if [ "${dry_run}" -eq 0 ] && [ ! -d "${source_path}" ]; then
    echo "Source raw run not found: ${source_path}" >&2
    exit 1
  fi
  if [ "${dry_run}" -eq 1 ]; then
    printf 'ln -sfn %q %q\n' "${source_run}" "${raw_link}"
  else
    ln -sfn "${source_run}" "${raw_link}"
  fi
  raw_input="${raw_link}"
fi

if [ -z "${raw_input}" ]; then
  raw_input="${raw_link}"
elif [[ "${raw_input}" != /* ]] && [ -e "${raw_root}/${raw_input}" ]; then
  raw_input="${raw_root}/${raw_input}"
fi

label="$(basename "${raw_input}")"
safe_label="${label//-/_}"
if [ -z "${output_name}" ]; then
  output_name="timewalk_correction_${safe_label}.root"
fi
if [[ "${output_name}" != *.root ]]; then
  output_name="${output_name}.root"
fi

calib_dir="${qa_dir}/calibration"
output_dir="${qa_dir}/output"
mkdir -p "${calib_dir}" "${output_dir}" "${data_root}" "$(dirname "${calib_link}")"

if [[ "${output_name}" == */* ]]; then
  out_root="${output_name}"
else
  out_root="${calib_dir}/${output_name}"
fi
out_base="$(basename "${out_root}")"
out_base="${out_base%.root}"
runlist="${output_dir}/${out_base}_runlist.tsv"
out_pdf="${output_dir}/${out_base}.pdf"
out_txt="${output_dir}/${out_base}.txt"
log_path="${output_dir}/log_${out_base}.txt"

if [ "${dry_run}" -eq 0 ]; then
  if [ ! -d "${raw_input}" ]; then
    echo "Raw input not found: ${raw_input}" >&2
    exit 1
  fi
  if [ ! -s "${fine_calib}" ]; then
    echo "Fine TDC calibration not found: ${fine_calib}" >&2
    echo "Create/update calibration/fine_calibration.root before extracting TW corrections." >&2
    exit 1
  fi
fi

decode_cmd=(env DATA_ROOT="${data_root}" "${script_dir}/decode_raw.sh" "${raw_input}")
if [ "${force_decode}" -eq 1 ]; then
  decode_cmd+=(--force)
fi

if [ "${do_decode}" -eq 1 ]; then
  echo "== Decode raw TW input: ${raw_input}"
  if [ "${dry_run}" -eq 1 ]; then
    printf '%q ' "${decode_cmd[@]}"
    printf '\n'
  else
    "${decode_cmd[@]}"
  fi
fi

has_root_files() {
  local dir="$1"
  compgen -G "${dir}/alcdaq.fifo_*.root" > /dev/null
}

decoded_base="${data_root}/${label}"
decoded_dir=""
if [ "${dry_run}" -eq 0 ]; then
  if [[ -d "${decoded_base}/decoded" ]] && has_root_files "${decoded_base}/decoded"; then
    decoded_dir="${decoded_base}/decoded"
  else
    filtered=()
    while IFS= read -r cand; do
      if has_root_files "${cand}"; then
        filtered+=("${cand}")
      fi
    done < <(find -L "${decoded_base}" -maxdepth 3 -type d -name decoded 2>/dev/null)
    if [ "${#filtered[@]}" -eq 1 ]; then
      decoded_dir="${filtered[0]}"
    else
      echo "No unique decoded ROOT directory found under ${decoded_base}" >&2
      echo "Decode the TW raw input first or check DATA_ROOT." >&2
      exit 1
    fi
  fi
else
  decoded_dir="${decoded_base}/kc705-196/decoded"
fi

if [ "${dry_run}" -eq 1 ]; then
  channels_tag="${sensor_channels//,/_}_${trigger_channel}"
  printf 'cat > %q <<EOF\n' "${runlist}"
  printf '# run_label\tinput_path\tintensity\tchannels\tthresholds\tspill\tvbias\tnote\n'
  printf '%s\t%s\t0\t%s\t\t\t\t%s\n' "${label}" "${decoded_dir}" "${channels_tag}" "${raw_input}"
  printf 'EOF\n'
else
  channels_tag="${sensor_channels//,/_}_${trigger_channel}"
  {
    printf '# run_label\tinput_path\tintensity\tchannels\tthresholds\tspill\tvbias\tnote\n'
    printf '%s\t%s\t0\t%s\t\t\t\t%s\n' "${label}" "${decoded_dir}" "${channels_tag}" "${raw_input}"
  } > "${runlist}"
fi

macro_path="${qa_dir}/macro/laser_intensity_scan_rdf.cxx"
cmd=(root -l -b -q "${macro_path}(\"${runlist}\",\"${fine_calib}\",\"\",\"${out_pdf}\",\"${out_root}\",\"${out_txt}\",${trigger_channel},\"${sensor_channels}\",${match_window_ns},${duration_ns},${clock_mhz},${use_fine},${use_lut},${fine_cut},${require_tot},${trigger_deadtime_ns},${trigger_period_ns},${trigger_period_tolerance_ns},${signed_dt},\"${timewalk_fit_ranges}\",\"${timewalk_fit_model}\",\"${dt_tot_cut}\",\"${trigger_tot_window}\",\"${spill_range}\",\"${channel_tot_windows}\",${edge_spill},\"${edge_channels}\",${edge_phase_period_ns},${edge_spill_fraction})")

echo "== Run list: ${runlist}"
echo "== Decoded input: ${decoded_dir}"
echo "== Fine TDC calibration: ${fine_calib}"
echo "== Output TW calibration: ${out_root}"
echo "== Stable TW symlink: ${calib_link}"
echo "== Trigger channel: ${trigger_channel}"
echo "== Sensor channels: ${sensor_channels}"
echo "== Timewalk fit ranges: ${timewalk_fit_ranges}"
echo "== Timewalk fit model: ${timewalk_fit_model}"
echo "== dt/ToT selection cut: ${dt_tot_cut}"
echo "== Trigger ToT window: ${trigger_tot_window}"
echo "== Spill range: ${spill_range}"
echo "== Channel ToT windows: ${channel_tot_windows}"
echo "== Edge diagnostic spill: ${edge_spill}"
echo "== Edge diagnostic channels: ${edge_channels}"
echo "== Edge diagnostic phase period: ${edge_phase_period_ns}"
echo "== Edge diagnostic initial fraction: ${edge_spill_fraction}"

if [ "${dry_run}" -eq 1 ]; then
  printf '%q ' "${cmd[@]}"
  printf '\n'
  if [ "${update_link}" -eq 1 ]; then
    printf 'ln -sfn %q %q\n' "$(basename "${out_root}")" "${calib_link}"
  fi
  exit 0
fi

"${cmd[@]}" 2>&1 | tee "${log_path}"

if [ ! -s "${out_root}" ]; then
  echo "TW calibration output was not produced: ${out_root}" >&2
  exit 1
fi

if [ "${update_link}" -eq 1 ]; then
  link_dir="$(cd "$(dirname "${calib_link}")" && pwd)"
  out_dir="$(cd "$(dirname "${out_root}")" && pwd)"
  if [ "${link_dir}" = "${out_dir}" ]; then
    link_target="$(basename "${out_root}")"
  else
    link_target="${out_root}"
  fi
  ln -sfn "${link_target}" "${calib_link}"
  echo "== Updated TW calibration symlink: ${calib_link} -> ${link_target}"
fi
