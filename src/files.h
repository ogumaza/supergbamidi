// SPDX-License-Identifier: MIT

// File access with UTF-8 paths, including on Windows.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace supergbamidi
{

inline std::filesystem::path PathFromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

inline std::string Utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Makes a single filename component valid on Windows as well as macOS and Linux. Keeps UTF-8 characters intact.
inline std::string SafeFileName(std::string name)
{
    constexpr std::string_view kForbidden = "<>:\"/\\|?*";
    for (char& c : name)
    {
        if (static_cast<unsigned char>(c) < 32 || kForbidden.find(c) != std::string_view::npos)
        {
            c = '_';
        }
    }

    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
    {
        name.pop_back();
    }
    if (name.empty())
    {
        return "output";
    }

    // Device names are reserved even with an extension. Windows also recognises superscript 1, 2 and 3.
    std::string device = name.substr(0, name.find('.'));
    for (char& c : device)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = char(c - ('a' - 'A'));
        }
    }
    const std::string number = device.size() > 3 ? device.substr(3) : "";
    const bool numbered = (device.compare(0, 3, "COM") == 0 || device.compare(0, 3, "LPT") == 0) &&
                          ((number.size() == 1 && number[0] >= '1' && number[0] <= '9') || number == "\xC2\xB9" ||
                           number == "\xC2\xB2" || number == "\xC2\xB3");
    if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" || numbered)
    {
        name.insert(name.begin(), '_');
    }

    return name;
}

// Reads the whole file at `path` into `out`. Returns false if it can't be opened.
inline bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(PathFromUtf8(path), std::ios::binary);
    if (!f)
    {
        return false;
    }

    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());

    return true;
}

// Writes `data` to the file at `path`, replacing it. Returns false and sets `error` on failure.
inline bool WriteFile(const std::string& path, const std::vector<uint8_t>& data, std::string& error)
{
    std::ofstream f(PathFromUtf8(path), std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));

    // Closing flushes buffered data and can report a full disk.
    f.close();
    if (!f)
    {
        error = "can't write " + path;
        return false;
    }

    return true;
}

} // namespace supergbamidi
