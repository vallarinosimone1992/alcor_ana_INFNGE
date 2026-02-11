#! /usr/bin/env bash

if [ -z $1 ] || [ -z $2 ] || [ -z $3 ]; then
    echo "usage: $0 [filename] [delta-threshold-even] [delta-threshold-odd] [off-channels]"
    exit 1
fi


FILE=$1
DELTATHR_EVEN=$2
DELTATHR_ODD=$3
CHANNELS="$4"

while read -r line; do
    if [[ "$line" =~ ^#.*  ]]; then
	echo "$line"
    else
	read -r channel LE2DAC Vth Range Threshold Offset1 OpMode Offset2 Gain1 Gain2 Polarity <<< $line

	dochannel=$(/au/readout/python/mapping.py --eo2do $channel)
	xychannel=$(/au/readout/python/mapping.py --eo2xy $channel)
	evenodd=$(( (dochannel / 4) % 2 ))

	if [ "$evenodd" == "0" ]; then
	    DELTATHR=$DELTATHR_EVEN
	else
	    DELTATHR=$DELTATHR_ODD
	fi
	    
	Threshold=$((Threshold + DELTATHR))
	if [ "$Threshold" -gt "63" ]; then
	    Threshold=63
	fi
	for ch in $CHANNELS; do
	    if [[ $ch == $channel ]]; then
		OpMode=0
	    fi
	done
	echo -e "$channel \t $LE2DAC \t $Vth \t $Range \t $Threshold \t $Offset1 \t $OpMode \t $Offset2 \t $Gain1 \t $Gain2 \t $Polarity"
    fi

done < $FILE

