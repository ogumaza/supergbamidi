// SPDX-License-Identifier: MIT

#include "rare/soundfont.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace supergbamidi::rare
{
namespace
{

// The shortest time a SoundFont envelope can give a phase, in timecents: 1 ms.
constexpr int kInstant = -12000;

// The greatest attenuation, in centibels.
constexpr int kSilent = 1440;

// The shortest loop a sample gets, in points. A shorter one is repeated until it's at least this long, since some
// players can't play a very short loop.
constexpr uint32_t kMinLoop = 32;

// The points after a loop's end that repeat its start, for players that read past the end when they interpolate.
constexpr uint32_t kLoopGuard = 8;

// Returns a time in timecents, the unit of SoundFont envelope times.
int Timecents(double seconds)
{
    if (seconds <= 0.001)
    {
        return kInstant;
    }

    return std::clamp(int(std::lround(1200.0 * std::log2(seconds))), kInstant, 8000);
}

// Returns the SoundFont time for a decay or release of `frames` frames that falls to silence. The driver's fade is a
// straight line in level, which stays loud for most of its length, where a SoundFont's is a straight line in decibels,
// which drops sooner. Stretching the SoundFont's keeps the loudness of the two close while the fade can be heard. A
// fade of a few frames falls in steps, and needs less stretching than a longer one.
double FadeSeconds(int frames)
{
    const double stretch = std::clamp(3.0 + 3.0 * (frames - 5) / 11.0, 3.0, 6.0);
    return stretch * frames / kFrameRate;
}

// Returns the sustain level of an instrument with sustain value 0-99 as the driver works it out, from 0 to 0x8000.
uint32_t SustainLevel(uint32_t sustain)
{
    const uint32_t value = std::min<uint32_t>(sustain, 99);
    const uint32_t whole = (value << 7) / 99;
    const uint32_t part = (((value << 7) % 99) << 8) / 99;
    return (whole << 8) | part;
}

std::string Name(const char* kind, uint32_t address)
{
    char b[32];
    std::snprintf(b, sizeof b, "%s %08X", kind, unsigned(address));

    return b;
}

} // namespace

Sf2Envelope EnvelopeFor(const DriverInfo& info, const Instrument& inst)
{
    Sf2Envelope env;

    // The attack rises in a straight line, as a SoundFont's does, and reaches the full level after `steps` + 1 frames.
    const uint32_t attack = std::min<uint32_t>(inst.attack, 99);
    const uint32_t steps = ((99 - attack) << 5) / 99;
    if (steps > 0)
    {
        env.attack = Timecents((steps + 1) / kFrameRate);
    }

    // The decay falls to the sustain level. At a sustain of 0, the driver stops the note when the decay ends.
    const uint32_t sustain = SustainLevel(inst.sustain);
    const int decay = FadeFrames(info, inst.decay);
    if (sustain < 0x8000)
    {
        const double level = sustain / 32768.0;
        if (sustain << 4 < 0x1000)
        {
            env.sustain = kSilent;
            env.decay = decay ? Timecents(FadeSeconds(decay)) : kInstant;
        }
        else
        {
            // A SoundFont's decay time is for a fall of 100 dB, so it reaches the sustain level in proportion.
            env.sustain = std::min(kSilent, int(std::lround(-200.0 * std::log10(level))));
            env.decay = decay && env.sustain > 0 ? Timecents(1000.0 / env.sustain * decay / kFrameRate) : kInstant;
        }
    }

    const int release = FadeFrames(info, inst.release);
    if (release)
    {
        env.release = Timecents(FadeSeconds(release));
    }

    return env;
}

SoundfontBuilder::SoundfontBuilder(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

int SoundfontBuilder::InstrumentFor(uint32_t address)
{
    const auto cached = instruments_.find(address);
    if (cached != instruments_.end())
    {
        return cached->second;
    }

    instruments_[address] = -1;
    Instrument top;
    if (!ReadInstrument(rom_, address, top))
    {
        return -1;
    }

    // The global zone makes velocity and controller 7 scale the level in a straight line, as the driver does. The
    // default curves square them, and these take the place of the defaults.
    Sf2Instrument si;
    si.name = Name("Instrument", address);
    Sf2Zone global;
    global.mods.push_back(
        {sf2src::kNoteOnVelocity | sf2src::kNegative | sf2src::kConcave, sf2gen::kInitialAttenuation, 480, 0, 0});
    global.mods.push_back({uint16_t(sf2src::kController | 7) | sf2src::kNegative | sf2src::kConcave,
                           sf2gen::kInitialAttenuation, 480, 0, 0});
    si.zones.push_back(global);

    if (top.IsSample())
    {
        const int sample = SampleFor(top);
        if (sample >= 0)
        {
            si.zones.push_back(ZoneFor(top, sample, 0, 127, false));
        }
    }
    else
    {
        // A drum kit or key split gets a zone for each run of keys that play the same instrument. The driver plays any
        // other type of instrument as a drum kit.
        const bool drums = top.type != kInstKeySplit;
        int key = 0;
        while (key < 128)
        {
            const uint32_t sub_address = SplitInstrument(rom_, top, key);
            int last = key;
            while (last + 1 < 128 && SplitInstrument(rom_, top, last + 1) == sub_address)
            {
                last++;
            }

            Instrument sub;
            if (sub_address && ReadInstrument(rom_, sub_address, sub) && sub.IsSample())
            {
                const int sample = SampleFor(sub);
                if (sample >= 0)
                {
                    si.zones.push_back(ZoneFor(sub, sample, key, last, drums));
                }
            }

            key = last + 1;
        }
    }

    if (si.zones.size() < 2)
    {
        return -1;
    }

    file_.instruments.push_back(si);
    instruments_[address] = int(file_.instruments.size()) - 1;

    return instruments_[address];
}

void SoundfontBuilder::AddPreset(int bank, int program, int instrument)
{
    for (const Sf2Preset& p : file_.presets)
    {
        if (p.bank == bank && p.program == program)
        {
            return;
        }
    }

    Sf2Preset p;
    p.name = "Program " + std::to_string(program);
    p.bank = uint16_t(bank);
    p.program = uint16_t(program);
    p.instrument = instrument;
    file_.presets.push_back(p);
}

int SoundfontBuilder::SampleFor(const Instrument& inst)
{
    if (!SampleValid(rom_, inst))
    {
        return -1;
    }

    const bool loops = inst.Loops();
    const auto key = std::make_tuple(inst.start, inst.end, loops ? inst.loop_length : 0, uint32_t(loops), inst.rate,
                                     std::min<uint32_t>(inst.root_key, 127));
    const auto cached = samples_.find(key);
    if (cached != samples_.end())
    {
        return cached->second;
    }

    // The driver's samples are signed 8-bit.
    Sf2Sample s;
    s.name = Name("Sample", inst.start);
    s.rate = inst.rate;
    s.root_key = uint8_t(std::min<uint32_t>(inst.root_key, 127));
    const uint32_t length = inst.end - inst.start;
    for (uint32_t i = 0; i < length; i++)
    {
        s.pcm.push_back(int16_t(rom_.S8(inst.start + i) * 256));
    }

    // A loop is the sample's last loop_length points. A short one is repeated after the end until it's long enough.
    if (loops)
    {
        s.loop = true;
        s.loop_start = length - inst.loop_length;
        s.loop_end = length;
        while (s.loop_end - s.loop_start < kMinLoop)
        {
            for (uint32_t i = 0; i < inst.loop_length; i++)
            {
                s.pcm.push_back(s.pcm[s.loop_start + i]);
            }

            s.loop_end += inst.loop_length;
        }

        for (uint32_t i = 0; i < kLoopGuard; i++)
        {
            s.pcm.push_back(s.pcm[s.loop_start + i % (s.loop_end - s.loop_start)]);
        }
    }

    file_.samples.push_back(s);
    samples_[key] = int(file_.samples.size()) - 1;

    return samples_[key];
}

Sf2Zone SoundfontBuilder::ZoneFor(const Instrument& voice, int sample, int low, int high, bool fixed_pitch) const
{
    Sf2Zone z;
    z.gens.push_back(Sf2Gen::Range(sf2gen::kKeyRange, low, high));

    // Envelope phases at their defaults are left out.
    const Sf2Envelope env = EnvelopeFor(info_, voice);
    const std::pair<uint16_t, int> phases[] = {
        {sf2gen::kAttackVolEnv, env.attack}, {sf2gen::kDecayVolEnv, env.decay}, {sf2gen::kReleaseVolEnv, env.release}};
    for (const auto& [op, value] : phases)
    {
        if (value != kInstant)
        {
            z.gens.push_back(Sf2Gen::Value(op, value));
        }
    }
    if (env.sustain)
    {
        z.gens.push_back(Sf2Gen::Value(sf2gen::kSustainVolEnv, env.sustain));
    }

    // A fine tune of a semitone or more goes partly into the coarse tune, since the fine tune only reaches 99 cents.
    if (voice.fine_tune / 100)
    {
        z.gens.push_back(Sf2Gen::Value(sf2gen::kCoarseTune, std::clamp(voice.fine_tune / 100, -120, 120)));
    }
    if (voice.fine_tune % 100)
    {
        z.gens.push_back(Sf2Gen::Value(sf2gen::kFineTune, voice.fine_tune % 100));
    }

    if (fixed_pitch)
    {
        z.gens.push_back(Sf2Gen::Value(sf2gen::kScaleTuning, 0));
    }
    if (voice.Loops())
    {
        z.gens.push_back(Sf2Gen::Value(sf2gen::kSampleModes, 1));
    }
    z.gens.push_back(Sf2Gen::Value(sf2gen::kSampleId, sample));

    return z;
}

} // namespace supergbamidi::rare
