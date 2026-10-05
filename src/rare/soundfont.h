// SPDX-License-Identifier: MIT

// Builds SoundFont instruments from the driver's instruments and samples.

#pragma once

#include <cstdint>
#include <map>
#include <tuple>

#include "rare/driver.h"
#include "rare/song.h"
#include "rom.h"
#include "sf2.h"

namespace supergbamidi::rare
{

// A SoundFont volume envelope: times in timecents and the sustain level in centibels of attenuation.
struct Sf2Envelope
{
    int attack = -12000;
    int decay = -12000;
    int sustain = 0;
    int release = -12000;
};

// Returns the SoundFont envelope that comes closest to an instrument's envelope in the driver. The driver's decays and
// releases fall in a straight line in level, where a SoundFont's fall in a straight line in decibels.
Sf2Envelope EnvelopeFor(const DriverInfo& info, const Instrument& inst);

// A builder of SoundFont samples and instruments from the ROM, which reuses any already built. The converter adds the
// presets.
class SoundfontBuilder
{
public:
    // Keeps references to `rom` and `info`, which have to outlive the builder.
    SoundfontBuilder(const Rom& rom, const DriverInfo& info);

    Sf2File& File()
    {
        return file_;
    }

    // Returns the index of the SoundFont instrument for the driver's instrument at `address`, or -1 if it has no sample
    // that can be played.
    int InstrumentFor(uint32_t address);

    // Adds a preset for `program` in `bank` that plays SoundFont instrument `instrument`, unless the bank has a preset
    // for the program already.
    void AddPreset(int bank, int program, int instrument);

private:
    // Returns the index of the SoundFont sample for a sample instrument's sample, or -1 if it isn't valid.
    int SampleFor(const Instrument& inst);

    // Returns a zone that plays sample instrument `voice` on keys `low` to `high`. A drum kit's zones play at the
    // sample's pitch whatever the key.
    Sf2Zone ZoneFor(const Instrument& voice, int sample, int low, int high, bool fixed_pitch) const;

    const Rom& rom_;
    const DriverInfo& info_;
    Sf2File file_;
    std::map<uint32_t, int> instruments_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, int> samples_;
};

} // namespace supergbamidi::rare
