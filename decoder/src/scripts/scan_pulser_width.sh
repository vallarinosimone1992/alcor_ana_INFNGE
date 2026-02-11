#! /usr/bin/env bash

TAGNAME="threshold_15"
TIME="0.1" ### seconds
TIMER=$(echo "scale=0; ($TIME * 32000000) / 1" | bc -l)
THRESHOLD_SETTINGS="--vth 3 --range 3 --threshold 15"
STANDAMAP="/au/standa/maps/20220206/standa_map_extra.dat"

echo " --- moving home "
/au/standa/home > /dev/null

for CHIP in 0 1; do
    for ROW in {A..H}; do
	for COL in {1..4}; do
	    
	    XYCHANNEL=$ROW$COL
	    EOCHANNEL=$(/au/readout/python/mapping.py --xy2eo ${XYCHANNEL})
	    COORDINATES=$(/au/standa/maps/read_map.sh ${STANDAMAP} ${CHIP} ${XYCHANNEL})
	    FILENAME=$TAGNAME.scan_pulser_width.chip${CHIP}.${XYCHANNEL}
	    if [ -z "${COORDINATES}" ]; then
		echo " coordinates not found for chip ${CHIP} channel ${XYCHANNEL} in ${STANDAMAP} "
		continue
	    fi

	    echo " --- moving to $CHIP $XYCHANNEL: $COORDINATES "
	    /au/standa/move $COORDINATES > /dev/null

	    for WIDTH in {5..50}; do
		/au/pulser/cmd "SOURCE2:PULSE:WIDTH ${WIDTH} ns";

		/au/pulser/off
		OUTPUT_OFF=$(/au/control/alcorInit.sh 0 /tmp/ true &> /dev/null && /au/readout/bin/rate --connection /au/etc/connection2.xml --chip ${CHIP} --channel ${EOCHANNEL} --min_timer ${TIMER} ${THRESHOLD_SETTINGS})

		/au/pulser/on
		OUTPUT_ON=$(/au/control/alcorInit.sh 0 /tmp/ true &> /dev/null && /au/readout/bin/rate --connection /au/etc/connection2.xml --chip ${CHIP} --channel ${EOCHANNEL} --min_timer ${TIMER} ${THRESHOLD_SETTINGS})

		echo "pulser_width = ${WIDTH} ${OUTPUT_OFF} ${OUTPUT_ON}"

	    done | tee ${FILENAME}.dat

	    (echo "pulser_width/F:rate_off/F:rate_on/F"; awk '{print $3, $18, $36}' ${FILENAME}.dat) > ${FILENAME}.tree

	done
    done
done

echo " --- moving home "
/au/standa/home > /dev/null
