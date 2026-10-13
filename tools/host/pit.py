"""8253 timer and 8259 interrupt controller, as far as the game uses them."""
import time


class PIT:
    BASE_HZ = 1193182

    def __init__(self, m):
        self.m = m
        self.reload = 0x10000
        self.latch_byte = None
        self.write_lo = None
        self.last = time.perf_counter()
        self.acc = 0.0
        self.imr = 0xB8
        self.speaker = 0

    @property
    def hz(self):
        return self.BASE_HZ / (self.reload or 0x10000)

    def tick(self):
        now = time.perf_counter()
        self.acc += (now - self.last) * self.hz
        self.last = now
        if self.acc >= 1:
            self.acc = min(self.acc - 1, 4)
            if not self.imr & 1:
                self.m.raise_irq(0)

    def io_in(self, port):
        if port == 0x40:
            cnt = int((1 - (self.acc % 1)) * (self.reload or 0x10000)) & 0xFFFF
            if self.latch_byte is None:
                self.latch_byte = cnt >> 8
                return cnt & 0xFF
            v, self.latch_byte = self.latch_byte, None
            return v
        if port == 0x21:
            return self.imr
        if port == 0x20:
            return 0
        if port == 0x61:
            return self.speaker
        return None

    def io_out(self, port, v):
        if port == 0x43:
            self.write_lo = None
            return True
        if port == 0x40:
            if self.write_lo is None:
                self.write_lo = v
            else:
                self.reload = self.write_lo | v << 8
                self.write_lo = None
                self.m.log(f'PIT reload {self.reload} -> {self.hz:.1f} Hz')
            return True
        if port == 0x42:
            return True
        if port == 0x21:
            self.imr = v
            return True
        if port == 0x20:
            return True
        if port == 0x61:
            self.speaker = v
            return True
        return False
