#! /usr/bin/env bash

while true; do
    line=$(/au/readout/bin/register --connection /au/etc/connection2.xml --node regfile.status)
    echo -ne "$line"\\r
done
