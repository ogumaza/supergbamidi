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

// The drivers in the order detection looks for them. Rare's, Quintet's, Nintendo R&D2's and Brownie Brown's drivers and
// MP2K come first, since their detection uses code alone, where Konami's can fall back on a data scan, which might
// mistake another driver's data for a song table. R&D2's comes before MP2K, since a game can play most of its music
// with R&D2's driver and the rest with MP2K.
const struct
{
    Driver driver;
    Open open;
} kDrivers[] = {
    {Driver::kRare, rare::OpenMusic}, {Driver::kQuintet, quintet::OpenMusic}, {Driver::kRd2, rd2::OpenMusic},
    {Driver::kMp2k, mp2k::OpenMusic}, {Driver::kBrownie, brownie::OpenMusic}, {Driver::kKonami, konami::OpenMusic},
};

// Returns the error for a game in which detection finds none of the drivers that `driver` allows. Rare's driver and
// MP2K can read the songs that --song-table gives without the driver's code, so their errors suggest it. Quintet's,
// Nintendo R&D2's and Brownie Brown's drivers need their code to find their other tables.
std::string NotFound(Driver driver)
{
    const std::string kUnknown =
        std::string(": this game's music uses another engine, or a driver version ") + kProgramName + " doesn't know";
    switch (driver)
    {
    case Driver::kKonami:
        return "no Konami sound driver found" + kUnknown;
    case Driver::kRare:
        return "no Rare sound driver found (try --song-table)";
    case Driver::kQuintet:
        return "no Quintet sound driver found" + kUnknown;
    case Driver::kRd2:
        return "no Nintendo R&D2 sound driver found" + kUnknown;
    case Driver::kMp2k:
        return "no MP2K sound driver found (try --song-table)";
    case Driver::kBrownie:
        return "no Brownie Brown sound driver found" + kUnknown;
    default:
        return "no Konami, Rare, Quintet, Nintendo R&D2, Brownie Brown or MP2K sound driver found" + kUnknown;
    }
}

// Finds the drivers as OpenAllMusic() does, but stops at the first unless `all` is true.
std::vector<FoundMusic> FindMusic(const Rom& rom, const Overrides& overrides, bool all, std::string& error)
{
    error.clear();
    std::vector<FoundMusic> found;
    for (const auto& d : kDrivers)
    {
        if (overrides.driver != Driver::kAny && overrides.driver != d.driver)
        {
            continue;
        }

        // Tables that can't be read are an error in the first driver found, and leave out any later one.
        std::string driver_error;
        std::unique_ptr<Music> music = d.open(rom, overrides, driver_error);
        if (music)
        {
            found.push_back({d.driver, std::move(music)});
        }
        else if (found.empty() && !driver_error.empty())
        {
            error = driver_error;
            return {};
        }

        if (!all && !found.empty())
        {
            break;
        }
    }

    if (found.empty())
    {
        error = NotFound(overrides.driver);
    }

    return found;
}

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    std::vector<FoundMusic> found = FindMusic(rom, overrides, false, error);
    return found.empty() ? nullptr : std::move(found.front().music);
}

std::vector<FoundMusic> OpenAllMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    return FindMusic(rom, overrides, true, error);
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
