#! /usr/bin/env python3

from multiprocessing import shared_memory
from multiprocessing.resource_tracker import unregister
import sys

if len(sys.argv) < 2:
    print('usage:', sys.argv[0], '[mode]')
    exit(1)

try:
    mode=int(sys.argv[1], 16)
except ValueError:
    print('not a valid hex number:', sys.argv[1])
    exit(1)

shm = shared_memory.SharedMemory(name = 'MySharedMemory', create=False)
unregister(shm._name, 'shared_memory')
shm.buf[4:8] = mode.to_bytes(4, 'little')
shm.close()

#in_run=bool.from_bytes(buffer[0:1], signed=False, byteorder='little')
#in_spill=bool.from_bytes(buffer[2:3], signed=False, byteorder='little')
#mode=int.from_bytes(buffer[4:8], signed=False, byteorder='little')
#status=int.from_bytes(buffer[9:12], signed=False, byteorder='little')

#print(mode)
#print(int(mode, 16))
#print(mode)
#mode=0x1

#print(mode.to_bytes(4, 'little'))

