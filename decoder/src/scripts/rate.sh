#! /usr/bin/env bash
#
if [ -x $1 ] || [ -x $2 ]; then
    echo "usage: rate.sh [chip] [xy_channel] [timer]"
    exit 1
fi
#
chip=$1
xy_channel=$2
eo_channel=$(/au/readout/python/mapping.py --xy2eo $xy_channel)
min_timer=3200000
if [ ! -x $3 ]; then
    min_timer=$3
fi
max_timer=3200000
delta_threshold=3
#
/au/control/alcorInit.sh 0 /tmp/ true &> /dev/null && /au/readout/bin/rate --connection /au/etc/connection2.xml --chip $chip --channel $eo_channel --min_timer $min_timer --max_timer $max_timer --delta_threshold $delta_threshold
#
