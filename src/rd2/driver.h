// SPDX-License-Identifier: MIT

// Locating Nintendo R&D2's GBA sound driver, its tables, and the settings the game gives it in a ROM.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace supergbamidi::rd2
{

// The frame rate of the GBA's display, which the driver's per-frame routine follows: 280896 CPU cycles a frame.
constexpr double kFrameRate = 16777216.0 / 280896.0;

// The size of the game's settings for the driver: 7 table addresses and a word the driver doesn't read.
constexpr uint32_t kSettingsSize = 32;

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t settings = 0; // the game's settings for the driver
    int sequence_count = 0;
};

// The driver's tables in a ROM, and the tables the game gives it. Each table of offsets holds a word for each entry,
// counted from the table's start.
struct DriverInfo
{
    uint32_t init = 0;             // the driver's init routine (Thumb), or 0 if it wasn't found
    uint32_t settings = 0;         // the game's settings, which it passes to init
    uint32_t sample_sets = 0;      // the offset of each sample set's table of sample offsets
    uint32_t banks = 0;            // the offset of each bank of instruments
    uint32_t sequences = 0;        // the offset of each sequence
    uint32_t effects = 0;          // the offset of each of the sound effects' sequences
    uint32_t bank_sample_sets = 0; // a halfword for each bank: the sample set that its instruments play
    uint32_t bank_lists = 0;       // the offset of each sequence's list of banks
    uint32_t pitch_table = 0;      // 120 words: a sample's pitch for each pitch index, 0x8000 at the sample's rate
    uint32_t frequency_table = 0;  // 120 halfwords: a square or wave voice's frequency setting for each pitch index
    uint32_t noise_table = 0;      // 120 bytes: a noise voice's NR43 for each pitch index
    uint32_t lfo_table = 0;        // 256 signed bytes: one cycle of the LFO's sine
    uint32_t wave_volumes = 0;     // 5 bytes: the wave voice's NR32 for each of its volumes
    uint32_t voice_classes = 0;    // a byte for each region type: the voices that can play it
    uint32_t key_envelope = 0;     // the envelope of instruments that pick a sample for each key
    std::vector<uint32_t> sequence_addresses;
    std::vector<uint32_t> bank_list_addresses;
    std::vector<std::string> log;      // detection results for --info
    std::vector<std::string> warnings; // assumptions detection had to make
};

// Finds the driver in `rom`. Returns false and sets `error` if the driver's tables can't be read, or returns false with
// `error` empty if the game shows no sign of the driver.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// Returns `v` as 0x and 8 hexadecimal digits, for messages.
std::string Hex(uint32_t v);

} // namespace supergbamidi::rd2
