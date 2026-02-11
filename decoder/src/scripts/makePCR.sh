#! /usr/bin/env bash

DIR=$1
CHIP=$2
RANGE=$3

run() {
    
cat << EOF
#
#  PIXEL Configuration Register file
#
#  This file contains settings of PCR2 and PCR3 fields for a given chip
#  All lines starting with # are skipped
#  PCR2 and PCR3 registers are updated for given channels according to the values below
#
# the configuration can be loaded via:
#  ./alcorInit.py connection2.xml kc705 -c [chip #] --pcrfile [filename]
#
# This file has been automatically generated.
#
# CH   LE2DAC   Vth    Range    Thr   Offset1 OpMode Offset2   Gain1   Gain2  Polarity  
EOF

for channel in {0..31}; do

    FILE=$DIR/THRESHOLD/scanthr.chip_$CHIP.channel_$channel.threshold
    
    LE2DAC=63
    Vth=$(awk -v r=$RANGE '$4 == r' $FILE | awk '{print $3}')
    Range=$RANGE
    Threshold=$(awk -v r=$RANGE '$4 == r' $FILE | awk '{print $6}')
    Offset1=$(awk -v r=$RANGE '$4 == r' $FILE | awk '{print $5}')
    OpMode=1
    Offset2=0
    Gain1=0
    Gain2=0
    Polarity=1
    
    echo -e "$channel \t $LE2DAC \t $Vth \t $Range \t $Threshold \t $Offset1 \t $OpMode \t $Offset2 \t $Gain1 \t $Gain2 \t $Polarity"
    
done

cat << EOF
#
#   --- don't add anything beyond this line
EOF

}

mkdir -p $DIR/PCR
run $DIR > $DIR/PCR/chip$CHIP.range$RANGE.pcr
cp $DIR/PCR/chip$CHIP.range$RANGE.pcr $DIR/PCR/chip$CHIP.range$RANGE.current.pcr
