// SPDX-License-Identifier: MIT

// SoundFont 2.01 writer.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace supergbamidi
{

// The SF2 generator operators this program uses.
namespace sf2gen
{

enum : uint16_t
{
    kStartAddrsOffset = 0,
    kStartAddrsCoarseOffset = 4,
    kPan = 17,
    kDelayVolEnv = 33,
    kAttackVolEnv = 34,
    kHoldVolEnv = 35,
    kDecayVolEnv = 36,
    kSustainVolEnv = 37,
    kReleaseVolEnv = 38,
    kInstrument = 41,
    kKeyRange = 43,
    kInitialAttenuation = 48,
    kCoarseTune = 51,
    kFineTune = 52,
    kSampleId = 53,
    kSampleModes = 54,
    kScaleTuning = 56,
    kOverridingRootKey = 58,
};

} // namespace sf2gen

// SF2 modulator sources: a controller in the low 7 bits, with flags for the controller palette, direction, polarity and
// curve.
namespace sf2src
{

enum : uint16_t
{
    kNoteOnVelocity = 2,
    kController = 0x80, // a MIDI controller, whose number is in the low 7 bits
    kNegative = 0x100,  // from the maximum down to the minimum
    kConcave = 0x400,
};

} // namespace sf2src

struct Sf2Sample
{
    std::string name;
    std::vector<int16_t> pcm;
    uint32_t rate = 22050;
    uint8_t root_key = 60;
    bool loop = false;
    uint32_t loop_start = 0; // relative to the sample start
    uint32_t loop_end = 0;   // exclusive
};

struct Sf2Gen
{
    // Returns a generator that sets a range, such as a key range.
    static Sf2Gen Range(uint16_t op, int lo, int hi)
    {
        return {op, uint16_t((lo & 0xFF) | ((hi & 0xFF) << 8))};
    }

    // Returns a generator that sets a signed amount.
    static Sf2Gen Value(uint16_t op, int v)
    {
        return {op, uint16_t(int16_t(v))};
    }

    uint16_t op;
    uint16_t amount;
};

// A modulator: `amount` times the value of `source` (and of `amount_source`, if it's not 0) added to generator `dest`.
struct Sf2Mod
{
    uint16_t source = 0;
    uint16_t dest = 0;
    int16_t amount = 0;
    uint16_t amount_source = 0;
    uint16_t transform = 0;
};

// A zone: a list of generators and modulators. keyRange must come first and the sample or instrument reference last,
// which Sf2File::Write() enforces. An instrument's first zone is its global zone if it has no sample.
struct Sf2Zone
{
    std::vector<Sf2Gen> gens;
    std::vector<Sf2Mod> mods;
};

struct Sf2Instrument
{
    std::string name;
    std::vector<Sf2Zone> zones;
};

struct Sf2Preset
{
    std::string name;
    uint16_t bank = 0;
    uint16_t program = 0;
    int instrument = 0; // index into Sf2File::instruments
};

struct Sf2File
{
    // Writes the file. Returns false and sets `error` if it's too big for the format or can't be written.
    bool Write(const std::string& path, std::string& error) const;

    std::string name = "Untitled";
    std::string comment;
    std::vector<Sf2Sample> samples;
    std::vector<Sf2Instrument> instruments;
    std::vector<Sf2Preset> presets;
};

} // namespace supergbamidi
