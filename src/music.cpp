// SPDX-License-Identifier: MIT

#include "music.h"

#include <algorithm>
#include <numeric>
#include <utility>

#include "program.h"

namespace supergbamidi
{
namespace
{

using Open = std::unique_ptr<Music> (*)(const Rom&, const Overrides&, std::string&);

// The drivers in the order detection looks for them. Rare's, Quintet's and Nintendo R&D2's drivers and MP2K come first,
// since their detection uses code alone, where Konami's can fall back on a data scan, which might mistake another
// driver's data for a song table. R&D2's comes before MP2K, since a game can play most of its music with R&D2's driver
// and the rest with MP2K.
const struct
{
    Driver driver;
    Open open;
} kDrivers[] = {{Driver::kRare, rare::OpenMusic},
                {Driver::kQuintet, quintet::OpenMusic},
                {Driver::kRd2, rd2::OpenMusic},
                {Driver::kMp2k, mp2k::OpenMusic},
                {Driver::kKonami, konami::OpenMusic}};

} // namespace

std::vector<std::unique_ptr<Music>> OpenAllMusic(const Rom& rom, const Overrides& overrides)
{
    std::vector<std::unique_ptr<Music>> found;
    for (const auto& d : kDrivers)
    {
        if (overrides.driver != Driver::kAny && overrides.driver != d.driver)
        {
            continue;
        }

        std::string error;
        std::unique_ptr<Music> music = d.open(rom, overrides, error);
        if (music)
        {
            found.push_back(std::move(music));
        }
    }

    return found;
}

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    error.clear();
    for (const auto& d : kDrivers)
    {
        if (overrides.driver != Driver::kAny && overrides.driver != d.driver)
        {
            continue;
        }

        std::unique_ptr<Music> music = d.open(rom, overrides, error);
        if (music || !error.empty())
        {
            return music;
        }
    }

    // No driver was detected.
    const std::string kUnknown =
        std::string(": this game's music uses another engine, or a driver version ") + kProgramName + " doesn't know";
    switch (overrides.driver)
    {
    case Driver::kKonami:
        error = "no Konami sound driver found" + kUnknown;
        break;
    case Driver::kRare:
        error = "no Rare sound driver found (try --song-table)";
        break;
    case Driver::kQuintet:
        error = "no Quintet sound driver found (try --song-table)";
        break;
    case Driver::kRd2:
        error = "no Nintendo R&D2 sound driver found (try --song-table)";
        break;
    case Driver::kMp2k:
        error = "no MP2K sound driver found (try --song-table)";
        break;
    default:
        error = "no Konami, Rare, Quintet, Nintendo R&D2 or MP2K sound driver found" + kUnknown;
        break;
    }

    return nullptr;
}

uint64_t LoopLength(const std::vector<uint64_t>& lengths)
{
    const uint64_t longest = *std::max_element(lengths.begin(), lengths.end());
    const uint64_t most = longest * kMaxLoopFactor;
    uint64_t length = 1;
    for (uint64_t l : lengths)
    {
        const uint64_t times = length / std::gcd(length, l);
        if (times > most / l)
        {
            return longest;
        }

        length = times * l;
    }

    return length;
}

} // namespace supergbamidi
