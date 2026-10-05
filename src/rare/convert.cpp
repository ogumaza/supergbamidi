// SPDX-License-Identifier: MIT

#include "rare/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "files.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "rare/sequencer.h"
#include "rare/song.h"

namespace supergbamidi::rare
{
namespace
{

// Maximum conversion duration: one hour, measured in frames.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The MIDI channel that General MIDI players keep for drums, and the bank they look in for its programs.
constexpr int kDrumChannel = 9;
constexpr int kDrumBank = 128;

// A semitone in the model's pitch offsets, which are 32.32 fixed point.
constexpr int64_t kSemitone = int64_t(1) << 32;

// Maximum number of commands DumpSong() lists per track.
constexpr int kMaxListedCommands = 200000;

std::string TwoDigits(int n)
{
    char b[16];
    std::snprintf(b, sizeof b, "%02d", n);

    return b;
}

// The stretch of a tune that the conversion covers: its loop, and the tick at which the MIDI file ends.
struct Plan
{
    bool loops = false;
    uint64_t loop_start = 0; // the latest of the looping tracks' loop starts and the other tracks' last commands
    uint64_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the tracks' loops
    uint64_t end = 0;
};

// Plans a tune: a looping tune plays its loop `loops` times, and a tune without one goes on until its last track has
// ended. The tracks of a tune may loop at different points and lengths, and each goes on looping in its own way, so the
// loop starts where every looping track has started its loop, and where every track without a loop has played its
// last command other than a delay or the end, and lasts until every looping track is back where its loop started, if
// that isn't too long (see LoopLength()).
Plan PlanTune(const Rom& rom, const DriverInfo& info, const TuneHeader& header, int loops)
{
    Plan plan;
    uint64_t longest_end = 0;
    uint64_t last_command = 0;
    std::vector<uint64_t> lengths;
    for (uint32_t address : header.tracks)
    {
        const TrackLayout layout = ScanTrack(rom, address, info.format);
        if (layout.loops && layout.loop_end > layout.loop_start)
        {
            plan.loops = true;
            plan.loop_start = std::max(plan.loop_start, layout.loop_start);
            lengths.push_back(layout.loop_end - layout.loop_start);
        }
        else if (!layout.loops)
        {
            longest_end = std::max(longest_end, layout.end);
            last_command = std::max(last_command, layout.last_command);
        }
    }

    if (plan.loops)
    {
        const uint64_t length = LoopLength(lengths);
        plan.loop_start = std::max(plan.loop_start, last_command);
        plan.loop_end = plan.loop_start + length;
        plan.end = plan.loop_start + uint64_t(loops) * length;
    }
    else
    {
        plan.end = longest_end;
    }

    return plan;
}

// A clock that converts frame start times to ticks, following the tempo.
class TickClock
{
public:
    explicit TickClock(uint32_t ticks_per_quarter) : per_quarter_(ticks_per_quarter)
    {
    }

    // Returns the tick at which frame `frame` starts.
    uint64_t FrameTick(uint32_t frame) const
    {
        const int64_t elapsed = int64_t(frame) * kFrameTime - time_;
        if (tempo_ == 0 || elapsed <= 0)
        {
            return tick_;
        }

        return tick_ + uint64_t(elapsed) * per_quarter_ / (uint64_t(tempo_) * 256);
    }

    // Returns true once a tempo has been set.
    bool Known() const
    {
        return tempo_ != 0;
    }

    // Starts a tempo of `tempo` microseconds a quarter note at `tick`.
    void SetTempo(uint64_t tick, uint32_t tempo)
    {
        if (tempo_ != 0 && tick > tick_)
        {
            time_ += int64_t((tick - tick_) * tempo_ * 256 / per_quarter_);
        }

        tick_ = std::max(tick_, tick);
        tempo_ = tempo;
    }

private:
    uint32_t per_quarter_;
    uint64_t tick_ = 0; // the start of the tempo's segment
    int64_t time_ = 0;  // its time, in 1/256 microseconds
    uint32_t tempo_ = 0;
};

// A channel pitch-offset change and its tick.
struct PitchChange
{
    uint64_t tick;
    int channel;
    int64_t offset;
};

// Results of running a tune through the model: track actions, channel pitch changes and warnings.
struct Simulation
{
    std::vector<Action> actions;
    std::vector<PitchChange> pitches;
    std::vector<std::string> warnings;
    std::optional<uint64_t> cut_off; // the tick where the model gave up, at the frame limit
};

// Runs the model until the plan's end or until the driver has stopped the tune, for an hour at most.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, const TuneHeader& header, const Plan& plan)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    TickClock clock(header.ticks_per_quarter);
    std::array<int64_t, kChannels> last = {};
    uint32_t frames = 0;
    for (uint32_t f = 0; f < kMaxFrames; f++)
    {
        // A pitch change that a bend or program change makes, without vibrato, belongs at that command's tick. Vibrato
        // changes the pitch at the start of each frame.
        const uint64_t frame_tick = clock.FrameTick(f);
        std::array<int64_t, kChannels> command_tick;
        command_tick.fill(-1);
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kTempo)
            {
                clock.SetTempo(a.tick, a.value);
            }
            if (a.kind == Action::kBend || a.kind == Action::kProgram)
            {
                command_tick[a.channel] = int64_t(a.tick);
            }

            sim.actions.push_back(a);
        }

        for (int ch = 0; ch < kChannels; ch++)
        {
            const int64_t offset = seq.PitchOffset(ch);
            if (offset == last[size_t(ch)])
            {
                continue;
            }

            last[size_t(ch)] = offset;
            const bool vibrato = seq.Channel(ch).modulation != 0;
            const int64_t at = command_tick[size_t(ch)];
            sim.pitches.push_back({!vibrato && at >= 0 ? uint64_t(at) : frame_tick, ch, offset});
        }

        // Stop once every track has read up to the end, and the frames have reached it. A track counts the ticks of a
        // delay as it starts it, so its count reaches the end before the frames do.
        frames = f + 1;
        bool done = clock.Known() ? clock.FrameTick(f + 1) >= plan.end : true;
        for (int t = 0; t < seq.TrackCount() && done; t++)
        {
            done = seq.TrackDone(t) || seq.TrackTick(t) >= plan.end;
        }
        if (done || seq.Ended())
        {
            break;
        }
    }

    if (frames == kMaxFrames)
    {
        sim.warnings.push_back("the tune was cut off after an hour");
        sim.cut_off = clock.FrameTick(frames);
    }
    sim.warnings.insert(sim.warnings.end(), seq.Warnings().begin(), seq.Warnings().end());

    return sim;
}

// A note as the MIDI file plays it.
struct Note
{
    int track = 0;
    int channel = 0;
    int key = 0;
    int velocity = 0;
    int level = 0; // the velocity + 1 of each of the driver's voices that play it, added up
    int program = -1;
    uint32_t instrument = 0;
    uint64_t on = 0;
    uint64_t off = 0;
    uint32_t frame = 0; // the frame the driver starts it on
    bool open = true;
    int voices = 1;               // the driver's voices that play it
    bool before_programs = false; // it plays with the program from before its track's program changes on its tick
};

// Works out the MIDI file's notes from the model's note ons and offs. A MIDI channel can't play one key twice at once,
// so where the driver's channel does, the older note ends when the newer one starts, unless they start on the same
// frame. A note ends where the driver releases its slot, where a later note takes its slot, or at the end of the
// conversion.
std::vector<Note> CollectNotes(const Simulation& sim, const Plan& plan, uint16_t track_mask, int slot_count)
{
    std::vector<Note> notes;
    std::array<std::array<int, 128>, kChannels> owner;
    for (auto& keys : owner)
    {
        keys.fill(-1);
    }
    std::vector<int> slot_note(size_t(slot_count), -1), slot_level(size_t(slot_count), 0);

    // A track can play a note just before it gives the channel its first program, at the same tick. The game plays the
    // note with whatever instrument the channel had from the tune before, so the MIDI file gives it that program.
    std::map<std::pair<int, uint64_t>, const Action*> first_program;
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kProgram)
        {
            first_program.emplace(std::make_pair(int(a.channel), a.tick), &a);
        }
    }

    auto close = [&](int index, uint64_t tick)
    {
        // A note lasts at least a tick, so that its note off can't come before its note on.
        Note& n = notes[size_t(index)];
        if (n.open)
        {
            n.open = false;
            n.off = std::max(tick, n.on + 1);
            owner[size_t(n.channel)][size_t(n.key)] = -1;
        }
    };

    // Ends a slot's voice, ending the MIDI note when its last voice ends. A voice that ends in the note's first frame
    // sounds for at most two frames, so the note keeps the combined velocity of the remaining voices.
    auto release = [&](int slot, uint64_t tick, uint32_t frame)
    {
        const int index = slot_note[size_t(slot)];
        slot_note[size_t(slot)] = -1;
        if (index < 0)
        {
            return;
        }

        Note& n = notes[size_t(index)];
        if (--n.voices == 0)
        {
            close(index, tick);
        }
        else if (frame == n.frame)
        {
            n.level -= slot_level[size_t(slot)];
            n.velocity = std::min(127, n.level - 1);
        }
    };

    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            release(a.slot, a.tick, a.frame);

            const Action* program = &a;
            if (!a.playable && a.instrument == 0)
            {
                const auto found = first_program.find(std::make_pair(int(a.channel), a.tick));
                program = found != first_program.end() ? found->second : nullptr;
            }
            if ((!a.playable && program == &a) || !program || !(track_mask >> a.track & 1) || a.tick >= plan.end)
            {
                continue;
            }

            // A key played twice on the same frame is one MIDI note, as loud as the two voices together, since the
            // driver starts both on the frame.
            int& held = owner[a.channel][a.a & 0x7F];
            slot_level[size_t(a.slot)] = a.b + 1;
            if (held >= 0 && notes[size_t(held)].frame == a.frame)
            {
                Note& n = notes[size_t(held)];
                n.level += a.b + 1;
                n.velocity = std::min(127, n.level - 1);
                n.voices++;
                slot_note[size_t(a.slot)] = held;
                continue;
            }

            // A player takes a tick's events track by track. So where the older note is in a later track, it ends a
            // tick before the newer one starts, or else its note off would end the newer note too. If the older note
            // started a tick before, the newer one starts a tick late instead.
            uint64_t on = a.tick;
            if (held >= 0 && notes[size_t(held)].track > a.track)
            {
                on = std::max(on, notes[size_t(held)].on + 2);
                close(held, on - 1);
            }
            else if (held >= 0)
            {
                close(held, on);
            }

            Note n;
            n.track = a.track;
            n.channel = a.channel;
            n.key = a.a & 0x7F;
            n.velocity = a.b;
            n.level = a.b + 1;
            n.program = program == &a ? a.program : program->a;
            n.instrument = program->instrument;
            n.on = on;
            n.frame = a.frame;
            notes.push_back(n);
            held = int(notes.size()) - 1;
            slot_note[size_t(a.slot)] = held;
        }
        else if (a.kind == Action::kNoteOff)
        {
            // The release of an older note with the same key, which the MIDI file has ended already, leaves the newer
            // one playing. A note off that releases nothing ends a note that has ended by itself in the driver.
            const int held = owner[a.channel][a.a & 0x7F];
            if (held >= 0 && a.slot < 0)
            {
                close(held, std::min(a.tick, plan.end));
            }
            else if (held >= 0 && slot_note[size_t(a.slot)] == held)
            {
                release(a.slot, std::min(a.tick, plan.end), a.frame);
            }
        }
    }

    for (size_t i = 0; i < notes.size(); i++)
    {
        close(int(i), plan.end);
    }

    return notes;
}

// Returns the pitch bend range of each channel, in semitones: wide enough for its largest pitch offset, and its
// instruments' bend range, so that a bend without vibrato keeps its value.
std::array<int, kChannels> BendRanges(const Rom& rom, const Simulation& sim)
{
    std::array<int, kChannels> range = {};
    for (const PitchChange& p : sim.pitches)
    {
        const int64_t size = p.offset < 0 ? -p.offset : p.offset;
        range[size_t(p.channel)] = std::max(range[size_t(p.channel)], int((size + kSemitone - 1) / kSemitone));
    }

    // The bends' instrument ranges count for channels with any pitch change.
    std::array<uint32_t, kChannels> instrument = {};
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kProgram)
        {
            instrument[a.channel] = a.instrument;
        }
        if (a.kind == Action::kBend && range[a.channel] > 0)
        {
            Instrument inst;
            if (ReadInstrument(rom, instrument[a.channel], inst))
            {
                range[a.channel] = std::max(range[a.channel], std::clamp(int(inst.bend_range), 0, 127));
            }
        }
    }

    for (int& r : range)
    {
        r = std::min(r, 127);
    }

    return range;
}

// Returns the MIDI pitch bend value for a pitch offset, with a bend range of `range` semitones, at least 1.
int BendValue(int64_t offset, int range)
{
    const int64_t scaled = offset * 8192;
    const int64_t unit = int64_t(range) * kSemitone;
    const int64_t steps = scaled >= 0 ? (scaled + unit / 2) / unit : -((-scaled + unit / 2) / unit);
    return int(std::clamp<int64_t>(8192 + steps, 0, 16383));
}

// Returns the elapsed seconds at `tick`, using the GBA's frame rate and the tempo changes in `actions`.
double TickSeconds(const std::vector<Action>& actions, uint32_t per_quarter, uint64_t tick)
{
    // Add up the duration of each tempo segment that starts before `tick`.
    double seconds = 0;
    uint64_t at = 0;
    uint32_t tempo = 500000;
    for (const Action& a : actions)
    {
        if (a.kind != Action::kTempo || a.tick >= tick)
        {
            continue;
        }

        seconds += double(a.tick - at) * tempo / per_quarter / 1e6;
        at = a.tick;
        tempo = a.value;
    }

    // The final segment runs from the last tempo change to `tick`.
    seconds += double(tick - at) * tempo / per_quarter / 1e6;

    return seconds * kTempoScale;
}

// Fills in a summary's length, loop and tempo.
void Summarize(const Simulation& sim, const Plan& plan, uint32_t per_quarter, SongSummary& sum)
{
    sum.seconds = TickSeconds(sim.actions, per_quarter, plan.end);
    if (plan.loops)
    {
        sum.loop_start = TickSeconds(sim.actions, per_quarter, plan.loop_start);
        sum.loop_end = TickSeconds(sim.actions, per_quarter, plan.loop_end);
    }

    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kTempo && a.value)
        {
            sum.bpm = 60000000.0 / (a.value * kTempoScale);
            break;
        }
    }
}

// Writes a channel's settings at the start: the driver's volume (unless the tune sets one at the start), no pan,
// effects or expression, and the bend range.
void WriteChannelSetup(MidiTrack& mt, int ch, int range, int bank, bool volume_at_start)
{
    if (bank)
    {
        mt.Bank(0, ch, bank);
    }
    if (!volume_at_start)
    {
        mt.Control(0, ch, cc::kVolume, 127);
    }
    mt.Control(0, ch, cc::kPan, 64);
    mt.Control(0, ch, cc::kExpression, 127);
    mt.Control(0, ch, cc::kReverb, 0);
    mt.Control(0, ch, cc::kChorus, 0);
    if (range > 0)
    {
        // RPN 0 sets the bend range; the null RPN then keeps later data entry from changing it.
        mt.Control(0, ch, cc::kRpnMsb, 0);
        mt.Control(0, ch, cc::kRpnLsb, 0);
        mt.Control(0, ch, cc::kDataEntry, range);
        mt.Control(0, ch, cc::kDataEntryLsb, 0);
        mt.Control(0, ch, cc::kRpnMsb, 127);
        mt.Control(0, ch, cc::kRpnLsb, 127);
        mt.PitchBend(0, ch, 8192);
    }
}

// Adds a preset for each program the notes play, in the drum bank too for those on the drum channel. Returns the
// programs whose instruments have nothing to play.
std::set<int> AddPresets(SoundfontBuilder& sf, const std::vector<Note>& notes, int bank)
{
    std::set<int> missing;
    for (const Note& n : notes)
    {
        const int instrument = sf.InstrumentFor(n.instrument);
        if (instrument < 0)
        {
            missing.insert(n.program);
            continue;
        }

        sf.AddPreset(bank, n.program, instrument);
        if (n.channel == kDrumChannel && bank == 0)
        {
            sf.AddPreset(kDrumBank, n.program, instrument);
        }
    }

    return missing;
}

// A program change, and the track of the MIDI file it goes in, given by the tune's track number.
struct ProgramChange
{
    uint64_t tick;
    int track;
    int channel;
    int program;
};

// Makes the MIDI file play each note with the program that the driver plays it with. A player takes a tick's events
// track by track, and in a track program changes before note ons, where the driver runs each track's commands of a
// frame in turn. So where a track gives a channel a new program on the tick of one of its notes after the note, or a
// track before it in the file does on the same tick, the note would play with the new program. Such a note goes before
// its track's program changes on its tick, or gets a program change of its own.
void MatchPrograms(std::vector<Note>& notes, std::vector<ProgramChange>& programs)
{
    auto in_file_order = [](const ProgramChange& a, const ProgramChange& b)
    {
        return a.tick != b.tick ? a.tick < b.tick : a.track < b.track;
    };
    std::stable_sort(programs.begin(), programs.end(), in_file_order);

    std::vector<size_t> order(notes.size());
    for (size_t i = 0; i < order.size(); i++)
    {
        order[i] = i;
    }
    auto note_order = [&](size_t a, size_t b)
    {
        return notes[a].on != notes[b].on ? notes[a].on < notes[b].on : notes[a].track < notes[b].track;
    };
    std::stable_sort(order.begin(), order.end(), note_order);

    for (size_t i : order)
    {
        // The program the file gives the note, and the one it would give it before its track's changes on its tick.
        Note& n = notes[i];
        int before = -1, after = -1;
        for (const ProgramChange& p : programs)
        {
            if (p.tick > n.on || (p.tick == n.on && p.track > n.track))
            {
                break;
            }
            if (p.channel != n.channel)
            {
                continue;
            }

            after = p.program;
            if (p.tick < n.on || p.track < n.track)
            {
                before = p.program;
            }
        }

        if (n.program < 0 || n.program == after)
        {
            continue;
        }
        if (n.program == before)
        {
            n.before_programs = true;
            continue;
        }

        const ProgramChange change = {n.on, n.track, n.channel, n.program};
        programs.insert(std::upper_bound(programs.begin(), programs.end(), change, in_file_order), change);
    }
}

// Writes the MIDI file: a conductor track, then a track for each of the tune's tracks that plays notes.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const TuneHeader& header,
               const Simulation& sim, const Plan& plan, const std::vector<Note>& notes,
               const std::array<int, kChannels>& range, int bank, std::string& error)
{
    MidiFile midi(uint16_t(header.ticks_per_quarter));
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    if (plan.loops)
    {
        conductor.Meta(uint32_t(plan.loop_start), 0x06, "loopStart");
        conductor.Meta(uint32_t(plan.loop_end), 0x06, "loopEnd");
    }
    conductor.SetEnd(uint32_t(plan.end));

    // A track for each of the tune's tracks with notes, in order. A channel's events go in the first one that plays it.
    std::vector<bool> plays(header.track_count, false);
    std::array<int, kChannels> first = {};
    first.fill(int(header.track_count));
    for (const Note& n : notes)
    {
        plays[size_t(n.track)] = true;
        first[size_t(n.channel)] = std::min(first[size_t(n.channel)], n.track);
    }

    std::vector<MidiTrack*> track(header.track_count, nullptr);
    for (uint32_t t = 0; t < header.track_count; t++)
    {
        if (plays[t])
        {
            track[t] = &midi.AddTrack();
            track[t]->Name("Track " + std::to_string(t));
            track[t]->SetEnd(uint32_t(plan.end));
        }
    }

    std::array<MidiTrack*, kChannels> home = {};
    for (int ch = 0; ch < kChannels; ch++)
    {
        home[size_t(ch)] = first[size_t(ch)] < int(header.track_count) ? track[size_t(first[size_t(ch)])] : nullptr;
    }

    // The tempo goes in the conductor track, at the GBA's speed. Program changes and volume go in the track that gave
    // them, if it's in the file, or else in the channel's track.
    std::array<bool, kChannels> volume_at_start = {};
    std::vector<ProgramChange> programs;
    for (const Action& a : sim.actions)
    {
        if (a.tick > plan.end || (a.tick == plan.end && a.kind != Action::kTempo))
        {
            continue;
        }

        const int destination = track[a.track] ? a.track : first[a.channel];
        MidiTrack* mt = destination < int(header.track_count) ? track[size_t(destination)] : nullptr;
        switch (a.kind)
        {
        case Action::kTempo:
            conductor.Tempo(uint32_t(a.tick), uint32_t(std::lround(a.value * kTempoScale)));
            break;

        case Action::kProgram:
            if (mt)
            {
                programs.push_back({a.tick, destination, a.channel, a.a});
            }
            break;

        case Action::kVolume:
            if (mt)
            {
                mt->Control(uint32_t(a.tick), a.channel, cc::kVolume, a.a);
                volume_at_start[a.channel] = volume_at_start[a.channel] || a.tick == 0;
            }
            break;

        default:
            break;
        }
    }

    std::vector<Note> ordered = notes;
    MatchPrograms(ordered, programs);
    for (const ProgramChange& p : programs)
    {
        track[size_t(p.track)]->Program(uint32_t(p.tick), p.channel, p.program);
    }

    for (int ch = 0; ch < kChannels; ch++)
    {
        if (home[size_t(ch)])
        {
            WriteChannelSetup(*home[size_t(ch)], ch, range[size_t(ch)], bank, volume_at_start[size_t(ch)]);
        }
    }

    for (const PitchChange& p : sim.pitches)
    {
        if (home[size_t(p.channel)] && range[size_t(p.channel)] > 0 && p.tick < plan.end)
        {
            home[size_t(p.channel)]->PitchBend(uint32_t(p.tick), p.channel,
                                               BendValue(p.offset, range[size_t(p.channel)]));
        }
    }

    for (const Note& n : ordered)
    {
        MidiTrack& mt = *track[size_t(n.track)];
        mt.NoteOn(uint32_t(n.on), n.channel, n.key, n.velocity, n.before_programs);
        mt.NoteOff(uint32_t(n.off), n.channel, n.key);
    }

    return midi.Write(path, error);
}

// Converts a tune, writing its files if `write` is set.
SongSummary Run(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt, SoundfontBuilder* shared,
                bool write)
{
    SongSummary sum;
    TuneHeader header;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), header))
    {
        sum.warnings.push_back("its header can't be read");
        return sum;
    }

    Plan plan = PlanTune(rom, info, header, opt.loops);
    const Simulation sim = Simulate(rom, info, song, header, plan);

    // A tune that the model gave up on ends where it stopped.
    if (sim.cut_off)
    {
        plan.end = std::min(plan.end, *sim.cut_off);
    }

    sum.warnings = sim.warnings;
    const std::vector<Note> notes = CollectNotes(sim, plan, opt.track_mask, kChannels * info.slots_per_channel);
    Summarize(sim, plan, header.ticks_per_quarter, sum);

    std::set<int> tracks;
    for (const Note& n : notes)
    {
        tracks.insert(n.track);
    }

    int dropped = 0;
    for (const Action& a : sim.actions)
    {
        dropped += a.kind == Action::kNoteDropped && a.tick < plan.end ? 1 : 0;
    }
    if (dropped)
    {
        sum.warnings.push_back(
            std::to_string(dropped) + " note" + (dropped == 1 ? "" : "s") +
            " found no available slot on their channel; omitted from the MIDI file to match the game");
    }

    sum.tracks = int(tracks.size());
    sum.silent = notes.empty();
    if (sum.silent || !write)
    {
        sum.ok = true;
        return sum;
    }

    // The SoundFont's presets, in the song's file or the shared one.
    SoundfontBuilder own(rom, info);
    SoundfontBuilder& sf = shared ? *shared : own;
    for (int program : AddPresets(sf, notes, opt.bank))
    {
        sum.warnings.push_back("program " + std::to_string(program) + "'s instrument has no samples to play");
    }

    const std::string title = rom.Title() + " #" + TwoDigits(song);
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) tune %d, header at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(header.address), kProgramName);
    const std::string stem =
        Utf8(PathFromUtf8(opt.out_dir) / PathFromUtf8(SafeFileName(opt.base_name) + "_" + TwoDigits(song)));
    std::string error;
    sum.midi_path = stem + ".mid";
    if (!WriteMidi(sum.midi_path, title, about, header, sim, plan, notes, BendRanges(rom, sim), opt.bank, error))
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

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    TuneHeader h;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), h))
    {
        error = "invalid tune";
        return false;
    }

    std::string text;
    char line[256];
    std::snprintf(line, sizeof line,
                  "; %s (%s) tune %d\n; header 0x%08X: %u tracks, %u ticks a quarter note, program map 0x%08X, "
                  "instruments 0x%08X\n; columns: address, tick, bytes, command\n",
                  rom.Title().c_str(), rom.GameCode().c_str(), song, unsigned(h.address), unsigned(h.track_count),
                  unsigned(h.ticks_per_quarter), unsigned(h.program_map), unsigned(h.instruments));
    text += line;

    // Each track's commands, up to its end or the end of its first loop.
    for (uint32_t t = 0; t < h.track_count; t++)
    {
        std::snprintf(line, sizeof line, "\n; ---- track %u at 0x%08X ----\n", unsigned(t), unsigned(h.tracks[t]));
        text += line;

        uint32_t address = h.tracks[t];
        uint64_t tick = 0;
        for (int n = 0; n < kMaxListedCommands; n++)
        {
            Event e;
            if (!DecodeEvent(rom, address, info.format, e))
            {
                std::snprintf(line, sizeof line, "0x%08X %8llu  %02X  (unknown command; the track stops here)\n",
                              unsigned(address), static_cast<unsigned long long>(tick), rom.U8(address));
                text += line;
                break;
            }

            std::string bytes;
            for (uint32_t i = 0; i < e.size; i++)
            {
                char b[4];
                std::snprintf(b, sizeof b, "%02X ", rom.U8(address + i));
                bytes += b;
            }
            std::snprintf(line, sizeof line, "0x%08X %8llu  %-15s %s\n", unsigned(address),
                          static_cast<unsigned long long>(tick), bytes.c_str(), DescribeEvent(e).c_str());
            text += line;

            address += e.size;
            if (e.command == kCmdDelay1 || e.command == kCmdDelay2 || e.command == kCmdDelay3)
            {
                tick += e.value;
            }
            if (e.command == kCmdEnd || (e.command == kCmdControl && e.a == kCtrlLoopEnd))
            {
                break;
            }
        }
    }

    std::vector<uint8_t> data(text.begin(), text.end());
    return WriteFile(path, data, error);
}

} // namespace supergbamidi::rare
