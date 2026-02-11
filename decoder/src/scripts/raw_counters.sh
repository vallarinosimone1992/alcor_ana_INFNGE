#! /usr/bin/env bash

if [ $# -ne 1 ]; then
    echo " usage: $0 [filename] "
    exit 1
fi
filename=$1
    
nroll=$(od -An -tx4 ${filename} -w4 -v | grep "^ 5c5c5c5c" | wc -l)
nsos=$(od -An -tx4 ${filename} -w4 -v | grep "^ 7" | wc -l)
neos=$(od -An -tx4 ${filename} -w4 -v | grep "^ f" | wc -l)

echo " --- ${filename}: SOS = ${nsos} | EOS = ${neos} | ROLL = ${nroll} "
