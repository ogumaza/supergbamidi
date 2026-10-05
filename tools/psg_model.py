# SPDX-License-Identifier: MIT

"""Game Boy APU model (as on the GBA) driven by captured register writes.

Output is normalised like the DirectSound output of konami/driver_emu.py and quintet/driver_emu.py: one PSG channel at
volume v contributes v * 8 * (master + 1) / 4 in mGBA's units, and a FIFO sample s contributes s * 4, so both are
divided by 4 * 128.
"""
import numpy as np

RATE = 32768
FRAME_SAMPLES = RATE * 280896 / 16777216
DUTY = [0x01, 0x81, 0x87, 0x7E]


class APU:
    def __init__(self):
        self.reg = {}
        self.square = [dict(duty=0, vol=0, dac=False, on=False, x=0, phase=0.0) for _ in range(2)]
        self.wave = dict(dac=False, bank=0, volcode=0, force75=False, x=0, on=False, pos=0.0,
                         ram=[bytearray(16), bytearray(16)])
        self.noise = dict(vol=0, dac=False, on=False, nr43=0, lfsr=0x7FFF, acc=0.0)
        self.nr50 = 0x77
        self.nr51 = 0xFF

    def write(self, address, size, value):
        for i in range(size):
            self._write8(address + i, (value >> (8 * i)) & 0xFF)

    def _half(self, base):
        return self.reg.get(base, 0) | (self.reg.get(base + 1, 0) << 8)

    def _write8(self, a, b):
        self.reg[a] = b
        if a in (0x04000062, 0x04000063, 0x04000068, 0x04000069):
            ch = 0 if a < 0x04000068 else 1
            v = self._half(0x04000062 if ch == 0 else 0x04000068)
            sq = self.square[ch]
            sq.update(duty=(v >> 6) & 3, vol=v >> 12, dac=(v & 0xF800) != 0)
            if not sq['dac']:
                sq['on'] = False
        elif a in (0x04000064, 0x04000065, 0x0400006C, 0x0400006D):
            ch = 0 if a < 0x04000068 else 1
            v = self._half(0x04000064 if ch == 0 else 0x0400006C)
            sq = self.square[ch]
            sq['x'] = v & 0x7FF
            if a & 1 and v & 0x8000:
                sq['on'] = sq['dac']
                sq['phase'] = 0.0
        elif a == 0x04000070:
            w = self.wave
            w.update(dac=bool(b & 0x80), bank=(b >> 6) & 1)
            if not w['dac']:
                w['on'] = False
        elif a in (0x04000072, 0x04000073):
            v = self._half(0x04000072)
            self.wave.update(volcode=(v >> 13) & 3, force75=bool(v & 0x8000))
        elif a in (0x04000074, 0x04000075):
            v = self._half(0x04000074)
            w = self.wave
            w['x'] = v & 0x7FF
            if a & 1 and v & 0x8000:
                w['on'] = w['dac']
                w['pos'] = 0.0
        elif 0x04000090 <= a < 0x040000A0:
            # The CPU writes the bank that isn't playing.
            self.wave['ram'][1 - self.wave['bank']][a - 0x04000090] = b
        elif a in (0x04000078, 0x04000079):
            v = self._half(0x04000078)
            n = self.noise
            n.update(vol=v >> 12, dac=(v & 0xF800) != 0)
            if not n['dac']:
                n['on'] = False
        elif a in (0x0400007C, 0x0400007D):
            v = self._half(0x0400007C)
            n = self.noise
            n['nr43'] = v & 0xFF
            if a & 1 and v & 0x8000:
                n.update(on=n['dac'], lfsr=0x7FFF, acc=0.0)
        elif a == 0x04000080:
            self.nr50 = b
        elif a == 0x04000081:
            self.nr51 = b

    def render(self, count, only=None):
        out = np.zeros((count, 2), dtype=np.float32)
        for i in range(count):
            vals = [0, 0, 0, 0]
            for c in range(2):
                sq = self.square[c]
                if sq['on']:
                    step = int(sq['phase']) & 7
                    vals[c] = sq['vol'] if (DUTY[sq['duty']] >> (7 - step)) & 1 else 0
                    sq['phase'] += 1048576.0 / (2048 - sq['x']) / RATE
            w = self.wave
            if w['on'] and w['dac']:
                idx = int(w['pos']) & 31
                byte = w['ram'][w['bank']][idx >> 1]
                s = byte >> 4 if idx % 2 == 0 else byte & 15
                vals[2] = s * 3 // 4 if w['force75'] else [0, s, s >> 1, s >> 2][w['volcode']]
                w['pos'] += 2097152.0 / (2048 - w['x']) / RATE
            n = self.noise
            if n['on']:
                vals[3] = n['vol'] if (~n['lfsr'] & 1) else 0
                shift, ratio = n['nr43'] >> 4, n['nr43'] & 7
                if shift < 14:
                    n['acc'] += 524288.0 / (ratio if ratio else 0.5) / (1 << (shift + 1)) / RATE
                while n['acc'] >= 1:
                    n['acc'] -= 1
                    bit = (n['lfsr'] ^ (n['lfsr'] >> 1)) & 1
                    n['lfsr'] = (n['lfsr'] >> 1) | (bit << 14)
                    if n['nr43'] & 8:
                        n['lfsr'] = (n['lfsr'] & ~0x40) | (bit << 6)
            left = right = 0
            for c in range(4):
                if only is not None and c != only:
                    continue
                if (self.nr51 >> (c + 4)) & 1:
                    left += vals[c]
                if (self.nr51 >> c) & 1:
                    right += vals[c]
            out[i, 0] = left * 8 * (((self.nr50 >> 4) & 7) + 1) / 4 / 4 / 128
            out[i, 1] = right * 8 * ((self.nr50 & 7) + 1) / 4 / 4 / 128
        return out


def render_writes(init_writes, frame_writes, frames, only=None):
    """Returns the APU's stereo output at RATE for `frames` frames. init_writes [(address, size, value)] are applied
    first, and frame_writes [(frame, address, size, value)] at the start of their frame. `only` renders one channel."""
    apu = APU()
    for a, s, v in init_writes:
        apu.write(a, s, v)
    by_frame = {}
    for f, a, s, v in frame_writes:
        by_frame.setdefault(f, []).append((a, s, v))
    chunks, acc = [], 0.0
    for f in range(frames):
        for a, s, v in by_frame.get(f, []):
            apu.write(a, s, v)
        acc += FRAME_SAMPLES
        n = int(acc)
        acc -= n
        chunks.append(apu.render(n, only))
    return np.concatenate(chunks)
