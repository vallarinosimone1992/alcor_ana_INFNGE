#! /usr/bin/env python3

# CH LE2DAC Vth Range Threshold Offset1 OpMode Offset2 Gain1 Gain2 Polarity


import argparse
import os

parser = argparse.ArgumentParser()
parser.add_argument('--filein', type=str,  required=True, help="input file name")
parser.add_argument('--opmode', type=int,  required=False, help="update OpMode")
parser.add_argument('--offset', type=int,  required=False, help="modify offset")
parser.add_argument('--range', type=int,  required=False, help="modify range")
parser.add_argument('--threshold', type=int, required=False, help="modify threshold")
parser.add_argument('--delta_threshold', type=int, required=False, help="increment threshold")
parser.add_argument('--one_channel', type=int, required=False, help="enable only one channel")
args = vars(parser.parse_args())

f = open(args['filein'], "r")
lines = f.readlines()
for line in lines:
    line = line.strip('\n')

    if line.startswith('#'):
        print(line)
        continue

    CH, LE2DAC, Vth, Range, Threshold, Offset1, OpMode, Offset2, Gain1, Gain2, Polarity = line.split(' \t')

    ### update OpMode if requested
    if args['opmode'] is not None and int(OpMode) != 0:
        OpMode = args['opmode']
        
    ### modify threshold
    if args['threshold'] is not None:
        Threshold = args['threshold']
    if args['delta_threshold'] is not None:
        Threshold = int(Threshold) + args['delta_threshold']
    if Threshold is not None and int(Threshold) > 63:
        Threshold = 63

    ### modify offset
    if args['offset'] is not None:
        Offset1 = args['offset']
        
    ### modify range
    if args['range'] is not None:
        Range = args['range']
        
    ### enable only one channel
    if args['one_channel'] is not None:
        if int(CH) is not args['one_channel']:
            OpMode = 0
        
    print(CH, LE2DAC, Vth, Range, Threshold, Offset1, OpMode, Offset2, Gain1, Gain2, Polarity, sep='\t')
