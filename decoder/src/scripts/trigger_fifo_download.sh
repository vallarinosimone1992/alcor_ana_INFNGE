#! /usr/bin/env bash

while true; do
    hex=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_occupancy | awk {'print $3'})
    hex=$(($hex & 0xFFFF))
    if [ $hex = 0 ]; then
	break
    fi
    hex=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_data | awk {'print $3'})
    printf "0x%08x \n" $((hex))
done
