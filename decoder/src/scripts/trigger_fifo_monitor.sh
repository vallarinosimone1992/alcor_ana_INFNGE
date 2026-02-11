#! /usr/bin/env bash

/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x0 > /dev/null
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x1 > /dev/null
/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_reset --write 1 > /dev/null
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 3 > dev/null #0x3fd > /dev/null
/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_reset --write 1 > /dev/null

while true; do
    hex=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_occupancy | awk {'print $3'})
    hex=$(($hex & 0xFFFF))
#    echo -ne "$((hex / 2))"\\r
    echo "$((hex / 2))"
    sleep 0.1
done
