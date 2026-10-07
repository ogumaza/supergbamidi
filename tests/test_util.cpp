// SPDX-License-Identifier: MIT

#include "test_util.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
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

    m.division = (f[12] << 8) | f[13];
    size_t pos = 14;
    while (pos + 8 <= f.size())
    {
        const size_t end = pos + 8 + Be32(&f[pos + 4]);
        size_t i = pos + 8;
        uint32_t tick = 0;
        m.tracks.emplace_back();
        while (i < end)
        {
            uint32_t delta = 0;
            uint8_t b = 0;
            do
            {
                b = f[i++];
                delta = (delta << 7) | (b & 0x7F);
            } while (b & 0x80);
            tick += delta;

            // The file writes no running status, so every event starts with its status byte.
            const uint8_t status = f[i];
            size_t size = 3;
            if (status == 0xFF)
            {
                size = 3 + f[i + 2];
            }
            else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            {
                size = 2;
            }
            m.tracks.back().push_back({tick, std::vector<uint8_t>(f.begin() + long(i), f.begin() + long(i + size))});
            i += size;
        }
        pos = end;
    }

    return m;
}

Sf2Records ReadSf2(const std::string& path)
{
    Sf2Records r;
    const std::vector<uint8_t> f = ReadAll(path);
    size_t pos = 12;
    while (pos + 8 <= f.size())
    {
        const std::string id(reinterpret_cast<const char*>(&f[pos]), 4);
        const uint32_t size = Le32(&f[pos + 4]);
        if (id == "LIST")
        {
            size_t inner = pos + 12;
            while (inner + 8 <= pos + 8 + size)
            {
                const std::string sub(reinterpret_cast<const char*>(&f[inner]), 4);
                const uint32_t sub_size = Le32(&f[inner + 4]);
                r.chunks[sub] =
                    std::vector<uint8_t>(f.begin() + long(inner + 8), f.begin() + long(inner + 8 + sub_size));
                inner += 8 + sub_size + (sub_size & 1);
            }
        }
        pos += 8 + size + (size & 1);
    }

    return r;
}

} // namespace supergbamidi::test
