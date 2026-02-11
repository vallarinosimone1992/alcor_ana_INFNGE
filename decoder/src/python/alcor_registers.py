from register import *

class BCR0246(Register):
    def __init__(self):
        Register.__init__(self, 'BCR 0,2,4,6')
        self.fields['iblatchDAC'] = Field( 0, 0x03, 1)
        self.fields['iTDC']       = Field( 2, 0x1f, 15)
        self.fields['cal']        = Field( 7, 0x03, 0)
        self.fields['void']       = Field(10, 0x3f, 0)

class BCR1357(Register):
    def __init__(self):
        Register.__init__(self, 'BCR 1,3,5,6')
        self.fields['Bit_cg']    = Field( 0, 0x1f, 12)
        self.fields['Bit_boost'] = Field( 5, 0x1f, 25)
        self.fields['S0']        = Field(10, 0x01, 1)
        self.fields['ib_sF']     = Field(11, 0x01, 0)
        self.fields['ib_3']      = Field(12, 0x03, 0)
        self.fields['ib_2']      = Field(14, 0x03, 3)

class ECCR(Register):
    def __init__(self):
        Register.__init__(self, 'ECCR')
        self.fields['Column 0 Enable']       = Field( 0, 0x01, 1)
        self.fields['Column 0 Safe Bit']     = Field( 1, 0x01, 1)
        self.fields['Column 0 Iratio']       = Field( 2, 0x01, 0)
        self.fields['Column 1 Enable']       = Field( 3, 0x01, 1)
        self.fields['Column 1 Safe Bit']     = Field( 4, 0x01, 1)
        self.fields['Column 1 Iratio']       = Field( 5, 0x01, 0)
        self.fields['Not used']              = Field( 6, 0x1f, 0)
        self.fields['Raw Data Mode']         = Field(11, 0x01, 0)
        self.fields['8b10b Encoder Enable']  = Field(12, 0x01, 1)
        self.fields['Serializer Enable']     = Field(13, 0x01, 1)
        self.fields['Serializer Align Mode'] = Field(14, 0x01, 0)
        self.fields['Enable Status Words']   = Field(15, 0x01, 0)

class PCR0(Register):
    def __init__(self):
        Register.__init__(self, 'PCR0')
        self.fields['cDAC_TDC0'] = Field( 0, 0x0f, 7)
        self.fields['cDAC_TDC1'] = Field( 4, 0x0f, 7)
        self.fields['cDAC_TDC2'] = Field( 8, 0x0f, 7)
        self.fields['cDAC_TDC3'] = Field(12, 0x0f, 7)

class PCR1(Register):
    def __init__(self):
        Register.__init__(self, 'PCR1')
        self.fields['fDAC_TDC0'] = Field( 0, 0x0f, 8)
        self.fields['fDAC_TDC1'] = Field( 4, 0x0f, 8)
        self.fields['fDAC_TDC2'] = Field( 8, 0x0f, 8)
        self.fields['fDAC_TDC3'] = Field(12, 0x0f, 8)

class PCR2(Register):
    def __init__(self):
        Register.__init__(self, 'PCR2')
        self.fields['LE1DAC']     = Field( 0, 0x3f, 63)
        self.fields['LEDACrange'] = Field( 6, 0x03, 3)
        self.fields['LEDACVth']   = Field( 8, 0x03, 0)
        self.fields['LE2DAC']     = Field(10, 0x3f, 63)

class PCR3(Register):
    OpMode = {'OFF':0,'LET':1}
    def __init__(self):
        Register.__init__(self, 'PCR3')
        self.fields['void']         = Field( 0, 0x01, 0)
        self.fields['Polarisation'] = Field( 1, 0x01, 1)
        self.fields['Gain2']        = Field( 2, 0x03, 0)
        self.fields['Gain1']        = Field( 4, 0x03, 0)
        self.fields['Offset2']      = Field( 6, 0x07, 0)
        self.fields['OpMode']       = Field( 9, 0x0f, self.OpMode['LET'])
        self.fields['Offset1']      = Field(13, 0x07, 0)

BCR0246 = BCR0246()
BCR1357 = BCR1357()
ECCR = ECCR()
PCR0 = PCR0()
PCR1 = PCR1()
PCR2 = PCR2()
PCR3 = PCR3()

registers = {'BCR0246' : BCR0246,
             'BCR1357' : BCR1357,
             'ECCR'    : ECCR,
             'PCR0'    : PCR0,
             'PCR1'    : PCR1,
             'PCR2'    : PCR2,
             'PCR3'    : PCR3 }

