// SPDX-License-Identifier: MIT

// Builds SoundFont instruments from the driver's voices and samples.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <utility>

#include "mp2k/driver.h"
#include "mp2k/song.h"
#include "rom.h"
#include "sf2.h"

namespace supergbamidi::mp2k
{

// A SoundFont volume envelope: times in timecents and the sustain level in centibels of attenuation.
struct Sf2Envelope
{
    int attack = -12000;
    int decay = -12000;
    int sustain = 0;
    int release = -12000;
};

// Returns the SoundFont envelope that comes closest to a voice's envelope in the driver.
Sf2Envelope EnvelopeFor(const Voice& voice);

// A preset's bank and program.
struct PresetSlot
{
    int bank = 0;
    int program = 0;
};

// Builds SoundFont samples and instruments from the ROM, reusing any already built. The converter adds the presets.
class SoundfontBuilder
{
public:
    // Keeps references to `rom` and `info`, which have to outlive the builder.
    SoundfontBuilder(const Rom& rom, const DriverInfo& info);

    Sf2File& File()
    {
        return file_;
    }

    // Returns the index of the SoundFont instrument for the voice at `address`, or -1 if it has nothing to play.
    int InstrumentFor(uint32_t address);

    // Adds a preset for `program` in `bank` that plays SoundFont instrument `instrument`, unless the bank has a preset
    // for the program already. Returns false if that preset plays another instrument.
    bool AddPreset(int bank, int program, int instrument);

    // Returns the preset of a shared SoundFont that plays SoundFont instrument `instrument` as program `program`,
    // adding one if there isn't one. Each instrument keeps its program, and the banks tell apart the instruments that
    // songs play as the same program: the first gets bank 0, the next bank 1, and so on up to 127. Once a program is
    // taken in every bank, its other instruments get the first free program of the lowest bank that has one. Returns a
    // bank of -1 if there's none.
    PresetSlot SharedPreset(int program, int instrument);

private:
    // Returns the index of the SoundFont sample for a voice's sound, or -1 if it has none.
    int SampleFor(const Voice& voice);

    int DirectSample(uint32_t address);
    int SquareSample(int duty);
    int WaveSample(uint32_t address);
    int NoiseSample(int narrow);

    // Adds a zone that plays `voice` on keys `low` to `high`, at the pitch of key `fixed_key` if it isn't -1, and with
    // the drum kit's pan `rhythm_pan`. Adds nothing if the voice has nothing to play.
    void AddZone(Sf2Instrument& instrument, const Voice& voice, int low, int high, int fixed_key, int rhythm_pan);

    const Rom& rom_;
    const DriverInfo& info_;
    Sf2File file_;
    std::map<std::array<uint8_t, 12>, int> instruments_; // by the voice's bytes
    std::map<std::pair<int, uint32_t>, int> samples_;    // by kind and source
    std::map<std::pair<int, int>, PresetSlot> slots_;    // SharedPreset()'s presets, by program and instrument
};

} // namespace supergbamidi::mp2k
