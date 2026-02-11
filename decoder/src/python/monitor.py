#! /usr/bin/env python

import sys, signal
import matplotlib.pyplot as plt

fifos = [0, 1, 2, 3, 4, 5]
plots = ['max occupancy', 'number of words', 'words per second', 'bytes per second']

plt.ion()
fig, ax = plt.subplots(len(plots), len(fifos), figsize=(15, 15))
fig.tight_layout()

update_timer = 1
update_plots = False
elapsed_ = {}
words_ = {}
maxoc_ = {}
wordr_ = {}
byter_ = {}
for fifo in fifos:
    elapsed_[fifo] = []
    words_[fifo] = []
    maxoc_[fifo] = []
    wordr_[fifo] = []
    byter_[fifo] = []
    for i in range(0, len(plots)):
        ax[i][fifo].set_title(plots[i])

def sigint_handler(signum, frame):
    global interrupted
    interrupted = True
    print('--- interrupted, so long')

def sigalrm_handler(signum, frame):
    global update_plots
    update_plots = True
    signal.alarm(update_timer)
    print('--- alarm')

        
def do_update_plots():
    for fifo in fifos:
        ax[0][fifo].plot(elapsed_[fifo], words_[fifo], 'ro')
        ax[1][fifo].plot(elapsed_[fifo], maxoc_[fifo], 'ro')
        ax[2][fifo].plot(elapsed_[fifo], wordr_[fifo], 'ro')
        ax[3][fifo].plot(elapsed_[fifo], byter_[fifo], 'ro')
    fig.tight_layout()
    fig.canvas.flush_events()
        
signal.signal(signal.SIGINT, sigint_handler)
signal.signal(signal.SIGALRM, sigalrm_handler)
signal.alarm(update_timer)

interrupted = False
in_monitor = False
elapsed = 0
plt.ion()

while not interrupted:
    print('running')
    
    ### read line and split data
    line = sys.stdin.readline()
    if len(line) == 0:
        continue
    line = line.strip();
    data = line.split()
    
    ### check start/end of monitor flag
    if data[0] == 'SOM':
        in_monitor = True
        elapsed = float(data[2])
        continue
    elif data[0] == 'EOM':
        in_monitor = False
        if update_plots == True:
            print('--- plot update requested')
            do_update_plots()
            update_plots = False
            continue
        
    ### check if we are inside monitor data block
    if not in_monitor:
        continue;
    
    ### convert data block into numbers
    fifo = int(data[0])
    words = int(data[1])
    maxoc = int(data[2])
    wordr = float(data[3])
    byter = float(data[4])
    
    ### fill arrays
    elapsed_[fifo].append(elapsed);
    words_[fifo].append(words);
    maxoc_[fifo].append(maxoc);
    wordr_[fifo].append(wordr);
    byter_[fifo].append(byter);
        
#        ax[0][fifo].plot(elapsed, words, 'ro')
#        ax[1][fifo].plot(elapsed, maxoc, 'ro')
#        ax[2][fifo].plot(elapsed, wordr, 'ro')
#        ax[3][fifo].plot(elapsed, byter, 'ro')

