#! /usr/bin/env bash

TAGNAME="threshold_15"
TIME="0.1" # [s]
TIMER=$(echo "scale=0; ($TIME * 32000000) / 1" | bc -l)
THRESHOLD_SETTINGS="--vth 3 --range 3 --threshold 15"
STANDAMAP="/au/standa/maps/20220206/standa_map_extra.dat"
WIDTH="50" # [ns]

CHIPS="0 1"
ROWS=$(echo {A..H})
COLS=$(echo {1..4})

#CHIPS=0
#ROWS=C
#COLS=1

NLOOPS=10
STEP=100
SCANVALUESX=$(seq -4000 ${STEP} 4000)
SCANVALUESY=$(seq -4000 ${STEP} 4000)

UREADOUTINTEGRATED=100

main() {

    ### setup pulser
    echo " --- setting pulser width: ${WIDTH} "
    /au/pulser/cmd "SOURCE2:PULSE:WIDTH ${WIDTH} ns"
    /au/pulser/status

    ### go home
    echo " --- moving home "
    /au/standa/home > /dev/null

    ### loop over chips and sensors
    for CHIP in $CHIPS; do
	for ROW in $ROWS; do
	    for COL in $COLS; do
		
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
		
		measure $CHIP $XYCHANNEL
		
	    done
	done
    done

    ### go home
    echo " --- moving home "
    /au/standa/home > /dev/null

}

measure() {
    CHIP=$1
    XYCHANNEL=$2
    CHANNEL=$(/au/readout/python/mapping.py --xy2eo ${XYCHANNEL})
    local STARTX=$(/au/standa/cmd --axis 1 --cmd gpos)
    local STARTY=$(/au/standa/cmd --axis 2 --cmd gpos)

    ### scan x
    /au/standa/move $STARTX $STARTY
    start_scan "${SCANVALUESX}" 0 "ledY1mm.chip${CHIP}.${XYCHANNEL}.xscan.standa_superscan_xy.dat"

    ### scan y
    /au/standa/move $STARTX $STARTY
    start_scan 0 "${SCANVALUESY}" "ledY1mm.chip${CHIP}.${XYCHANNEL}.yscan.standa_superscan_xy.dat"

    ### ureadout
#    go_to $STARTX $STARTY
#    /au/pulser/off
#    /au/readout/bin/ureadout --connection /au/etc/connection2.xml --output ~/DATA/20220206/STANDASUPER/centre_off.chip${CHIP}.${XYCHANNEL} --chip $CHIP --channel $CHANNEL --timer 3200000 --integrated $UREADOUTINTEGRATED --delta_threshold $DELTATHRESHOLD
#    /au/pulser/on
#    /au/readout/bin/ureadout --connection /au/etc/connection2.xml --output ~/DATA/20220206/STANDASUPER/centre_on.chip${CHIP}.${XYCHANNEL} --chip $CHIP --channel $CHANNEL --timer 3200000 --integrated $UREADOUTINTEGRATED --delta_threshold $DELTATHRESHOLD
}


standa() {
    echo " --- standa axis $1: $2 "
    $HOME/alcor/alcor-utils/standa/standa_client.py --axis $1 --cmd "$2" > /dev/null
}

start_scan()
{

    echo " --- start scan "
    VALUESX="$1"
    VALUESY="$2"
    FILENAME="$3"
    echo " valuesX: "$VALUESX
    echo " valuesY: "$VALUESY 
    echo " filename: $FILENAME "

    rm -rf $FILENAME

    local STARTX=$(/au/standa/cmd --axis 1 --cmd gpos)
    local STARTY=$(/au/standa/cmd --axis 2 --cmd gpos)
    
    for VALUEX in $VALUESX; do
	
	standa 1 "move $(($STARTX + $VALUEX))"
	standa 1 "wait"

	GPOSX=$(/au/standa/cmd --axis 1 --cmd gpos)
	
	for VALUEY in $VALUESY; do
	    
	    standa 2 "move $(($STARTY + $VALUEY))"
	    standa 2 "wait"

	    GPOSY=$(/au/standa/cmd --axis 2 --cmd gpos)

	    for I in $(seq 1 $NLOOPS); do
		
		/au/pulser/off
		# /au/pilas/off
		OUTPUT_OFF=$($HOME/alcor/alcorInit.sh 0 /tmp true &> /dev/null && $HOME/alcor/alcor-utils/readout/bin/rate --connection ${HOME}/alcor/alcor-utils/etc/connection2.xml --chip $CHIP --channel $CHANNEL --min_timer $TIMER $THRESHOLD_SETTINGS)
		
		/au/pulser/on
		# /au/pilas/on
		OUTPUT_ON=$($HOME/alcor/alcorInit.sh 0 /tmp true &> /dev/null && $HOME/alcor/alcor-utils/readout/bin/rate --connection ${HOME}/alcor/alcor-utils/etc/connection2.xml --chip $CHIP --channel $CHANNEL --min_timer $TIMER $THRESHOLD_SETTINGS)
		
		echo "standa_x = ${GPOSX} standa_y = ${GPOSY} $OUTPUT_OFF $OUTPUT_ON" | tee -a $FILENAME
		
		
	    done
	done
    done

    echo "standa_x/I:standa_y/I:rate_off/F:rate_on/F" > tree.$FILENAME
    cat $FILENAME | awk '{print $3, $6, $21, $39}' >> tree.$FILENAME
    
}

main
