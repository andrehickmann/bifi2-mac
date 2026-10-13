"""Keyboard controller port 0x60/0x64 plus a BIOS key buffer."""


class Keyboard:
    def __init__(self, m):
        self.m = m
        self.scancodes = []
        self.current = 0
        self.buffer = []

    def press(self, scancode, ascii_code=0, release=False):
        self.scancodes.append(scancode | (0x80 if release else 0))
        if not release:
            self.buffer.append(scancode << 8 | ascii_code)
        self.m.raise_irq(1)

    def bios_int9(self):
        pass

    def pop_key(self):
        return self.buffer.pop(0) if self.buffer else None

    def peek_key(self):
        return self.buffer[0] if self.buffer else None

    def io_in(self, port):
        if port == 0x60:
            if self.scancodes:
                self.current = self.scancodes.pop(0)
                self.m.log(f'kbd read {self.current:02x}')
                if self.scancodes:
                    self.m.raise_irq(1)
            return self.current
        if port == 0x64:
            return 0x1C | (1 if self.scancodes else 0)
        return None

    def io_out(self, port, v):
        return port in (0x60, 0x64)
