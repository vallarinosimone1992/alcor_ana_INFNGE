#! /usr/bin/env bash

if [ -z $1 ]; then
    echo " usage: ./draw_lanechannel [directory] "
    exit
fi
dir=$1

for chip in 0 1 2 3 4 5; do
for channel in {0..31}; do
    root -b -q -l "${ALCOR_DIR}/readout/macros/draw_lanechannel.C(\"$dir\", $chip, $channel, {\"hvzero\"})" &
done
wait
done
mkdir -p $dir/PNG
mv $dir/*.png $dir/PNG/.

mkdir -p $dir/THRESHOLD
mv $dir/*.threshold $dir/THRESHOLD/.

for CHIP in {0..5}; do
    for RANGE in {0..3}; do
	/au/readout/scripts/makePCR.sh $dir $CHIP $RANGE
    done
done

for DELTA in {0..25}; do
    /au/readout/scripts/doUpdatePCR.sh $dir $DELTA
done
