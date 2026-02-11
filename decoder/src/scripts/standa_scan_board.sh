#! /usr/bin/env bash
#
if [ -x $1 ]; then
    echo "usage: $0 [chip]"
    exit 1
fi
out_filename="standa_scan_board.dat"
rm -rf $out_filename

# Setting home the 
/au/standa/home
#
chip=$1
map_filename="/au/standa/maps/20220206/standa_map.dat"
for row in {A..H}; do
    for col in {1..4}; do
	xy_channel="$row$col"
	do_channel=$(/au/readout/python/mapping.py --xy2do $xy_channel)
	x_steps=$(/au/standa/maps/read_map.sh $map_filename $chip $xy_channel | awk '{print $1 }' )
	y_steps=$(/au/standa/maps/read_map.sh $map_filename $chip $xy_channel | awk '{print $2 }' )
	if [ -x $x_steps ] || [ -x $y_steps ]; then
	    echo "[Warning] No XY position found for chip $chip channel $xy_channel in map file $map_filename"
	    continue;
	fi
	/au/standa/move $x_steps $y_steps
	output=$(/au/readout/scripts/rate.sh $chip $xy_channel)
	echo "xy_channel = $xy_channel do_channel = $do_channel $output" | tee -a $out_filename
    done
done
#
