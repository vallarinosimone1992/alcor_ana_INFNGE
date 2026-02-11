#! /usr/bin/env bash

/au/control/alcorInit.sh 666 /tmp

/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x0
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x1
/au/readout/bin/register --connection /au/etc/connection2.xml --node alcor_readout_id0_lane2.fifo_reset --write 1
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x20b

while true; do
#    /au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_occupancy
    /au/readout/bin/register --connection /au/etc/connection2.xml --node alcor_readout_id0_lane2.fifo_reset --write 1 > /dev/null
    hex=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node alcor_readout_id0_lane2.fifo_occupancy | awk {'print $3'})
    hex=$(($hex & 0xFFFF))
    printf "%d\n" $(($hex))
    sleep 0.1
done
