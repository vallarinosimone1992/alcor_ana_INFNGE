#! /usr/bin/env bash
#
chip=$1
for row in {A..H}; do
    for col in {1..4}; do
	xy_channel="$row$col"
	do_channel=$(/au/readout/python/mapping.py --xy2do $xy_channel)
	output=$(/au/readout/scripts/rate.sh $chip $xy_channel)
	echo "xy_channel = $xy_channel do_channel = $do_channel $output"
    done
done
#
