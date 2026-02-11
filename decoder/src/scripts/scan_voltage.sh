#! /usr/bin/env bash

TAGNAME="minus30c"
TIME="0.1" ### seconds
TIMER=$(echo "scale=0; ($TIME * 32000000) / 1" | bc -l)
THRESHOLD_SETTINGS="--delta_threshold 3"
VBIASES=$(seq 2500 50 3000)
VBIASES="2470 2522 2574 2587 2600 2613 2626 2678 2731 2783 2835 2887"
#VBIASES="2600"
LOOPS=10

if [ -x $1 ] || [ -x $2 ]; then
    echo " usage: $0 [chip] [xychannel] "
    exit 1
fi

CHIP=$1
XYCHANNEL=$2
EOCHANNEL=$(/au/readout/python/mapping.py --xy2eo ${XYCHANNEL})
FILENAME=$TAGNAME.scan_voltage.chip${CHIP}.${XYCHANNEL}

main() {

    ### clean file
    rm -rf ${FILENAME}.dat

    ### loop over vbias
    for VBIAS in $VBIASES; do
	### set vbias
	/au/masterlogic/set $CHIP $VBIAS
	### sleep
	sleep 0.1
	### measure
	measure | tee -a ${FILENAME}.dat
    done

    ### make tree
#    /au/measure/tree.sh ${FILENAME}.dat vbias rate_on rate_off
    
}

measure() {

#    /au/pulser/off
#    /au/readout/bin/ureadout --connection /au/etc/connection2.xml --output "pulser_off.vbias=${VBIAS}" --chip ${CHIP} --channel ${EOCHANNEL} --timer 320000 --integrated 10. --delta_threshold 3
#    ./process.sh "pulser_off.vbias=${VBIAS}" ${CHIP} ${EOCHANNEL} &> /dev/null & 

    for LOOP in $(seq 1 $LOOPS); do
	
	/au/pulser/on
	/au/readout/bin/ureadout --connection /au/etc/connection2.xml --output "pulser_on.loop_${LOOP}.vbias=${VBIAS}" \
				 --chip ${CHIP} --channel ${EOCHANNEL} --timer 320000 --integrated 1. --delta_threshold 3 && \
	    ./process.sh "pulser_on.loop_${LOOP}.vbias=${VBIAS}" ${CHIP} ${EOCHANNEL}
	
    done
    
    hadd -f coincidence.pulser_on.vbias=${VBIAS}.chip_${CHIP}.channel_${EOCHANNEL}.miniframe.root \
	 coincidence.pulser_on.loop_*.vbias=${VBIAS}.chip_${CHIP}.channel_${EOCHANNEL}.miniframe.root && \
	rm -rf coincidence.pulser_on.loop_*.vbias=${VBIAS}.chip_${CHIP}.channel_${EOCHANNEL}.miniframe.root
    
    return
    
    for LOOP in $(seq 1 $LOOPS); do
	### pulser off
	/au/pulser/off
	OUTPUT_OFF=$(/au/control/alcorInit.sh 0 /tmp/ true &> /dev/null && /au/readout/bin/rate --connection /au/etc/connection2.xml --chip ${CHIP} --channel ${EOCHANNEL} --min_timer ${TIMER} ${THRESHOLD_SETTINGS} --tag "off")
	### pulser on
	/au/pulser/on
	OUTPUT_ON=$(/au/control/alcorInit.sh 0 /tmp/ true &> /dev/null && /au/readout/bin/rate --connection /au/etc/connection2.xml --chip ${CHIP} --channel ${EOCHANNEL} --min_timer ${TIMER} ${THRESHOLD_SETTINGS} --tag "on")
	### output
	if [ ! -z "$OUTPUT_OFF" ] && [ ! -z "$OUTPUT_ON" ]; then
	    echo "vbias = ${VBIAS} ${OUTPUT_OFF} ${OUTPUT_ON}"
	fi
    done
}

main
