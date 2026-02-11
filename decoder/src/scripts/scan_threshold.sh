#! /usr/bin/env bash

TAGNAME="100ns"
TIME="0.1" ### seconds
TIMER=$(echo "scale=0; ($TIME * 32000000) / 1" | bc -l)
THRESHOLD_SETTINGS="--vth 3 --range 3 --threshold 63"
STANDAMAP="/au/standa/maps/20220206/standa_map_extra.dat"
WIDTH="100"

echo " --- setting pulser width: ${WIDTH} "
/au/pulser/cmd "SOURCE2:PULSE:WIDTH ${WIDTH} ns"
/au/pulser/status

echo " --- moving home "
/au/standa/home > /dev/null

for CHIP in 0 1; do
    for ROW in {A..H}; do
	for COL in {1..4}; do
	    
	    XYCHANNEL=$ROW$COL
	    EOCHANNEL=$(/au/readout/python/mapping.py --xy2eo ${XYCHANNEL})
	    COORDINATES=$(/au/standa/maps/read_map.sh ${STANDAMAP} ${CHIP} ${XYCHANNEL})
	    FILENAME=$TAGNAME.scan_threshold.chip${CHIP}.${XYCHANNEL}
	    if [ -z "${COORDINATES}" ]; then
		echo " coordinates not found for chip ${CHIP} channel ${XYCHANNEL} in ${STANDAMAP} "
		continue
	    fi

	    echo " --- moving to $CHIP $XYCHANNEL: $COORDINATES "
	    /au/standa/move $COORDINATES > /dev/null

	    for THRESHOLD in {0..63}; do
		THRESHOLD_SETTINGS="--vth 3 --range 3 --threshold ${THRESHOLD}"
		
		/au/pulser/off
		OUTPUT_OFF=$(/au/control/alcorInit.sh 0 /tmp/ true &> /dev/null && /au/readout/bin/rate --connection /au/etc/connection2.xml --chip ${CHIP} --channel ${EOCHANNEL} --min_timer ${TIMER} ${THRESHOLD_SETTINGS} --tag "off")

		/au/pulser/on
		OUTPUT_ON=$(/au/control/alcorInit.sh 0 /tmp/ true &> /dev/null && /au/readout/bin/rate --connection /au/etc/connection2.xml --chip ${CHIP} --channel ${EOCHANNEL} --min_timer ${TIMER} ${THRESHOLD_SETTINGS} --tag "on")

		echo "threshold = ${THRESHOLD} ${OUTPUT_OFF} ${OUTPUT_ON}"

	    done | tee ${FILENAME}.dat

	    (echo "threshold/F:rate_off/F:rate_on/F"; awk '{print $3, $18, $36}' ${FILENAME}.dat) > ${FILENAME}.tree

	done
    done
done

echo " --- moving home "
/au/standa/home > /dev/null
