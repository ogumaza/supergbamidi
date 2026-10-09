// SPDX-License-Identifier: MIT

#include "test_util.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "files.h"

namespace supergbamidi::test
{

std::filesystem::path TempPath(const std::string& name)
{
    return g_temp / PathFromUtf8(name);
}

std::FILE* OpenTempFile(const std::string& name)
{
    // MSVC warns that std::fopen is unsafe, so it gets fopen_s, which does the same here.
    const std::string path = TempPath(name).string();
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "wb") != 0)
    {
        return nullptr;
    }
#else
    f = std::fopen(path.c_str(), "wb");
#endif

    return f;
}

std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::vector<uint8_t> data;
    ReadFile(path, data);

    return data;
}

uint32_t Be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

uint16_t Le16(const uint8_t* p)
{
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t Le32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

MidiEvents ReadMidi(const std::string& path)
{
    MidiEvents m;
    const std::vector<uint8_t> f = ReadAll(path);
    if (f.size() < 14)
    {
        return m;
    }

    // Reads a variable-length number at `i`, up to `end`.
    const auto read_number = [&f](size_t& i, size_t end)
    {
        uint32_t value = 0;
        uint8_t b = 0x80;
        while ((b & 0x80) && i < end)
        {
            b = f[i++];
            value = (value << 7) | (b & 0x7F);
        }

        return value;
    };

    // A chunk or an event that runs past the end of the file, or a track past the end of its chunk, ends there.
    m.division = (f[12] << 8) | f[13];
    size_t pos = 14;
    while (pos + 8 <= f.size())
    {
        const size_t end = size_t(std::min<uint64_t>(uint64_t(pos) + 8 + Be32(&f[pos + 4]), f.size()));
        size_t i = pos + 8;
        uint32_t tick = 0;
        m.tracks.emplace_back();
        while (i < end)
        {
            tick += read_number(i, end);
            if (i >= end)
            {
                break;
            }

            // The file writes no running status. Every event starts with its status byte. A meta event's length is a
            // variable-length number.
            const uint8_t status = f[i];
            size_t size = 3;
            if (status == 0xFF)
            {
                size_t at = i + 2;
                const uint32_t length = read_number(at, end);
                size = at - i + length;
            }
            else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            {
                size = 2;
            }
            if (i + size > end)
            {
                break;
            }

            m.tracks.back().push_back({tick, std::vector<uint8_t>(f.begin() + long(i), f.begin() + long(i + size))});
            i += size;
        }
        pos = end;
    }

    return m;
}

bool RepeatsTempoAtLoopStart(const MidiEvents& midi)
{
    if (midi.tracks.empty())
    {
        return false;
    }

    // Follow the tempo up to the marker, and then look for a tempo change after it at the same tick.
    std::vector<uint8_t> tempo;
    std::optional<uint32_t> marker;
    for (const auto& [tick, bytes] : midi.tracks[0])
    {
        if (marker && tick != *marker)
        {
            break;
        }

        const bool is_tempo = bytes.size() == 6 && bytes[0] == 0xFF && bytes[1] == 0x51;
        if (marker && is_tempo)
        {
            return bytes == tempo;
        }
        if (is_tempo)
        {
            tempo = bytes;
        }
        else if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0x06 &&
                 std::string(bytes.begin() + 3, bytes.end()) == "loopStart")
        {
            marker = tick;
        }
    }

    return false;
}

Sf2Records ReadSf2(const std::string& path)
{
    Sf2Records r;
    const std::vector<uint8_t> f = ReadAll(path);
    // A chunk that runs past the end of the file, or past the end of the list that holds it, ends there. A chunk of an
    // odd size has a byte of padding after it.
    size_t pos = 12;
    while (pos + 8 <= f.size())
    {
        const std::string id(reinterpret_cast<const char*>(&f[pos]), 4);
        const size_t end = size_t(std::min<uint64_t>(uint64_t(pos) + 8 + Le32(&f[pos + 4]), f.size()));
        if (id == "LIST")
        {
            size_t inner = pos + 12;
            while (inner + 8 <= end)
            {
                const std::string sub(reinterpret_cast<const char*>(&f[inner]), 4);
                const size_t sub_end = size_t(std::min<uint64_t>(uint64_t(inner) + 8 + Le32(&f[inner + 4]), end));
                r.chunks[sub] = std::vector<uint8_t>(f.begin() + long(inner + 8), f.begin() + long(sub_end));
                inner = sub_end + ((sub_end - inner) & 1);
            }
        }
        pos = end + ((end - pos) & 1);
    }

    return r;
}

} // namespace supergbamidi::test
