#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run a game's MusicPlayer2000 (MP2K) sound engine under the Unicorn ARM emulator.

This is the reference for checking supergbamidi's model of the engine. It calls the engine's init, song start and
per-frame routines directly (no video, and only the two BIOS calls the engine makes), and captures what the engine
produces:

    trace       every sound channel's state after each frame, one line per channel that plays
    render      the DirectSound mixer's output: 8-bit stereo samples at the engine's rate

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM render SONG FRAMES OUT.wav [--tracks 0,3]

The routine addresses are chosen by the ROM's game code. Those of Pokemon Emerald (BPEE) and The Legend of Zelda: A
Link to the Past & Four Swords (AZLE) are built in. Another game needs its own values; see GAMES and docs/mp2k.md.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

import numpy as np
from unicorn import UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_UNMAPPED, UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, \
    UC_ARM_REG_R2, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
SOUND_INFO_PTR = 0x03007FF0  # the engine keeps its SoundInfo's address here

# Offsets and sizes in the engine's structures.
SOUND_INFO_CHANS = 0x50
SOUND_INFO_PCM_BUFFER = 0x350
PCM_DMA_BUF_SIZE = 1584
CHANNEL_SIZE = 0x40
TRACK_SIZE = 0x50


class Addresses:
    """BPEE (Pokemon Emerald)."""
    init = 0x082e0070        # m4aSoundInit
    song_start = 0x082e0130  # m4aSongNumStart: r0 = song
    main = 0x082e0124        # m4aSoundMain, which the VBlank interrupt calls
    vsync = 0x082dfa10       # m4aSoundVSync, which the VCount interrupt calls
    song_table = 0x086b49f0
    player_table = 0x086b49c0


class AddressesAZLE(Addresses):
    """AZLE (The Legend of Zelda: A Link to the Past & Four Swords): MP2K plays Four Swords and the menus."""
    init = 0x08134684
    song_start = 0x08134708
    main = 0x081346fc
    vsync = 0x08134028
    song_table = 0x083c3bbc
    player_table = 0x083c3a3c


GAMES = {b'BPEE': Addresses, b'AZLE': AddressesAZLE}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no engine addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class Channel:
    """A DirectSound channel (kind 'd') or one of the four PSG channels (kind 'c'), as the engine leaves it after a
    frame."""

    def __init__(self, raw, kind, index, track):
        self.kind = kind
        self.index = index
        self.track = track
        self.status, self.type, self.right, self.left = raw[0:4]
        self.attack, self.decay, self.sustain, self.release = raw[4:8]
        self.key, self.envelope = raw[8], raw[9]
        self.echo_volume, self.echo_length = raw[12], raw[13]
        self.gate, self.midi_key, self.velocity, self.priority = raw[16:20]
        self.rhythm_pan = struct.unpack('<b', raw[20:21])[0]
        if kind == 'd':
            self.count, self.fraction, self.frequency, self.wav, self.position = struct.unpack('<iIIII', raw[24:44])
        else:
            self.goal, self.counter = raw[10], raw[11]
            self.sustain_goal, self.n4, self.pan, self.pan_mask, self.modify, self.length, self.sweep = raw[25:32]
            self.frequency, self.wav = struct.unpack('<II', raw[32:40])

    def line(self, frame):
        """Returns the channel's line in the trace."""
        common = '%d %s%d %02x %d %d %d %d %d %d %d %d' % (frame, self.kind, self.index, self.status, self.track,
                                                          self.midi_key, self.key, self.velocity, self.priority,
                                                          self.envelope, self.right, self.left)
        if self.kind == 'd':
            offset = self.position - (self.wav + 16)
            return common + ' %d %08x %d %d %d' % (self.frequency, self.wav, self.count, offset, self.fraction)
        return common + ' %d %d %d %d %02x %08x' % (self.frequency, self.goal, self.counter, self.sustain_goal,
                                                  self.pan, self.wav)


class EngineEmulator:
    def __init__(self, rom):
        addr = addresses_for(rom)
        self.addr = addr
        self.rom = rom
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        size = (len(rom) + 0xFFF) & ~0xFFF
        uc.mem_map(ROM_BASE, size)
        uc.mem_write(ROM_BASE, rom)
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_READ, self._io_read, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._swi)
        self.uc = uc
        self.vcount = 0
        self.fixed_vcount = None
        self.muted = set()
        self.call(addr.init)
        self.info = self.u32(SOUND_INFO_PTR)
        self.players = []
        for i in range(64):
            entry = addr.player_table + 12 * i
            player, tracks, count = self.u32(entry), self.u32(entry + 4), rom[entry + 8 - ROM_BASE]
            if not 0x02000000 <= player < 0x03008000:
                break
            self.players.append((player, tracks, count))

    def _swi(self, uc, intno, user):
        # Unicorn calls this after the SWI instruction, without taking the exception. The engine's init copies its
        # mixer to IWRAM and clears its structures with CpuSet.
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        number = self.u16(pc - 2) & 0xFF if thumb else (self.u32(pc - 4) >> 16) & 0xFF
        src, dst, control = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if number == 0x0B:
            unit = 4 if control & (1 << 26) else 2
            count = control & 0x1FFFFF
        elif number == 0x0C:
            unit = 4
            count = ((control & 0x1FFFFF) + 7) & ~7
        else:
            raise SystemExit('the engine made BIOS call %02x' % number)
        fill = control & (1 << 24)
        value = bytes(uc.mem_read(src, unit))
        for i in range(count):
            uc.mem_write(dst + i * unit, value if fill else bytes(uc.mem_read(src + i * unit, unit)))

    def _unmapped(self, uc, access, address, size, value, user):
        uc.mem_map(address & ~0xFFF, 0x1000)
        return True

    def _io_read(self, uc, access, address, size, value, user):
        # SampleFreqSet waits for the vertical count to reach line 159, so each read during the init moves it on a
        # line. During a frame it stays at the start of the VBlank, so that a game with a time limit on its mixer never
        # runs out of time.
        if address <= 0x04000006 < address + size:
            if self.fixed_vcount is None:
                self.vcount = (self.vcount + 1) % 228
                uc.mem_write(0x04000006, struct.pack('<H', self.vcount))
            else:
                uc.mem_write(0x04000006, struct.pack('<H', self.fixed_vcount))

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def u8(self, address):
        return self.uc.mem_read(address, 1)[0]

    def u16(self, address):
        return struct.unpack('<H', self.uc.mem_read(address, 2))[0]

    def u32(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def play(self, song):
        """Starts `song` on its music player, which plays it from the next frame."""
        self.call(self.addr.song_start, (song,))
        entry = self.addr.song_table + 8 * song - ROM_BASE
        self.player = self.players[struct.unpack('<H', self.rom[entry + 4:entry + 6])[0]]

    def frame(self):
        """Runs one frame. Returns the mixer's output for it as two arrays of signed 8-bit samples, right and left."""
        info = self.info
        self.fixed_vcount = 160
        self.call(self.addr.vsync)
        counter = self.u8(info + 4)
        period = self.u8(info + 11)
        per_frame = self.u32(info + 16)
        offset = (period - (counter - 1)) * per_frame if counter > 1 else 0
        self._mute()
        self.call(self.addr.main)
        self.fixed_vcount = None
        buffer = info + SOUND_INFO_PCM_BUFFER + offset
        right = np.frombuffer(bytes(self.uc.mem_read(buffer, per_frame)), dtype=np.int8)
        left = np.frombuffer(bytes(self.uc.mem_read(buffer + PCM_DMA_BUF_SIZE, per_frame)), dtype=np.int8)
        return right, left

    def _mute(self):
        # A channel whose volumes are 0 is mixed silently. The sequencer sets them again when the track's volume or
        # pan changes, so they're cleared before every frame.
        if not self.muted:
            return
        for c in self.channels():
            if c.kind == 'd' and c.track in self.muted:
                base = self.info + SOUND_INFO_CHANS + CHANNEL_SIZE * c.index
                self.uc.mem_write(base + 2, b'\0\0')

    def playing(self):
        """Returns true while the song's player has tracks that haven't ended, or any channel still sounds."""
        status = self.u32(self.player[0] + 4)
        if status & 0xFFFF and not status & 0x80000000:
            return True
        return any(c.status for c in self.channels())

    def track_index(self, track):
        """Returns the index of a track in the song's player, or -1."""
        first, count = self.player[1], self.player[2]
        if track and first <= track < first + TRACK_SIZE * count:
            return (track - first) // TRACK_SIZE
        return -1

    def channels(self):
        """Returns the DirectSound channels up to the engine's limit, then the PSG channels."""
        info = self.info
        out = []
        limit = self.u8(info + 6)
        raw = bytes(self.uc.mem_read(info + SOUND_INFO_CHANS, CHANNEL_SIZE * limit))
        for i in range(limit):
            r = raw[i * CHANNEL_SIZE:(i + 1) * CHANNEL_SIZE]
            out.append(Channel(r, 'd', i, self.track_index(struct.unpack('<I', r[44:48])[0])))
        cgb = self.u32(info + 28)
        if cgb:
            raw = bytes(self.uc.mem_read(cgb, CHANNEL_SIZE * 4))
            for i in range(4):
                r = raw[i * CHANNEL_SIZE:(i + 1) * CHANNEL_SIZE]
                out.append(Channel(r, 'c', i, self.track_index(struct.unpack('<I', r[44:48])[0])))
        return out

    def rate(self):
        """Returns the mixer's output rate in Hz, from the timer the init started."""
        reload = struct.unpack('<H', self.uc.mem_read(0x04000100, 2))[0]
        return 16777216 / (0x10000 - reload)


def write_wav(path, right, left, rate):
    stereo = np.stack([left, right], axis=1).astype(np.float64) * 256
    data = np.clip(stereo, -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(data.tobytes())


def trace(rom, song, frames):
    emu = EngineEmulator(rom)
    emu.play(song)
    for f in range(frames):
        emu.frame()
        for c in emu.channels():
            if c.status:
                print(c.line(f))
        if not emu.playing():
            break


def render(rom, song, frames, tracks=None):
    """Returns the mixer's output for the first `frames` frames of `song` as right and left arrays, and its rate in Hz.
    If `tracks` is given, the DirectSound channels of the song's other tracks are mixed silently. They still take up
    channels, so the result is what the game plays with those tracks muted."""
    emu = EngineEmulator(rom)
    emu.play(song)
    if tracks is not None:
        emu.muted = set(range(16)) - set(tracks)
    right, left = [], []
    for _ in range(frames):
        r, l = emu.frame()
        right.append(r)
        left.append(l)
    return np.concatenate(right), np.concatenate(left), emu.rate()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render'])
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--tracks', help='render: only these tracks of the song, such as 0,3')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.song, a.frames)
        return
    if not a.out:
        p.error('an output file is needed')
    tracks = [int(t) for t in a.tracks.split(',')] if a.tracks else None
    right, left, rate = render(rom, a.song, a.frames, tracks)
    write_wav(a.out, right, left, rate)


if __name__ == '__main__':
    main()
