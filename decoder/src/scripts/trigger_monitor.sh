#! /usr/bin/env bash

source ${HOME}/alcor/alcor-utils/etc/env.sh

/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x0
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x1
/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_reset --write 1
/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.mode --write 0x3
/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_reset --write 1

start_time=$(date +%s)
rm -rf trigger_monitor.log


while true; do
#    /au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_occupancy
    hex=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_occupancy | awk {'print $3'})
    hex=$(($hex & 0xFFFF))
    hex=$(($hex / 2))
    end_time=$(date +%s)
    elapsed=$(( end_time - start_time ))
    if [ "$elapsed" -ge 10 ]; then
	echo "elapsed: $elapsed ntriggers: $hex " | tee -a trigger_monitor.log
	start_time=$(date +%s)
	data="trigger_monitor value=$hex"
	echo $data
	curl -i -XPOST 'http://localhost:8086/write?db=mydb' --data-binary "$data"
	/au/readout/bin/register --connection /au/etc/connection2.xml --node trigger_info.fifo_reset --write 1
    fi
    sleep 1
done
