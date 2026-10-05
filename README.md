# supergbamidi

`supergbamidi` converts the music of Game Boy Advance games that use Konami's,
Rare's, Quintet's or Nintendo R&D2's sound driver, or Nintendo's MP2K, to MIDI
files and SoundFonts.

MP2K, often called the "Sappy" engine, is the sound driver in most GBA games.
Konami's, Rare's and Quintet's games, and *The Legend of Zelda: A Link to the
Past*, use drivers of their own, which the usual MP2K tools such as Sappy,
gba-mus-ripper and agbplay can't read. `supergbamidi` finds the game's driver
in a ROM, plays each song through a model of that driver, and converts it to a
Standard MIDI File with a matching SoundFont built from the game's samples and
instruments. For Konami's, Quintet's and Nintendo R&D2's drivers and MP2K, the
SoundFonts also include Game Boy PSG waveforms.

## Supported games

### Konami's driver

This part of the tool was developed on *Yu-Gi-Oh! Ultimate Masters Edition:
World Championship Tournament 2006*, and it supports these games too:

* *Shaman King: Master of Spirits*
* *Shaman King: Master of Spirits 2*
* *Yu-Gi-Oh! Day of the Duelist: World Championship Tournament 2005*
* *Yu-Gi-Oh! GX: Duel Academy*

These games use variants of the *Ultimate Masters Edition* driver.

The following games use older driver revisions with different commands.
These are supported too:

* *Yu-Gi-Oh! World Championship Tournament 2004*
* *Rave Master: Special Attack Force*
* *Yu-Gi-Oh! The Eternal Duelist Soul*
* *Yu-Gi-Oh! Worldwide Edition: Stairway to the Destined Duel*
* *Yu-Gi-Oh! Dungeon Dice Monsters*

The tool reports unrecognised revisions of Konami's driver.

### Rare's driver

* *Donkey Kong Country*
* *Donkey Kong Country 2*
* *Banjo-Kazooie: Grunty's Revenge*
* *Banjo-Pilot*
* *Sabre Wulf*
* *It's Mr. Pants*

### Quintet's driver

* *Super Robot Taisen A*
* *Super Robot Taisen D*
* *Super Robot Taisen R*
* *Super Robot Taisen J*, which has a later revision of the driver

### Nintendo R&D2's driver

* *The Legend of Zelda: A Link to the Past*, in *A Link to the Past & Four
  Swords*. The cartridge's game selection and *Four Swords* play their music
  with MP2K, which `--driver mp2k` converts.

### MP2K

* *Pokémon Emerald*

### Other games

Each driver is located from its own code rather than from fixed addresses, so
other games that use the same driver revisions should work too. Their songs
may still use unsupported commands. An unknown command stops the track and
produces a warning. `--info` names the driver it found and lists the detected
tables and any assumptions made during detection. If detection fails, you can
name the driver and supply table addresses with the override options.

## Building

For 64-bit Windows, download the zip containing `supergbamidi.exe` from
GitHub Releases.

You need a C++20 compiler and CMake 3.20 or later. There are no other
dependencies.

* **Windows:** Visual Studio 2022 or later with the "Desktop development with
  C++" workload, which includes CMake. MinGW-w64 works too.
* **macOS:** the Xcode command line tools (`xcode-select --install`) and CMake,
  for example from Homebrew (`brew install cmake`).
* **Linux:** GCC 10 or Clang 10 or later, and CMake from your distribution,
  for example `sudo apt install build-essential cmake` on Debian and Ubuntu.

Then, in the source folder:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The binary is `build/supergbamidi` (`supergbamidi.exe` on Windows). Visual
Studio and Xcode builds put it in `build/Release/` instead. On macOS the build
also makes `build/Supergbamidi.app`, which you can drop files on.

`ctest` runs the unit tests, and on macOS it also checks the report that
`Supergbamidi.app` puts together from the program's output. CI builds
supergbamidi and runs the tests on Windows, macOS and Linux.

## Usage

```sh
supergbamidi game.gba
```

This writes one `.mid` and one `.sf2` per song to a folder beside the input
file, with the same name: for example `game/game_00.mid` and
`game/game_00.sf2`. Pass several files to convert them together. Characters
that Windows forbids in filenames are replaced with underscores. Trailing dots
and spaces are removed, and reserved device names such as `CON` get an
underscore prefix.

Input can be a raw `.gba` ROM or a GSF rip (`.gsflib`, `.minigsf` or `.gsf`).
A `.minigsf` selects a song from the library named in its `_lib` tag. The
library is loaded from the same folder and supplies the output name. If you
pass several `.minigsf` files from one set, the library is converted once.
Conversion includes all songs in the ROM, regardless of the GSF's selected song.

Use a ROM if you have one. A GSF rip keeps only the bytes used during playback,
so a song that the rip didn't play may be missing. If every song cuts a
sample short, the unused part is silent in the SoundFont.

### Drag and drop

* **Windows:** drop `.gba`, `.gsflib` or `.minigsf` files on
  `supergbamidi.exe`. A console window shows what was converted and stays open
  until you press Enter.
* **macOS:** drop them on `Supergbamidi.app`, or open it and choose them. It
  then shows what was converted and offers to open the output folders.

Each file's results go in a folder next to it, as above.

### Options

| Option | Meaning |
|---|---|
| `-o, --output DIR` | output directory (default: a folder next to each input, named like the output files) |
| `-n, --name NAME` | base name of the output files (default: input name) |
| `-s, --songs LIST` | only these songs, e.g. `0,3,7-9` |
| `-l, --loops N` | play each song's loop N times (default 2) |
| `-t, --tracks LIST` | only these tracks, e.g. `4-15`; in Konami's driver, 0-3 are the PSG channels and 4 and up the sample voices, and in Quintet's, 0-3 are the PSG channels and 4 and 5 the PCM channels |
| `--single-sf2` | one SoundFont for all songs (see [Output](#output) for its banks) |
| `--voice-channels` | MP2K only: a MIDI channel for each of the driver's sound channels instead of each track, so that notes stop where the game cuts them off, though a track's notes then move between channels (see [MIDI from MP2K](#midi-from-mp2k)) |
| `--dump` | also write a text listing of every command of each song (`NAME_NN.txt`) |
| `--info` | print each driver found, with its tables and a list of its songs, then exit. A game with more than one driver lists the one a conversion uses first. The track count, length and loop reflect the current conversion options |
| `--driver NAME` | use `konami`, `rare`, `quintet`, `rd2` or `mp2k` instead of detecting the driver |
| `--song-table ADDR`, `--song-count N` | override detection: the song table's address (hex), or for Nintendo R&D2's driver the address of the game's settings for it, and the number of songs |
| `--sample-table ADDR`, `--mix-rate HZ` | override detection in Konami's driver: the sample table's address (hex) and the mixer's rate |
| `-q, --quiet` | only print warnings and errors |
| `--trace SONG`, `--trace-frames N` | print the driver model's state after each frame (for the driver's `compare_trace.py` in `tools/`) |

Detection looks for Rare's driver first, then Quintet's, Nintendo R&D2's and
MP2K, because they're found from their code alone, and then for Konami's. A
game with two drivers uses the first one found; `--driver` selects the
other. With `--song-table` but no `--driver`, the table goes to whichever driver
detection finds, so a game whose driver isn't recognised needs `--driver` as
well. `--sample-table` and `--mix-rate` imply `--driver konami`.

To listen, load the pair into any SoundFont player, for example:

```sh
fluidsynth -ni -F song10.wav game/game_10.sf2 game/game_10.mid
```

### Known issues

* `-t` given twice keeps only the last list, and a selection that matches no
  song still creates the empty output folder.
* An input file without an extension needs `-o`, since its default output
  folder would have the same path as the file. A path that ends in a slash shows
  an empty file name in messages.
* `--info` stops at the first driver whose tables it can't read; `--driver`
  reaches the others. It lists each song's length and loop without checking
  that the song converts, and leaves out the song's warnings.
* In an MP2K game with more than 100 songs, whose files get three-digit
  numbers, `--dump` numbers the listings of songs below 100 with two digits.
* On a case-sensitive file system, a `.minigsf` whose `_lib` tag differs in case
  from the library's file name doesn't find it.
* On Windows, the console window waits for Enter even when the output is
  redirected, the console's code page isn't restored afterwards, and redirected
  output and error messages can come out in a different order.

## Output

Songs that loop are written with the loop played twice by default. The loop is
also marked with `loopStart` and `loopEnd` marker events, which loop-aware
players honour.

A song's tracks can loop from different points and at different lengths. The
loop starts where every track is looping and every track without a loop has
finished, and it lasts until every track is back where its loop started: 12
bars for tracks that loop 3 and 4 bars, for example, so that it repeats
seamlessly. If that would be more than 8 times as long as the longest track's
loop, the loop is the longest track's, and the other tracks fall out of step
each time a player repeats it. Konami's driver sends every track back to its
loop point at once, so each of its songs loops from the first of those points.
A player that silences its notes when it repeats the loop also cuts short any
note held across the loop's start.

### MIDI from Konami's driver

Each MIDI file has a conductor track and one track per game track that plays
notes. Tracks are named `Square 1`, `Square 2`, `Wave`, `Noise`, and
`Voice 0`-`Voice 11` (game tracks 4-15, which play the driver's voices 0-11).
The older Rave Master, Eternal Duelist and Dungeon Dice Monsters revisions have
8, 6 and 4 voices. MIDI channel 10 is used only if a song needs all 16
channels.

The game counts time in frames, so one MIDI tick is one frame (1/59.73 s) and
every event is exactly on the frame the game plays it. The game doesn't store
a tempo, so `supergbamidi` estimates the beat length from the note spacing.
This only affects how bars line up in an editor.

The game's controls map to MIDI like this:

| Game | MIDI |
|---|---|
| note | note on/off; velocity is always 127 |
| volume and pan | CC11 (expression) for loudness, CC10 for pan (see below) |
| pitch bend, vibrato | pitch bend, with the bend range set per channel by RPN 0 |
| echo | CC91 (reverb send) on the voices routed to it |
| instrument changes | program changes |

In the game, volume can change during a note. CC11 is used instead of velocity
so that such changes carry over.

CC11 and CC10 together reproduce the game's per-side levels under the
constant-power pan law that most SoundFont players use.

In the Eternal Duelist revision, a PSG note at the current volume changes the
channel's frequency without restarting it. The MIDI file represents this as a
pitch bend of the current note. A series of these notes can span more than
24 semitones; the bend range is adjusted to fit.

In the Dungeon Dice Monsters revision, songs set separate left and right PSG
volumes. These are included in each PSG track's CC10 and CC11 values.

### SoundFont from Konami's driver

Each song's SoundFont contains exactly the instruments that song uses.

* **Sample tracks** get one preset per track (sometimes more, for samples
  whose keys would collide). The preset maps MIDI keys to the samples the
  track plays:
  * Samples played at several pitches are placed at their musical pitch, which
    is estimated from the waveform, so melodies read as the right notes.
  * Samples played at a single pitch, typically drums, get a key of their own.
* **Square channels** get one preset per duty cycle.
* **Wave channel** gets one preset per wave and volume. The driver stores a
  scaled copy of each wave for every volume. Quiet notes have coarser waveforms,
  which the presets preserve. The Dungeon Dice Monsters revision keeps each
  wave at full scale and sets the volume in the hardware, so its songs get one
  preset per wave.
* **Noise channel** gets one preset with a key per noise note, synthesised from
  the Game Boy's LFSR, and more presets if a song plays more noise notes than
  there are keys.

Samples keep the game's rate and loop points.

With `--single-sf2`, each song's instruments are in the bank numbered after the
song.

### MIDI from Rare's driver

The tune format resembles MIDI: note, controller, program and pitch bend
commands on 16 MIDI channels, separated by delays in ticks. The MIDI file
keeps the tune's ticks per quarter note, channels, keys, velocities, programs
and volumes (controller 7). Each source track that plays notes gets a MIDI
track named `Track 0`, `Track 1` and so on. A separate first track holds the
tempo and loop markers.

The driver counts each frame as 1/60 s, but the GBA shows 59.73 frames a
second, so the games play every tune 0.46% slower than its tempo says. The
MIDI file's tempos are slowed down to match.

The MIDI file follows what the driver plays, where that differs from the
tune's commands:

* Each channel has a limited number of note slots. The driver drops a new
  note if all of its channel's slots are playing. The MIDI file leaves such
  notes out, and ends each note where the driver's slot stops playing it, such
  as when a note in mono mode (controller 126) cuts off the one before.
* A note off releases only one note with that key on the channel. Since a
  MIDI channel can't play the same key twice at once, the conversion ends
  the older note when the newer one starts.
* The driver's vibrato (controller 1) is written out as pitch bends, a frame
  at a time, together with the tune's bends. Each channel's bend range is
  set with RPN 0 to fit the bends and the vibrato.
* Notes that share a tick with a program change use the same program as in
  the driver.

### SoundFont from Rare's driver

Each tune's SoundFont has a preset for each program the tune plays, named
`Program N` and with the same number, in bank 0. Programs played on channel 10,
which General MIDI players keep for drums, are in bank 128 as well, so that
such players play the same instruments there.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their
  rate, root key, fine tune and loop. A very short loop is repeated until it's
  at least 32 points long, since some players can't play shorter ones.
* **Drum kits and key splits** become a zone for each range of keys that plays
  the same instrument. A drum kit's zones play their samples at their own
  pitch on every key, as the driver does.
* **Envelopes** follow the driver's attack, decay, sustain and release. The
  driver's fades are straight lines in level, where a SoundFont's are straight
  lines in decibels, so decays and releases are stretched to keep the
  loudness close.
* **Volume and velocity** scale the level in a straight line, as in the
  driver, through modulators in each instrument. The SoundFont default would
  square them.

With `--single-sf2`, the tunes that use the first tune's instruments use bank
0, and each other set of instruments gets a bank of its own, which the tunes
select with a bank change at the start.

### MIDI from Quintet's driver

Each MIDI file has a conductor track with the tempo and loop markers, and a
track for each of the song's channels that plays notes: `Square 1`,
`Square 2`, `Wave`, `Noise`, `PCM A` and `PCM B`, on MIDI channels 1-6.

The song's tempo comes from its first channel. The MIDI file has 3552 ticks
per quarter note, or 37 per song tick. At tempo T, a driver frame lasts
exactly T MIDI ticks. The driver plays each note at the start of the frame
that its tick falls in, and the MIDI file keeps each note on its own tick,
within that frame. When a channel loops or the tempo changes, the
driver drops the part of a frame it has counted, and the MIDI file's notes
follow it from there.

| Game | MIDI |
|---|---|
| note | note on/off; velocity is always 127 |
| volume, pan and the master volume | CC11 for loudness and CC10 for pan, as with Konami's driver |
| detune, slides, the pitch envelope, the LFO and the frequency sweep | pitch bend, a frame at a time, with the bend range set per channel by RPN 0 |
| duty and wave bank switches | instruments that switch samples during each note |

Square and wave notes are at the key of their pitch: the frequency table
starts at C2, which is key 36. The bends hold the difference between the pitch
the channel plays and its table entry, so the keys stay equal-tempered (see
[Accuracy](#accuracy)). The frequency sweep changes square 1's pitch within a
frame, so its steps become extra bends between the frame boundaries. PCM notes
play the sample at its own rate on key 60, and each step of the driver's PCM
pitch table is a key. Noise notes are drums, with a key for each different sound.

### SoundFont from Quintet's driver

Each song's SoundFont holds the instruments its notes play:

* **Square channels** get a sample for each duty, and the wave channel a
  sample for each wave pattern of 32 or 64 steps. An instrument that switches
  its duty or wave bank during a note switches samples at the same point.
* **Noise drums** are rendered from the driver's writes to the noise channel,
  including the note's macro and volume changes, using the Game Boy's LFSR,
  envelope and length counter. A drum that ends on a steady sound loops its
  last two frames, which is sooner than the hardware's noise repeats, so a long
  held drum can pick up a 30 Hz buzz.
* **PCM samples** are the game's 8-bit samples, widened to 16 bits. Each plays
  as the driver plays it: from the note's start until the driver stops it or
  goes back to its loop, and then the frames of each pass through the loop. The
  driver checks a sample once a frame, so the lengths depend on the rate a note
  plays at, and a sample that plays at several rates gets a copy for each.

The levels follow the GBA's mixer: a square at full volume is 30/128 as loud as
a full-scale PCM sample. With `--single-sf2`, each song's instruments are in
the bank numbered after the song.

### MIDI from MP2K

Each of the song's tracks that plays notes gets a MIDI track, named `Track 0`,
`Track 1` and so on, on the MIDI channel with its number. A separate first
track holds the tempo and loop markers. The MIDI file keeps the driver's 24
ticks per quarter note, and each note's key and velocity. As with Rare's
driver, the tempos are slowed down by 0.46% to match the GBA's frame rate.

| Game | MIDI |
|---|---|
| note | note on/off, with the note's velocity |
| `VOICE` | program change, with the voice's number |
| `VOL`, `PAN` | CC7 (volume), CC10 (pan) |
| `BEND`, `BENDR`, `TUNE`, `KEYSH`, vibrato | pitch bend, with the bend range set per channel by RPN 0 |
| the song's reverb | CC91 (reverb send) |

The MIDI file follows what the driver plays, where that differs from the
song's commands:

* The driver plays a note on one of a few DirectSound channels, or on the
  PSG channel its voice uses, and drops a note that finds none it can take.
  The MIDI file leaves such notes out, and ends a note where a later note takes
  its channel.
* The MIDI file leaves out notes that the driver releases before they start,
  since they never sound. It also leaves out PSG notes whose volume rounds to
  silence on the PSG's 16 levels.
* Where a song changes the tempo, the driver has already counted part of the
  next tick at the old tempo, and the MIDI file's tempo for that tick does the
  same.
* The driver's LFO, when it modulates the pitch, is written out as pitch bends,
  a tick at a time. When it modulates the volume or the pan, it's written into
  CC7 or CC10.
* The key shift is part of the pitch bend, so that key splits and drum kits
  pick the same voices as in the game.
* While every note a track plays is a PSG note, its pitch bends follow what the
  driver plays: a noise note steps through the driver's noise settings, which
  move about 3 semitones a key, and a square or wave note doesn't go below C2.

When the driver gives a channel to a new note, it stops the note that was
playing there at once, even partway through its release, but a SoundFont player
lets that note fade out, so busy passages can sound fuller than in the game.
`--voice-channels` fixes that, at the cost of files that are harder to edit. It
gives each of the driver's sound channels that plays notes a MIDI channel and a
track, named `DirectSound 1` to `DirectSound 12`, `Square 1`, `Square 2`, `Wave`
and `Noise`, and leaves channel 10, which players keep for drums, until last.
Each note plays on the channel the driver gives it, and an All Sound Off (CC120)
stops its sound where the driver's channel stops. So a track's notes move
between channels, and each channel's program, volume, pan and pitch bend follow
the track whose note it plays; `--tracks` still picks the song's tracks.

### SoundFont from MP2K

Each song's SoundFont has a preset for each voice the song plays, named
`Program N` and with the voice's number, in bank 0, and in bank 128 too for
those played on channel 10.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their
  rate and loop. A compressed sample is decoded, and a reversed one is stored
  backwards.
* **Fixed-pitch samples**, which the driver plays at its mixer's rate whatever
  the key, play at that rate on every key, and the pitch bend doesn't move them.
* **PSG voices** play the Game Boy's square waves, the voice's 32-point wave
  pattern, or noise from the Game Boy's LFSR. Noise gets a zone for each of the
  driver's noise settings, at its rate, though settings below 32 Hz play at
  about 32 Hz, the lowest a zone can be tuned to. The PSG voices are 6.3 dB
  quieter than a sample at its full level, as on the GBA.
* **Key splits and drum kits** become a zone for each range of keys that plays
  the same voice. A drum kit's zones play at their voices' keys and pans.
* **Envelopes** follow the voice's attack, decay, sustain and release. A
  sample's decay and release fall by a fraction each frame, which is a straight
  line in decibels, as in a SoundFont. The PSG's fades are straight lines in
  level, and they're stretched to keep the loudness close.
* **Volume and velocity** scale the level in a straight line, as in the driver,
  and the driver's master volume scales the samples.

With `--single-sf2`, each voice keeps its number as its program, and banks tell
apart the voices that songs play under the same number: the first one is in
bank 0, the next in bank 1, and so on. Each program change in the MIDI files
comes with a bank change. Track 9 would play on channel 10, where players look
for programs in bank 128, so it moves to the first free channel after 10, or
else the last free one before it. A song that plays on all 16 channels keeps
track 9 on channel 10, with copies of its voices in bank 128, and a warning if
another song has one of those programs there already. If the songs played more
than 128 voices under one number, the rest would take free program numbers.

### MIDI from Nintendo R&D2's driver

Each of the sequence's tracks that plays notes gets a MIDI track, named
`Track 0`, `Track 1` and so on. Tracks 0 to 8 are on MIDI channels 1 to 9, and
track 9 is on channel 11, away from the drum channel. A separate first track
holds the tempo and loop markers.

The driver counts time in 150ths of a tick, with 24 ticks to the quarter note,
and each frame takes the tempo off the count. The MIDI file has 3600 ticks per
quarter note, one for each 150th, so a frame at the driver's tempo T is exactly
T ticks long, and each note stays on its own tick.

| Game | MIDI |
|---|---|
| note | note on/off; a velocity of v is written as 127 × √(v/127), since a SoundFont player's level goes with the square of the velocity |
| `C2` instrument, `C7` bank | program change, numbering the sequence's instruments in the order it first plays them |
| `C3` pan, `E0` volume, `EA` the player's volume | CC10 for pan and CC11 for loudness, as with Konami's driver |
| slides, `E1` bend, the LFO | pitch bend, a frame at a time, with the bend range set per channel by RPN 0 |

The MIDI file follows what the driver plays:

* The driver has 7 voices for samples and one for each PSG channel, and a note
  only takes a voice that its priority allows. The MIDI file leaves out the
  notes the driver can't play, and ends a note where another takes its voice.
* A note ends when the driver releases it. Its length in frames depends on the
  tempo when it starts. The driver also releases it when its track starts or
  ends legato, ends, or restarts.
* A track's pitch bend follows its newest note.

### SoundFont from Nintendo R&D2's driver

Each sequence's SoundFont has a preset for each instrument the sequence plays,
named after its bank and number.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their rate
  and loop, tuned to play at their rate on the note their region gives.
* **Drum kits and key splits** become a zone for each key or range of keys that
  plays the same region. A drum plays at its region's pitch and pan, and so
  does an instrument with a sample for each key.
* **PSG voices** play the Game Boy's square waves, the voice's 32-step wave, or
  noise from the Game Boy's LFSR, with a zone for each key's noise setting.
  Their level relative to the samples matches the driver's mix.
* **Envelopes.** The driver's envelopes are straight lines in level between
  points, and the SoundFont's attack, hold, decay and sustain approximate them.
  A sample's release reduces its level by a fraction each frame, which is a
  straight line in decibels, as in a SoundFont.

With `--single-sf2`, each sequence's instruments are in the bank numbered after
the sequence.

### Sequence listing (`--dump`)

The listing shows every track command with its address, raw bytes and meaning.
For Konami's driver, it includes the frame and the delay after the command;
for Rare's and Quintet's, it includes the tick. For MP2K and Nintendo R&D2's
driver, it follows each track as the driver plays it, through its calls (and
MP2K's patterns and repeats), up to its end or the end of its first loop, with
the tick of each command. Use it to study a song or
check the format documentation against real data.

## Accuracy

Some things differ from the hardware, or can't be expressed in MIDI and
SoundFonts.

### Konami's driver

* **PSG tuning.** The Game Boy's 11-bit frequency registers can't hit every
  pitch, so the game plays its high PSG notes slightly out of tune. The
  driver's frequency table is within about 3 cents of equal temperament below
  C4, but up to about 7 cents off in the C4 octave, 13 in the C5 octave, 25
  in the C6 octave and 50 in the C7 octave. The wave channel plays an octave
  lower than the squares from the same table. The MIDI files keep the
  equal-tempered pitch.
* **Echo.** The driver's echo is a feedback delay (up to about 190 ms). It's
  approximated with the reverb send; the delay and feedback are written to the
  conductor track as text events.
* **Mixer character.** The GBA mixer resamples without interpolation at
  20 or 21 kHz and writes 8-bit output. The Dungeon Dice Monsters revision has
  no mixer, and plays 8-bit samples at their own rate of 10 kHz. A SoundFont
  player plays the same samples more cleanly.
* **PSG DC offset.** The Game Boy channels output a unipolar signal. The
  SoundFont uses the same waveforms without the DC offset, which the hardware's
  output capacitor removes anyway.

### Rare's driver

* **Voice limit.** The driver mixes at most 8 voices at a time and leaves the
  rest silent until voices free up. A SoundFont player plays every note, so a
  busy passage can have notes in the MIDI file that the game doesn't let you
  hear.
* **Envelopes.** The SoundFont's envelopes approximate the driver's straight
  fades, and some players treat very short envelope phases differently.
* **Timing.** The driver runs once a frame, so it plays every event on a frame
  boundary. The MIDI file keeps each event on its own tick, which can be up to
  a frame earlier.
* **Mixer character.** The driver mixes in mono at 13379 Hz with linear
  interpolation, and writes 8-bit output. A SoundFont player plays the same
  samples more cleanly, and the MIDI files leave every channel in the centre.
* **Sound effects** aren't converted. The driver plays them from a separate
  table, which only the game's code uses.

### Quintet's driver

* **Timing.** The driver plays each note at the start of the frame its tick
  falls in. The MIDI file keeps each note on its own tick, which can be up to a
  frame later.
* **Tuning.** The driver's frequency table is within about 3 cents of equal
  temperament in the C4 octave, but up to about 27 cents flat in the C2 octave,
  9 cents in the C3 octave, 19 to 34 cents off in the C5 to C7 octaves, and 43
  in the top octave. The PCM pitch table is within 4 cents, apart from its top
  entry, which is 14 cents flat. The MIDI files keep the keys equal-tempered.
* **Noise drums.** The noise channel's envelope steps on the sound hardware's
  64 Hz clock, which isn't tied to the frames, so in the game a drum's volume
  steps can come up to 1/128 s earlier or later than in its sample.
* **Pitch changes during a PCM note.** A sample's loop is as long as the driver
  makes it at the rate the note starts at. A note whose rate then changes, with
  a slide or the LFO, keeps those lengths.
* **Mixer character.** The GBA plays the FIFOs' 8-bit samples without
  interpolation. A SoundFont player plays them more cleanly. The PSG's
  waveforms are without the DC offset, which the hardware's output capacitor
  removes anyway.
* **Sound effects** aren't converted. The driver plays them on channels of its
  own, which only the game's code starts.

### Nintendo R&D2's driver

* **Timing.** The driver plays each note at the start of the first frame that
  reaches its tick. The MIDI file keeps each note on its own tick, which can be
  up to a frame earlier.
* **Pitch bends.** The driver bends each voice on its own, and its LFO starts
  again with each note. A MIDI channel has one bend, which follows the track's
  newest note, so notes that play together on a track share it.
* **Envelopes.** The SoundFont's envelopes approximate the driver's straight
  lines between points.
* **Legato.** The driver plays a track's notes in legato on one voice, which
  keeps its envelope. A SoundFont player starts each note's envelope again.
* **Echo.** The driver can add an echo to the voices of tracks with an echo
  send, but only the game's code turns it on, so the conversion leaves it out.
* **PSG.** The PSG's envelope steps the level through 16 values, where the
  SoundFont's envelope is smooth. The PSG's notes keep equal-tempered keys.
* **Mixer character.** The driver mixes in stereo at 10512 Hz without
  interpolation, and writes 8-bit output. A SoundFont player plays the same
  samples more cleanly.
* **Sound effects** aren't converted. Only the game's code starts them.

### MP2K

* **Timing.** The driver plays each event at the start of the first frame that
  reaches its tick. The MIDI file keeps each event on its own tick, which can
  be up to a frame earlier.
* **PSG tuning.** As with Konami's driver, the PSG's 11-bit frequency registers
  put the game's high PSG notes slightly out of tune, and the driver's table
  and its bends round down. The MIDI files keep the equal-tempered pitch.
* **PSG bends.** A track that plays samples and PSG notes at once has one pitch
  bend for both, which bends a noise note smoothly and can take a square or
  wave note below C2. The driver steps through its noise settings and stops at
  C2.
* **PSG sweep and length.** Square channel 1 can sweep a note's frequency up or
  down, and a PSG voice can have a length, after which the hardware stops the
  note. The SoundFont leaves both out.
* **Notes cut off.** The driver stops a note at once when it gives the note's
  channel to another, even partway through its release. Without
  `--voice-channels`, a SoundFont player lets the note fade out instead.
* **Echo.** A track can give its notes a quiet echo after their release. The
  SoundFont's release doesn't include it.
* **Reverb.** The driver's reverb is a short echo of its output. It's
  approximated with the reverb send.
* **Changed voices.** A track can change its copy of a voice with extended
  commands, and a note can start partway into its sample. The SoundFont plays
  the voice group's voices as they are, from the start.
* **Sound register writes.** A track can write to the sound registers. The
  conversion leaves these out, with a warning.
* **Mixer character.** The driver mixes at its own rate, often 13379 Hz, with
  linear interpolation, and writes 8-bit output. A SoundFont player plays the
  same samples more cleanly.

## Internals

`docs/konami.md`, `docs/rare.md`, `docs/quintet.md`, `docs/rd2.md` and
`docs/mp2k.md` document the drivers and their data formats in full, including
how the tool locates them. The code for each is in `src/konami/`, `src/rare/`,
`src/quintet/`, `src/rd2/` and `src/mp2k/`, behind the interface in
`src/music.h`.

`tools/` holds the scripts used to reverse engineer the drivers and validate
the conversion: a disassembler, harnesses that run each game's driver
under an ARM emulator, and comparison scripts. See `tools/README.md`.

## License

`supergbamidi` is released under the MIT License; see `LICENSE`. The DEFLATE
decoder in `src/inflate.cpp` is adapted from Mark Adler's puff under the zlib
license. Its notice is in that file and in `THIRD_PARTY_NOTICES`. Game data is
not covered by these licenses.
