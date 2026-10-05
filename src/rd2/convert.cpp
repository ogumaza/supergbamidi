// SPDX-License-Identifier: MIT

#include "rd2/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "files.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "rd2/sequencer.h"
#include "sf2.h"

namespace supergbamidi::rd2
{
namespace
{

// The MIDI file's resolution: a tick for each of the driver's units, 150 to the driver's tick and 24 of those to the
// quarter note, so that a frame at the driver's tempo T is exactly T ticks long.
constexpr uint32_t kTicksPerQuarter = 24 * kTickUnits;

// The CPU cycles in a frame, and a second's.
constexpr uint64_t kFrameCycles = 280896;
constexpr uint64_t kSecondCycles = 16777216;

// The longest conversion, in frames: about an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The slowest tempo a MIDI file can hold at this resolution: at 3 or less, a quarter note lasts more than the 2^24
// microseconds a tempo event can give it.
constexpr int kMinMidiTempo = 4;

// The widest pitch bend range the MIDI files set, in semitones.
constexpr int kMaxBendRange = 96;

// The MIDI channel that General MIDI players keep for drums, which the tracks leave out.
constexpr int kDrumChannel = 9;

// The pan that plays a voice on both sides alike.
constexpr int kCentre = 0x40;

// The pitch index that plays a sample at its own rate, a square at middle C and the wave at C3.
constexpr int kUnityIndex = 0x30;

// The highest pitch index the driver reads, one past its tables.
constexpr int kTopIndex = 0x78;

std::string TwoDigits(int n)
{
    char b[16];
    std::snprintf(b, sizeof b, "%02d", n);

    return b;
}

// Returns the seconds that `frames` frames take.
double FrameSeconds(double frames)
{
    return frames * double(kFrameCycles) / double(kSecondCycles);
}

// Returns the MIDI channel of the player's track `number`.
int ChannelOf(int number)
{
    return number < kDrumChannel ? number : number + 1;
}

// Returns how far a voice's pitch in the frame is from its note's, in semitones. A square or wave voice's pitch is a
// frequency setting, whose period is 2048 less the setting. A noise voice has none.
double Semitones(const Voice& v)
{
    if (v.type == kSampleType)
    {
        return v.note_pitch && v.pitch ? 12.0 * std::log2(double(v.pitch) / double(v.note_pitch)) : 0.0;
    }
    if (v.type == kNoiseType || v.pitch >= 2048 || v.note_pitch >= 2048)
    {
        return 0.0;
    }

    return 12.0 * std::log2((2048.0 - v.note_pitch) / (2048.0 - v.pitch));
}

// A frame's results for the conversion.
struct FrameData
{
    int tempo = kStartTempo;                // the units the frame lasts
    std::array<double, kPlayerTracks> bend; // semitones from its note of each track's newest voice, or NaN for none
};

// One of the model's events and the frame it happened in.
struct TimedEvent
{
    Event event;
    uint32_t frame = 0;
};

// The stretch of a sequence that the conversion covers, in MIDI ticks.
struct Plan
{
    bool loops = false;
    uint64_t loop_start = 0; // the latest of the looping tracks' loop starts and the other tracks' last commands
    uint64_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the tracks' loops
    uint64_t end = 0;
};

// The model's output for the stretch of a sequence that the plan covers, and its timing in the MIDI file.
struct Simulation
{
    std::vector<FrameData> frames;
    std::vector<TimedEvent> events;
    std::vector<uint64_t> frame_ticks; // the MIDI tick each frame starts at, and the one after the last frame
    Plan plan;
    std::vector<std::string> warnings;
};

// Runs the model until every track has looped or ended and the sequence has played its loop `loops` times. A track's
// units are the MIDI file's ticks: each frame lasts the tempo's units, and each command comes at its track's time.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    sim.frame_ticks.push_back(0);

    // Each track's first loop, its count of loops, its end, and the time by which it has read its last command and its
    // notes have ended, unless its end released them first. A track that F8 starts again doesn't hold the plan open.
    struct TrackPlan
    {
        bool started = false;
        bool restarted = false;
        bool ended = false;
        int loops = 0;
        uint64_t loop_start = 0;
        uint64_t loop_end = 0;
        uint64_t end = 0;
        uint64_t last_command = 0;
    };
    std::array<TrackPlan, kPlayerTracks> tracks;
    bool planned = false;
    bool looping = false;
    uint64_t loop_start = 0;
    uint64_t loop_length = 0;
    for (uint32_t f = 0; f < kMaxFrames && seq.Valid(); f++)
    {
        seq.Step();
        FrameData d;
        d.tempo = std::max(0, int(seq.GetPlayer().tempo) + seq.GetPlayer().tempo_adjust);
        d.bend.fill(std::numeric_limits<double>::quiet_NaN());
        for (int n = 0; n < kPlayerTracks; n++)
        {
            const int slot = seq.GetPlayer().tracks[size_t(n)];
            if (slot < 0)
            {
                continue;
            }

            tracks[size_t(n)].started = true;
            const int v = seq.Tracks()[size_t(slot)].voices;
            if (v >= 0 && seq.Voices()[size_t(v)].state == 1)
            {
                d.bend[size_t(n)] = Semitones(seq.Voices()[size_t(v)]);
            }
        }
        sim.frames.push_back(d);
        sim.frame_ticks.push_back(seq.Elapsed());

        for (const Event& e : seq.Events())
        {
            sim.events.push_back({e, f});
            if (e.track < 0 || e.track >= kPlayerTracks)
            {
                continue;
            }

            TrackPlan& t = tracks[size_t(e.track)];
            t.started = true;
            if (e.phase == Event::kCommands && e.kind != Event::kTrackEnd)
            {
                t.last_command = std::max(t.last_command, e.units + (e.kind == Event::kNote ? e.length : 0));
            }

            if (e.kind == Event::kJump && e.target_units != ~uint64_t(0) && e.target_units < e.units)
            {
                if (++t.loops == 1)
                {
                    t.loop_start = e.target_units;
                    t.loop_end = e.units;
                }
            }
            else if (e.kind == Event::kTrackEnd)
            {
                t.ended = true;
                t.end = e.units;
            }
            else if (e.kind == Event::kTrackStart && e.value < uint32_t(kPlayerTracks))
            {
                tracks[e.value].started = true;
                tracks[e.value].restarted = true;
            }
        }

        // Once every track has looped or ended, the loop is known, and the sequence goes on until it has played enough
        // times. The tracks of a sequence may loop at different points and lengths, and each goes on looping in its
        // own way, so the loop starts where every looping track has started its loop, and where every track without a
        // loop has read its last command and released its notes, and lasts until every looping track is back where
        // its loop started, if that isn't too long (see LoopLength()).
        if (!planned)
        {
            planned = true;
            loop_start = 0;
            uint64_t last_command = 0;
            std::vector<uint64_t> lengths;
            for (const TrackPlan& t : tracks)
            {
                if (!t.started || t.restarted)
                {
                    continue;
                }

                planned = planned && (t.loops > 0 || t.ended);
                if (t.loops > 0)
                {
                    loop_start = std::max(loop_start, t.loop_start);
                    lengths.push_back(t.loop_end - t.loop_start);
                }
                else
                {
                    last_command = std::max(last_command, std::min(t.last_command, t.end));
                }
            }

            looping = planned && !lengths.empty();
            if (looping)
            {
                loop_start = std::max(loop_start, last_command);
                loop_length = LoopLength(lengths);
            }
        }
        if ((planned && (!looping || sim.frame_ticks[f] >= loop_start + uint64_t(loops) * loop_length)) || seq.Ended())
        {
            break;
        }
    }

    // The loop, and the end: the last pass through the loop, or the last track's end if the sequence doesn't loop. A
    // sequence that the model gave up on ends where it stopped.
    Plan& plan = sim.plan;
    if (looping)
    {
        plan.loops = true;
        plan.loop_start = loop_start;
        plan.loop_end = loop_start + loop_length;
        plan.end = std::min(loop_start + uint64_t(loops) * loop_length, sim.frame_ticks.back());
    }
    else
    {
        for (const TrackPlan& t : tracks)
        {
            if (t.ended)
            {
                plan.end = std::max(plan.end, t.end);
            }
        }
    }
    if (plan.end == 0)
    {
        plan.end = sim.frame_ticks.back();
    }

    sim.warnings = seq.Warnings();
    if (sim.frames.size() == kMaxFrames)
    {
        sim.warnings.push_back("the sequence was cut off after an hour");
    }
    for (const FrameData& d : sim.frames)
    {
        if (d.tempo < kMinMidiTempo)
        {
            sim.warnings.push_back("the tempo drops below " + std::to_string(kMinMidiTempo) +
                                   ", which the MIDI file plays at " + std::to_string(kMinMidiTempo));
            break;
        }
    }

    return sim;
}

// Returns the MIDI file's tempo changes before the end, as (MIDI tick, tempo).
std::vector<std::pair<uint64_t, int>> TempoMap(const Simulation& sim)
{
    std::vector<std::pair<uint64_t, int>> tempos;
    for (size_t f = 0; f < sim.frames.size() && (f == 0 || sim.frame_ticks[f] < sim.plan.end); f++)
    {
        const int tempo = std::max(sim.frames[f].tempo, kMinMidiTempo);
        if (tempos.empty() || tempos.back().second != tempo)
        {
            tempos.emplace_back(sim.frame_ticks[f], tempo);
        }
    }
    if (tempos.empty())
    {
        tempos.emplace_back(0, kStartTempo);
    }

    return tempos;
}

// Returns the seconds from the start to MIDI tick `tick`.
double TickSeconds(const Simulation& sim, uint64_t tick)
{
    const std::vector<uint64_t>& starts = sim.frame_ticks;
    const size_t f = size_t(std::upper_bound(starts.begin(), starts.end(), tick) - starts.begin()) - 1;
    const double into = f + 1 < starts.size() && starts[f + 1] > starts[f]
                            ? double(tick - starts[f]) / double(starts[f + 1] - starts[f])
                            : 0.0;
    return FrameSeconds(double(f) + into);
}

// Returns the microseconds of a quarter note at the driver's tempo `tempo`.
uint32_t QuarterMicros(int tempo)
{
    constexpr uint64_t kNumerator = uint64_t(kTicksPerQuarter) * kFrameCycles * 1000000;
    const uint64_t denominator = uint64_t(tempo) * kSecondCycles;
    return uint32_t((kNumerator + denominator / 2) / denominator);
}

// The sound a SoundFont zone plays for a key.
struct ZoneSound
{
    bool operator<(const ZoneSound& o) const
    {
        return std::tie(type, sound, envelope, release, root, coarse, pan) <
               std::tie(o.type, o.sound, o.envelope, o.release, o.root, o.coarse, o.pan);
    }

    bool operator==(const ZoneSound& o) const
    {
        return !(*this < o) && !(o < *this);
    }

    uint8_t type = kSampleType;
    uint32_t sound = 0;    // a sample's header, a wave's data, a square's duty or a noise's NR43
    uint32_t envelope = 0; // the envelope's points
    uint8_t release = 0;
    int root = 60;  // the key that plays the sound at its own pitch
    int coarse = 0; // semitones added to it
    int pan = 0;    // the SoundFont's pan, from -500 to 500
};

// A note as the MIDI file plays it.
struct Note
{
    int track = 0;
    int key = 60;
    int velocity = 127;
    int program = 0;
    uint64_t on = 0; // MIDI ticks
    uint64_t off = 0;
    uint64_t length = 0;
    uint32_t frame = 0; // the frame the driver starts it in
    bool open = true;
};

// An instrument of the driver's banks, as a program of the MIDI file.
struct Program
{
    uint16_t bank = 0;
    uint16_t instrument = 0;
    std::map<int, ZoneSound> keys; // the sound of each key its notes play
};

// The conversion of a sequence's notes into the MIDI file's, and of its instruments into programs.
class NoteMaker
{
public:
    NoteMaker(const Rom& rom, const DriverInfo& info, const Simulation& sim, uint16_t track_mask)
        : rom_(rom), info_(info), sim_(sim), mask_(track_mask)
    {
    }

    // Works out the notes, the end of each, and the programs they play.
    std::vector<Note> Make()
    {
        std::vector<Note> notes;
        std::map<uint32_t, size_t> by_id;
        std::array<uint32_t, kVoices> voice_note = {};
        const uint64_t end = sim_.plan.end;

        auto close = [&](uint32_t id, uint64_t tick)
        {
            const auto found = by_id.find(id);
            if (found == by_id.end() || !notes[found->second].open)
            {
                return;
            }

            // A note lasts at least a tick, so that its note off can't come before its note on.
            Note& n = notes[found->second];
            n.open = false;
            n.off = std::max(std::min(tick, end), n.on + 1);
        };

        for (const TimedEvent& te : sim_.events)
        {
            const Event& e = te.event;
            if (e.kind == Event::kNote && e.voice >= 0)
            {
                // A voice that plays a new note in legato ends its last one.
                close(voice_note[size_t(e.voice)], e.units);
                voice_note[size_t(e.voice)] = 0;
                if (e.velocity == 0 || e.track < 0 || !(mask_ >> e.track & 1) || e.units >= end)
                {
                    continue;
                }

                Note n;
                n.track = e.track;
                n.velocity = std::clamp(int(std::lround(127 * std::sqrt(e.velocity / 127.0))), 1, 127);
                n.on = e.units;
                n.length = e.length;
                n.frame = te.frame;
                n.program = ProgramOf(e, n.key);
                if (n.program < 0)
                {
                    continue;
                }

                by_id[e.id] = notes.size();
                notes.push_back(n);
                voice_note[size_t(e.voice)] = e.id;
            }
            else if (e.kind == Event::kRelease || e.kind == Event::kStop)
            {
                const auto found = by_id.find(e.id);
                if (found != by_id.end())
                {
                    close(e.id, OffTick(notes[found->second], e, te.frame));
                }
            }
        }

        for (size_t i = 0; i < notes.size(); i++)
        {
            if (notes[i].open)
            {
                notes[i].open = false;
                notes[i].off = std::max(end, notes[i].on + 1);
            }
        }

        return notes;
    }

    const std::vector<Program>& Programs() const
    {
        return programs_;
    }

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // Returns the tick at which a note's release or stop ends it in the MIDI file. A command's comes at its time. The
    // PSG voices' update comes at the start of its frame, and the mixer at the end, before the next frame: a release
    // there keeps to the note's length within the frame it starts, and a stop comes at that frame's start.
    uint64_t OffTick(const Note& n, const Event& e, uint32_t frame) const
    {
        if (e.phase == Event::kCommands)
        {
            return e.units;
        }

        const size_t g = std::min<size_t>(e.phase == Event::kMixer ? frame + 1 : frame, sim_.frame_ticks.size() - 1);
        const uint64_t hi = sim_.frame_ticks[g];
        const uint64_t lo = g > 0 ? std::min(sim_.frame_ticks[g - 1] + 1, hi) : 0;
        return e.kind == Event::kRelease ? std::clamp(n.on + n.length, lo, hi) : hi;
    }

    // Returns the program of a note's instrument, and sets the note's key: a sample voice's note, a square's or the
    // wave's pitch, or a noise voice's note. Returns -1 for a note that can't be converted.
    int ProgramOf(const Event& e, int& key)
    {
        const Lookup& lk = e.lookup;
        const uint32_t region = lk.region;
        const uint8_t tuning = region ? rom_.U8(region + 7) : uint8_t(kUnityIndex);
        const uint8_t note = lk.drum || lk.key_sample ? uint8_t(kUnityIndex) : e.key;
        const int raw = int16_t(uint16_t(note + kUnityIndex - tuning));
        const int index = std::clamp(raw, 0, kTopIndex);
        const bool fixed = lk.drum || lk.key_sample;

        ZoneSound z;
        z.type = e.type;
        z.envelope = lk.envelope;
        z.release = region ? rom_.U8(region + 6) : 0;
        if (lk.drum && lk.drum_pan != kCentre)
        {
            z.pan = std::clamp(int(std::lround((lk.drum_pan - kCentre) / 63.0 * 500)), -500, 500);
        }

        switch (e.type)
        {
        case kSampleType:
            z.sound = e.sample;
            key = e.key;
            if (fixed)
            {
                z.root = key;
                z.coarse = index - kUnityIndex;
            }
            else
            {
                z.root = std::clamp(int(tuning), 0, 127);
                z.coarse = z.root - tuning;
            }
            break;

        case kSquare1Type:
        case kSquare2Type:
            {
                // The duty the voice holds after its first frame.
                uint32_t duty = e.psg & 0xFF;
                if (region && (rom_.U8(region + 1) & 1))
                {
                    const uint32_t count = rom_.U16(e.psg);
                    duty = count > 1 ? rom_.U8(e.psg + 3) : rom_.U8(e.psg + 1 + count);
                }
                z.sound = duty & 3;
                key = index + 12;
                break;
            }

        case kWaveType:
            z.sound = e.psg;
            z.root = 48;
            key = index;
            break;

        case kNoiseType:
            {
                uint8_t nr43 = rom_.U8(info_.noise_table + uint32_t(std::min(index, kTopIndex - 1)));
                const bool table = region && (rom_.U8(region + 1) & 1);
                if ((table ? rom_.U8(e.psg + 2) : uint8_t(e.psg)) != 0)
                {
                    nr43 |= 8;
                }
                z.sound = nr43;
                key = e.key;
                z.root = key;
                break;
            }

        default:
            return -1;
        }
        if (key < 0 || key > 127)
        {
            warnings_.push_back("a note's key is outside MIDI's range, so it's left out");
            return -1;
        }

        // The program for the instrument, in the order the sequence first plays them.
        size_t p = 0;
        while (p < programs_.size() && (programs_[p].bank != e.bank || programs_[p].instrument != e.instrument))
        {
            p++;
        }
        if (p == programs_.size())
        {
            programs_.push_back({e.bank, e.instrument, {}});
            if (e.type == kSampleType && !fixed)
            {
                FillKeys(programs_.back());
            }
        }

        programs_[p].keys.emplace(key, z);

        return int(p);
    }

    // Gives a sample instrument without drums or a sample for each key a zone for every key, from its regions, so that
    // the SoundFont plays it on keys the sequence doesn't.
    void FillKeys(Program& program) const
    {
        for (int key = 0; key < 128; key++)
        {
            const Lookup lk = LookUpInstrument(rom_, info_, program.bank, program.instrument, uint8_t(key));
            if (!lk.found || !lk.region || lk.drum || lk.key_sample || rom_.U8(lk.region) != kSampleType)
            {
                continue;
            }

            const uint8_t tuning = rom_.U8(lk.region + 7);
            ZoneSound z;
            z.sound = SampleAddress(rom_, info_, program.bank, rom_.U16(lk.region + 2));
            z.envelope = lk.envelope;
            z.release = rom_.U8(lk.region + 6);
            z.root = std::clamp(int(tuning), 0, 127);
            z.coarse = z.root - tuning;
            program.keys.emplace(key, z);
        }
    }

    const Rom& rom_;
    const DriverInfo& info_;
    const Simulation& sim_;
    uint16_t mask_;
    std::vector<Program> programs_;
    std::vector<std::string> warnings_;
};

// Adds the SoundFont presets of a sequence's programs, in bank `bank` and those after it if there are more than 128.
// Returns the warnings for sounds that can't be read.
std::vector<std::string> AddPresets(const Rom& rom, SoundfontBuilder& sf, const std::vector<Program>& programs,
                                    int bank)
{
    std::vector<std::string> warnings;
    for (size_t p = 0; p < programs.size(); p++)
    {
        const Program& program = programs[p];

        // Keys in a row with the same sound share a zone.
        std::vector<Sf2Zone> zones;
        for (auto it = program.keys.begin(); it != program.keys.end();)
        {
            const int first = it->first;
            const ZoneSound z = it->second;
            int last = first;
            for (++it; it != program.keys.end() && it->first == last + 1 && it->second == z; ++it)
            {
                last = it->first;
            }

            int sample = -1;
            double share = 1.0;
            switch (z.type)
            {
            case kSampleType:
                sample = sf.GameSample(z.sound);
                break;

            case kSquare1Type:
            case kSquare2Type:
                sample = sf.SquareSample(int(z.sound));
                share = kPsgShare;
                break;

            case kWaveType:
                sample = sf.WaveSample(z.sound);
                share = kPsgShare;
                break;

            default:
                sample = sf.NoiseSample(uint8_t(z.sound));
                share = kPsgShare;
                break;
            }
            if (sample < 0)
            {
                char text[96];
                std::snprintf(text, sizeof text, "the sample at 0x%08X can't be read, so its notes are left out",
                              unsigned(z.sound));
                warnings.push_back(text);
                continue;
            }

            Sf2Zone zone;
            zone.gens.push_back(Sf2Gen::Range(sf2gen::kKeyRange, first, last));
            zone.gens.push_back(Sf2Gen::Value(sf2gen::kOverridingRootKey, z.root));
            if (z.coarse)
            {
                zone.gens.push_back(Sf2Gen::Value(sf2gen::kCoarseTune, z.coarse));
            }
            if (z.pan)
            {
                zone.gens.push_back(Sf2Gen::Value(sf2gen::kPan, z.pan));
            }
            zone.gens.push_back(Sf2Gen::Value(sf2gen::kSampleModes, sf.File().samples[size_t(sample)].loop ? 1 : 0));
            zone.gens.push_back(Sf2Gen::Value(sf2gen::kSampleId, sample));

            Sf2Envelope env = EnvelopeFor(rom, z.envelope, z.release,
                                          z.type == kSquare1Type || z.type == kSquare2Type || z.type == kNoiseType);
            env.attenuation = std::min(1440, env.attenuation + int(std::lround(-200.0 * std::log10(share))));
            AddEnvelope(zone, env);
            zones.push_back(zone);
        }

        char name[48];
        std::snprintf(name, sizeof name, "Bank %u instrument %u", unsigned(program.bank), unsigned(program.instrument));
        sf.AddPreset(name, bank + int(p) / 128, int(p) % 128, sf.AddInstrument(name, std::move(zones)));
    }

    return warnings;
}

// Returns the pitch bend value for `bend` semitones with a bend range of `range`.
int BendValue(double bend, int range)
{
    return std::clamp(8192 + int(std::lround(bend / range * 8192)), 0, 16383);
}

// Returns a track's levels on each side (full scale 128) for a voice at full velocity and envelope, from its pan and
// volume and the player's volume, as the driver works out a sample voice's.
std::pair<double, double> TrackLevels(int pan, int volume, int player_volume)
{
    const double level = 3.0 * 127 * (player_volume / 128.0) * (volume / 256.0) * (32767.0 / 32768.0);
    pan = std::clamp(pan, 0, 127);

    return {(127 - pan) * level / 256 * 127 / 128, pan * level / 256 * 127 / 128};
}

// A track's settings that the MIDI file's controllers follow, and the player's volume.
struct Levels
{
    int pan = kCentre;
    int volume = 0x80;
    int player_volume = 0x80;
};

// Returns each track's levels and the ticks they change at. They change when the track's pan or volume changes, when
// the player's volume does, and when F8 starts the track with another track's settings. Changes at the same tick are
// one change.
std::array<std::vector<std::pair<uint64_t, Levels>>, kPlayerTracks> TrackChanges(const Simulation& sim)
{
    std::array<std::vector<std::pair<uint64_t, Levels>>, kPlayerTracks> changes;
    std::array<Levels, kPlayerTracks> levels;
    for (const TimedEvent& te : sim.events)
    {
        const Event& e = te.event;
        if (e.track < 0 || e.track >= kPlayerTracks || e.units >= sim.plan.end)
        {
            continue;
        }

        // Each event changes the tracks it names; the player's volume changes them all.
        std::vector<int> changed;
        if (e.kind == Event::kPan || e.kind == Event::kTrackVolume)
        {
            int& setting = e.kind == Event::kPan ? levels[size_t(e.track)].pan : levels[size_t(e.track)].volume;
            setting = int(e.value);
            changed.push_back(e.track);
        }
        else if (e.kind == Event::kTrackStart && e.value < uint32_t(kPlayerTracks))
        {
            levels[e.value] = levels[size_t(e.track)];
            changed.push_back(int(e.value));
        }
        else if (e.kind == Event::kVolume)
        {
            for (int n = 0; n < kPlayerTracks; n++)
            {
                levels[size_t(n)].player_volume = int(e.value);
                changed.push_back(n);
            }
        }

        for (int n : changed)
        {
            auto& list = changes[size_t(n)];
            if (!list.empty() && list.back().first == e.units)
            {
                list.pop_back();
            }
            list.push_back({e.units, levels[size_t(n)]});
        }
    }

    return changes;
}

// Writes a track's levels: CC10 for pan and CC11 for loudness, at the start and whenever they change.
void WriteLevels(MidiTrack& mt, int c, const std::vector<std::pair<uint64_t, Levels>>& changes)
{
    int cc10 = -1, cc11 = -1;

    auto write = [&](uint64_t tick, const Levels& l)
    {
        const auto [left, right] = TrackLevels(l.pan, l.volume, l.player_volume);
        int n10 = cc10 < 0 ? 64 : cc10, n11 = cc11 < 0 ? 0 : cc11;
        LevelsToControllers(left, right, n10, n11);
        if (n10 != cc10)
        {
            mt.Control(uint32_t(tick), c, cc::kPan, cc10 = n10);
        }
        if (n11 != cc11)
        {
            mt.Control(uint32_t(tick), c, cc::kExpression, cc11 = n11);
        }
    };

    if (changes.empty() || changes[0].first != 0)
    {
        write(0, Levels());
    }
    for (const auto& [tick, levels] : changes)
    {
        write(tick, levels);
    }
}

// Writes a track's pitch bends: each frame's, at the frame's start, and each note's first frame's at its note on, so
// that the note starts with it.
void WriteBends(MidiTrack& mt, int c, const Simulation& sim, int n, const std::vector<const Note*>& notes, int range)
{
    std::vector<std::pair<uint64_t, double>> bends;
    for (size_t f = 0; f < sim.frames.size() && sim.frame_ticks[f] < sim.plan.end; f++)
    {
        if (!std::isnan(sim.frames[f].bend[size_t(n)]))
        {
            bends.push_back({sim.frame_ticks[f], sim.frames[f].bend[size_t(n)]});
        }
    }
    for (const Note* note : notes)
    {
        if (!std::isnan(sim.frames[note->frame].bend[size_t(n)]))
        {
            bends.push_back({note->on, sim.frames[note->frame].bend[size_t(n)]});
        }
    }
    std::stable_sort(bends.begin(), bends.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    int bend = 8192;
    for (const auto& [tick, semitones] : bends)
    {
        const int v = BendValue(semitones, range);
        if (v != bend)
        {
            mt.PitchBend(uint32_t(tick), c, bend = v);
        }
    }
}

// Writes a track's notes, each with its program, which the track starts with `program`. A MIDI channel can't play a key
// twice at once, so a note ends where the next one with its key starts, and two notes of a key that start together are
// one MIDI note, the first.
void WriteNotes(MidiTrack& mt, int c, const std::vector<const Note*>& notes, int bank, int program)
{
    std::vector<uint64_t> offs(notes.size());
    std::vector<bool> merged(notes.size());
    std::array<int, 128> held;
    held.fill(-1);
    for (size_t i = 0; i < notes.size(); i++)
    {
        offs[i] = notes[i]->off;
        int& before = held[size_t(notes[i]->key)];
        if (before >= 0 && notes[size_t(before)]->on == notes[i]->on)
        {
            merged[i] = true;
            offs[size_t(before)] = std::max(offs[size_t(before)], offs[i]);
            continue;
        }
        if (before >= 0 && offs[size_t(before)] > notes[i]->on)
        {
            offs[size_t(before)] = notes[i]->on;
        }
        before = int(i);
    }

    for (size_t i = 0; i < notes.size(); i++)
    {
        const Note* note = notes[i];
        if (merged[i])
        {
            continue;
        }

        if (note->program != program)
        {
            if (note->program / 128 != program / 128)
            {
                mt.Bank(uint32_t(note->on), c, bank + note->program / 128);
            }

            program = note->program;
            mt.Program(uint32_t(note->on), c, program % 128);
        }
        mt.NoteOn(uint32_t(note->on), c, note->key, note->velocity);
        mt.NoteOff(uint32_t(std::max(offs[i], note->on + 1)), c, note->key);
    }
}

// Writes the MIDI file: a conductor track, then a track for each of the player's tracks that plays notes.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const Simulation& sim,
               const std::vector<Note>& notes, int bank, std::string& error)
{
    constexpr uint16_t kDivision = kTicksPerQuarter;
    const Plan& plan = sim.plan;
    const uint32_t end = uint32_t(plan.end);
    MidiFile midi(kDivision);

    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    for (const auto& [tick, tempo] : TempoMap(sim))
    {
        conductor.Tempo(uint32_t(tick), QuarterMicros(tempo));
    }
    if (plan.loops)
    {
        conductor.Meta(uint32_t(plan.loop_start), 0x06, "loopStart");
        conductor.Meta(uint32_t(plan.loop_end), 0x06, "loopEnd");
    }
    conductor.SetEnd(end);

    const auto changes = TrackChanges(sim);
    for (int n = 0; n < kPlayerTracks; n++)
    {
        std::vector<const Note*> list;
        for (const Note& note : notes)
        {
            if (note.track == n)
            {
                list.push_back(&note);
            }
        }
        if (list.empty())
        {
            continue;
        }

        const int c = ChannelOf(n);
        MidiTrack& mt = midi.AddTrack();
        mt.Name("Track " + std::to_string(n));
        mt.SetEnd(end);

        // The pitch bend range covers the track's largest bend.
        double widest = 0;
        for (const FrameData& d : sim.frames)
        {
            if (!std::isnan(d.bend[size_t(n)]))
            {
                widest = std::max(widest, std::fabs(d.bend[size_t(n)]));
            }
        }
        const int range = widest > 0.005 ? std::clamp(int(std::ceil(widest - 1e-9)), 2, kMaxBendRange) : 0;

        const int program = list[0]->program;
        mt.Bank(0, c, bank + program / 128);
        mt.Program(0, c, program % 128);
        mt.Control(0, c, cc::kVolume, 127);
        mt.Control(0, c, cc::kReverb, 0);
        mt.Control(0, c, cc::kChorus, 0);
        if (range)
        {
            // RPN 0 sets the bend range; the null RPN then keeps later data entry from changing it.
            mt.Control(0, c, cc::kRpnMsb, 0);
            mt.Control(0, c, cc::kRpnLsb, 0);
            mt.Control(0, c, cc::kDataEntry, range);
            mt.Control(0, c, cc::kDataEntryLsb, 0);
            mt.Control(0, c, cc::kRpnMsb, 127);
            mt.Control(0, c, cc::kRpnLsb, 127);
            mt.PitchBend(0, c, 8192);
            WriteBends(mt, c, sim, n, list, range);
        }

        WriteLevels(mt, c, changes[size_t(n)]);
        WriteNotes(mt, c, list, bank, program);
    }

    return midi.Write(path, error);
}

// Converts a sequence, writing its files if `write` is set.
SongSummary Run(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt, SoundfontBuilder* shared,
                bool write)
{
    SongSummary sum;
    if (song < 0 || song >= int(info.sequence_addresses.size()))
    {
        sum.warnings.push_back("there's no such sequence");
        return sum;
    }

    const Simulation sim = Simulate(rom, info, song, opt.loops);
    sum.warnings = sim.warnings;

    NoteMaker maker(rom, info, sim, opt.track_mask);
    const std::vector<Note> notes = maker.Make();
    sum.warnings.insert(sum.warnings.end(), maker.Warnings().begin(), maker.Warnings().end());

    std::array<bool, kPlayerTracks> played = {};
    for (const Note& n : notes)
    {
        played[size_t(n.track)] = true;
    }
    for (bool p : played)
    {
        sum.tracks += p ? 1 : 0;
    }

    sum.seconds = TickSeconds(sim, sim.plan.end);
    if (sim.plan.loops)
    {
        sum.loop_start = TickSeconds(sim, sim.plan.loop_start);
        sum.loop_end = TickSeconds(sim, sim.plan.loop_end);
    }
    sum.bpm = 60.0 / FrameSeconds(double(kTicksPerQuarter) / TempoMap(sim).front().second);

    sum.silent = sum.tracks == 0;
    if (sum.silent || !write)
    {
        sum.ok = true;
        return sum;
    }

    // The SoundFont's presets, in the sequence's file or the shared one.
    SoundfontBuilder own(rom);
    SoundfontBuilder& sf = shared ? *shared : own;
    for (const std::string& w : AddPresets(rom, sf, maker.Programs(), opt.bank))
    {
        sum.warnings.push_back(w);
    }

    const std::string title = rom.Title() + " #" + TwoDigits(song);
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) sequence %d at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(info.sequence_addresses[size_t(song)]), kProgramName);
    const std::string stem =
        Utf8(PathFromUtf8(opt.out_dir) / PathFromUtf8(SafeFileName(opt.base_name) + "_" + TwoDigits(song)));
    std::string error;
    sum.midi_path = stem + ".mid";
    if (!WriteMidi(sum.midi_path, title, about, sim, notes, opt.bank, error))
    {
        sum.warnings.push_back(error);
        return sum;
    }

    if (!shared)
    {
        Sf2File& file = sf.File();
        file.name = title;
        file.comment = "Instruments for " + title + ", extracted by " + kProgramName;
        sum.sf2_path = stem + ".sf2";
        if (!file.Write(sum.sf2_path, error))
        {
            sum.warnings.push_back(error);
            return sum;
        }
    }

    sum.ok = true;

    return sum;
}

} // namespace

SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt)
{
    return Run(rom, info, song, opt, nullptr, false);
}

SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        SoundfontBuilder* shared)
{
    return Run(rom, info, song, opt, shared, true);
}

} // namespace supergbamidi::rd2
