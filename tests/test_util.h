// SPDX-License-Identifier: MIT

// Checks and helpers for the unit tests, which build small synthetic cartridges in memory, so no game ROM is needed.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace supergbamidi::test
{

// The checks made so far, and the ones that failed.
inline int g_checks = 0, g_failures = 0;

// Output folder for this test run.
inline std::filesystem::path g_temp;

// Returns the path of `name` in this run's temp folder.
std::filesystem::path TempPath(const std::string& name);

// Opens `name` in this run's temp folder for writing, in binary mode, or returns null if it can't be opened.
std::FILE* OpenTempFile(const std::string& name);

// Returns a file's bytes, or none if it can't be read.
std::vector<uint8_t> ReadAll(const std::string& path);

// Returns the number at `p`, in big-endian or little-endian byte order.
uint32_t Be32(const uint8_t* p);
uint16_t Le16(const uint8_t* p);
uint32_t Le32(const uint8_t* p);

// The events of a MIDI file's tracks, as (tick, bytes), with the file's division.
struct MidiEvents
{
    // Returns the events of all tracks whose first byte is `status` (with the channel in its low nibble for channel
    // messages), in order of tick.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> Find(uint8_t status) const
    {
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> out;
        for (const auto& t : tracks)
        {
            for (const auto& e : t)
            {
                if (e.second[0] == status)
                {
                    out.push_back(e);
                }
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        return out;
    }

    int division = 0;
    std::vector<std::vector<std::pair<uint32_t, std::vector<uint8_t>>>> tracks;
};

// Reads a MIDI file's events.
MidiEvents ReadMidi(const std::string& path);

// Returns true if the first track has a loopStart marker and, at the marker's tick and after it, a tempo change to the
// tempo in effect there.
bool RepeatsTempoAtLoopStart(const MidiEvents& midi);

// A SoundFont's records, as read back from a file.
struct Sf2Records
{
    // Returns record `index` of a chunk of records of `size` bytes.
    const uint8_t* Record(const std::string& chunk, size_t size, size_t index) const
    {
        return &chunks.at(chunk)[index * size];
    }

    // Returns the number of records of `size` bytes in a chunk, or 0 if the file has no such chunk.
    size_t Count(const std::string& chunk, size_t size) const
    {
        return chunks.count(chunk) ? chunks.at(chunk).size() / size : 0;
    }

    // Returns the largest size of any point of sample `sample`.
    int SamplePeak(size_t sample) const
    {
        const uint8_t* header = Record("shdr", 46, sample);
        const std::vector<uint8_t>& points = chunks.at("smpl");
        int peak = 0;
        for (uint32_t i = Le32(header + 20); i < Le32(header + 24); i++)
        {
            peak = std::max(peak, std::abs(int(int16_t(Le16(&points[2 * size_t(i)])))));
        }

        return peak;
    }

    // Returns the generators of instrument zone `zone`, as operator -> amount.
    std::map<uint16_t, uint16_t> ZoneGens(size_t zone) const
    {
        std::map<uint16_t, uint16_t> gens;
        const uint16_t first = Le16(Record("ibag", 4, zone)), last = Le16(Record("ibag", 4, zone + 1));
        for (uint16_t g = first; g < last; g++)
        {
            gens[Le16(Record("igen", 4, g))] = Le16(Record("igen", 4, g) + 2);
        }

        return gens;
    }

    std::map<std::string, std::vector<uint8_t>> chunks;
};

// Reads a SoundFont's records.
Sf2Records ReadSf2(const std::string& path);

} // namespace supergbamidi::test

// Each driver's tests, in konami_test.cpp, rare_test.cpp, quintet_test.cpp, mp2k_test.cpp, rd2_test.cpp,
// brownie_test.cpp, krawall_test.cpp and ubimilan_test.cpp.
namespace supergbamidi::konami
{

void RunTests();

} // namespace supergbamidi::konami

namespace supergbamidi::rare
{

void RunTests();

} // namespace supergbamidi::rare

namespace supergbamidi::quintet
{

void RunTests();

} // namespace supergbamidi::quintet

namespace supergbamidi::mp2k
{

void RunTests();

} // namespace supergbamidi::mp2k

namespace supergbamidi::rd2
{

void RunTests();

} // namespace supergbamidi::rd2

namespace supergbamidi::brownie
{

void RunTests();

} // namespace supergbamidi::brownie

namespace supergbamidi::krawall
{

void RunTests();

} // namespace supergbamidi::krawall

namespace supergbamidi::ubimilan
{

void RunTests();

} // namespace supergbamidi::ubimilan

#define SUPERGBAMIDI_CHECK(cond)                                                          \
    do                                                                                    \
    {                                                                                     \
        ::supergbamidi::test::g_checks++;                                                 \
        if (!(cond))                                                                      \
        {                                                                                 \
            ::supergbamidi::test::g_failures++;                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

#define SUPERGBAMIDI_CHECK_EQ(a, b)                                                                                   \
    do                                                                                                                \
    {                                                                                                                 \
        static_assert(!std::is_floating_point_v<std::decay_t<decltype(a)>> &&                                         \
                          !std::is_floating_point_v<std::decay_t<decltype(b)>>,                                       \
                      "CHECK_EQ compares whole numbers; use CHECK for floating-point values");                        \
        ::supergbamidi::test::g_checks++;                                                                             \
        const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                               \
        if (va != vb)                                                                                                 \
        {                                                                                                             \
            ::supergbamidi::test::g_failures++;                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, \
                         vb);                                                                                         \
        }                                                                                                             \
    } while (0)

// Like SUPERGBAMIDI_CHECK and SUPERGBAMIDI_CHECK_EQ, but a failure also ends the test. A test checks a count with them
// before it indexes what it counted. A wrong count then fails the check instead of reading past the end.
#define SUPERGBAMIDI_REQUIRE(cond)                                    \
    do                                                                \
    {                                                                 \
        const int failures_before = ::supergbamidi::test::g_failures; \
        SUPERGBAMIDI_CHECK(cond);                                     \
        if (::supergbamidi::test::g_failures != failures_before)      \
        {                                                             \
            return;                                                   \
        }                                                             \
    } while (0)

#define SUPERGBAMIDI_REQUIRE_EQ(a, b)                                 \
    do                                                                \
    {                                                                 \
        const int failures_before = ::supergbamidi::test::g_failures; \
        SUPERGBAMIDI_CHECK_EQ(a, b);                                  \
        if (::supergbamidi::test::g_failures != failures_before)      \
        {                                                             \
            return;                                                   \
        }                                                             \
    } while (0)
