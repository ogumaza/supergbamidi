// SPDX-License-Identifier: MIT

// Unit tests for shared code. Each driver's tests are in a separate file.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "beat_grid.h"
#include "files.h"
#include "inflate.h"
#include "midi.h"
#include "music.h"
#include "rom.h"
#include "sf2.h"
#include "test_util.h"
#include "thumb.h"

namespace fs = std::filesystem;

namespace supergbamidi::test
{
namespace
{

void TestFileNames()
{
    SUPERGBAMIDI_CHECK(SafeFileName("game") == "game");
    SUPERGBAMIDI_CHECK(SafeFileName("\xE9\x9F\xB3\xE6\xA5\xBD caf\xC3\xA9") == "\xE9\x9F\xB3\xE6\xA5\xBD caf\xC3\xA9");
    SUPERGBAMIDI_CHECK(SafeFileName("<>:\"/\\|?*\x01") == "__________");
    SUPERGBAMIDI_CHECK(SafeFileName("game. . ") == "game");
    SUPERGBAMIDI_CHECK(SafeFileName("..") == "output");
    SUPERGBAMIDI_CHECK(SafeFileName("") == "output");
    SUPERGBAMIDI_CHECK(SafeFileName("con") == "_con");
    SUPERGBAMIDI_CHECK(SafeFileName("NuL.backup") == "_NuL.backup");
    SUPERGBAMIDI_CHECK(SafeFileName("COM1") == "_COM1");
    SUPERGBAMIDI_CHECK(SafeFileName("lpt9") == "_lpt9");
    SUPERGBAMIDI_CHECK(SafeFileName("COM\xC2\xB9") == "_COM\xC2\xB9");
    SUPERGBAMIDI_CHECK(SafeFileName("console") == "console");
    SUPERGBAMIDI_CHECK(SafeFileName("COM10") == "COM10");
}

// Returns a zlib stream holding `data` in stored blocks.
std::vector<uint8_t> ZlibStored(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do
    {
        const size_t n = std::min<size_t>(data.size() - pos, 0xFFFF);
        const bool last = pos + n == data.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), data.begin() + long(pos), data.begin() + long(pos + n));
        pos += n;
    } while (pos < data.size());

    uint32_t a = 1, b = 0;
    for (uint8_t c : data)
    {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8)
    {
        z.push_back(uint8_t(adler >> s));
    }

    return z;
}

// Writes a GSF file whose program puts `data` at `address`, with the given tags.
void WriteGsf(const fs::path& path, uint32_t address, const std::vector<uint8_t>& data, const std::string& tags)
{
    std::vector<uint8_t> program;
    for (uint32_t v : {kRomBase, address, uint32_t(data.size())})
    {
        for (int i = 0; i < 4; i++)
        {
            program.push_back(uint8_t(v >> (8 * i)));
        }
    }
    program.insert(program.end(), data.begin(), data.end());

    const std::vector<uint8_t> z = ZlibStored(program);
    std::vector<uint8_t> f = {'P', 'S', 'F', 0x22};
    for (uint32_t v : {0u, uint32_t(z.size()), 0u})
    {
        for (int i = 0; i < 4; i++)
        {
            f.push_back(uint8_t(v >> (8 * i)));
        }
    }
    f.insert(f.end(), z.begin(), z.end());
    if (!tags.empty())
    {
        const std::string t = "[TAG]" + tags;
        f.insert(f.end(), t.begin(), t.end());
    }

    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

void TestInflate()
{
    static const uint8_t kStored[] = {0x78, 0x01, 0x01, 0x15, 0x00, 0xEA, 0xFF, 0x47, 0x42, 0x41, 0x20,
                                      0x73, 0x6F, 0x75, 0x6E, 0x64, 0x20, 0x64, 0x72, 0x69, 0x76, 0x65,
                                      0x72, 0x20, 0x74, 0x65, 0x73, 0x74, 0x4E, 0xB2, 0x07, 0xA0};
    static const uint8_t kFixed[] = {0x78, 0x01, 0x73, 0x77, 0x72, 0x54, 0x28, 0xCE, 0x2F, 0xCD,
                                     0x4B, 0x51, 0x48, 0x29, 0xCA, 0x2C, 0x4B, 0x2D, 0x52, 0x28,
                                     0x49, 0x2D, 0x2E, 0x01, 0x00, 0x4E, 0xB2, 0x07, 0xA0};
    static const uint8_t kDynamic[] = {
        0x78, 0xDA, 0x4D, 0xCD, 0x41, 0x0E, 0x80, 0x30, 0x08, 0x04, 0xC0, 0xAF, 0xEC, 0xD7, 0x6A, 0xE5, 0x60,
        0x6C, 0x0B, 0x51, 0xE2, 0xFB, 0x8D, 0x0B, 0x89, 0x5C, 0x08, 0x81, 0x61, 0x59, 0xEA, 0x02, 0xBF, 0x5A,
        0x3F, 0x71, 0xB7, 0x69, 0x43, 0xF0, 0xE8, 0xD1, 0x05, 0x43, 0xD5, 0xE0, 0x32, 0x4D, 0xB1, 0x3E, 0xB2,
        0xC9, 0xDA, 0xD3, 0x05, 0x48, 0xCD, 0x25, 0x31, 0x3B, 0x96, 0x38, 0xE3, 0x45, 0x0D, 0xAB, 0x4F, 0x4A,
        0x26, 0x71, 0x38, 0x4E, 0x53, 0x04, 0xFF, 0x93, 0x4B, 0x5C, 0x88, 0x17, 0x22, 0x42, 0x44, 0xB8};
    const std::string kText = "GBA sound driver test";
    const std::string kWords =
        "note track sample voice loop tempo note bend track voice sample note loop note note tempo bend voice loop "
        "track sample note bend tempo voice note sample track loop note bend voice sample";

    // A dynamic block holding "A", with a distance code that no symbol uses: a single code, one bit long.
    static const uint8_t kOneBitDistance[] = {0x78, 0x01, 0x05, 0xC0, 0x01, 0x09, 0x00, 0x00, 0x00, 0x80,
                                              0xA0, 0x6D, 0xFE, 0x3F, 0xA5, 0x02, 0x00, 0x42, 0x00, 0x42};

    std::vector<uint8_t> out;
    std::string err;

    SUPERGBAMIDI_CHECK(ZlibDecompress(kStored, sizeof kStored, out, err));
    SUPERGBAMIDI_CHECK(std::string(out.begin(), out.end()) == kText);
    SUPERGBAMIDI_CHECK(ZlibDecompress(kFixed, sizeof kFixed, out, err));
    SUPERGBAMIDI_CHECK(std::string(out.begin(), out.end()) == kText);
    SUPERGBAMIDI_CHECK(ZlibDecompress(kDynamic, sizeof kDynamic, out, err));
    SUPERGBAMIDI_CHECK(std::string(out.begin(), out.end()) == kWords);

    std::vector<uint8_t> bad(kDynamic, kDynamic + sizeof kDynamic);
    bad.back() ^= 1; // Adler-32 mismatch

    SUPERGBAMIDI_CHECK(!ZlibDecompress(bad.data(), bad.size(), out, err));
    SUPERGBAMIDI_CHECK(!ZlibDecompress(kDynamic, 20, out, err));                  // truncated
    SUPERGBAMIDI_CHECK(!ZlibDecompress(kDynamic, sizeof kDynamic - 4, out, err)); // no Adler-32

    // A single code that doesn't fill its code space has to be one bit long, as zlib and puff require. The same block
    // with a distance code of two bits is corrupt.
    std::vector<uint8_t> two_bit(kOneBitDistance, kOneBitDistance + sizeof kOneBitDistance);
    two_bit[14] = 0x65;

    SUPERGBAMIDI_CHECK(ZlibDecompress(kOneBitDistance, sizeof kOneBitDistance, out, err));
    SUPERGBAMIDI_CHECK(std::string(out.begin(), out.end()) == "A");
    SUPERGBAMIDI_CHECK(!ZlibDecompress(two_bit.data(), two_bit.size(), out, err));
}

void TestMidiWriter()
{
    // A note at tick 0, then an event of every kind at tick 96, added in the opposite of the order they're written in.
    MidiFile midi(96);
    MidiTrack& t = midi.AddTrack();
    t.NoteOn(0, 0, 60, 100);
    t.NoteOn(96, 0, 62, 100);
    t.PitchBend(96, 0, 8192);
    t.Control(96, 0, 11, 64);
    t.Program(96, 0, 5);
    t.Bank(96, 0, 1);
    t.NoteOff(96, 0, 60);
    t.Meta(96, 0x06, "x");

    const std::string path = Utf8(TempPath("test.mid"));
    std::string err;

    SUPERGBAMIDI_CHECK(midi.Write(path, err));

    // At the same tick, meta events come first, then note-offs, bank selects, program changes, controllers, pitch bends
    // and note-ons.
    const std::vector<uint8_t> f = ReadAll(path);
    const std::vector<uint8_t> kExpected = {
        'M',  'T',  'h',  'd',  0,    0,    0,  6,  // the header, 6 bytes long
        0,    1,    0,    1,    0,    96,           // format 1, one track, 96 ticks to a quarter note
        'M',  'T',  'r',  'k',  0,    0,    0,  40, // the track, 40 bytes long
        0x00, 0x90, 60,   100,                      // note-on
        0x60, 0xFF, 0x06, 1,    'x',                // marker, 96 ticks later
        0x00, 0x80, 60,   0,                        // note-off
        0x00, 0xB0, 0,    1,    0x00, 0xB0, 32, 0,  // bank select
        0x00, 0xC0, 5,                              // program change
        0x00, 0xB0, 11,   64,                       // controller
        0x00, 0xE0, 0x00, 0x40,                     // pitch bend
        0x00, 0x90, 62,   100,                      // note-on
        0x00, 0xFF, 0x2F, 0x00};                    // end of the track
    SUPERGBAMIDI_CHECK(f == kExpected);
    fs::remove(PathFromUtf8(path));
}

void TestSf2Writer()
{
    Sf2File sf;
    sf.name = "Test";

    Sf2Sample s;
    s.name = "Saw";
    for (int i = 0; i < 100; i++)
    {
        s.pcm.push_back(int16_t(i * 100));
    }
    s.rate = 16000;
    s.loop = true;
    s.loop_start = 10;
    s.loop_end = 90;
    sf.samples.push_back(s);

    // A global zone with a modulator, then a zone whose generators are out of order on purpose.
    Sf2Instrument inst;
    inst.name = "Inst";
    Sf2Zone global;
    global.mods.push_back(
        {sf2src::kNoteOnVelocity | sf2src::kNegative | sf2src::kConcave, sf2gen::kInitialAttenuation, 480, 0, 0});
    inst.zones.push_back(global);
    Sf2Zone z;
    z.gens = {Sf2Gen::Value(sf2gen::kSampleId, 0), Sf2Gen::Range(sf2gen::kKeyRange, 0, 127)};
    inst.zones.push_back(z);
    sf.instruments.push_back(inst);
    sf.presets.push_back({"Preset", 0, 5, 0});

    const std::string path = Utf8(TempPath("test.sf2"));
    std::string err;

    SUPERGBAMIDI_CHECK(sf.Write(path, err));

    const std::vector<uint8_t> f = ReadAll(path);
    SUPERGBAMIDI_CHECK(f.size() > 12 && std::memcmp(f.data(), "RIFF", 4) == 0 && std::memcmp(&f[8], "sfbk", 4) == 0);
    SUPERGBAMIDI_CHECK_EQ(Le32(&f[4]) + 8, f.size());

    const Sf2Records r = ReadSf2(path);
    SUPERGBAMIDI_CHECK_EQ(r.chunks.at("smpl").size(), (100 + 46) * 2);
    SUPERGBAMIDI_CHECK_EQ(r.Count("shdr", 46), 2);
    const uint8_t* h = r.Record("shdr", 46, 0);
    SUPERGBAMIDI_CHECK(std::string(reinterpret_cast<const char*>(h)) == "Saw");
    SUPERGBAMIDI_CHECK_EQ(Le32(h + 20), 0);
    SUPERGBAMIDI_CHECK_EQ(Le32(h + 24), 100);
    SUPERGBAMIDI_CHECK_EQ(Le32(h + 28), 10);
    SUPERGBAMIDI_CHECK_EQ(Le32(h + 32), 90);
    SUPERGBAMIDI_CHECK_EQ(Le32(h + 36), 16000);

    // The global zone's bag points at its modulator and no generators, the zone's bag at its two generators after the
    // modulator, and the zone's keyRange comes first.
    SUPERGBAMIDI_CHECK_EQ(r.Count("imod", 10), 2);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("imod", 10, 0)), 0x0502);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("imod", 10, 0) + 2), sf2gen::kInitialAttenuation);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("imod", 10, 0) + 4), 480);
    SUPERGBAMIDI_CHECK_EQ(r.Count("ibag", 4), 3);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("ibag", 4, 0)), 0);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("ibag", 4, 0) + 2), 0);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("ibag", 4, 1)), 0);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("ibag", 4, 1) + 2), 1);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("ibag", 4, 2)), 2);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("igen", 4, 0)), sf2gen::kKeyRange);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("igen", 4, 1)), sf2gen::kSampleId);
    fs::remove(PathFromUtf8(path));
}

void TestGsfLoading()
{
    // A GSF set in a folder whose name isn't ASCII: the library holds the cartridge, and the mini-GSF names it and
    // patches one byte.
    std::error_code ec;
    const fs::path dir = TempPath("gsf_\xC3\xA9\xE3\x81\x82");
    fs::create_directories(dir, ec);
    SUPERGBAMIDI_CHECK(!ec);
    if (ec)
    {
        return;
    }

    std::vector<uint8_t> cart(0x1000, 0);
    const uint8_t kMarker[] = {0x78, 0x56, 0x34, 0x12};
    std::copy(std::begin(kMarker), std::end(kMarker), cart.begin() + 0x100);
    WriteGsf(dir / "set.gsflib", kRomBase, cart, "");
    WriteGsf(dir / "set-01.minigsf", kRomBase + 0xF00, {0x5A}, "title=Song 1\n_lib=set.gsflib\n");

    Rom rom;
    std::string err;

    SUPERGBAMIDI_CHECK(rom.Load(Utf8(dir / "set-01.minigsf"), err));
    SUPERGBAMIDI_CHECK(rom.FromGsf());
    SUPERGBAMIDI_CHECK(rom.GsfLibrary() == Utf8(dir / "set.gsflib"));
    SUPERGBAMIDI_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678); // from the library
    SUPERGBAMIDI_CHECK_EQ(rom.U8(kRomBase + 0xF00), 0x5A);        // from the mini-GSF

    SUPERGBAMIDI_CHECK(rom.Load(Utf8(dir / "set.gsflib"), err));
    SUPERGBAMIDI_CHECK(rom.GsfLibrary().empty());

    // _lib2 goes on top of the file's program.
    WriteGsf(dir / "patch.gsflib", kRomBase + 0xF00, {0xA5}, "");
    WriteGsf(dir / "set-02.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=set.gsflib\n_lib2=patch.gsflib\n");

    SUPERGBAMIDI_CHECK(rom.Load(Utf8(dir / "set-02.minigsf"), err));

    SUPERGBAMIDI_CHECK_EQ(rom.U8(kRomBase + 0xF00), 0xA5);
    SUPERGBAMIDI_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678);

    WriteGsf(dir / "orphan.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=missing.gsflib\n");

    SUPERGBAMIDI_CHECK(!rom.Load(Utf8(dir / "orphan.minigsf"), err));
    SUPERGBAMIDI_CHECK(err.find("missing.gsflib") != std::string::npos);

    // A folder dropped on the program by mistake.
    SUPERGBAMIDI_CHECK(!rom.Load(Utf8(dir), err));
    SUPERGBAMIDI_CHECK(err.find("is a folder") != std::string::npos);

    // A zipped ROM, which has to be taken out of its archive first.
    std::vector<uint8_t> zip(0x200, 0);
    std::memcpy(zip.data(), "PK\x03\x04", 4);
    std::string write_error;
    SUPERGBAMIDI_CHECK(WriteFile(Utf8(dir / "game.zip"), zip, write_error));

    SUPERGBAMIDI_CHECK(!rom.Load(Utf8(dir / "game.zip"), err));

    SUPERGBAMIDI_CHECK(err.find("is an archive") != std::string::npos);
    fs::remove_all(dir, ec);
}

// A game with none of the drivers gets an error that names the drivers looked for, from OpenMusic() and OpenAllMusic()
// alike.
void TestNoDriver()
{
    Rom rom;
    rom.Assign(std::vector<uint8_t>(0x1000, 0));
    Overrides konami_only, rare_only, quintet_only, rd2_only, mp2k_only, brownie_only, krawall_only;
    konami_only.driver = Driver::kKonami;
    rare_only.driver = Driver::kRare;
    quintet_only.driver = Driver::kQuintet;
    rd2_only.driver = Driver::kRd2;
    mp2k_only.driver = Driver::kMp2k;
    brownie_only.driver = Driver::kBrownie;
    krawall_only.driver = Driver::kKrawall;
    std::string any, konami, rare, quintet, rd2, mp2k, brownie, krawall, all, all_mp2k;

    const bool found_any = OpenMusic(rom, Overrides(), any) != nullptr;
    const bool found_konami = OpenMusic(rom, konami_only, konami) != nullptr;
    const bool found_rare = OpenMusic(rom, rare_only, rare) != nullptr;
    const bool found_quintet = OpenMusic(rom, quintet_only, quintet) != nullptr;
    const bool found_rd2 = OpenMusic(rom, rd2_only, rd2) != nullptr;
    const bool found_mp2k = OpenMusic(rom, mp2k_only, mp2k) != nullptr;
    const bool found_brownie = OpenMusic(rom, brownie_only, brownie) != nullptr;
    const bool found_krawall = OpenMusic(rom, krawall_only, krawall) != nullptr;
    const bool found_all = !OpenAllMusic(rom, Overrides(), all).empty();
    const bool found_all_mp2k = !OpenAllMusic(rom, mp2k_only, all_mp2k).empty();

    SUPERGBAMIDI_CHECK(!found_any && !found_konami && !found_rare && !found_quintet && !found_rd2 && !found_mp2k &&
                       !found_brownie && !found_krawall);
    SUPERGBAMIDI_CHECK(!found_all && !found_all_mp2k);
    SUPERGBAMIDI_CHECK(all == any && all_mp2k == mp2k);
    SUPERGBAMIDI_CHECK(
        any ==
        "no Konami, Rare, Quintet, Nintendo R&D2, Brownie Brown, Krawall or MP2K sound driver found: this game's "
        "music uses another engine, or a driver version supergbamidi doesn't know");
    SUPERGBAMIDI_CHECK(konami ==
                       "no Konami sound driver found: this game's music uses another engine, or a driver version "
                       "supergbamidi doesn't know");
    SUPERGBAMIDI_CHECK(rare == "no Rare sound driver found (try --song-table)");
    SUPERGBAMIDI_CHECK(quintet ==
                       "no Quintet sound driver found: this game's music uses another engine, or a driver version "
                       "supergbamidi doesn't know");
    SUPERGBAMIDI_CHECK(rd2 ==
                       "no Nintendo R&D2 sound driver found: this game's music uses another engine, or a driver "
                       "version supergbamidi doesn't know");
    SUPERGBAMIDI_CHECK(mp2k == "no MP2K sound driver found (try --song-table)");
    SUPERGBAMIDI_CHECK(brownie ==
                       "no Brownie Brown sound driver found: this game's music uses another engine, or a driver "
                       "version supergbamidi doesn't know");
    SUPERGBAMIDI_CHECK(krawall ==
                       "no Krawall sound driver found: this game's music uses another engine, or a driver version "
                       "supergbamidi doesn't know");
}

// A song's loop lasts until every track's loop is back where it started: loops of 3 and 4 bars make one of 12, and a
// loop that divides the longest leaves it as it is. A loop more than 8 times the longest isn't used: 72 ticks for loops
// of 8 and 9 is, but 90 for loops of 9 and 10 isn't, and the longest loop is the song's.
void TestLoopLength()
{
    SUPERGBAMIDI_CHECK_EQ(LoopLength({7}), uint64_t(7));
    SUPERGBAMIDI_CHECK_EQ(LoopLength({960, 480}), uint64_t(960));
    SUPERGBAMIDI_CHECK_EQ(LoopLength({9216, 12288}), uint64_t(36864));
    SUPERGBAMIDI_CHECK_EQ(LoopLength({8, 9}), uint64_t(72));
    SUPERGBAMIDI_CHECK_EQ(LoopLength({9, 10}), uint64_t(10));
    SUPERGBAMIDI_CHECK_EQ(LoopLength({6138, 6144}), uint64_t(6144));
}

// Returns true if `a` and `b` are within `tolerance` of each other.
bool Near(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) <= tolerance;
}

// A steady beat of 16th notes that should last 6.5 frames, written as 6 and 7: the notes go onto the beat, a quarter
// note lasts 26 frames, and channels playing quarter notes, an echo 11 frames behind and chords played as quick
// arpeggios keep in step with them. The echo keeps its delay, and the arpeggios' quick notes keep theirs.
void TestBeatGridSteady()
{
    std::vector<std::vector<uint32_t>> channels(4);
    for (uint32_t k = 0; k < 64; k++)
    {
        channels[0].push_back(1 + k * 13 / 2);
        channels[2].push_back(12 + k * 13 / 2);
    }
    channels[2].insert(channels[2].begin(), 1);
    for (uint32_t k = 0; k <= 16; k++)
    {
        channels[1].push_back(1 + 26 * k);
    }
    for (uint32_t k = 0; k < 32; k++)
    {
        for (uint32_t i = 0; i < 3; i++)
        {
            channels[3].push_back(1 + 13 * k + i);
        }
    }

    const BeatGrid grid(channels, {});

    SUPERGBAMIDI_CHECK_EQ(grid.Tempos().size(), size_t(1));
    SUPERGBAMIDI_CHECK(Near(grid.Tempos()[0].frames, 26, 0.02));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, 1), 0));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, 7), 0.25));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, 14), 0.5));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, 20), 0.75));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(1, 27), 1));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(2, 18) - grid.Quarters(0, 7), 11.0 / 26, 0.002));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(3, 14), 0.5));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(3, 15) - grid.Quarters(3, 14), 1.0 / 26, 0.002));
}

// A ritardando: quarter notes of 23 frames, then six that slow down, then 23 again. The slow notes set the tempo one by
// one, so that the notes after them are on the beat again.
void TestBeatGridRitardando()
{
    std::vector<uint32_t> notes;
    uint32_t frame = 1;
    for (int i = 0; i < 16; i++, frame += 23)
    {
        notes.push_back(frame);
    }
    for (const uint32_t length : {26u, 26u, 26u, 28u, 28u, 30u})
    {
        notes.push_back(frame);
        frame += length;
    }
    const uint32_t again = frame;
    for (int i = 0; i < 16; i++, frame += 23)
    {
        notes.push_back(frame);
    }
    notes.push_back(frame);

    const BeatGrid grid({notes, notes}, {});

    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, 1 + 23 * 15), 15));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, again), 22));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, again + 23), 23));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(again + 46), 24));
    SUPERGBAMIDI_CHECK(grid.Tempos().size() >= 4);
    SUPERGBAMIDI_CHECK(Near(grid.Tempos().back().frames, 23, 0.01));
}

// A tempo that changes from note to note, in a rubato: every note is an 8th note, and has its own tempo. An echo 6
// frames behind comes after its note and before the next 8th note.
void TestBeatGridRubato()
{
    std::vector<uint32_t> notes = {1};
    for (const uint32_t length :
         {20u, 20u, 20u, 19u, 19u, 18u, 17u, 17u, 16u, 15u, 15u, 15u, 15u, 16u, 18u, 19u, 20u, 22u, 24u})
    {
        notes.push_back(notes.back() + length);
    }
    std::vector<uint32_t> echo = {1};
    for (uint32_t f : notes)
    {
        echo.push_back(f + 6);
    }

    const BeatGrid grid({notes, notes, echo}, {});

    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, notes[1]), 0.5));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(1, notes[10]), 5));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, notes[19]), 9.5));
    SUPERGBAMIDI_CHECK(grid.Quarters(2, echo[11]) > 5 && grid.Quarters(2, echo[11]) < 5.5);
    SUPERGBAMIDI_CHECK(Near(grid.Tempos()[0].frames, 40, 0.01));
}

// A rubato that a loop repeats, each pass slowing at its end to an 8th note of 32 frames: each pass lasts as many
// quarter notes as the first, rather than taking its quarter note from the slow end of the pass before.
void TestBeatGridRubatoLoop()
{
    std::vector<uint32_t> notes = {1};
    std::vector<uint32_t> hints;
    for (int pass = 0; pass < 4; pass++)
    {
        if (pass > 0)
        {
            hints.insert(hints.end(), {notes.back(), notes.back()});
        }
        for (const uint32_t length : {20u, 20u, 20u, 19u, 19u, 18u, 17u, 17u, 16u, 15u, 15u,
                                      15u, 15u, 16u, 18u, 19u, 20u, 22u, 22u, 24u, 26u, 32u})
        {
            notes.push_back(notes.back() + length);
        }
    }

    const BeatGrid grid({notes, notes}, hints);

    const double first = grid.Quarters(0, hints[0]);
    for (size_t i = 2; i < hints.size(); i += 2)
    {
        SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, hints[i]) - grid.Quarters(0, hints[i - 2]), first));
    }
}

// Quarter notes of 22.5 frames, a rubato of quarter notes of 15 to 32 frames, quarter notes of 30 frames, and a rubato
// of 8th notes of about 15 frames. The second rubato takes its quarter note from the beat before it, and not from the
// first rubato's notes of 15 frames, which aren't a beat.
void TestBeatGridRubatoAfterRubato()
{
    std::vector<uint32_t> notes = {1};
    std::vector<uint32_t> hints;
    const auto add = [&](std::initializer_list<uint32_t> lengths, int times)
    {
        for (int i = 0; i < times; i++)
        {
            for (const uint32_t length : lengths)
            {
                notes.push_back(notes.back() + length);
            }
        }
        hints.insert(hints.end(), {notes.back(), notes.back()});
    };

    add({22, 23}, 6);
    add({20, 20, 20, 19, 19, 18, 17, 17, 16, 15, 15, 15, 15, 16, 18, 19, 20, 22, 22, 24, 26, 32}, 1);
    add({30}, 12);
    const uint32_t rubato = notes.back();
    add({16, 15, 15, 15, 14, 15, 15, 16, 15, 16, 17, 18, 19, 20, 21, 22, 24, 26}, 1);
    add({30}, 8);

    const BeatGrid grid({notes, notes}, hints);

    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, rubato + 16) - grid.Quarters(0, rubato), 0.5));
}

// The tempo changes where a hint says it may: from 16th notes of 6.5 frames to 7.5, so from 26 frames a quarter note to
// 30. A loop back that doesn't change the tempo doesn't split the beat.
void TestBeatGridTempoChange()
{
    std::vector<uint32_t> notes;
    for (uint32_t k = 0; k < 48; k++)
    {
        notes.push_back(1 + k * 13 / 2);
    }
    const uint32_t kChange = 1 + 48 * 13 / 2;
    for (uint32_t k = 0; k <= 48; k++)
    {
        notes.push_back(kChange + k * 15 / 2);
    }

    const BeatGrid grid({notes, notes}, {kChange, kChange, 100, 100});

    SUPERGBAMIDI_CHECK_EQ(grid.Tempos().size(), size_t(2));
    SUPERGBAMIDI_CHECK(Near(grid.Tempos()[0].frames, 26, 0.01));
    SUPERGBAMIDI_CHECK(Near(grid.Tempos()[1].quarter, 12));
    SUPERGBAMIDI_CHECK(Near(grid.Tempos()[1].frames, 30, 0.01));
    SUPERGBAMIDI_CHECK(Near(grid.Quarters(0, kChange + 15), 12.5));
}

// A run of 32nd notes of 4, 4, 4, 3 and 4 frames before 32nd notes of 10/3 frames: the run sets the tempo note by note,
// and lasts a whole number of 32nd notes, so that the notes after it stay on 32nd notes.
void TestBeatGridWholeUnits()
{
    std::vector<uint32_t> run = {0, 4, 8, 12, 15};
    std::vector<uint32_t> eighths = {0};
    for (uint32_t k = 0; k < 96; k++)
    {
        run.push_back(19 + (10 * k + 1) / 3);
    }
    for (uint32_t k = 0; k < 48; k++)
    {
        eighths.push_back(19 + (40 * k + 1) / 3);
    }

    const BeatGrid grid({run, run, eighths}, {});

    for (size_t i = 5; i < 69; i++)
    {
        const double thirty_seconds = grid.Quarters(0, run[i]) * 8;
        SUPERGBAMIDI_CHECK(Near(thirty_seconds, std::round(thirty_seconds)));
    }
}

// A marker on a frame, such as a loop's start, goes where the channels put the events on it: an 8th-note triplet among
// 16th notes of 20/3 frames goes onto the triplet, a little before the frame's place on the beat, and so does the
// marker. A channel's events before the frame, or well after it, don't move the marker.
void TestBeatGridMarker()
{
    std::vector<std::vector<uint32_t>> channels(2);
    for (uint32_t k = 0; k < 96; k++)
    {
        channels[0].push_back(k * 20 / 3);
    }
    for (uint32_t q = 0; q <= 24; q++)
    {
        channels[1].push_back(q * 80 / 3);
        if (q == 3)
        {
            channels[1].push_back(89);
        }
    }

    const BeatGrid grid(channels, {});

    SUPERGBAMIDI_CHECK(Near(grid.Quarters(1, 89), 3 + 1.0 / 3));
    SUPERGBAMIDI_CHECK(grid.Snap(89) > grid.Quarters(1, 89) + 0.001);
    SUPERGBAMIDI_CHECK(Near(grid.Marker(89), grid.Quarters(1, 89)));
    SUPERGBAMIDI_CHECK(Near(grid.Marker(80), 3));
    SUPERGBAMIDI_CHECK(Near(grid.Marker(93), 3.5));
}

// RunThumb() runs ldmia and stmia: one routine loads two words from the ROM with ldmia and adds them, and another
// stores its arguments on the stack with stmia and loads the second one back. A routine that stores a byte at the top
// of the address space, which isn't on the stack, gets no result.
void TestRunThumb()
{
    // The first routine is ldr r1, =words; ldmia r1!, {r0, r2}; adds r0, r0, r2; bx lr, followed by the words' address,
    // 5 and 7. The second, at 0x14, is sub sp, #8; mov r3, sp; stmia r3!, {r0, r1}; ldr r0, [sp, #4]; add sp, #8;
    // bx lr. The third, at 0x20, is movs r0, #0; subs r0, #1; strb r0, [r0]; ldrb r0, [r0]; bx lr.
    Rom rom;
    rom.Assign({0x01, 0x49, 0x05, 0xC9, 0x80, 0x18, 0x70, 0x47, 0x0C, 0x00, 0x00, 0x08, 0x05, 0x00,
                0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x82, 0xB0, 0x6B, 0x46, 0x03, 0xC3, 0x01, 0x98,
                0x02, 0xB0, 0x70, 0x47, 0x00, 0x20, 0x01, 0x38, 0x00, 0x70, 0x00, 0x78, 0x70, 0x47});

    const std::optional<uint32_t> sum = RunThumb(rom, kRomBase, {0, 0, 0, 0});
    const std::optional<uint32_t> second = RunThumb(rom, kRomBase + 0x14, {3, 9, 0, 0});
    const std::optional<uint32_t> top = RunThumb(rom, kRomBase + 0x20, {0, 0, 0, 0});

    SUPERGBAMIDI_CHECK(sum && *sum == 12);
    SUPERGBAMIDI_CHECK(second && *second == 9);
    SUPERGBAMIDI_CHECK(!top);
}

// Creates a unique directory in the system's temp folder so concurrent test runs don't share files. Returns false on
// failure.
bool MakeTempFolder()
{
    std::random_device random;
    for (int attempt = 0; attempt < 100; attempt++)
    {
        char name[48];
        std::snprintf(name, sizeof name, "supergbamidi_tests_%08x%08x", unsigned(random()),
                      unsigned(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        g_temp = fs::temp_directory_path(ec) / name;
        if (!ec && fs::create_directory(g_temp, ec))
        {
            return true;
        }
    }

    return false;
}

// Runs every test. Returns the exit code.
int Run()
{
    if (!MakeTempFolder())
    {
        std::fprintf(stderr, "can't create a temporary folder for the test output\n");
        return 1;
    }

    TestFileNames();
    TestInflate();
    TestMidiWriter();
    TestSf2Writer();
    TestGsfLoading();
    TestNoDriver();
    TestLoopLength();
    TestBeatGridSteady();
    TestBeatGridRitardando();
    TestBeatGridRubato();
    TestBeatGridRubatoLoop();
    TestBeatGridRubatoAfterRubato();
    TestBeatGridTempoChange();
    TestBeatGridWholeUnits();
    TestBeatGridMarker();
    TestRunThumb();
    konami::RunTests();
    rare::RunTests();
    quintet::RunTests();
    mp2k::RunTests();
    rd2::RunTests();
    brownie::RunTests();
    krawall::RunTests();

    std::error_code ec;
    fs::remove_all(g_temp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);

    return g_failures ? 1 : 0;
}

} // namespace
} // namespace supergbamidi::test

int main()
{
    return supergbamidi::test::Run();
}
