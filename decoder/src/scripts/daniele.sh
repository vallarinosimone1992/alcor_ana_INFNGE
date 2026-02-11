#! /usr/bin/env bash

EXE="/au/readout/bin/alcor_register"
CONN="/au/etc/connection2.xml"
CHIP="4"
CMD="${EXE} --connection ${CONN} --chip ${CHIP}"

BCR0246="0x3d"

BCR1357_Bit_cg=12
BCR1357_Bit_boost=25 
BCR1357_S0=1
BCR1357_ib_sF=0
BCR1357_ib_3=0
BCR1357_ib_2=3

BCR1357=$(printf "0x%x" $(( (${BCR1357_Bit_cg} << 0) + (${BCR1357_Bit_boost} << 5) + (${BCR1357_S0} << 10) + (${BCR1357_ib_sF} << 11) + (${BCR1357_ib_3} << 12) + (${BCR1357_ib_2} << 14) )))

PIXEL=0
COLUMN=0
PCR0="0x7777"
PCR1="0x8888"
PCR2="0xfcff"
PCR3="0x202" ## on
#PCR3="0x2"   ## off

### configure bias control registers
for bcr in 0 2 4 6; do ${CMD} --bcr $bcr --write ${BCR0246}; done
for bcr in 1 3 5 7; do ${CMD} --bcr $bcr --write ${BCR1357}; done

### configure pixel control registers
${CMD} --pcr 0 --pixel ${PIXEL} --column ${COLUMN} --write ${PCR0}
${CMD} --pcr 1 --pixel ${PIXEL} --column ${COLUMN} --write ${PCR1}
${CMD} --pcr 2 --pixel ${PIXEL} --column ${COLUMN} --write ${PCR2}
${CMD} --pcr 3 --pixel ${PIXEL} --column ${COLUMN} --write ${PCR3}
