class Field:
    def __init__(self, offset, mask, default):
        self.offset = offset
        self.mask = mask
        self.default = default
        self.value = default
    def reset(self):
        self.value = self.default
    
class Register():
    def __init__(self, name):
        self.name = name
        self.fields = {}
    def __getitem__(self, item):
        return self.fields[item].value
    def __setitem__(self, item, value):
        self.fields[item].value = value & self.fields[item].mask
    def reset(self):
        for field in self.fields.items():
            field[1].reset()
    def get(self):
        value = 0x0
        for field in self.fields.items():
            value |= (field[1].value & field[1].mask) << field[1].offset
        return value
    def set(self, value):
        for field in self.fields.items():
            field[1].value = (value >> field[1].offset) & field[1].mask
    def __str__(self):
        return hex(self.get())
    def __repr__(self):
        return str(hex(self.get()))
    def dump(self):
        print(self.name)
        for field in self.fields.items():
            print('%s = %d' % (field[0], field[1].value))


