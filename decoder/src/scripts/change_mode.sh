#! /usr/bin/env bash

if [ $# -ne 2 ]; then
    echo " usage: [device] [mode] "
    exit 1
fi
device=$1
mode=$2

/au/readout/bin/register --connection ${AU_IPBUS_CONNECTIONS} --device $device --node regfile.mode --write $mode
