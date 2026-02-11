from alcor_registers import *
from alcor_registers import registers as reg

### we can access registers by name
reg['ECCR']['Raw Data Mode'] = 1
print('ECCR = 0x%04x' % reg['ECCR'].get())

### or directly
ECCR['Column 0 Iratio'] = 1
print('ECCR = 0x%04x' % ECCR.get())

### we can set the value of the register
BCR1357.set(0x1111)
### and inspect a specific field
print('Bit_boost = %d' % BCR1357['Bit_boost'])
### we can reser to default values
BCR1357.reset()
### and inspect again a specific field
print('Bit_boost = %d' % BCR1357['Bit_boost'])

