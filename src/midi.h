// SPDX-License-Identifier: MIT

// Standard MIDI File (format 1) writer.

#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace supergbamidi
{

// MIDI controller numbers.
namespace cc
{

constexpr int kBankSelect = 0;
constexpr int kDataEntry = 6;
constexpr int kVolume = 7;
constexpr int kPan = 10;
constexpr int kExpression = 11;
constexpr int kBankSelectLsb = 32;
constexpr int kDataEntryLsb = 38;
constexpr int kReverb = 91;
constexpr int kChorus = 93;
constexpr int kRpnLsb = 100;
constexpr int kRpnMsb = 101;
constexpr int kAllSoundOff = 120;

} // namespace cc

// Sets CC10 and CC11 for the levels `left` and `right` (full scale 128). CC11 carries the loudness: under the SF2
// default modulator a controller value c scales amplitude by (c/127)^2. CC10 carries the ratio between the sides, for a
// constant-power pan law. When both levels are 0, only CC11 changes.
void LevelsToControllers(double left, double right, int& cc10, int& cc11);

// A track of a Standard MIDI File. Events can be added in any order: they're written by tick, and at the same tick meta
// events come first, then note-offs, bank selects, program changes, controllers, pitch bends and note-ons, each kind in
// the order it was added.
class MidiTrack
{
public:
    void Meta(uint32_t tick, uint8_t type, const std::string& text);

    void Name(const std::string& text)
    {
        Meta(0, 0x03, text);
    }

    void Tempo(uint32_t tick, uint32_t micros_per_quarter);
    void TimeSignature(uint32_t tick, int numerator, int denominator_pow2);

    // Adds a note on. With `before_programs`, it goes before the bank selects and program changes at the same tick, so
    // that it plays with the program from before them.
    void NoteOn(uint32_t tick, int ch, int key, int velocity, bool before_programs = false);

    void NoteOff(uint32_t tick, int ch, int key);
    void Control(uint32_t tick, int ch, int cc, int value);
    void Program(uint32_t tick, int ch, int program);

    // Adds a bank select (CC0 + CC32), which goes before program changes at the same tick.
    void Bank(uint32_t tick, int ch, int bank);

    // Adds a pitch bend of `value`, from 0 to 16383 with the centre at 8192.
    void PitchBend(uint32_t tick, int ch, int value);

    // Makes the track last at least until `tick`.
    void SetEnd(uint32_t tick)
    {
        end_ = std::max(end_, tick);
    }

    // Moves each event, and the track's end, to the tick that `place` gives for its own tick, which mustn't put any two
    // ticks out of order. A note whose note on and note off then come on the same tick no longer plays, so it's left
    // out.
    void Retime(const std::function<uint32_t(uint32_t)>& place);

    // Returns the track's events as the data of an MTrk chunk.
    std::vector<uint8_t> Encode() const;

private:
    // The order of events at the same tick.
    enum Order : int
    {
        kMeta = 0,
        kNoteOff = 10,
        kNoteOnFirst = 12,
        kBank = 15,
        kProgram = 20,
        kControl = 30,
        kBend = 40,
        kNoteOn = 50
    };

    struct Event
    {
        uint32_t tick;
        int order;
        std::vector<uint8_t> bytes;
    };

    void AddEvent(uint32_t tick, int order, std::vector<uint8_t> bytes);

    std::vector<Event> events_;
    uint32_t end_ = 0;
};

// A Standard MIDI File of format 1: tracks played together, timed in ticks of 1/`division` of a quarter note.
class MidiFile
{
public:
    explicit MidiFile(uint16_t division) : division_(division)
    {
    }

    MidiTrack& AddTrack()
    {
        tracks_.emplace_back();
        return tracks_.back();
    }

    // Sets the ticks in a quarter note.
    void SetDivision(uint16_t division)
    {
        division_ = division;
    }

    // Moves the events of every track, as MidiTrack::Retime() does.
    void Retime(const std::function<uint32_t(uint32_t)>& place)
    {
        for (MidiTrack& track : tracks_)
        {
            track.Retime(place);
        }
    }

    // Writes the file. Returns false and sets `error` if it can't be written.
    bool Write(const std::string& path, std::string& error) const;

private:
    uint16_t division_;
    std::deque<MidiTrack> tracks_; // deque: AddTrack() references stay valid
};

} // namespace supergbamidi
