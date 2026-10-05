# Research and validation tools

These Python scripts run a game's sound driver from a ROM you supply. Use
them to study the drivers or check a change to `supergbamidi` against the
game's code. They aren't needed for conversion.

Setup (Python 3.10 or later):

```sh
python3 -m venv .venv
. .venv/bin/activate        # on Windows: .venv\Scripts\activate
pip install -r tools/requirements.txt
```

`compare_audio.py` also needs `ffmpeg`, and rendering a MIDI file for comparison
needs a SoundFont player such as `fluidsynth`.

These scripts in `tools/` serve more than one driver:

| Script | Description |
|---|---|
| `gbarom.py` | loads a `.gba` ROM or a GSF rip; used by the other scripts |
| `gbadis.py` | recursive-descent Thumb/ARM disassembler that resolves literal pools, marks code a GSF rip has zeroed out, and can read code at the address the game copies it to |
| `psg_model.py` | Game Boy APU model that renders a driver's PSG register writes to audio |

Driver-specific scripts are in `tools/konami/`, `tools/rare/`, `tools/quintet/`,
`tools/rd2/` and `tools/mp2k/`. Run the commands below from the repository
root.

## Konami's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator: per-frame track output records, DirectSound mixer output, PSG register writes |
| `compare_trace.py` | diffs `supergbamidi --trace` against the emulated driver, frame by frame, for every song |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the emulated driver, note by note |
| `compare_audio.py` | compares two renders: loudness envelopes, spectra and level ratio |
| `test_song.py` | writes a copy of a ROM with a test song for commands and outputs that the game's songs may not use |

### Checking supergbamidi against Konami's driver

```sh
python tools/konami/compare_trace.py rom.gba build/supergbamidi        # the sequencer: every song, 12000 frames
build/supergbamidi -q -o rom rom.gba                                   # convert every song into rom/
python tools/konami/compare_notes.py rom.gba build/supergbamidi rom    # the conversion, note by note
python tools/konami/driver_emu.py rom.gba render-ds 10 3000 ref.wav    # the driver's sample mix, song 10
build/supergbamidi -q -o out -s 10 -t 4-15 rom.gba                     # the same tracks as MIDI + SF2
fluidsynth -ni -R 0 -C 0 -F mine.wav out/rom_10.sf2 out/rom_10.mid
python tools/konami/compare_audio.py ref.wav mine.wav
```

`compare_notes.py` checks every note in a folder of MIDI files against what
the driver does with the hardware on the same frame: the note's start and stop,
the sample, duty cycle, wave or noise setting that plays, its pitch, its level
on each side and its echo send. If a MIDI track or an entire song is absent
from the conversion, the corresponding channel or song must also be silent in
the driver. It lists the differences and the largest pitch and level
deviations. PSG notes are written at their equal-tempered pitch, while the
driver's frequency table can only get within one step of the Game Boy's 11-bit
frequency register, so that much is allowed for.

`render-ds` leaves the echo out unless you pass `--echo`, because MIDI can't
reproduce it, and fluidsynth's reverb and chorus are turned off to match.
`render-psg` covers the four PSG channels, and `--channel N` renders one of
them. `--voices` on `render-ds` renders chosen sample voices.

### Working out Konami's driver

A GSF rip is a ROM from which `gsfopt` has zeroed every byte that the songs
never touched, so it holds only the code and data the music uses. That made
the driver easy to isolate:

1. **Find the entry points.** In a GSF rip, the ripper's patch calls the
   driver's "play song" and "start song" routines. Main's IRQ table points at
   the VBlank handler, which calls the per-frame routine.
2. **Disassemble.** `gbadis.py` was run from those entry points. The per-frame
   routine's command dispatch gave the byte code, and the output stage showed
   how each track's record reaches the PSG registers and the DirectSound
   voices. The mixer was found as ARM code copied to IWRAM.
3. **Model and compare.** The sequencer was reimplemented from that reading. It
   was then compared, record for record, with the driver running in
   `driver_emu.py`, until the two matched, loops included.
4. **Check the audio.** The converted MIDI and SoundFonts were rendered and
   compared with the driver's mixer output, and with `psg_model.py`, for
   timing, pitch and level.

`driver_emu.py` picks the routine addresses by the ROM's game code. Those of
Yu-Gi-Oh! Ultimate Masters Edition: World Championship Tournament 2006 (`BY6J`,
`BY6E`), Shaman King: Master of Spirits 1 and 2 (`BSOE`, `B2ME`), Yu-Gi-Oh! Day
of the Duelist (`BY7E`), Yu-Gi-Oh! GX: Duel Academy (`BYGE`), Yu-Gi-Oh! World
Championship Tournament 2004 (`BYWP`), Rave Master: Special Attack Force
(`BRME`), Yu-Gi-Oh! The Eternal Duelist Soul (`AY5E`), Yu-Gi-Oh! Worldwide
Edition (`AYWE`) and Yu-Gi-Oh! Dungeon Dice Monsters (`AYDE`) are built in. For
another game, find the same routines, starting from the patterns in
`docs/konami.md`, and add them to `GAMES`. A game whose driver has the same code
as one of these usually places the routines at different addresses. Match
the code while ignoring literal pools and call targets. Read the init arguments
from the game's call to the init routine, and the RAM addresses from its literal
pools.

An entry in `GAMES` also says how many tracks and voices the driver has,
where the track output records are and where their pan bytes are, how the
mixer scales a voice's level, whether it has echo and which voices it takes
from echo bus 0, and whether a pitch change reloads the wave RAM. Those differ
in the older revisions of `BYWP`, `BRME`, `AY5E`, `AYWE` and `AYDE`, whose
output stage is part of the sequencer. `compare_trace.py` and `compare_notes.py`
then check them the same way as the others. In `AY5E` and `AYWE`, a voice has
one level for both sides, and the per-frame entry runs the mixer itself, which
fills a ring buffer as far as the DMA 1 interrupt has moved its position. The
emulator skips that call, and when it renders, it moves the position on and
runs the mixer itself.

`AYDE` has no mixer. Its DMA 1 and DMA 2 interrupt routines each mix two voices
into a FIFO, 16 samples at a time, and each FIFO plays at its timer's rate,
set by the last note started on it. When the emulator renders, it plays each
FIFO out at its timer's rate, runs the routine whenever a FIFO has 16 samples or
fewer left, and samples the two at 32768 Hz. `compare_notes.py` takes a voice's
pitch from its FIFO's timer period, and the entry says how its voice records and
sample table are laid out. Every write to a square channel restarts it,
including vibrato updates. The wave channel restarts only when it loads a new
wave. `compare_notes.py` therefore allows a PSG note to start on any frame that
writes to its channel. A note must start when a silent channel begins playing,
the wave changes or the noise restarts. DMA transfers into the sound registers,
such as `AYDE`'s wave RAM loads, count as register writes.

A game's songs may not use every command and output its driver has, so
`test_song.py` covers the others with a test song. It writes a copy of the ROM
with the test song in place of song 0. The script's docstring shows how to
check it.

## Rare's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator and records its note slots after each frame or renders the mixer's output |
| `compare_trace.py` | diffs `supergbamidi --trace` against the emulated driver, frame by frame, for every tune |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the emulated driver, note by note |
| `compare_audio.py` | compares two renders: loudness envelopes, spectra and level ratio |
| `test_song.py` | writes a copy of a ROM with a test song for commands and modes that the game's tunes may not use |

### Checking supergbamidi against Rare's driver

```sh
python tools/rare/compare_trace.py rom.gba build/supergbamidi     # the model: every tune, 12000 frames
build/supergbamidi -q -o rom rom.gba                              # convert every tune into rom/
python tools/rare/compare_notes.py rom.gba rom                    # the conversion, note by note
python tools/rare/driver_emu.py rom.gba render 10 3000 ref.wav    # the driver's output for tune 10
fluidsynth -ni -R 0 -C 0 -F mine.wav rom/rom_10.sf2 rom/rom_10.mid
python tools/rare/compare_audio.py ref.wav mine.wav
```

`compare_trace.py` runs `supergbamidi --trace` and the driver side by side, and
compares every note slot after each frame: its state, channel, keys, velocity,
envelope phase and level, instrument, and its position in the sample to a
2^23rd of a byte. It reports the first difference in each tune that has one.
`-j` compares several tunes at once.

`compare_notes.py` checks every note in a folder of MIDI files against what the
driver plays on the same frame: the note's start and release, its velocity, the
channel's volume, the sample that the SoundFont plays for it, and its pitch on
every frame, from the SoundFont zone's tuning and the channel's pitch bend. The
MIDI file keeps each event on its own tick, where the driver plays it on a
frame, so it allows a frame's difference either way. It lists the differences
and the largest pitch deviation.

`driver_emu.py render` writes the driver's mono output at its own rate, and
`--channels` renders only the notes of the MIDI channels it names. The other
notes still count towards the mixer's limit of 8 voices, so the result is what
the game would play with those channels muted.

### Working out Rare's driver

The driver's init is easy to find in a ROM, because it copies the driver's ARM
code to IWRAM with a sequence that's the same in every revision (see
`docs/rare.md`). From there:

1. **Find the entry points.** The init stores the tune table's address and
   calls the routines that set the mix rate and the voice limit. The game calls
   the request routine with a tune number from many places, and the per-frame
   routine from one.
2. **Disassemble.** `gbadis.py` was run from those entry points, and with
   `--copy` it read the ARM code at the address the init copies it to. The
   sequencer's command table gave the byte code, and the mixer showed how each
   note's level and pitch are worked out.
3. **Model and compare.** The sequencer, the envelopes and the mixer's choice
   of voices were reimplemented from that reading. They were then compared,
   slot for slot, with the driver running in `driver_emu.py`, until the two
   matched, loops included.
4. **Check the conversion.** The converted MIDI files and SoundFonts were
   checked note by note with `compare_notes.py`, and rendered and compared with
   the driver's mixer output for timing, pitch and level.

`driver_emu.py` picks the routine and RAM addresses by the ROM's game code.
Those of *Donkey Kong Country* (`A5NE`), *Sabre Wulf* (`AWUE`), *It's Mr.
Pants* (`BPIE`), *Banjo-Kazooie: Grunty's Revenge* (`BKZX`), *Donkey Kong
Country 2* (`B2DE`) and *Banjo-Pilot* (`BAJE`) are built in. For another game,
find the same routines, starting from the patterns in `docs/rare.md`, and add
them to `GAMES`:

* `init`, `request` and `frame` are the init, the routine that requests a tune
  and the per-frame routine. In each of the games above, the request routine
  comes just before the init in ROM, and the per-frame routine a little after
  it.
* `tune`, `note_slots`, `fx_slots`, `buffer_flag`, `buffers` and
  `samples_per_frame` come from the per-frame routine and the mixer's literal
  pools: the tune playing, the note slots and sound effect records, the output
  buffer flag, the two words that hold the addresses of the output buffer's
  halves, and the samples a frame.
* `channel_volume`, the channels' controller 7 values, comes from the
  controller handler.
* `slots_per_channel` and `fx_count` give the note slots each channel has and
  the number of sound effect records, if they aren't 6 and 5. The table at the
  start of `docs/rare.md` gives them for each checked game.

`driver_emu.py` identifies the mixer and the note-advance routine in IWRAM by
their code, so you don't need to supply their addresses.

A game's tunes may not use every command and mode its driver has, so
`test_song.py` covers the others with a test song. It writes a copy of the ROM
with the test song in place of tune 0. The script's docstring shows how to
check it.

## Quintet's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator: its writes to the sound, DMA and timer registers, each music channel's position and countdown, and a render of its output |
| `compare_trace.py` | diffs `supergbamidi --trace` against the emulated driver's register writes, frame by frame, for every song |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against what the emulated driver plays, frame by frame |

### Checking supergbamidi against Quintet's driver

```sh
python tools/quintet/compare_trace.py rom.gba build/supergbamidi      # the model: every song, 12000 frames
build/supergbamidi -q -o rom rom.gba                                  # convert every song into rom/
python tools/quintet/compare_notes.py rom.gba build/supergbamidi rom  # the conversion, frame by frame
python tools/quintet/driver_emu.py rom.gba render 10 3000 ref.wav     # the driver's output for song 10
build/supergbamidi -q -l 1 -o out -s 10 rom.gba
fluidsynth -ni -R 0 -C 0 -F mine.wav out/rom_10.sf2 out/rom_10.mid
python tools/konami/compare_audio.py ref.wav mine.wav
```

`compare_trace.py` runs `supergbamidi --trace` and the driver side by side, and
compares every write the driver makes to the sound registers, DMA 1 and 2 and
timers 0 and 1, in order, starting with the play routine's. It leaves out the
bytes a FIFO start writes into the FIFO, which follow from the DMA's source. It
reports the first difference in each song that has one. `-j` compares several
songs at once.

`compare_notes.py` works out, from the driver's register writes, what each of
the 6 channels plays in each frame: whether it sounds, its level on each side,
its pitch, and its duty, wave pattern or sample. The PSG's envelope, length
counter and frequency sweep are modelled, and so is each FIFO's rate. It then
checks the MIDI file and the SoundFont against that, frame by frame: the
square and wave channels' frequency settings against the frequency table's
entry for each note's key, and the PCM channels' rates against the PCM pitch
table's, so that the tables' tuning, which the MIDI file leaves out, isn't
counted. A noise drum's sample includes the channel's volume, which is checked
frame by frame, allowing one step either way while the hardware envelope runs.
Since the MIDI file keeps each note on its own tick, up to a frame after the
driver's frame, each frame is compared with the same stretch of the note.

`driver_emu.py render` writes the driver's stereo output at 32768 Hz: the PSG
through `psg_model.py`, and each FIFO's sample at its timer's rate.
`--channels` renders only the music channels it names, all four PSG channels,
one of them, or none, with any of the FIFOs.

### Working out Quintet's driver

The games keep the driver in one block of Thumb code, which their sound
routines call into. From there:

1. **Find the entry points.** The game's sound init calls the driver's init and
   gives it the game's files, its play-song routine looks up the music file
   and calls the driver's play routine with a song's address, and a short
   routine calls the per-frame routine.
2. **Disassemble.** `gbadis.py` was run from those entry points. The note
   reader's command table gave the byte code, and the output stage showed how
   the channels' register image reaches the hardware.
3. **Model and compare.** The driver was reimplemented from that reading, and
   compared, write for write, with the driver running in `driver_emu.py`,
   until the two matched, loops included. The J revision was then worked out
   by comparing each of its routines with the A revision's.
4. **Check the conversion.** The converted MIDI files and SoundFonts were
   checked frame by frame with `compare_notes.py`, and rendered and compared
   with the driver's output for timing, pitch and level.

`driver_emu.py` picks the addresses by the ROM's game code. Those of *Super
Robot Taisen A* (`ASRJ`), *D* (`A6SJ`), *R* (`AJ9J`) and *J* (`B6JJ`) are
built in. For another game, add an entry to `GAMES` with:

* `sound_init`, `play` and `frame`: the game's routines that call the
  driver's init and set up its files, play a song by number, and run the
  per-frame routine. The play-song routine is the one that calls the driver's
  play routine (see `docs/quintet.md`), and the other two are near it.
* `channels`: the table of the 6 music channel records' addresses, which the
  driver's play routine loads.
* `countdown`: the offset of the countdown in a channel's record, `0x18` in
  the A revision and `0x1A` in the J revision.

## Nintendo R&D2's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator: each voice's state and the PSG register writes, frame by frame, the players and tracks in use, and a render of the mixer's output |
| `compare_trace.py` | diffs `supergbamidi --trace` against the emulated driver's voices and PSG register writes, frame by frame, for every sequence |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the notes the emulated driver plays |
| `test_song.py` | copies a ROM and replaces sequence 0 with a test sequence for the commands and kinds of instrument that a game's sequences may not use |

### Checking supergbamidi against Nintendo R&D2's driver

```sh
python tools/rd2/compare_trace.py rom.gba build/supergbamidi      # the model: every sequence, 12000 frames
build/supergbamidi -q -o rom rom.gba                              # convert every sequence into rom/
python tools/rd2/compare_notes.py rom.gba build/supergbamidi rom  # the conversion, note by note
python tools/rd2/test_song.py rom.gba test.gba                    # the test sequence, as sequence 0
python tools/rd2/compare_trace.py test.gba build/supergbamidi --songs 0
python tools/rd2/driver_emu.py rom.gba render 5 3000 ref.wav --tracks 1  # track 1 of sequence 5
build/supergbamidi -q -l 1 -o out -s 5 -t 1 rom.gba
fluidsynth -ni -r 10512 -F mine.wav out/rom_05.sf2 out/rom_05.mid
python tools/konami/compare_audio.py ref.wav mine.wav
```

`compare_trace.py` and `compare_notes.py` ask `supergbamidi` for the driver by
name, `--driver rd2`, since a game can have another driver as well.

`compare_trace.py` runs `supergbamidi --trace` and the driver side by side.
After each frame it compares every field of each voice that plays, the order of
the mixer's list of sample voices, and every write to the PSG's registers. It
reports the first difference in each sequence that has one. `-j` compares
several sequences at once.

`compare_notes.py` runs the driver with hooks in its note on routine, which
give each note's track, key, velocity, voice, region and sample, and follows
each voice from frame to frame. Each note the driver plays has to be in the
MIDI file, and the other way round: on its track's channel, with the key and
velocity the conversion gives it, starting in the frame the driver starts it in
or the frame before, and ending within a frame of the driver's release or stop
of its voice. Its SoundFont zone has to play the driver's sample at the pitch
the driver plays it at, to within a cent, and while it's the newest note of its
track, the MIDI file's pitch bend has to follow its voice's pitch, frame by
frame.

`driver_emu.py render` writes the mixer's stereo output at 10512 Hz: the 8-bit
samples the driver sends to the FIFOs, without the PSG. `--tracks` mutes the
player's other tracks with their mute flags, so that a track's output can be
compared with a render of `supergbamidi -t` alone.

### Working out Nintendo R&D2's driver

The driver is a block of Thumb code, with three ARM routines that its init
copies to IWRAM. From there:

1. **Find the entry points.** The game's sound init calls the driver's init
   with the game's settings, and the game's VBlank handler calls the driver's
   VBlank routine and its per-frame routine. The game's play-music routine
   requests a sequence for a player, and its per-frame sound routine hands the
   requests over.
2. **Disassemble.** `gbadis.py` was run from those entry points. The track
   reader's command table gave the byte code, and the voice routines showed
   how the mixer and PSG play each voice.
3. **Model and compare.** The driver was reimplemented from that reading, and
   compared voice for voice with the driver running in `driver_emu.py`, until
   the two matched, loops included.
4. **Check the conversion.** The converted MIDI files and SoundFonts were
   checked note by note with `compare_notes.py`, and track by track against
   the driver's renders for timing and level.

`driver_emu.py` picks the addresses by the ROM's game code. Those of *The
Legend of Zelda: A Link to the Past & Four Swords* (`AZLE`) are built in. For
another game, add an entry to `GAMES` with:

* `sound_init`: the game's routine that calls the driver's init with its
  settings.
* `request` and `commit`: the driver's routines that request a sequence for a
  player and hand the requests over (see `docs/rd2.md`).
* `vblank`, `frame` and `requests`: the driver's VBlank routine, its per-frame
  routine, and the per-frame routine's first step, which carries out the
  requests.
* `voices`, `active`, `tracks`, `players` and `fixed_region`: the driver's
  voices, its list of the sample voices that play, its tracks and players, and
  its region in RAM for instruments with a sample for each key.
* `outputs` and `output_index`: the output buffers, and the variable that picks
  the one the mixer writes.
* `note_on`, `note_key`, `note_voice` and `note_done`: the note on routine, and
  the points in it where it has added the transpose, found a voice, and set the
  voice's sample.

## MP2K

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's MP2K driver under the Unicorn ARM emulator and records its sound channels after each frame, or renders the mixer's output |
| `compare_trace.py` | diffs `supergbamidi --trace` against the emulated driver, frame by frame, for every song |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the emulated driver, note by note |

### Checking supergbamidi against MP2K

```sh
python tools/mp2k/compare_trace.py rom.gba build/supergbamidi     # the model: every song, 12000 frames
build/supergbamidi -q -o rom rom.gba                              # convert every song into rom/
python tools/mp2k/compare_notes.py rom.gba rom                    # the conversion, note by note
python tools/mp2k/driver_emu.py rom.gba render 10 3000 ref.wav    # the driver's DirectSound output for song 10
```

In games with another driver as well as MP2K, such as `AZLE`, detection finds
the other driver first. `compare_trace.py` selects MP2K by name; pass
`--driver mp2k` when converting these games too.

`compare_trace.py` runs `supergbamidi --trace` and the driver side by side, and
compares every sound channel that plays after each frame: its status, track,
keys, velocity, priority, envelope level and volumes, then a DirectSound
channel's rate, sample, and position in it to a 2^23rd of a point, or a PSG
channel's frequency setting, envelope goal and counter, sustain level, outputs
and wave. It reports the first difference in each song that has one. `-j`
compares several songs at once.

`compare_notes.py` checks every note the driver starts against the MIDI file:
the note's start and release, its velocity, its track's volume, the sound that
the SoundFont zone plays for it, and its pitch on every frame until its
release, from the zone's tuning and the channel's pitch bend. It reads the
driver's channels just before the PSG routine runs in each frame, when the
notes that the tracks started in the frame still have their start flag. A note
whose envelope never rises above 0 doesn't count. A PSG square or wave note can
be two steps of the 11-bit frequency register from equal temperament (see
`docs/mp2k.md`), and that much is allowed for. It needs a conversion made
without `--voice-channels`, so that each MIDI channel holds one track.

`driver_emu.py render` writes the DirectSound mixer's stereo output at its own
rate. It leaves the PSG channels out. `--tracks` mixes only the notes of the
tracks it names, and the others still take up channels, so the result is what
the game would play with those tracks muted.

### Working out MP2K

The driver's routines have the names of the SDK's m4a library, and the
patterns that `supergbamidi` finds them by are in `docs/mp2k.md`:

1. **Find the entry points.** `m4aSongNumStart` has the same code in every
   version, and its literal pool gives the song table and the music player
   table. `m4aSoundMain`, which calls the driver's main routine, comes just
   before it, and `m4aSoundInit`, which sets the driver's mode, a little before
   that. `m4aSoundVSync` restarts the sound DMA each frame.
2. **Model and compare.** The sequencer, the choice of channels, the
   envelopes and the mixer's progress through each sample were reimplemented
   from the driver's code. They were then compared, channel for channel, with
   the driver running in `driver_emu.py`, until the two matched.
3. **Check the conversion.** The converted MIDI files and SoundFonts were
   checked note by note with `compare_notes.py`.

`driver_emu.py` picks the routine addresses by the ROM's game code. Those of
*Pokémon Emerald* (`BPEE`) and *The Legend of Zelda: A Link to the Past & Four
Swords* (`AZLE`), whose Four Swords half and menus use MP2K, are built in. For
another game, find the same routines and tables, and add them to `GAMES`:
`init`, `song_start`, `main` and `vsync` are `m4aSoundInit`,
`m4aSongNumStart`, `m4aSoundMain` and `m4aSoundVSync`, and `song_table` and
`player_table` come from `m4aSongNumStart`'s literal pool. The emulator finds
the driver's state, its channels and the PSG routine from the structure whose
address the init stores at `0x03007FF0`.
