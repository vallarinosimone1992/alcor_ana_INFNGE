#! /usr/bin/env python3

import argparse

eochoices = [x for x in range(0, 32)]
dochoices = [x for x in range(0, 32)]
xychoices = [str(row + col) for row in ('A', 'B', 'C', 'D', 'E', 'F', 'G', 'H') for col in ('1', '2', '3', '4')]

parser = argparse.ArgumentParser()
g = parser.add_mutually_exclusive_group()
g.add_argument('--do2eo', help="Detector-oriented to electronics-oriented channel number")
g.add_argument('--eo2do', help="Electronics-oriented to detector-oriented channel number")
g.add_argument('--eo2xy', help="Electronics-oriented to channel oriented")
g.add_argument('--xy2do', type=str, choices=xychoices, help="Carrier-oriented to detector-oriented channel number")
g.add_argument('--xy2eo', type=str, choices=xychoices, help="Carrier-oriented to electronics-oriented channel number")
args = vars(parser.parse_args())

if not any(args.values()):
    parser.error('no arguments provided')

eo2xy = {}
    
def build_xy2doeo():
    doch = 0
    xy2do = {}
    xy2eo = {}
    for row in ('A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'):
        for col in ('1', '2', '3', '4'):
            xy2do[row + col] = doch
            xy2eo[row + col] = do2eo[doch]
            eo2xy[do2eo[doch]] = row + col
            doch = doch + 1
    return (xy2do, xy2eo)

# detector-oriented channel from electronics-oriented
eo2do = [22, 20, 18, 16, 24, 26, 28, 30, 25, 27, 29, 31, 23, 21, 19, 17, 9, 11, 13, 15, 7, 5, 3, 1, 6, 4, 2, 0, 8, 10, 12, 14]

# electronics-oriented channel from detector-oriented
do2eo = [27, 23, 26, 22, 25, 21, 24, 20, 28, 16, 29, 17, 30, 18, 31, 19, 3, 15, 2, 14, 1, 13, 0, 12, 4, 8, 5, 9, 6, 10, 7, 11]

(xy2do, xy2eo) = build_xy2doeo()

if args['do2eo'] is not None:
    print(do2eo[args['do2eo']])

if args['eo2do'] is not None:
    print(eo2do[int(args['eo2do'])])

if args['eo2xy'] is not None:
    print(eo2xy[int(args['eo2xy'])])

if args['xy2eo'] is not None:
    print(xy2eo[args['xy2eo']])

if args['xy2do'] is not None:
    print(xy2do[args['xy2do']])
    
