#! /usr/bin/env bash

/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x0 > /dev/null
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x1 > /dev/null
/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_reset --write 1 > /dev/null
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x3fd > /dev/null

while true; do
#    /au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_occupancy
    hex=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_occupancy | awk {'print $3'})
    /au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_reset --write 1 > /dev/null
    hex=$(($hex & 0xFFFF))
    printf "%d " $(($hex))
    sleep 0.1
done
