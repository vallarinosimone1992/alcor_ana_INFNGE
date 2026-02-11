#! /usr/bin/env bash

staging=$(( 10 * 1024 * 1024 ))
occupancy=1024

### ALCOR active lanes
chip0=$(( 0xf ))
chip1=$(( 0xf ))
chip2=$(( 0xf ))
chip3=$(( 0xf ))
chip4=$(( 0xf ))
chip5=$(( 0xf ))

### trigger
trigg=1

### FPGA clock (must be in data)
clock=320

### mode selection 
bit_mode_run=0
bit_mode_spill_sw=1
bit_mode_spill_ext=2
bit_mode_tp_alcor_0=3
bit_mode_tp_alcor_1=4
bit_mode_tp_alcor_2=5
bit_mode_tp_alcor_3=6
bit_mode_tp_alcor_4=7
bit_mode_tp_alcor_5=8
bit_mode_tp_rev_pol=9

### filter
filter=$(( 0xf ))

### prepare output
runname=$(date +%Y%m%d-%H%M%S)
outputdir=${HOME}/DATA/2022-testbeam/physics/$runname
ln -sfn $outputdir ${HOME}/DATA/2022-testbeam/physics/latest
mkdir -p $outputdir
mkdir -p $outputdir/cfg
mkdir -p $outputdir/log
mkdir -p $outputdir/raw
mkdir -p $outputdir/decoded
mkdir -p $outputdir/miniframe
echo $runname > /tmp/current.runname
echo $outputdir > /tmp/current.rundir

echo " --- new run started: $runname " | tee -a $outputdir/log/start-readout-processes.log
echo " --- running from: $outputdir " | tee -a $outputdir/log/start-readout-processes.log

### copy relevant configuration files
cp -L $0 $outputdir/cfg/$(basename "$0")
cp -L /au/conf/readout.conf $outputdir/cfg/readout.conf
for chip in {0..5}; do
    bcrfile=$(grep "^1" /au/conf/readout.conf | awk {'print $4'})
    cp /au/conf/readout.conf/bcr/$bcrfile.bcr $outputdir/cfg/chip$chip.bcr
    pcrfile=$(grep "^1" /au/conf/readout.conf | awk {'print $5'})
    cp /au/conf/readout.conf/bcr/$pcrfile.bcr $outputdir/cfg/chip$chip.pcr
done

### save masterlogic values
for I in {0..3}; do /au/masterlogic/dac12 $I &> $outputdir/cfg/masterlogic.$I.dac12 & done; wait
for I in {0..3}; do /au/masterlogic/dac8  $I &> $outputdir/cfg/masterlogic.$I.dac8  & done; wait
for I in {0..3}; do /au/masterlogic/temp  $I &> $outputdir/cfg/masterlogic.$I.temp  & done; wait

### readout options
connection="/au/etc/connection.ch.8.xml"   
ctrl_readout_options="--connection $connection --usleep 1000 --filter $filter"
nano_readout_options="--connection $connection --usleep 1000 --staging $staging --occupancy $occupancy --clock $clock --decode --output $outputdir/raw/alcdaq"
trig_readout_options="$nano_readout_options --trigger"
echo " --- ctrl-readout options: $ctrl_readout_options " | tee -a $outputdir/log/start-readout-processes.log
echo " --- nano-readout options: $nano_readout_options " | tee -a $outputdir/log/start-readout-processes.log

### build fifo bit map
fifo=$(( (chip0 << 0) + (chip1 << 4) + (chip2 << 8) + (chip3 << 12)  + (chip4 << 16)  + (chip5 << 20) ))

### start ctrl-readout process
/au/readout/bin/ctrl-readout $ctrl_readout_options &> $outputdir/log/ctrl-readout.log &
echo " --- started ctrl-readout process " | tee -a $outputdir/log/start-readout-processes.log
sleep 1

### start ALCOR nano-readout processes
for chip in {0..5}; do
    for lane in {0..3}; do	
       	bit=$(( 1 << ( lane + 4 * chip ) ))
	ififo=$(( lane + 4 * chip ))
	if [ $(( bit & fifo )) = 0 ]; then continue; fi
	/au/readout/bin/nano-readout $nano_readout_options --chip $chip --lane $lane &> $outputdir/log/nano-readout.chip$chip.lane$lane.log && \
	    /au/readout/bin/decoder --input $outputdir/raw/alcdaq.fifo_$ififo.dat --output $outputdir/decoded/alcdaq.fifo_$ififo.root &> $outputdir/log/decoder.chip$chip.lane$lane.log && \
	    root -b -q -l "${HOME}/alcor/alcor-utils/measure/fastMiniFrame.C(\"$outputdir/decoded/alcdaq.fifo_$ififo.root\", \"$outputdir/miniframe/alcdaq.fifo_$ififo.miniframe.root\")" &
	echo " --- started nano-readout process: chip $chip, lane $lane " | tee -a $outputdir/log/start-readout-processes.log
    done
done

### start TRIGGER nano-readout process
if [ $trigg = 1 ]; then
    /au/readout/bin/nano-readout $nano_readout_options --trigger --chip -1 --lane -1 &> $outputdir/log/nano-readout.trigger.log && \
	/au/readout/bin/decoder --input $outputdir/raw/alcdaq.fifo_24.dat --output $outputdir/decoded/alcdaq.fifo_24.root &> $outputdir/log/decoder.trigger.log && \
	root -b -q -l "${HOME}/alcor/alcor-utils/measure/fastMiniFrame.C(\"$outputdir/decoded/alcdaq.fifo_24.root\", \"$outputdir/miniframe/alcdaq.fifo_24.miniframe.root\")" &
    echo " --- started nano-readout process: trigger " | tee -a $outputdir/log/start-readout-processes.log
fi

### update readout system monitor
/au/measure/readout-box/readout_system_monitor.sh

### current runname                                                                                     
data="readout_system_monitor,system=run_name message=\"$runname\""
curl -i -XPOST 'http://localhost:8086/write?db=mydb' --data-binary "$data" &> /dev/null

### current rundir
run_dir=$(cat /tmp/current.rundir)
data="readout_system_monitor,system=run_dir message=\"$outputdir\""
curl -i -XPOST 'http://localhost:8086/write?db=mydb' --data-binary "$data" &> /dev/null


### wait for processes to complete
echo " --- waiting for processes to complete " | tee -a $outputdir/log/start-readout-processes.log
wait

### online data analysis
echo " --- start online data analysis " | tee -a $outputdir/log/start-readout-processes.log
hadd -f $outputdir/miniframe/alcdaq.miniframe.root $outputdir/miniframe/alcdaq.fifo_*.miniframe.root | tee -a $outputdir/log/start-readout-processes.log
root -b -q -l "${HOME}/alcor/alcor-utils/measure/readout-box/drawing_routines/scintillator_coincidence.C(\"$outputdir/miniframe/alcdaq.miniframe.root\")" | tee -a $outputdir/log/start-readout-processes.log

### send email
attachments=$(ls $outputdir/miniframe/*.png)
recipients="osipenko@ge.infn.it"
mail -r osipenko@ge.infn.it \
     -s "[Physics] $runname" \
     $(for i in $recipients; do echo "$i,"; done) \
     $(for i in $attachments; do echo "-A $i"; done) <<EOF
Sending from $outputdir.

(Do not reply, we won't read it)
EOF
