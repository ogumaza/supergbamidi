// SPDX-License-Identifier: MIT

// Converts the music of GBA games with Konami's, Rare's, Quintet's or Nintendo R&D2's sound driver, or Nintendo's MP2K,
// to MIDI files and SoundFonts.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX // MinGW's libstdc++ defines it already
#define NOMINMAX
#endif
#include <windows.h>
// shellapi.h needs the types from windows.h, so it has to come second.
#include <shellapi.h>
#endif

#include "files.h"
#include "music.h"
#include "program.h"
#include "rom.h"

#ifndef SUPERGBAMIDI_VERSION
#define SUPERGBAMIDI_VERSION "dev"
#endif

namespace fs = std::filesystem;
using namespace supergbamidi;

namespace
{

// Prints the help text to `f`.
void Usage(FILE* f)
{
    std::fprintf(f,
                 "%s %s - music converter for GBA games\n"
                 "\n"
                 "usage: %s [options] <file> [<file> ...]\n"
                 "\n"
                 "Converts every song in each GBA ROM (.gba) or GSF rip (.gsflib, .minigsf) to\n"
                 "a MIDI file and a matching SoundFont, for games whose music uses Konami's,\n"
                 "Rare's, Quintet's or Nintendo R&D2's sound driver, or Nintendo's MP2K. The\n"
                 "results go in a folder beside the input file, with the same name. You can\n"
                 "also drop files on the program.\n"
                 "\n"
                 "options:\n"
                 "  -o, --output DIR      output directory (default: <file's folder>/<base name>)\n"
                 "  -n, --name NAME       base name of the output files (default: file name)\n"
                 "  -s, --songs LIST      songs to convert, e.g. 0,3,7-9 (default: all)\n"
                 "  -l, --loops N         play each song's loop N times (default: 2)\n"
                 "  -t, --tracks LIST     only convert these tracks, e.g. 4-15 (default: all; in\n"
                 "                        Konami's driver, 0-3 are the PSG channels and 4 and up\n"
                 "                        the sample voices, and in Quintet's, 0-3 are the PSG\n"
                 "                        channels and 4 and 5 the PCM channels)\n"
                 "      --single-sf2      write one SoundFont for all songs\n"
                 "      --voice-channels  give each of the driver's sound channels a MIDI channel\n"
                 "                        (MP2K), so that notes stop where the game cuts them\n"
                 "                        off, though a track's notes then move between channels\n"
                 "      --dump            also write a text listing of each song's sequence data\n"
                 "      --info            print each driver's detected tables and songs, then\n"
                 "                        exit\n"
                 "      --driver NAME     use konami, rare, quintet, rd2 or mp2k instead of\n"
                 "                        detecting the driver\n"
                 "      --song-table ADDR    use this song table address (hex; in Nintendo R&D2's\n"
                 "                           driver, the address of the game's settings for it)\n"
                 "      --song-count N       use this many songs\n"
                 "      --sample-table ADDR  use this sample table address (hex; Konami's driver)\n"
                 "      --mix-rate HZ        use this DirectSound mixer rate (Konami's driver)\n"
                 "      --trace SONG      print the driver model's state after each frame of SONG:\n"
                 "                        in Konami's driver, each track's output (frame track\n"
                 "                        pitch b2 vol trig flags key, and 2 pan fields, which\n"
                 "                        depend on the revision); in Rare's, each note slot\n"
                 "                        that plays (frame slot state channel key pitch-key\n"
                 "                        velocity phase level instrument position fraction);\n"
                 "                        in Quintet's, each write to the sound, DMA and timer\n"
                 "                        registers (frame address size value, with frame -1\n"
                 "                        for the song's start); in MP2K, each sound channel\n"
                 "                        that plays (frame channel status track key pitch-key\n"
                 "                        velocity priority level right left, then rate sample\n"
                 "                        count position fraction, or a PSG channel's frequency\n"
                 "                        goal counter sustain pan wave); in Nintendo R&D2's,\n"
                 "                        each write to the PSG's registers (frame address size\n"
                 "                        value), each voice that plays (frame voice state type\n"
                 "                        track priority note velocity, and the rest of its\n"
                 "                        record) and the order the mixer takes them in\n"
                 "      --trace-frames N  frames to trace (default: 3000)\n"
                 "  -q, --quiet           only print warnings and errors\n"
                 "  -h, --help            show this help\n"
                 "      --version         show the version\n",
                 kProgramName, SUPERGBAMIDI_VERSION, kProgramName);
}

struct Options
{
    std::vector<std::string> inputs;
    std::string out_dir, name;
    std::set<int> songs;
    ConvertSettings convert;
    Overrides overrides;
    bool info = false, dump = false, quiet = false, single_sf2 = false;
    int trace_song = -1;
    long long trace_frames = 3000;
};

// Prints an error about the options, after the program's name.
void OptionError(const std::string& message)
{
    std::fprintf(stderr, "%s: %s\n", kProgramName, message.c_str());
}

// Parses the whole of `s` as a number.
bool ParseNumber(const std::string& s, long long& out, int base)
{
    char* end = nullptr;
    out = std::strtoll(s.c_str(), &end, base);
    return !s.empty() && *end == 0;
}

// Parses the whole of `s` as a decimal number.
bool ParseNumber(const std::string& s, double& out)
{
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return !s.empty() && *end == 0;
}

// Parses a list such as "0,3,7-9" into `values`.
bool ParseList(const std::string& s, std::set<int>& values)
{
    size_t start = 0;
    while (start <= s.size())
    {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos)
        {
            comma = s.size();
        }

        // An item is a number or a range: "7" or "7-9".
        const std::string item = s.substr(start, comma - start);
        const size_t dash = item.find('-');
        const std::string first = item.substr(0, dash);
        const std::string last = dash == std::string::npos ? first : item.substr(dash + 1);
        long long a = 0, b = 0;
        if (!ParseNumber(first, a, 10) || !ParseNumber(last, b, 10) || a < 0 || b < a || b > 4096)
        {
            return false;
        }

        for (long long i = a; i <= b; i++)
        {
            values.insert(int(i));
        }

        start = comma + 1;
    }

    return true;
}

// Returns 0 to go on, -1 if there's nothing more to do (after --help or --version), or the exit code.
int ParseArgs(const std::vector<std::string>& args, Options& o)
{
    for (size_t i = 1; i < args.size(); i++)
    {
        auto value = [&](const char* what, std::string& out)
        {
            if (i + 1 >= args.size())
            {
                OptionError(std::string(what) + " needs a value");
                return false;
            }

            out = args[++i];

            return true;
        };

        const std::string& a = args[i];
        std::string v;
        long long n;
        if (a == "-h" || a == "--help")
        {
            Usage(stdout);
            return -1;
        }
        else if (a == "--version")
        {
            std::printf("%s %s\n", kProgramName, SUPERGBAMIDI_VERSION);
            return -1;
        }
        else if (a == "-o" || a == "--output")
        {
            if (!value("--output", o.out_dir))
            {
                return 2;
            }
        }
        else if (a == "-n" || a == "--name")
        {
            if (!value("--name", o.name))
            {
                return 2;
            }
        }
        else if (a == "-s" || a == "--songs")
        {
            if (!value("--songs", v))
            {
                return 2;
            }
            if (!ParseList(v, o.songs))
            {
                OptionError("bad song list");
                return 2;
            }
        }
        else if (a == "-l" || a == "--loops")
        {
            if (!value("--loops", v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 10) || n < 1 || n > 100)
            {
                OptionError("--loops must be between 1 and 100");
                return 2;
            }

            o.convert.loops = int(n);
        }
        else if (a == "-t" || a == "--tracks")
        {
            if (!value("--tracks", v))
            {
                return 2;
            }

            std::set<int> tracks;
            if (!ParseList(v, tracks) || tracks.empty() || *tracks.rbegin() >= 16)
            {
                OptionError("bad track list (tracks are 0-15)");
                return 2;
            }

            o.convert.track_mask = 0;
            for (int t : tracks)
            {
                o.convert.track_mask |= uint16_t(1u << t);
            }
        }
        else if (a == "--single-sf2")
        {
            o.single_sf2 = true;
        }
        else if (a == "--voice-channels")
        {
            o.convert.voice_channels = true;
        }
        else if (a == "--dump")
        {
            o.dump = true;
        }
        else if (a == "--info")
        {
            o.info = true;
        }
        else if (a == "--driver")
        {
            if (!value("--driver", v))
            {
                return 2;
            }

            if (v == "konami")
            {
                o.overrides.driver = Driver::kKonami;
            }
            else if (v == "rare")
            {
                o.overrides.driver = Driver::kRare;
            }
            else if (v == "quintet")
            {
                o.overrides.driver = Driver::kQuintet;
            }
            else if (v == "rd2")
            {
                o.overrides.driver = Driver::kRd2;
            }
            else if (v == "mp2k")
            {
                o.overrides.driver = Driver::kMp2k;
            }
            else
            {
                OptionError("--driver needs konami, rare, quintet, rd2 or mp2k");
                return 2;
            }
        }
        else if (a == "--song-table" || a == "--sample-table")
        {
            if (!value(a.c_str(), v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 16) || n <= 0 || n > 0xFFFFFFFF)
            {
                OptionError(a + " needs a hex address");
                return 2;
            }

            (a == "--song-table" ? o.overrides.song_table : o.overrides.sample_table) = uint32_t(n);
        }
        else if (a == "--song-count")
        {
            if (!value("--song-count", v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 10) || n < 1 || n > 4096)
            {
                OptionError("--song-count needs a number from 1 to 4096");
                return 2;
            }

            o.overrides.song_count = int(n);
        }
        else if (a == "--mix-rate")
        {
            if (!value("--mix-rate", v))
            {
                return 2;
            }

            // The same range that Konami's driver detection accepts from the driver's timer setting.
            double& rate = o.overrides.mix_rate;
            if (!ParseNumber(v, rate) || !(rate >= 4000 && rate <= 65536))
            {
                OptionError("--mix-rate needs a rate from 4000 to 65536 Hz");
                return 2;
            }
        }
        else if (a == "--trace")
        {
            if (!value("--trace", v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 10) || n < 0 || n > std::numeric_limits<int>::max())
            {
                OptionError("--trace needs a song number");
                return 2;
            }

            o.trace_song = int(n);
        }
        else if (a == "--trace-frames")
        {
            if (!value("--trace-frames", v))
            {
                return 2;
            }
            if (!ParseNumber(v, o.trace_frames, 10) || o.trace_frames < 1)
            {
                OptionError("--trace-frames needs a positive number");
                return 2;
            }
        }
        else if (a == "-q" || a == "--quiet")
        {
            o.quiet = true;
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            OptionError("unknown option " + a + " (see --help)");
            return 2;
        }
        else
        {
            o.inputs.push_back(a);
        }
    }

    if (o.inputs.empty())
    {
        Usage(stderr);
        return 2;
    }
    if (o.inputs.size() > 1 && (!o.name.empty() || o.trace_song >= 0))
    {
        OptionError("--name and --trace take a single input file");
        return 2;
    }

    // Only Konami's driver has a sample table and a mixer rate to override.
    if (o.overrides.sample_table || o.overrides.mix_rate > 0)
    {
        if (o.overrides.driver != Driver::kAny && o.overrides.driver != Driver::kKonami)
        {
            OptionError("--sample-table and --mix-rate are for Konami's driver");
            return 2;
        }

        o.overrides.driver = Driver::kKonami;
    }

    return 0;
}

// Returns a time as minutes and seconds, such as 1:35.23. It's rounded to hundredths before it's split, so that 59.999
// seconds reads 1:00.00 and not 0:60.00.
std::string TimeString(double seconds)
{
    const long long hundredths = std::llround(seconds * 100);
    char b[32];
    std::snprintf(b, sizeof b, "%lld:%05.2f", hundredths / 6000, double(hundredths % 6000) / 100);

    return b;
}

// Prints the --info report: the ROM's title, then for each driver the game has, its detection log and warnings,
// followed by each song's data address, note-playing track count, length and loop points, calculated with the current
// conversion options. The first driver is the one a conversion uses, and with --driver it's the only one.
void PrintInfo(const Rom& rom, const std::vector<std::unique_ptr<Music>>& drivers, const Options& o)
{
    std::printf("ROM: %s (%s)%s\n", rom.Title().c_str(), rom.GameCode().c_str(), rom.FromGsf() ? ", from GSF" : "");

    for (size_t d = 0; d < drivers.size(); d++)
    {
        if (d > 0)
        {
            std::printf("\n");
        }

        const Music& music = *drivers[d];
        for (const std::string& l : music.Log())
        {
            std::printf("  %s\n", l.c_str());
        }
        for (const std::string& w : music.Warnings())
        {
            std::printf("  warning: %s\n", w.c_str());
        }

        std::printf("\n song  address     tracks  length     loop\n");
        for (int s = 0; s < music.SongCount(); s++)
        {
            if (!music.HasSong(s))
            {
                std::printf("  %3d  (empty)\n", s);
                continue;
            }

            const SongReport r = music.InspectSong(s, o.convert);
            std::string loop = "-";
            if (r.loop_end >= 0)
            {
                loop = TimeString(r.loop_start) + " - " + TimeString(r.loop_end);
            }

            std::printf("  %3d  0x%08X  %6d  %-9s  %s\n", s, unsigned(r.address), r.tracks,
                        TimeString(r.seconds).c_str(), loop.c_str());
        }
    }
}

// Prints the driver model's state after each frame of the song --trace names, which the driver's
// tools/*/compare_trace.py compares with the driver.
int Trace(const Music& music, const Options& o)
{
    std::vector<std::string> warnings;
    if (!music.Trace(o.trace_song, o.trace_frames, stdout, warnings))
    {
        std::fprintf(stderr, "%s: song %d is invalid\n", kProgramName, o.trace_song);
        return 1;
    }

    for (const std::string& w : music.Warnings())
    {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    }
    for (const std::string& w : warnings)
    {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    }

    return 0;
}

// Reports the conversion result, including songs skipped because they play no notes.
void ReportSong(int song, const SongReport& r, uint16_t track_mask)
{
    if (r.silent)
    {
        std::printf("song %2d: %s, skipped\n", song,
                    track_mask == 0xFFFF ? "plays no notes" : "no notes on the chosen tracks");
        return;
    }

    std::string loop;
    if (r.loop_end >= 0)
    {
        loop = ", loop " + TimeString(r.loop_start) + "-" + TimeString(r.loop_end);
    }

    const std::string midi = Utf8(PathFromUtf8(r.midi_path).filename());
    const std::string sf2 = r.sf2_path.empty() ? "" : ", " + Utf8(PathFromUtf8(r.sf2_path).filename());
    std::printf("song %2d: %s%s, %d track%s, %.1f BPM -> %s%s\n", song, TimeString(r.seconds).c_str(), loop.c_str(),
                r.tracks, r.tracks == 1 ? "" : "s", r.bpm, midi.c_str(), sf2.c_str());
}

// Converts one input file, which messages call `label`. `done` holds the ROM images already converted in this run, so
// dropping several mini-GSFs of one set converts it once.
int ConvertFile(const std::string& input, const std::string& label, const Options& o, std::vector<fs::path>& done)
{
    Rom rom;
    std::string error;
    if (!rom.Load(input, error))
    {
        std::fprintf(stderr, "%s: %s\n", label.c_str(), error.c_str());
        return 1;
    }

    const std::unique_ptr<Music> music = OpenMusic(rom, o.overrides, error);
    if (!music)
    {
        std::fprintf(stderr, "%s: %s\n", label.c_str(), error.c_str());
        return 1;
    }

    if (o.info)
    {
        // A game can have more than one driver, and the report covers each.
        PrintInfo(rom, OpenAllMusic(rom, o.overrides), o);
        return 0;
    }
    if (o.trace_song >= 0)
    {
        return Trace(*music, o);
    }

    // A mini-GSF only selects a song of its library, so the output is the library's music and is named after it.
    const fs::path source = PathFromUtf8(rom.GsfLibrary().empty() ? input : rom.GsfLibrary());

    // Compare file identities so case differences and links don't cause duplicate conversions.
    std::error_code ec;
    for (const fs::path& previous : done)
    {
        if (fs::equivalent(source, previous, ec))
        {
            if (!o.quiet)
            {
                std::printf("%s: same music as %s, already converted\n", label.c_str(),
                            Utf8(source.filename()).c_str());
            }

            return 0;
        }
    }
    done.push_back(source);

    const std::string name = SafeFileName(o.name.empty() ? Utf8(source.stem()) : o.name);
    const fs::path out_dir = o.out_dir.empty() ? source.parent_path() / PathFromUtf8(name) : PathFromUtf8(o.out_dir);
    fs::create_directories(out_dir, ec);
    if (ec)
    {
        std::fprintf(stderr, "%s: can't create %s: %s\n", label.c_str(), Utf8(out_dir).c_str(), ec.message().c_str());
        return 1;
    }

    ConvertSettings settings = o.convert;
    settings.out_dir = Utf8(out_dir);
    settings.base_name = name;

    // Report what detection found. Its warnings are printed even with --quiet, which leaves out the line that names the
    // file, so then the warnings and errors about the file start with its name.
    const std::string prefix = o.quiet ? label + ": " : "  ";
    if (!o.quiet)
    {
        std::printf("%s: %s (%s), %d song%s\n", label.c_str(), rom.Title().c_str(), rom.GameCode().c_str(),
                    music->SongCount(), music->SongCount() == 1 ? "" : "s");
        for (const std::string& l : music->Log())
        {
            std::printf("  %s\n", l.c_str());
        }
    }
    for (const std::string& w : music->Warnings())
    {
        std::fprintf(stderr, "%swarning: %s\n", prefix.c_str(), w.c_str());
    }
    if (o.convert.voice_channels && !music->SupportsVoiceChannels())
    {
        std::fprintf(stderr, "%swarning: --voice-channels only changes MP2K's conversions, so it's ignored\n",
                     prefix.c_str());
    }

    if (o.single_sf2)
    {
        music->ShareSoundfont();
    }

    int failures = 0, converted = 0, silent = 0, selected = 0;
    for (int s = 0; s < music->SongCount(); s++)
    {
        if ((!o.songs.empty() && !o.songs.count(s)) || !music->HasSong(s))
        {
            continue;
        }

        selected++;
        const SongReport r = music->ConvertSong(s, settings);
        if (!r.ok)
        {
            failures++;
        }
        else
        {
            (r.silent ? silent : converted)++;
            if (!o.quiet)
            {
                ReportSong(s, r, o.convert.track_mask);
            }
        }

        for (const std::string& w : r.warnings)
        {
            std::fprintf(stderr, "%ssong %d: %s\n", prefix.c_str(), s, w.c_str());
        }

        if (o.dump)
        {
            char file[32];
            std::snprintf(file, sizeof file, "_%02d.txt", s);
            if (!music->DumpSong(s, Utf8(out_dir / PathFromUtf8(name + file)), error))
            {
                std::fprintf(stderr, "%ssong %d: %s\n", prefix.c_str(), s, error.c_str());
                failures++;
            }
        }
    }

    if (o.single_sf2 && converted > 0)
    {
        const fs::path path = out_dir / PathFromUtf8(name + ".sf2");
        if (!music->WriteSharedSoundfont(Utf8(path), error))
        {
            std::fprintf(stderr, "%s: %s\n", label.c_str(), error.c_str());
            failures++;
        }
        else if (!o.quiet)
        {
            std::printf("soundfont -> %s\n", Utf8(path.filename()).c_str());
        }
    }

    if (selected == 0)
    {
        std::fprintf(stderr, "%s: no songs selected\n", label.c_str());
        return 1;
    }

    if (!o.quiet)
    {
        std::printf("converted %d song%s", converted, converted == 1 ? "" : "s");
        if (silent)
        {
            std::printf(" (%d without notes skipped)", silent);
        }
        if (failures)
        {
            std::printf(", %d failed", failures);
        }
        std::printf("\noutput: %s\n", Utf8(fs::absolute(out_dir, ec).lexically_normal()).c_str());
    }

    return failures ? 1 : 0;
}

// Converts every input the arguments name. Returns the exit code.
int Run(const std::vector<std::string>& args)
{
#ifndef _WIN32
    // Keep progress and errors in order when both go to one pipe (the macOS droplet).
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif

    Options o;
    const int parsed = ParseArgs(args, o);
    if (parsed)
    {
        return parsed < 0 ? 0 : parsed;
    }

    std::vector<fs::path> done;
    int result = 0;
    for (size_t i = 0; i < o.inputs.size(); i++)
    {
        if (i > 0 && !o.quiet)
        {
            std::printf("\n");
        }

        // On Windows a path that isn't valid UTF-8, such as a GSF's _lib name in another encoding, throws, and so does
        // running out of memory. That ends this file's conversion, but not the others'. The message starts with the
        // file's name, like the first line about any file, which the macOS droplet relies on.
        std::string label = o.inputs[i];
        try
        {
            label = Utf8(PathFromUtf8(o.inputs[i]).filename());
            if (ConvertFile(o.inputs[i], label, o, done))
            {
                result = 1;
            }
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", label.c_str(), e.what());
            result = 1;
        }
    }

    return result;
}

#ifdef _WIN32
// Returns the program's arguments in UTF-8. They're read as wide strings, so that paths outside the ANSI code page
// survive.
std::vector<std::string> CommandLine()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; argv && i < argc; i++)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &s[0], n, nullptr, nullptr);
        }

        args.push_back(s);
    }

    if (argv)
    {
        LocalFree(argv);
    }

    return args;
}

// Waits for Enter if the program has a console window of its own. Dropping files on the .exe (or double-clicking it)
// gives it one, which closes as soon as the program exits, so this keeps it open for the results to be read.
void WaitIfOwnConsole()
{
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1)
    {
        std::printf("\nPress Enter to close this window.");
        std::fflush(stdout);
        std::getchar();
    }
}
#endif

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    const int result = Run(CommandLine());
    WaitIfOwnConsole();
    (void)argc;
    (void)argv;

    return result;
#else
    return Run(std::vector<std::string>(argv, argv + argc));
#endif
}
