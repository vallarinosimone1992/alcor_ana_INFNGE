#! /usr/bin/env bash

/au/control/alcorInit.sh 666 /tmp true

/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x0
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x1
#/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0xb
#/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x3
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x243

while true; do
    /au/readout/bin/register --connection /au/etc/connection2.xml --node alcor_readout_id3_lane0.fifo_reset --write 1 > /dev/null
    sleep 0.1
    hex=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node alcor_readout_id3_lane0.fifo_occupancy | awk {'print $3'})
    hex=$(($hex & 0xFFFF))
    printf "%d\n" $(($hex))
done
