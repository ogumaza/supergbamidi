#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Nintendo R&D2's GBA sound driver under the Unicorn ARM emulator.

This is the reference for checking supergbamidi's model of the driver. It calls the game's sound init, the
driver's routine that asks for a sequence, and the driver's per-frame routines (no video and no interrupts; the BIOS
calls the driver makes are emulated), and captures what the driver does:

    trace       each voice's state after each frame, and the writes to the PSG's registers, in the format of
                supergbamidi --trace
    dump        the players and tracks in use after a number of frames
    render      the mixer's output, the samples the driver sends to the FIFOs, as a WAV file at the mixer's rate
                (the PSG isn't rendered)

    driver_emu.py ROM trace SEQUENCE FRAMES
    driver_emu.py ROM dump SEQUENCE FRAMES
    driver_emu.py ROM render SEQUENCE FRAMES OUT.wav

The routine and RAM addresses are chosen by the ROM's game code. Those of The Legend of Zelda: A Link to the Past
(AZLE) are built in. Another game needs its own values; see GAMES and docs/rd2.md.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R6, UC_ARM_REG_R7,
                               UC_ARM_REG_SP)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000

# The PSG's registers, whose writes the trace lists: NR10 to NR44, the wave RAM, and NR50 to NR52.
PSG = (0x04000060, 0x040000A0)

# The music player, which the driver's play request starts a sequence on.
MUSIC_PLAYER = 0x12

VOICE_SIZE = 0x78
TRACK_SIZE = 0x54
PLAYER_SIZE = 0x48


class Addresses:
    """AZLE (The Legend of Zelda: A Link to the Past & Four Swords, US)."""
    sound_init = 0x08129E60  # the game's sound init, which calls the driver's init with the game's settings
    request = 0x0812C718     # the driver's play request: r0 = player, r1 = sequence
    commit = 0x0812C704      # the driver's routine that hands the requests made since its last call to the next frame
    requests = 0x0812CC24    # the per-frame routine's first step: it carries out the requests handed over
    vblank = 0x0812A798      # the driver's VBlank routine: restarts timer 0 and the output DMAs
    frame = 0x0812A7A4       # the driver's per-frame routine: requests, sequencer, PSG voices and mixer
    voices = 0x03001760      # 7 DirectSound voices, then 4 PSG voices, of 0x78 bytes
    active = 0x03000168      # the first DirectSound voice that plays, in the order the mixer takes them
    tracks = 0x03000C20      # 24 tracks of 0x54 bytes
    players = 0x03001C88     # 20 players of 0x48 bytes
    fixed_region = 0x03000158  # the region that instruments with a sample for each key play
    outputs = 0x03000140     # the two output buffers, each 176 left samples then 176 right ones
    output_index = 0x03000152  # the buffer the mixer writes this frame
    note_on = 0x0812B7B4     # the driver's note on: r0 = track, r1 = note, r2 = velocity, r3 = length in 1/150 ticks
    note_key = 0x0812B7F2    # in the note on, after the instrument lookup: r7 = the note with the track's transpose
    note_voice = 0x0812B8A2  # in the note on, once it has a voice: r4 = the voice, r6 = the note's region, r7 = the
                             # note its pitch is for
    note_done = 0x0812B964   # in the note on, once it has set the voice's sample or PSG setting: r4 = the voice


GAMES = {b'AZLE': Addresses}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class DriverEmulator:
    def __init__(self, rom):
        self.addr = addresses_for(rom)
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, rom)
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=PSG[0], end=PSG[1] - 1)
        self.uc = uc
        self.writes = []
        self.call(self.addr.sound_init)

    def _unmapped(self, uc, access, address, size, value, user):
        raise SystemExit('the driver accessed unmapped memory at %08x' % address)

    def _io_write(self, uc, access, address, size, value, user):
        self.writes.append((address, size, value & ((1 << (8 * size)) - 1)))

    def _svc(self, uc, intno, user):
        # The driver calls the BIOS's Div, CpuSet and CpuFastSet.
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        number = uc.mem_read(pc - 2, 1)[0] if thumb else (struct.unpack('<I', uc.mem_read(pc - 4, 4))[0] >> 16) & 0xFF
        r0, r1, r2 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if number == 6:
            n = r0 - (1 << 32) if r0 & 0x80000000 else r0
            d = r1 - (1 << 32) if r1 & 0x80000000 else r1
            q = int(n / d) if d else 0
            uc.reg_write(UC_ARM_REG_R0, q & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R1, (n - q * d) & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R3, abs(q) & 0xFFFFFFFF)
        elif number in (0x0B, 0x0C):
            fill = r2 & (1 << 24)
            words = number == 0x0C or r2 & (1 << 26)
            count = r2 & 0x1FFFFF
            if number == 0x0C:
                count = (count + 7) & ~7
            unit = 4 if words else 2
            value = bytes(uc.mem_read(r0, unit))
            for i in range(count):
                if not fill:
                    value = bytes(uc.mem_read(r0 + i * unit, unit))
                uc.mem_write(r1 + i * unit, value)
                # Writes from the host don't reach the hooks, so the ones to the PSG, such as a wave's, are kept here.
                if PSG[0] <= r1 + i * unit < PSG[1]:
                    self.writes.append((r1 + i * unit, unit, int.from_bytes(value, 'little')))
        else:
            raise SystemExit('the driver made BIOS call %02x' % number)

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode first: SP/LR are banked
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def u32(self, address):
        return struct.unpack('<I', self.read(address, 4))[0]

    def play(self, sequence):
        """Asks for a sequence on the music player, as the game's play routine does. The driver starts it in the next
        frame."""
        self.writes = []
        self.call(self.addr.request, (MUSIC_PLAYER, sequence))
        self.call(self.addr.commit)
        return self.writes

    def frame(self):
        """Runs one frame as the game's VBlank handler does: the driver's VBlank routine, then its per-frame routine.
        Returns the PSG register writes."""
        self.writes = []
        self.call(self.addr.vblank)
        self.call(self.addr.frame)
        return self.writes

    def track_number(self, track):
        """Returns the music player's number for the track at `track`, or -1."""
        a = self.addr
        for n in range(10):
            if self.u32(a.players + PLAYER_SIZE * MUSIC_PLAYER + 8 + 4 * n) == track:
                return n
        return -1

    def mute_tracks_except(self, numbers):
        """Mutes the music player's tracks other than `numbers`, with each track's mute flag, which stops it starting
        notes."""
        a = self.addr
        for n in range(10):
            track = self.u32(a.players + PLAYER_SIZE * MUSIC_PLAYER + 8 + 4 * n)
            if track:
                self.uc.mem_write(track + 0x4A, bytes([0 if n in numbers else 1]))

    def playing(self):
        return self.read(self.addr.players + PLAYER_SIZE * MUSIC_PLAYER + 0x42, 1)[0] != 0

    def voice_lines(self):
        """Returns each voice's state as the trace prints it, for the voices in use, and the order of the DirectSound
        voices that play."""
        a = self.addr
        lines = []
        for i in range(11):
            base = a.voices + VOICE_SIZE * i
            raw = self.read(base, VOICE_SIZE)
            if raw[1] == 0:
                continue
            u8 = lambda o: raw[o]
            u16 = lambda o: struct.unpack_from('<H', raw, o)[0]
            s16 = lambda o: struct.unpack_from('<h', raw, o)[0]
            u32 = lambda o: struct.unpack_from('<I', raw, o)[0]
            s32 = lambda o: struct.unpack_from('<i', raw, o)[0]
            track = u32(0x04)
            track = (track - a.tracks) // TRACK_SIZE if track else -1
            region = u32(0x54)
            region = 'fixed' if region == a.fixed_region else '%08x' % region
            lines.append('v%d %d %d t%d p%d n%d v%d %x %x vol%d f%d e%d %d:%d lfo%d,%d sl%d,%d,%d,%d,%d '
                         'env%d,%d,%d,%d,%d %s r%d s%08x pos%x x%x' % (
                             i, u8(0x01), u8(0x00), track, u8(0x08), u8(0x09), u8(0x0A), u32(0x0C), u32(0x10),
                             u32(0x14), u16(0x18), u8(0x1A), u8(0x1B), u8(0x1C), u32(0x20), u32(0x24), u32(0x2C),
                             u32(0x30), s32(0x34), s32(0x38), s32(0x3C), s32(0x40), s32(0x44), u16(0x48), s16(0x4A),
                             u8(0x50), region, u8(0x58), u32(0x5C), u32(0x60), u32(0x64)))
        order = []
        v = self.u32(a.active)
        while v and len(order) < 7:
            order.append((v - a.voices) // VOICE_SIZE)
            v = self.u32(v + 0x6C)
        lines.append('active' + ''.join(' v%d' % i for i in order))
        return lines


def render(rom, sequence, frames, out, tracks=None):
    emu = DriverEmulator(rom)
    emu.play(sequence)
    a = emu.addr
    if tracks is not None:
        # Carry out the request now, so that the tracks can be muted before they play anything. The per-frame routine
        # then finds no requests, which is the only difference.
        emu.call(a.requests)
    pcm = bytearray()
    for f in range(frames):
        if tracks is not None:
            emu.mute_tracks_except(tracks)
        emu.frame()
        buffer = emu.u32(a.outputs + 4 * emu.read(a.output_index, 1)[0])
        data = emu.read(buffer, 352)
        for i in range(176):
            for side in (data[i], data[176 + i]):
                pcm += struct.pack('<h', (side - 256 if side >= 128 else side) * 256)
    with wave.open(str(out), 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(10512)
        w.writeframes(bytes(pcm))


class NoteLog:
    """Records each note the driver starts, with hooks in its note on routine."""

    def __init__(self, emu):
        self.emu = emu
        self.notes = []
        self.pending = None
        a = emu.addr
        for at in (a.note_on, a.note_key, a.note_voice, a.note_done):
            emu.uc.hook_add(UC_HOOK_CODE, self._hook, begin=at, end=at)

    def _hook(self, uc, address, size, user):
        a = self.emu.addr
        if address == a.note_on:
            track = uc.reg_read(UC_ARM_REG_R0)
            self.pending = {'track': self.emu.track_number(track), 'velocity': uc.reg_read(UC_ARM_REG_R2) & 0xFF}
        elif address == a.note_key and self.pending is not None:
            self.pending['key'] = uc.reg_read(UC_ARM_REG_R7) & 0xFF
        elif address == a.note_voice and self.pending is not None:
            voice = uc.reg_read(UC_ARM_REG_R4)
            self.pending['voice'] = (voice - a.voices) // VOICE_SIZE
            self.pending['pitch_note'] = uc.reg_read(UC_ARM_REG_R7) & 0xFF
            self.pending['region'] = uc.reg_read(UC_ARM_REG_R6)
            self.notes.append(self.pending)
            self.pending = None
        elif address == a.note_done and self.notes:
            voice = uc.reg_read(UC_ARM_REG_R4)
            note = self.notes[-1]
            note['type'] = self.emu.read(voice, 1)[0]
            note['sample'] = self.emu.u32(voice + 0x5C)


def notes(rom, sequence, frames):
    """Returns each frame's new notes and the voices' state after it: for each frame, (notes, voices, newest), where a
    note is a dict, voices[i] is (state, pitch) or None for a free voice, and newest[n] is the newest voice that track
    n plays, or -1."""
    emu = DriverEmulator(rom)
    log = NoteLog(emu)
    emu.play(sequence)
    a = emu.addr
    result = []
    for f in range(frames):
        log.notes = []
        emu.frame()
        for n in log.notes:
            n['frame'] = f
        voices = []
        for i in range(11):
            raw = emu.read(a.voices + VOICE_SIZE * i, VOICE_SIZE)
            if raw[1] == 0:
                voices.append(None)
                continue
            voices.append((raw[1], struct.unpack_from('<I', raw, 0x10)[0]))
        newest = []
        for n in range(10):
            track = emu.u32(a.players + PLAYER_SIZE * MUSIC_PLAYER + 8 + 4 * n)
            head = emu.u32(track + 0x0C) if track else 0
            newest.append((head - a.voices) // VOICE_SIZE if head else -1)
        result.append((log.notes, voices, newest))
        if f > 0 and not emu.playing():
            break
    return result


def format_write(frame, write):
    address, size, value = write
    return '%d %08x %d %0*x' % (frame, address, size, 2 * size, value)


def trace(rom, sequence, frames):
    emu = DriverEmulator(rom)
    for w in emu.play(sequence):
        print(format_write(-1, w))
    for f in range(frames):
        for w in emu.frame():
            print(format_write(f, w))
        for line in emu.voice_lines():
            print('%d %s' % (f, line))
        if f > 0 and not emu.playing():
            break


def dump(rom, sequence, frames):
    emu = DriverEmulator(rom)
    emu.play(sequence)
    for f in range(frames):
        emu.frame()
    a = emu.addr
    for p in range(20):
        raw = emu.read(a.players + PLAYER_SIZE * p, PLAYER_SIZE)
        if raw[0x42]:
            print('player %2d: %s' % (p, raw.hex()))
    for t in range(24):
        raw = emu.read(a.tracks + TRACK_SIZE * t, TRACK_SIZE)
        if any(raw):
            print('track %2d: %s' % (t, raw.hex()))
    for line in emu.voice_lines():
        print(line)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'dump', 'render'])
    p.add_argument('sequence', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--tracks', help='render: only these of the player\'s tracks (0-9), such as 1,3')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.sequence, a.frames)
    elif a.command == 'render':
        if not a.out:
            p.error('render needs OUT.wav')
        render(rom, a.sequence, a.frames, a.out,
               [int(t) for t in a.tracks.split(',')] if a.tracks else None)
    else:
        dump(rom, a.sequence, a.frames)


if __name__ == '__main__':
    main()
