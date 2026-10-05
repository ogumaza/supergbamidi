// SPDX-License-Identifier: MIT

// Unit tests for shared code. Each driver's tests are in a separate file.

#include <algorithm>
#include <chrono>
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

    // The global zone's bag points at its modulator, and the zone's keyRange comes first.
    SUPERGBAMIDI_CHECK_EQ(r.Count("imod", 10), 2);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("imod", 10, 0)), 0x0502);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("imod", 10, 0) + 2), sf2gen::kInitialAttenuation);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("imod", 10, 0) + 4), 480);
    SUPERGBAMIDI_CHECK_EQ(Le16(r.Record("ibag", 4, 1) + 2), 1);
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

// A game with none of the drivers gets an error that names the drivers looked for.
void TestNoDriver()
{
    Rom rom;
    rom.Assign(std::vector<uint8_t>(0x1000, 0));
    Overrides konami_only, rare_only, quintet_only, rd2_only, mp2k_only;
    konami_only.driver = Driver::kKonami;
    rare_only.driver = Driver::kRare;
    quintet_only.driver = Driver::kQuintet;
    rd2_only.driver = Driver::kRd2;
    mp2k_only.driver = Driver::kMp2k;
    std::string any, konami, rare, quintet, rd2, mp2k;

    const bool found_any = OpenMusic(rom, Overrides(), any) != nullptr;
    const bool found_konami = OpenMusic(rom, konami_only, konami) != nullptr;
    const bool found_rare = OpenMusic(rom, rare_only, rare) != nullptr;
    const bool found_quintet = OpenMusic(rom, quintet_only, quintet) != nullptr;
    const bool found_rd2 = OpenMusic(rom, rd2_only, rd2) != nullptr;
    const bool found_mp2k = OpenMusic(rom, mp2k_only, mp2k) != nullptr;

    SUPERGBAMIDI_CHECK(!found_any && !found_konami && !found_rare && !found_quintet && !found_rd2 && !found_mp2k);
    SUPERGBAMIDI_CHECK(any ==
                       "no Konami, Rare, Quintet, Nintendo R&D2 or MP2K sound driver found: this game's music uses "
                       "another engine, or a driver version supergbamidi doesn't know");
    SUPERGBAMIDI_CHECK(konami ==
                       "no Konami sound driver found: this game's music uses another engine, or a driver version "
                       "supergbamidi doesn't know");
    SUPERGBAMIDI_CHECK(rare == "no Rare sound driver found (try --song-table)");
    SUPERGBAMIDI_CHECK(quintet == "no Quintet sound driver found (try --song-table)");
    SUPERGBAMIDI_CHECK(rd2 == "no Nintendo R&D2 sound driver found (try --song-table)");
    SUPERGBAMIDI_CHECK(mp2k == "no MP2K sound driver found (try --song-table)");
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
    TestRunThumb();
    konami::RunTests();
    rare::RunTests();
    quintet::RunTests();
    mp2k::RunTests();
    rd2::RunTests();

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
