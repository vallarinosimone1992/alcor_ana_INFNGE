#! /usr/bin/env bash

if [ -z $1 ] || [ -z $2 ]; then
    echo "usage: $0 [dir] [delta]"
    exit 1
fi

dir=$1
delta=$2

for chip in {0..5}; do
    for range in {0..3}; do
	/au/readout/scripts/updatePCR.sh $dir/PCR/chip$chip.range$range.pcr $delta > $dir/PCR/chip$chip.range$range.delta$delta.pcr;
    done
done
