// SPDX-License-Identifier: MIT

#include "midi.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "files.h"

namespace supergbamidi
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

// Appends `v` as a variable-length quantity: 7 bits a byte, the most significant first, with the top bit set on all but
// the last.
void PutVarLen(std::vector<uint8_t>& out, uint32_t v)
{
    uint8_t buf[5];
    int n = 0;
    buf[n++] = uint8_t(v & 0x7F);
    while (v >>= 7)
    {
        buf[n++] = uint8_t(0x80 | (v & 0x7F));
    }

    while (n)
    {
        out.push_back(buf[--n]);
    }
}

void Put32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8)
    {
        out.push_back(uint8_t(v >> s));
    }
}

void Put16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

uint8_t Clamp7(int v)
{
    return uint8_t(std::clamp(v, 0, 127));
}

} // namespace

void LevelsToControllers(double left, double right, int& cc10, int& cc11)
{
    const double gl = left / 128.0, gr = right / 128.0;
    const double amp = std::sqrt(gl * gl + gr * gr);
    if (amp <= 0)
    {
        cc11 = 0;
        return;
    }

    // Equal levels are centred. The formula puts them at exactly 63.5, so rounding them with it would depend on atan2()
    // returning exactly the double nearest pi/4.
    if (left == right)
    {
        cc10 = 64;
    }
    else
    {
        cc10 = std::clamp(int(std::lround(std::atan2(gr, gl) / (kPi / 2) * 127)), 0, 127);
    }

    const double kLoudestCentred = std::sqrt(2.0) * 127.0 / 128.0; // amplitude of the loudest centred voice
    cc11 = std::clamp(int(std::lround(127 * std::sqrt(amp / kLoudestCentred))), 0, 127);
}

void MidiTrack::AddEvent(uint32_t tick, int order, std::vector<uint8_t> bytes)
{
    events_.push_back({tick, order, std::move(bytes)});
    end_ = std::max(end_, tick);
}

void MidiTrack::Meta(uint32_t tick, uint8_t type, const std::string& text)
{
    std::vector<uint8_t> b = {0xFF, type};
    PutVarLen(b, uint32_t(text.size()));
    b.insert(b.end(), text.begin(), text.end());
    AddEvent(tick, kMeta, std::move(b));
}

void MidiTrack::Tempo(uint32_t tick, uint32_t micros_per_quarter)
{
    const uint32_t us = micros_per_quarter;
    AddEvent(tick, kMeta, {0xFF, 0x51, 0x03, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)});
}

void MidiTrack::TimeSignature(uint32_t tick, int numerator, int denominator_pow2)
{
    AddEvent(tick, kMeta, {0xFF, 0x58, 0x04, uint8_t(numerator), uint8_t(denominator_pow2), 24, 8});
}

void MidiTrack::NoteOn(uint32_t tick, int ch, int key, int velocity, bool before_programs)
{
    AddEvent(tick, before_programs ? kNoteOnFirst : kNoteOn,
             {uint8_t(0x90 | ch), Clamp7(key), std::max<uint8_t>(1, Clamp7(velocity))});
}

void MidiTrack::NoteOff(uint32_t tick, int ch, int key)
{
    AddEvent(tick, kNoteOff, {uint8_t(0x80 | ch), Clamp7(key), 0});
}

void MidiTrack::Control(uint32_t tick, int ch, int cc, int value)
{
    AddEvent(tick, kControl, {uint8_t(0xB0 | ch), uint8_t(cc & 0x7F), Clamp7(value)});
}

void MidiTrack::Program(uint32_t tick, int ch, int program)
{
    AddEvent(tick, kProgram, {uint8_t(0xC0 | ch), Clamp7(program)});
}

void MidiTrack::Bank(uint32_t tick, int ch, int bank)
{
    AddEvent(tick, kBank, {uint8_t(0xB0 | ch), cc::kBankSelect, uint8_t(bank & 0x7F)});
    AddEvent(tick, kBank, {uint8_t(0xB0 | ch), cc::kBankSelectLsb, 0});
}

void MidiTrack::PitchBend(uint32_t tick, int ch, int value)
{
    value = std::clamp(value, 0, 16383);
    AddEvent(tick, kBend, {uint8_t(0xE0 | ch), uint8_t(value & 0x7F), uint8_t(value >> 7)});
}

void MidiTrack::Retime(const std::function<uint32_t(uint32_t)>& place)
{
    for (Event& e : events_)
    {
        e.tick = place(e.tick);
    }
    end_ = place(end_);

    // Each note off ends the latest note on of its key on its channel, in the order they were added.
    std::map<std::pair<int, int>, size_t> sounding;
    std::vector<bool> left_out(events_.size());
    for (size_t i = 0; i < events_.size(); i++)
    {
        const std::vector<uint8_t>& b = events_[i].bytes;
        const int kind = b[0] & 0xF0;
        if (kind != 0x80 && kind != 0x90)
        {
            continue;
        }

        const std::pair<int, int> key = {b[0] & 0x0F, b[1]};
        if (kind == 0x90 && b[2])
        {
            sounding[key] = i;
            continue;
        }

        const auto it = sounding.find(key);
        if (it != sounding.end())
        {
            if (events_[it->second].tick == events_[i].tick)
            {
                left_out[it->second] = left_out[i] = true;
            }
            sounding.erase(it);
        }
    }

    size_t kept = 0;
    for (size_t i = 0; i < events_.size(); i++)
    {
        if (left_out[i])
        {
            continue;
        }
        if (kept != i)
        {
            events_[kept] = std::move(events_[i]);
        }
        kept++;
    }
    events_.resize(kept);
}

std::vector<uint8_t> MidiTrack::Encode() const
{
    std::vector<const Event*> sorted;
    sorted.reserve(events_.size());
    for (const Event& e : events_)
    {
        sorted.push_back(&e);
    }

    // Stable, so events of the same tick and order stay in the order they were added.
    auto by_tick_then_order = [](const Event* a, const Event* b)
    {
        return a->tick != b->tick ? a->tick < b->tick : a->order < b->order;
    };
    std::stable_sort(sorted.begin(), sorted.end(), by_tick_then_order);

    std::vector<uint8_t> out;
    uint32_t now = 0;
    for (const Event* e : sorted)
    {
        PutVarLen(out, e->tick - now);
        now = e->tick;
        out.insert(out.end(), e->bytes.begin(), e->bytes.end());
    }

    // The end-of-track event.
    PutVarLen(out, end_ - now);
    out.insert(out.end(), {0xFF, 0x2F, 0x00});

    return out;
}

bool MidiFile::Write(const std::string& path, std::string& error) const
{
    std::vector<uint8_t> out = {'M', 'T', 'h', 'd'};
    Put32(out, 6);
    Put16(out, 1);
    Put16(out, uint16_t(tracks_.size()));
    Put16(out, division_);

    for (const MidiTrack& t : tracks_)
    {
        const std::vector<uint8_t> body = t.Encode();
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        Put32(out, uint32_t(body.size()));
        out.insert(out.end(), body.begin(), body.end());
    }

    return WriteFile(path, out, error);
}

} // namespace supergbamidi
