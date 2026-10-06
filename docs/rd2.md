# Nintendo R&D2's GBA sound driver

This document covers the sequence format, sequencer, voices and mixer in
Nintendo R&D2's sound driver for *The Legend of Zelda: A Link to the Past*
on the Game Boy Advance. The cartridge, *The Legend of Zelda: A Link to the
Past & Four Swords*, plays the rest of its music with Nintendo's MP2K. The
document is based on the driver's code in the US release (game code `AZLE`),
and the addresses given as examples come from there. Other games put the
driver and its data elsewhere, and `supergbamidi` finds them from the code
(see [Locating the driver](#locating-the-driver)).

The driver mixes samples in software, at 10512 Hz in stereo, into a buffer for
each of the two DirectSound FIFOs: FIFO A plays the left side and FIFO B the
right. It has 7 sample voices for the mixer, and a voice for each of the Game
Boy's four PSG channels. There are 20 players for sequences, each with up to
10 tracks drawn from a shared pool of 24.

## Architecture

The driver is Thumb code in ROM, apart from three ARM routines that its init
copies to IWRAM: the mixer's inner loop, which adds a voice's samples into the
mix, the echo, and the conversion of the mix to the FIFOs' 8-bit samples. The
game calls the driver's init with its settings (see
[The game's settings](#the-games-settings)), its VBlank routine at each
VBlank, and its per-frame routine after that. The per-frame routine runs these
steps:

1. It carries out the requests that the game has made since the last frame:
   start a sequence on a player, start a sound effect, stop or fade a player,
   set the echo, and others (see [Requests](#requests)).
2. The sequencer runs each player's tracks (see [Players and tracks](#players-and-tracks)).
3. The PSG voices write their channels' registers (see [PSG voices](#psg-voices)).
4. The mixer mixes the sample voices into the buffers, which the VBlank
   routine's DMA sends to the FIFOs during the next frame (see [The mixer](#the-mixer)).

In `AZLE` the routines are:

| Address | Routine |
|---|---|
| `0x08129E60` | the game's sound init, which calls the driver's init with the game's settings at `0x08198C30` |
| `0x0812A6CC` | init: stores the settings, sets up the sound registers, copies the ARM routines from `0x0812CC58` to `0x03001400`, and sets up the voices, tracks and players |
| `0x0812A044` | the game's play-music routine: `r0` is the game's music number, which the byte table at `0x08198C50` turns into a sequence |
| `0x0812C718` | request a sequence: `r0` = player, `r1` = sequence |
| `0x0812C704` | hand over the requests made since its last call, for the next frame to carry out |
| `0x0812A798` | the VBlank routine: restarts timer 0 and the DMA of the next output buffers |
| `0x0812A7A4` | the per-frame routine |
| `0x0812CC24` | carry out the requests, through the table of handlers at `0x08198EC8` |
| `0x0812C3DC` | the sequencer: each player in turn |
| `0x0812BE54` | run a track for a frame; its command table, for `C2` to `FF`, is at `0x0812BF98` |
| `0x0812B7B4` | note on |
| `0x0812AB30` | look up an instrument |
| `0x0812BC64` | allocate a voice |
| `0x0812B980` | release a voice's note |
| `0x0812BA6C` | stop a voice |
| `0x0812B520` | the PSG voices' frame |
| `0x0812B328` | the mixer's frame |
| `0x0812A964` | mix a sample voice's frame |

The music plays on player 0x12. The game's per-frame sound routine, at
`0x08129E7C`, fades the music in and out through requests, and hands over the
requests at its end.

The driver's variables in `AZLE` are:

| Address | Variable |
|---|---|
| `0x03000140` | the two output buffers, each 176 left samples then 176 right ones |
| `0x03000152` | the output buffer the mixer writes this frame |
| `0x03000158` | the region that instruments with a sample for each key play (see [Instruments](#instruments)) |
| `0x03000160` | the echo's level: 0-15 shifts the echo right by that many bits each time round, and 16 turns it off |
| `0x03000164` | the wave that the wave channel's RAM holds |
| `0x03000168` | the first sample voice that plays, in the mixer's order |
| `0x0300016C` | the first free sample voice |
| `0x03000175` | the echo buffer the mixer reads this frame |
| `0x03000176` | the echo's delay in frames (18) |
| `0x03000177` | the echo level that the game asks for, which the level moves towards by 1 a frame |
| `0x03000180` | a routine of the game's that plays a player's notes instead of the driver, when the player has flag 1 |
| `0x03000184` | a routine of the game's that `CA` calls |
| `0x03000188` | the requests, 12 bytes each |
| `0x030003CC` | the game's settings |
| `0x0300069C` | the mix: 176 left samples and 176 right ones, 16 bits each |
| `0x0300095C` | the mix of the voices with an echo send |
| `0x03000C20` | the 24 tracks, 0x54 bytes each |
| `0x03001760` | the 7 sample voices, then the 4 PSG voices, 0x78 bytes each |
| `0x03001C88` | the 20 players, 0x48 bytes each |
| `0x02036000` | the echo's history: a mix like `0x0300095C` for each of the echo's frames |

## The game's settings

The game passes init the address of 8 words:

| Offset | Setting |
|---|---|
| `0x00` | the sample sets: a table of offsets of each set's table of samples |
| `0x04` | the banks: a table of offsets of each bank |
| `0x08` | the music's sequences: a table of offsets of each sequence |
| `0x0C` | the sound effects' sequences, in the same form |
| `0x10` | a halfword for each bank: the sample set its instruments play |
| `0x14` | a table of offsets of each sequence's list of banks |
| `0x18` | the same for the sound effects |
| `0x1C` | a word the driver doesn't read |

Each table of offsets holds a word for each entry, counted from the table's
start. The tables don't give their length, and `supergbamidi` takes a table to
end where its first entry's data starts. A sample set's table of samples is in
the same form: an offset from its start for each sample.

## Requests

A request is 12 bytes: a halfword whose high byte picks a table of handlers
and whose low byte picks the handler, then two words of arguments. The first
table's handlers start a sequence on a player (`0`, with the player and the
sequence), start a sound effect (`1`, with the player in the first word's high
half, the sound effect's sequence in its low half, and the track in the second
word), fade a player out and stop it (`2`), pause or resume it (`3`), and set
its tempo change (`4`), its second volume (`5`) and its flags (`6`). The second
table's handlers change the tracks of a player that a mask picks, such as their
mute and their second volume, and the fourth's include the echo. A request is
carried out in the first frame after the game hands the requests over.

A sound effect is one track of one of the sound effects' sequences, which a
player plays with a priority of 12 and an echo send of `7F`. `supergbamidi`
doesn't convert sound effects.

## Sequences

A sequence starts with a signed byte, its number of tracks, and a byte of 0.
Then come the tracks' offsets, a halfword each, from the sequence's start; an
offset of 0 means the track doesn't start with the sequence, although `F8` can
start it later. A sequence's list of banks is a list of halfwords: entry `n`
is the bank that `C7 n` picks, and a track starts with entry 0. The lists of
different sequences may share entries.

## Players and tracks

Starting a sequence on a player stops what the player was playing, and starts
each of its tracks on the first free track of the 24. A player's record:

| Offset | Field |
|---|---|
| `0x00` | the sequence's list of banks |
| `0x04` | the sequence |
| `0x08` | the 10 tracks, a word each |
| `0x30` | the tempo, 150 at the start; `E4` sets it |
| `0x32` | a signed change to the tempo, which the game can set |
| `0x34` | the fade's level, `0x8000` for full |
| `0x36` | the fade's step a frame |
| `0x38` | the fade's last level |
| `0x3A` | the fade's frames left |
| `0x3C` | flags: bit 0 pauses the player, which releases its tracks' notes each frame |
| `0x40` | the volume, `0x80` at the start; `EA` sets it |
| `0x41` | a second volume, `0x80`; a request sets it |
| `0x42` | 1 while the player plays, 2 to stop it when its fade ends |
| `0x43` | 1 for a sound effect |
| `0x44` | flags: bit 0 gives the player's notes to the game's routine at `0x03000180` |

A track's record:

| Offset | Field |
|---|---|
| `0x00` | the next byte to read |
| `0x04` | the table of samples of its bank's sample set |
| `0x08` | its player, or 0 if the track is free |
| `0x0C` | its voices: the newest, which links to the next through the voice's `0x74` |
| `0x10` | the LFO's delay in frames (`E5`), a halfword |
| `0x14` | the LFO's rate (`E6`), `0x22` at the start |
| `0x18` | the LFO's depth (`E7`); 0 turns the LFO off |
| `0x1C` | 1 while the next note slides (`Dx`) |
| `0x1D` | the slide's flags: `Dx`'s low nibble |
| `0x1E` | the slide's note |
| `0x20` | the slide's delay in frames |
| `0x22` | the slide's length, in 256ths of the note's frames |
| `0x24` | 3 return addresses for `F4` |
| `0x30` | the next free return address |
| `0x34` | the time left before the next command, a signed word (see [Timing](#timing)) |
| `0x40` | the entry of the sequence's list of banks (`C7`) |
| `0x42` | the instrument (`C2`) |
| `0x44` | the notes' length, 127 at the start |
| `0x46` | `C0`'s wait |
| `0x48` | the notes' velocity, 127 at the start |
| `0x49` | 1 in legato (`C5`) |
| `0x4A` | 1 to mute the track's notes; a request sets it |
| `0x4B` | the pan, `0x40` for the centre (`C3`) |
| `0x4C` | the echo send (`E3`); 0 at the start, `7F` for a sound effect |
| `0x4D` | the volume (`E0`), `0x80` at the start |
| `0x4E` | a second volume, `0x80`; a request sets it |
| `0x4F` | the bend, a signed byte (`E1`) |
| `0x50` | the bend's range (`E2`), 2 at the start |
| `0x51` | the transpose (`E9`) |
| `0x52` | the notes' priority (`C4`), 3 at the start, 12 for a sound effect |
| `0x53` | 1 if notes wait for their length (`C8`), as a sound effect's do |

Each frame, the sequencer takes the players in order, and each player's tracks
in the order of its 10 entries. A track that ends frees its entry, and a player
whose tracks have all ended stops.

## Commands

A track's bytes are notes and commands. A length is a byte, or two bytes with
the first's top bit set: `((b0 & 0x7F) << 8) | b1`.

| Bytes | Meaning |
|---|---|
| `00`-`5F` | a note, with the track's length and velocity |
| `60`-`BF` length velocity | a note `nn - 0x60`, with a new length and velocity, which the track keeps |
| `C0` | wait for `C0`'s wait |
| `C1` length | wait, and keep the length as `C0`'s wait |
| `C2` n | instrument `n` of the bank |
| `C3` n | pan, `0x40` for the centre |
| `C4` n | the priority of the track's notes |
| `C5` | release the track's notes, and start legato |
| `C6` | release the track's notes, and end legato |
| `C7` n | the bank in entry `n` of the sequence's list, with its first instrument |
| `C8` | notes wait for their length |
| `C9` | notes don't wait |
| `CA` n | call the game's routine at `0x03000184` with the track and `n`, if it has one |
| `Dx` note length [delay] | slide the next note (see [Slides](#slides)); the delay byte comes with `x & 1` |
| `E0` n | volume |
| `E1` n | bend, a signed byte |
| `E2` n | bend range |
| `E3` n | echo send |
| `E4` length | tempo |
| `E5` n | the LFO's delay in frames |
| `E6` n | the LFO's rate |
| `E7` n | the LFO's depth |
| `E8` | no slide for the next note |
| `E9` n | transpose |
| `EA` n | the player's volume |
| `F0` offset | jump to the halfword offset from the sequence's start |
| `F4` offset | call: keep the next byte's address and go to the offset |
| `F8` n offset | start the player's track `n` at the offset, with this track's bank, instrument, pan, echo send, volumes, priority, bend, bend range and transpose; a track already in entry `n` stops first |
| `FF` | return from a call, or end the track |

The other bytes from `C2` up do nothing and take no operand. A note plays the
track's transposed note number: see [Note on](#note-on).

## Timing

A track counts time in 150ths of a tick, and the tick is the unit of lengths
and waits. A wait of `n` ticks adds `150n` to the track's count, and while the
count is 0 or less the track reads commands. After that, each frame takes the
player's tempo, and its tempo change, off the count. So a tempo of 150 plays a
tick a frame, and the count carries the part of a frame left over from one
command to the next. A tempo change takes effect at the end of the frame its
command comes in, for the tracks after it in the player's order and for the
track itself, and from the next frame for the ones before it.

A track that `F8` starts runs from the first frame the sequencer reaches it in:
the same frame if it comes later in the player's order, and the next if it
comes earlier.

The note's length is in frames: `150 * length / tempo`, rounded down, with
the tempo change too unless the note's region has flag `0x10`. A note
without `C8` doesn't wait, so notes at the same time make a chord.

## Instruments

A bank starts with a table of halfwords, each instrument's offset from the
bank's start. The instrument numbers that `C2` gives are bytes, so a bank has
up to 256 instruments, and the table doesn't give its length. An instrument's
first byte says what it is:

| Kind | Instrument |
|---|---|
| `00`-`0F` | a region of its own |
| `10` | a drum kit: halfword 2 is the offset of its table of drums, and byte 4 the note of its first drum. Each drum takes 4 bytes: the region's offset, a pan, and a byte the driver doesn't read. A drum plays at its region's pitch, with its own pan |
| `11` | a sample for each key: halfword 2 is the offset of a table with a halfword for each note, the sample's number. The driver puts the sample in its region in RAM, which plays a sample voice at the sample's rate, with the envelope at `0x08198EB4` and no release |
| `12` | a key split: halfword 2 is the offset of a table of 4-byte entries, each the highest note it plays as a byte, then the region's offset at its halfword 2. A note plays the first entry whose highest note is at least as high |

The offsets are from the bank's start. A region:

| Offset | Field |
|---|---|
| `0x00` | the voice type: 0 for a sample, 1 and 2 for the squares, 3 for the wave and 4 for noise |
| `0x01` | flags: bit 0 gives a table at halfword 2 (see [PSG voices](#psg-voices)), and bit 4 leaves the tempo change out of the note's length |
| `0x02` | the sample's number in the bank's sample set, a square's duty, noise's width, or the offset of a table or a wave |
| `0x04` | the envelope's offset |
| `0x06` | the release |
| `0x07` | the tuning: the note that plays at the sample's rate |
| `0x08` | square 1's NR10, for its frequency sweep |

The wave voice's wave, 16 bytes, comes from halfword 2 of the instrument
rather than its region.

An envelope is a list of points, each a signed halfword of frames and a signed
halfword of level, from 0 up to `0x7FFF` for full. The level starts at 0 and
moves in a straight line to each point's level over its frames, and a point
with negative frames marks the end: the level stays at the last point's. Each
frame the level moves on by the segment's step, the difference divided by its
frames, rounded towards 0, and lands on the point's level when the segment
ends.

A sample has a header of 4 words: its length in points, its rate in Hz, its
loop's start and its loop's end, or 0 for no loop. Its 8-bit signed points
follow.

## Note on

A note adds the track's transpose, and looks up the track's instrument. A
track in legato plays it on the voice it already has, which keeps its
envelope, its region and its release, and takes the new note's pitch and
sample. Otherwise it allocates a voice of the region's type, which starts its
envelope: see [Voices](#voices). A muted track plays nothing.

The note's pitch comes from the pitch index: the note, plus `0x30`, less the
region's tuning, kept from 0 to `0x78`. Index `0x78` reads one entry past the
tables, which the driver doesn't guard against. A drum or a sample for each key
plays note `0x30`. The index picks:

* for a sample voice, a step from the pitch table at `0x08199040`, a word for
  each index, `0x800 * 2^(index/12)`, so that `0x8000` plays the sample at its
  rate;
* for a square or the wave, a frequency setting from the table at `0x08198ED8`,
  a halfword for each index, middle C at index `0x30` on a square and C3 on the
  wave, and 0 below index `0x18`;
* for noise, an index into the noise table at `0x08198FC8`, which gives NR43.

A note of no frames is released at once.

### Slides

`Dx` makes the next note slide between its own pitch and the slide's note,
which `Dx` gives with the track's transpose. The slide lasts the slide's length
times the note's frames, divided by 256, after its delay, and moves the pitch
by a step each frame: the pitch difference divided by the slide's frames,
rounded towards 0. With `x & 2` the slide goes from the note to the slide's
note, and without it from the slide's note to the note. With `x & 4` the slide
goes on for each note after it, from the note before; without it, it's for one
note.

## Voices

Each voice has a record:

| Offset | Field |
|---|---|
| `0x00` | its type |
| `0x01` | 0 free, 1 playing, 2 released |
| `0x04` | its track, while it plays |
| `0x08` | its note's priority |
| `0x09` | the note, before the transpose |
| `0x0A` | the velocity |
| `0x0C` | the note's pitch, or the slide's start |
| `0x10` | the pitch in the frame |
| `0x14` | the level |
| `0x18` | frames before the release |
| `0x1A` | the echo send |
| `0x1B` | 1 for a drum, which has a pan of its own |
| `0x1C` | the drum's pan, or the track's at the release |
| `0x20` | the LFO's phase |
| `0x24` | the LFO's delay |
| `0x28` | its track's LFO settings |
| `0x2C`-`0x3C` | the slide: delay, frames, offset so far, offset at the end, step |
| `0x40`-`0x50` | the envelope: level, the segment's end, its frames, its step, the points, and the point it heads for |
| `0x54` | the region |
| `0x58` | the release |
| `0x5C` | the sample |
| `0x60` | the sample's position in 256ths of a point, or a PSG voice's frames played |
| `0x64` | a square's duty or noise's width, or the address of a table of them, or the wave |
| `0x68`, `0x6C` | the links of the sample voices' lists |
| `0x70`, `0x74` | the links of the track's voices |

A region's type picks the voices that can play it through the byte table at
`0x08198EAC`, which gives each type its own class. A PSG note takes its
channel's voice, if the voice is free or released, or its note's priority isn't
above the new note's. A sample note takes a free sample voice, or failing that
the first voice of the mixer's list, on the same terms. The mixer's list has
the released voices first and then the ones that play, each kept in order of
priority, and among equal priorities in order of age. So a note takes a
released voice first, and then the oldest of the lowest priority, and a note
that finds no voice isn't played.

A voice's frames count down each frame it plays, and when they run out, the
voice is released, unless its track is in legato. A released sample voice
fades out by itself, while a square or noise voice hands its release to its
channel's envelope and is free at once. A voice stops at once, and is free,
when another note takes it, when its sample without a loop ends, or when its
fade reaches 0. When a track ends, `C5` or `C6` comes, or `F8` starts a track
in its place, its notes are released.

## Pitch

Each frame, the pitch of a voice that plays starts from its note's, and adds:

* the slide's offset;
* the track's bend, if it isn't 0. With `b` the bend and `r` the entry of the
  pitch table for index `0x30` plus the bend's range, a sample's pitch is
  multiplied by `(|b| * r + 0x400000) / 128 / 0x8000` for a bend up, and divided
  by it for a bend down. A square or the wave works on its period, 2048 less
  the frequency setting, the other way round;
* the LFO, once its delay has run out: a sine of 256 signed bytes at
  `0x08199220`, read at the phase divided by 2. The phase goes up by the rate
  each frame, and wraps at 512. A sample's pitch goes up by `sine * pitch *
  depth / 2^19` for the positive half, and down by the same share for the
  negative half, as a division; a square's or the wave's period does the same
  with its own scale.

## Volume

A sample voice's level in a frame, while it plays, is the velocity times the
player's fade and volumes, the track's volumes and the envelope's level, with
these shifts:

    level = velocity << 7
    level = level * fade >> 8
    level = level * player volume >> 7
    level = level * player volume 2 >> 8
    level = level * track volume >> 8
    level = level * track volume 2 >> 15
    level = level * envelope >> 11
    volume = level * 3 >> 9

The mixer gets `volume`, and splits it between the sides by the pan:
`(127 - pan) * volume >> 8` on the left and `pan * volume >> 8` on the right,
each kept to 8 bits. After the release, the level is multiplied by
`(release + 230) / 512` each frame, and the voice stops when `volume` reaches
0.

## The mixer

For each sample voice of its list, the mixer works out the frame's step from
the pitch and the sample's rate:

    step = (pitch >> 2) * rate / 10512 >> 5

The step is in 256ths of a point, so a pitch of `0x8000` plays the sample at
its rate. The inner loop adds each point, without interpolation, times the
left and right volumes into the mix. At the loop's end, the position goes back
by the loop's length; a sample without a loop stops its voice when the mix
reaches its end. Then the mixer releases the sample voices whose frames have
run out.

The echo, when the game turns it on, is a delay of 18 frames: the voices with
an echo send mix into a buffer that starts with the echo of 18 frames before,
which the mix adds, and which is kept for 18 frames on, shifted right by the
echo's level. The game sets the echo through a request; the sequences only
give the sends. Last, the mix is divided by 128, kept to 8 bits, and written to
the output buffers.

## PSG voices

Each frame, a PSG voice that plays works out its pitch as a sample voice's
does, and its channel's pan: both sides of NR51 for a pan of `0x40`, the left
for less, and the right for more. A voice on one side counts twice as loud.

The PSG's envelope plays each segment of the voice's envelope. At the start of
each segment, the voice works out the level of the segment's start and end,
from 0 to 15, as the velocity times the volumes and the envelope's level, and
NRx2 gets the start, with a step of the segment's frames plus 14, divided by
the levels' difference, at most 7, up or down. A segment that doesn't change
the level keeps it. In the voice's first frame, it starts its channel at the
note's pitch, with NRx2 and its duty or width; after that, it writes its pitch
each frame, and NRx2 and a restart when a segment starts. The wave voice leaves
the envelope's level out: at the start of each segment, it turns the velocity
times the volumes into one of its five volumes (none, 25%, 50%, 75% and 100%),
from the table at `0x08198EC0`. It loads its wave into the wave RAM when the
wave changes.

With flag 0 of its region, a square or noise voice has a table of duties or
widths: a halfword count, then a byte for each frame from its first, and the
last stays. Without it, the region's halfword 2 gives the duty or width. A
square 2 voice with a table starts with a duty from the table's address
rather than from the table. A square 1 voice writes its region's NR10 when it
starts, and between the restarts that its envelope's segments bring, it only
writes its pitch if NR10 is 8, without a sweep.

A square or noise voice's release writes NRx2 with its last level and a step of
the release's top 3 bits, which fades the channel, or 0, which turns it off,
and restarts the channel. A released wave voice fades itself: its level is
multiplied by `(release + 230) / 512` each frame, and it stops at 0. The
released wave voice reads its track's settings at the start of each segment,
and has no track by then, so it reads them from address 0.

## Locating the driver

`supergbamidi` finds the driver's init routine by the start of its code:

    B530 49xx 6008 49xx 2000 7008 2080 7008 3904 4Axx 1C10 8008 3102 200D 7008

The game's settings are the word that the `ldr r0` just before a call to init
loads. The driver's tables come from the code that reads them:

| Table | Code | ldr |
|---|---|---|
| pitch table, frequency table | `B500 0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 D107 48xx` | halfwords 20 and 30 |
| noise table | `B500 0400 0C01 2977 D900 2177 48xx 1808 7800` | 6 |
| LFO table | `68A0 6883 2B00 D0xx 6860 2800 D1xx 48xx 6A29 0849 1809 7809` | 7 |
| wave volumes | `2904 D900 2104 0609 0E09 4Axx 48xx 1809 7808 7010` | 6 |
| voice classes | `49xx 7830 1840 7800 1C29 3152 7809` | 0 |
| the envelope for a sample for each key | `8869 1859 4Axx 0070 1840 8800 8050 6022 48xx 6060` | 8 |

The sequence count is the smaller of the entry counts in the sequence table
and the table of bank lists. `--song-table` overrides the detected settings
address, and `--song-count` overrides the sequence count.
