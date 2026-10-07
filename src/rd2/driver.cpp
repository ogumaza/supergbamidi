// SPDX-License-Identifier: MIT

#include "rd2/driver.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "rd2/sequencer.h"
#include "thumb.h"

namespace supergbamidi::rd2
{
namespace
{

// The most sequences detection reads, so that unrelated ROM data can't keep it busy.
constexpr int kMaxSequences = 1024;

// The start of the driver's init routine, which stores the game's settings and sets up the sound hardware.
constexpr const char* kInitPattern = "B530 49xx 6008 49xx 2000 7008 2080 7008 3904 4Axx 1C10 8008 3102 200D 7008";

// The code that loads each of the driver's tables, and the ldr's index among its halfwords.
struct TablePattern
{
    const char* pattern;
    int index;
};

// A revision's code that loads the tables whose code differs between the revisions, which the games' compilers built
// with different prologues, registers and literal pools: the routine that works out a voice's pitch from its note,
// which loads the sample pitch table and then, at `frequency_index`, the frequency table; the routine that looks up a
// noise voice's NR43; the LFO's step in the routine that works out a voice's pitch for a frame; and the note on, which
// picks the voices that can play a region by its type.
struct RevisionPatterns
{
    TablePattern pitch_table;
    int frequency_index;
    TablePattern noise_table;
    TablePattern lfo_table;
    TablePattern voice_classes;
};

constexpr RevisionPatterns kLinkToThePastPatterns = {
    {"B500 0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 D107 48xx", 20},
    30,
    {"B500 0400 0C01 2977 D900 2177 48xx 1808 7800", 6},
    {"68A0 6883 2B00 D0xx 6860 2800 D1xx 48xx 6A29 0849 1809 7809", 7},
    {"49xx 7830 1840 7800 1C29 3152 7809", 0},
};

constexpr RevisionPatterns kSuperMarioAdvance2Patterns = {
    {"0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 D108 48xx", 19},
    30,
    {"0400 0C01 2977 D900 2177 48xx 1808 7800", 5},
    {"68A0 6882 2A00 D0xx 6860 2800 D1xx 48xx 6A29 0849 1809 7809", 7},
    {"49xx 7830 1840 7800 1C29 3152 780A 1C29", 0},
};

// The start of the routine that works out a sample voice's level in the Super Mario Advance 2 revision, which reads
// the velocity from the voice's byte 9 and shifts it left by 8: ldrb r4, [r5, #9]; lsls r4, r4, #8. A Link to the
// Past's reads byte 10 and shifts it by 7.
constexpr const char* kSuperMarioAdvance2Level = "B530 1C05 7868 2801 D1xx 7A6C 0224";

// The wave voice's fade after its release.
constexpr TablePattern kWaveVolumes = {"2904 D900 2104 0609 0E09 4Axx 48xx 1809 7808 7010", 6};

// The instrument lookup, for instruments with a sample for each key.
constexpr TablePattern kKeyEnvelope = {"8869 1859 4Axx 0070 1840 8800 8050 6022 48xx 6060", 8};

// Returns the literal that the ldr of the first match of a table's pattern loads, or 0 if there's no match.
uint32_t FindTable(const Rom& rom, const TablePattern& table, int index = -1)
{
    const std::vector<uint32_t> hits = FindThumb(rom, ParseThumbPattern(table.pattern));
    return hits.empty() ? 0 : ThumbLiteral(rom, hits[0] + 2 * uint32_t(index < 0 ? table.index : index));
}

// Returns the game's settings from its call to the driver's init: the ldr r0 just before the bl.
uint32_t FindSettings(const Rom& rom, uint32_t init)
{
    for (uint32_t call : FindCalls(rom, init))
    {
        if ((rom.U16(call - 2) & 0xFF00) == 0x4800)
        {
            const uint32_t settings = ThumbLiteral(rom, call - 2);
            if (rom.Contains(settings, kSettingsSize))
            {
                return settings;
            }
        }
    }

    return 0;
}

// Returns the address of entry `index` of a table of offsets from its own start, or 0 if it's outside the ROM.
uint32_t Entry(const Rom& rom, uint32_t table, uint32_t index)
{
    const uint32_t at = table + 4 * index;
    if (!rom.Contains(at, 4))
    {
        return 0;
    }

    const uint32_t address = table + rom.U32(at);
    return rom.Contains(address) ? address : 0;
}

// Returns the number of entries in a table of offsets, from where its first entry points: just past the table.
uint32_t EntryCount(const Rom& rom, uint32_t table)
{
    const uint32_t first = rom.U32(table);
    return first % 4 == 0 && rom.Contains(table, first) ? first / 4 : 0;
}

// Returns true if the sequence at `at` has a header the driver can read: 1 to 10 tracks, which start inside the ROM.
bool PlausibleSequence(const Rom& rom, uint32_t at)
{
    const int tracks = rom.S8(at);
    if (tracks < 1 || tracks > kPlayerTracks || !rom.Contains(at, 2 + 2 * uint32_t(tracks)))
    {
        return false;
    }

    for (int t = 0; t < tracks; t++)
    {
        const uint32_t offset = rom.U16(at + 2 + 2 * uint32_t(t));
        if (offset != 0 && (offset < 2 + 2 * uint32_t(tracks) || !rom.Contains(at + offset)))
        {
            return false;
        }
    }

    return true;
}

} // namespace

const char* RevisionName(Revision r)
{
    return r == Revision::kSuperMarioAdvance2 ? "Super Mario Advance 2" : "A Link to the Past";
}

std::string Hex(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", unsigned(v));

    return b;
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    error.clear();
    const std::vector<uint32_t> inits = FindThumb(rom, ParseThumbPattern(kInitPattern));
    info.init = inits.empty() ? 0 : inits[0];
    if (!info.init && !overrides.settings)
    {
        return false;
    }

    // The game passes the driver its settings when it starts the sound.
    info.settings = overrides.settings ? overrides.settings : FindSettings(rom, info.init);
    if (!info.settings || !rom.Contains(info.settings, kSettingsSize))
    {
        error = "found Nintendo R&D2's sound driver, but not the settings the game gives it";
        return false;
    }

    const uint32_t s = info.settings;
    info.sample_sets = rom.U32(s);
    info.banks = rom.U32(s + 4);
    info.sequences = rom.U32(s + 8);
    info.effects = rom.U32(s + 12);
    info.bank_sample_sets = rom.U32(s + 16);
    info.bank_lists = rom.U32(s + 20);
    for (uint32_t table : {info.sample_sets, info.banks, info.sequences, info.bank_sample_sets, info.bank_lists})
    {
        if (!rom.Contains(table, 4))
        {
            error = "the driver's settings at " + Hex(s) + " don't point to its tables";
            return false;
        }
    }

    // The driver's tables come from the code that reads them, which the revisions' compilers built differently.
    const bool mario = !FindThumb(rom, ParseThumbPattern(kSuperMarioAdvance2Level)).empty();
    info.revision = mario ? Revision::kSuperMarioAdvance2 : Revision::kLinkToThePast;
    const RevisionPatterns& patterns = mario ? kSuperMarioAdvance2Patterns : kLinkToThePastPatterns;
    info.pitch_table = FindTable(rom, patterns.pitch_table);
    info.frequency_table = FindTable(rom, patterns.pitch_table, patterns.frequency_index);
    info.noise_table = FindTable(rom, patterns.noise_table);
    info.lfo_table = FindTable(rom, patterns.lfo_table);
    info.wave_volumes = FindTable(rom, kWaveVolumes);
    info.voice_classes = FindTable(rom, patterns.voice_classes);
    info.key_envelope = FindTable(rom, kKeyEnvelope);
    const struct
    {
        uint32_t address;
        uint32_t size;
        const char* name;
    } tables[] = {
        {info.pitch_table, 480, "pitch table"},
        {info.frequency_table, 240, "frequency table"},
        {info.noise_table, 120, "noise table"},
        {info.lfo_table, 256, "LFO table"},
        {info.wave_volumes, 5, "wave volume table"},
        {info.voice_classes, 5, "voice class table"},
        {info.key_envelope, 4, "envelope for instruments with a sample for each key"},
    };
    for (const auto& t : tables)
    {
        if (!rom.Contains(t.address, t.size))
        {
            error = std::string("found Nintendo R&D2's sound driver, but not its ") + t.name;
            return false;
        }
    }

    // Each sequence has a list of banks. The table of sequences ends where its first sequence starts, and so does the
    // table of bank lists, so the shorter gives the count.
    uint32_t count = std::min(EntryCount(rom, info.sequences), EntryCount(rom, info.bank_lists));
    count = std::min<uint32_t>(count, kMaxSequences);
    if (overrides.sequence_count > 0)
    {
        count = std::min<uint32_t>(uint32_t(overrides.sequence_count), kMaxSequences);
    }
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t sequence = Entry(rom, info.sequences, i);
        const uint32_t banks = Entry(rom, info.bank_lists, i);
        if (!sequence || !banks)
        {
            break;
        }

        info.sequence_addresses.push_back(sequence);
        info.bank_list_addresses.push_back(banks);
    }
    if (info.sequence_addresses.empty())
    {
        error = "found Nintendo R&D2's sound driver, but its sequence table at " + Hex(info.sequences) + " is empty";
        return false;
    }

    int unreadable = 0;
    for (uint32_t sequence : info.sequence_addresses)
    {
        unreadable += PlausibleSequence(rom, sequence) ? 0 : 1;
    }
    if (unreadable > 0)
    {
        info.warnings.push_back(std::to_string(unreadable) + " of the " +
                                std::to_string(info.sequence_addresses.size()) +
                                " sequences have no tracks the driver can read");
    }

    info.log.push_back(std::string(RevisionName(info.revision)) +
                       " revision of the driver, init routine: " + Hex(info.init));
    info.log.push_back("the game's settings: " + Hex(info.settings));
    info.log.push_back("sequence table: " + Hex(info.sequences) + " (" +
                       std::to_string(info.sequence_addresses.size()) + " sequences)");
    info.log.push_back("bank lists: " + Hex(info.bank_lists) + ", banks: " + Hex(info.banks) + " (" +
                       std::to_string(EntryCount(rom, info.banks)) + ")");
    info.log.push_back("sample sets: " + Hex(info.sample_sets) + " (" +
                       std::to_string(EntryCount(rom, info.sample_sets)) +
                       "), each bank's set: " + Hex(info.bank_sample_sets));
    if (rom.Contains(info.effects, 4))
    {
        info.log.push_back("sound effects: " + Hex(info.effects) + " (" +
                           std::to_string(EntryCount(rom, info.effects)) + " sequences, not converted)");
    }
    info.log.push_back("pitch table: " + Hex(info.pitch_table) + ", frequency table: " + Hex(info.frequency_table) +
                       ", noise table: " + Hex(info.noise_table) + ", LFO table: " + Hex(info.lfo_table));

    return true;
}

} // namespace supergbamidi::rd2
