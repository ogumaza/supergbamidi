// SPDX-License-Identifier: MIT

#include "mp2k/driver.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "mp2k/song.h"
#include "thumb.h"

namespace supergbamidi::mp2k
{
namespace
{

// The mixer's output for a frame at each of its rates, from the driver's table.
constexpr uint16_t kSamplesPerFrame[12] = {96, 132, 176, 224, 264, 304, 352, 448, 528, 608, 672, 704};

// Maximum numbers of songs and music players to read during detection.
constexpr int kMaxSongs = 4096;
constexpr int kMaxPlayers = 64;

// The routine that starts a song, m4aSongNumStart: push {lr}; lsls r0, r0, #16; ldr r2, =players; ldr r1, =songs;
// lsrs r0, r0, #13; adds r0, r0, r1; ldrh r3, [r0, #4]; lsls r1, r3, #1; adds r1, r1, r3; lsls r1, r1, #2;
// adds r1, r1, r2; ldr r2, [r1]; ldr r1, [r0]; adds r0, r2, #0; bl MPlayStart; pop {r0}; bx r0. The two loads' offsets
// aren't compared.
constexpr uint16_t kSongStart[] = {0xB500, 0x0400, 0x4A00, 0x4900, 0x0B40, 0x1840, 0x8883,
                                   0x0059, 0x18C9, 0x0089, 0x1889, 0x680A, 0x6801, 0x1C10};
constexpr int kSongStartLength = sizeof kSongStart / sizeof kSongStart[0];

// The mixer's test for a voice that plays its sample backwards or a compressed sample, in the ARM code the init copies
// to IWRAM: ldrb r0, [r4, #1]; tst r0, #0x30. Older versions of the driver don't have it.
constexpr uint8_t kSpecialTest[8] = {0x01, 0x00, 0xD4, 0xE5, 0x30, 0x00, 0x10, 0xE3};

// The greatest distance, in bytes, that the init's setting of the driver's mode can be before its literal pool.
constexpr uint32_t kInitReach = 0x200;

std::string Hex(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", unsigned(v));

    return b;
}

// Returns true if a Thumb BL instruction starts at `at`.
bool IsBl(const Rom& rom, uint32_t at)
{
    return (rom.U16(at) & 0xF800) == 0xF000 && (rom.U16(at + 2) & 0xF800) == 0xF800;
}

// Returns true if `address` is in the GBA's work RAM, where the driver keeps its music players.
bool InRam(uint32_t address)
{
    return (address >= 0x02000000 && address < 0x02040000) || (address >= 0x03000000 && address < 0x03008000);
}

// Finds the routine that starts a song, and reads the addresses of the music player table and the song table from its
// literal pool. Returns false if there's none.
bool FindSongStart(const Rom& rom, DriverInfo& info)
{
    const uint8_t* data = rom.Ptr(kRomBase);
    const size_t size = rom.Size();
    for (size_t o = 0; o + 2 * (kSongStartLength + 4) <= size; o += 2)
    {
        bool match = true;
        for (int i = 0; i < kSongStartLength && match; i++)
        {
            const uint16_t h = uint16_t(data[o + 2 * size_t(i)] | (data[o + 2 * size_t(i) + 1] << 8));
            const uint16_t mask = i == 2 || i == 3 ? 0xFF00 : 0xFFFF;
            match = (h & mask) == kSongStart[i];
        }
        if (!match)
        {
            continue;
        }

        const uint32_t at = kRomBase + uint32_t(o);
        if (!IsBl(rom, at + 2 * kSongStartLength) || rom.U16(at + 2 * kSongStartLength + 4) != 0xBC01)
        {
            continue;
        }

        info.song_start = at;
        info.player_table = ThumbLiteral(rom, at + 4);
        info.song_table = ThumbLiteral(rom, at + 6);

        return true;
    }

    return false;
}

// Returns true if `mode` is a setting the driver's init could give it: a rate, the output's resolution, and nothing in
// the top byte.
bool IsSoundMode(uint32_t mode)
{
    const uint32_t rate = (mode >> 16) & 0xF;
    const uint32_t dac = (mode >> 20) & 0xF;
    return (mode >> 24) == 0 && rate >= 1 && rate <= 12 && dac >= 8 && dac <= 11;
}

// Finds the init's setting of the driver's mode: `ldr r0, =mode; bl m4aSoundMode`, a little before a literal pool that
// holds the music player table's address, which is the init's. Returns 0 if there's none.
uint32_t FindSoundMode(const Rom& rom, uint32_t player_table)
{
    const uint8_t* data = rom.Ptr(kRomBase);
    const size_t size = rom.Size();
    for (size_t o = 0; o + 4 <= size; o += 4)
    {
        const uint32_t word = data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | (uint32_t(data[o + 3]) << 24);
        if (word != player_table)
        {
            continue;
        }

        // The setting closest to the pool belongs to the init, whose pool it is.
        const uint32_t pool = kRomBase + uint32_t(o);
        uint32_t mode = 0;
        for (uint32_t at = pool > kRomBase + kInitReach ? pool - kInitReach : kRomBase; at + 6 <= pool; at += 2)
        {
            if ((rom.U16(at) >> 8) == 0x48 && IsBl(rom, at + 2) && IsSoundMode(ThumbLiteral(rom, at)))
            {
                mode = ThumbLiteral(rom, at);
            }
        }
        if (mode)
        {
            return mode;
        }
    }

    return 0;
}

// Applies the driver's mode setting, as m4aSoundMode does: each field that isn't 0 replaces the driver's setting.
void ApplySoundMode(uint32_t mode, DriverInfo& info)
{
    if (mode & 0xFF)
    {
        info.reverb = int(mode & 0x7F);
    }
    if (mode & 0xF00)
    {
        info.max_channels = int((mode >> 8) & 0xF);
    }
    if (mode & 0xF000)
    {
        info.master_volume = int((mode >> 12) & 0xF);
    }
    if (mode & 0xB00000)
    {
        info.dac_bits = uint8_t((mode & 0x300000) >> 14);
    }
    if (mode & 0xF0000)
    {
        info.rate_index = int((mode >> 16) & 0xF);
    }
}

// Returns true if the ROM holds the mixer's test for reversed and compressed samples.
bool HasSpecialSamples(const Rom& rom)
{
    const uint8_t* data = rom.Ptr(kRomBase);
    const uint8_t* end = data + rom.Size();
    return std::search(data, end, std::begin(kSpecialTest), std::end(kSpecialTest)) != end;
}

// Reads the music player table: 12 bytes a player, up to the first entry whose state or tracks aren't in RAM, or that
// has no tracks.
void ReadPlayers(const Rom& rom, DriverInfo& info)
{
    for (int p = 0; p < kMaxPlayers; p++)
    {
        const uint32_t at = info.player_table + 12 * uint32_t(p);
        if (!rom.Contains(at, 12) || !InRam(rom.U32(at)) || !InRam(rom.U32(at + 4)) || rom.U8(at + 8) == 0)
        {
            break;
        }

        Player player;
        player.track_count = rom.U8(at + 8) > 16 ? 16 : rom.U8(at + 8);
        info.players.push_back(player);
    }
}

// Returns how many songs the table at `table` has: entries up to the first that isn't a song, or names a music player
// the table of players doesn't have.
int CountSongs(const Rom& rom, const DriverInfo& info, uint32_t table)
{
    int count = 0;
    SongHeader header;
    while (count < kMaxSongs && rom.Contains(table + 8u * uint32_t(count), 8) &&
           ReadSongHeader(rom, rom.U32(table + 8u * uint32_t(count)), header) &&
           (info.players.empty() || rom.U16(table + 8u * uint32_t(count) + 4) < info.players.size()))
    {
        count++;
    }

    return count;
}

} // namespace

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();

    // Find the mixer's test for reversed and compressed samples, which a game given --song-table can have too.
    info.special_samples = HasSpecialSamples(rom);

    // Find the routine that starts a song, and from it the tables, and the init's setting of the driver's mode.
    if (FindSongStart(rom, info))
    {
        ReadPlayers(rom, info);
        info.sound_mode = FindSoundMode(rom, info.player_table);
        if (info.sound_mode)
        {
            ApplySoundMode(info.sound_mode, info);
        }
        else
        {
            info.warnings.push_back(
                "the driver's settings weren't found, so its defaults are assumed: 13379 Hz and "
                "8 DirectSound channels");
        }
    }

    if (overrides.song_table)
    {
        info.song_table = overrides.song_table;
    }
    if (info.song_table == 0)
    {
        error.clear();
        return false;
    }

    info.song_count = overrides.song_count ? overrides.song_count : CountSongs(rom, info, info.song_table);
    if (info.song_count == 0)
    {
        error = "the song table at " + Hex(info.song_table) + " has no valid songs";
        return false;
    }

    if (info.max_channels > kMaxDirectChannels)
    {
        info.max_channels = kMaxDirectChannels;
    }

    // The rate's setting gives the mixer's output for a frame, from which the driver works out its rate in Hz.
    info.samples_per_frame = kSamplesPerFrame[info.rate_index - 1];
    info.mix_rate = (597275 * info.samples_per_frame + 5000) / 10000;
    info.step_scale = (16777216u / uint32_t(info.mix_rate) + 1) >> 1;

    // Report what was found.
    if (info.song_start)
    {
        info.log.push_back("song start routine at " + Hex(info.song_start));
        std::string mixer = "mixer: " + std::to_string(info.mix_rate) + " Hz, up to " +
                            std::to_string(info.max_channels) + " DirectSound channels, master volume " +
                            std::to_string(info.master_volume);
        if (info.reverb)
        {
            mixer += ", reverb " + std::to_string(info.reverb);
        }

        info.log.push_back(mixer);
    }
    info.log.push_back("song table at " + Hex(info.song_table) + ": " + std::to_string(info.song_count) + " song" +
                       (info.song_count == 1 ? "" : "s") +
                       (info.players.empty() ? ""
                                             : ", on " + std::to_string(info.players.size()) + " music player" +
                                                   (info.players.size() == 1 ? "" : "s")));

    return true;
}

uint32_t SongAddress(const Rom& rom, const DriverInfo& info, int song)
{
    if (song < 0 || song >= info.song_count)
    {
        return 0;
    }

    return rom.U32(info.song_table + 8u * uint32_t(song));
}

Player SongPlayer(const Rom& rom, const DriverInfo& info, int song)
{
    const size_t number = song >= 0 && song < info.song_count ? rom.U16(info.song_table + 8u * uint32_t(song) + 4) : 0;
    if (number < info.players.size())
    {
        return info.players[number];
    }

    Player player;
    player.track_count = 16;

    return player;
}

} // namespace supergbamidi::mp2k
