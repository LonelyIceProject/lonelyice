// Client data extraction, run by the launcher as child processes of the same exe:
//   LonelyIce --tool maps <client dir> <data dir> [what] dbc, Cameras, maps (what: extractor mask, 7 = all three)
//   LonelyIce --tool tiles <client dir> <data dir> <locale>   maps built from the client's ADTs (as the server does)
//   LonelyIce --tool vmaps <client dir> <data dir>       <data>/Buildings (raw models)
//   LonelyIce --tool assemble <data dir>                 Buildings -> vmaps
//   LonelyIce --tool mmaps <data dir> <threads>          mmaps
// Output goes to stdout (the launcher reads it for progress). Each tool keeps process-wide state (globals, chdir,
// exit()), which is why they run in their own process.

#include "Assets.h"
#include "ClientData.h"
#include "GameClient.h"
#include "Platform.h"
#include "TileAssembler.h"
#include "../../../tools/mmaps_generator/MapBuilder.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int MapExtractorMain(int argc, char** argv);
int VmapExtractorMain(int argc, char** argv);

namespace fs = std::filesystem;

namespace
{
    void SetupStdio()
    {
        LonelyIce::Platform::UseParentConsole();
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }

    int Run(int (*fn)(int, char**), std::vector<std::string> args)
    {
        std::vector<char*> argv;
        for (std::string& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        return fn(int(args.size()), argv.data());
    }

    // The tools print paths with fopen/printf, so the process works in the ANSI code page on Windows (UTF-8 elsewhere):
    // arguments arrive as such, and fs::path(std::string) reads them the same way.
    std::string Slash(std::string p)
    {
        if (!p.empty() && p.back() != '\\' && p.back() != '/')
            p += char(fs::path::preferred_separator);
        return p;
    }

    int Maps(std::string const& client, std::string const& data, std::string const& what)
    {
        if (client.size() >= 120 || data.size() >= 120)
        {
            printf("@@LI fail path too long (map_extractor allows 127 characters)\n");
            return 2;
        }
        std::error_code ec;
        fs::create_directories(data, ec);
        return Run(MapExtractorMain, { "map_extractor", "-i", client, "-o", data, "-e", what });
    }

    int Tiles(std::string const& client, std::string const& data, std::string const& locale)
    {
        std::string error;
        bool const ok = LonelyIce::ClientData::BuildAllTiles(fs::path(client), locale, fs::path(data),
            [](uint32_t done, uint32_t total) { printf("@@LI tiles %u %u\n", done, total); }, error);
        if (!ok)
            printf("@@LI fail %s\n", error.c_str());
        return ok ? 0 : 1;
    }

    int Vmaps(std::string const& client, std::string const& data)
    {
        // Output is ./Buildings; a leftover folder makes the extractor stop and wait for a key.
        std::error_code ec;
        fs::remove_all(fs::path(data) / "Buildings", ec);
        fs::create_directories(data, ec);
        fs::current_path(fs::path(data), ec);
        if (ec)
        {
            printf("@@LI fail cannot enter %s\n", data.c_str());
            return 2;
        }
        // The client's Data folder, whatever its case on disk (Linux, macOS).
        std::string const clientData = LonelyIce::GameClient::Child(fs::path(client), "Data").string();
        return Run(VmapExtractorMain, { "vmap4_extractor", "-d", Slash(clientData) });
    }

    int Assemble(std::string const& data)
    {
        fs::path root(data);
        std::error_code ec;
        fs::create_directories(root / "vmaps", ec);
        VMAP::TileAssembler ta((root / "Buildings").string(), (root / "vmaps").string());
        if (!ta.convertWorld2())
        {
            printf("@@LI fail vmap assembler\n");
            return 1;
        }
        fs::remove_all(root / "Buildings", ec);
        return 0;
    }

    int Mmaps(std::string const& data, unsigned threads)
    {
        fs::path root(data);
        std::error_code ec;
        if (fs::is_empty(root / "maps", ec) || !fs::exists(root / "vmaps", ec))
        {
            printf("@@LI fail maps and vmaps are needed first\n");
            return 2;
        }
        fs::create_directories(root / "mmaps", ec);

        // The generator reads its settings from a yaml file; ours is embedded, with dataDir pointing at the data folder.
        std::string yaml;
        LonelyIce::AssetFileInterface assets;
        if (Rml::FileHandle f = assets.Open("tools/mmaps-config.yaml"))
        {
            yaml.resize(assets.Length(f));
            assets.Read(yaml.data(), yaml.size(), f);
            assets.Close(f);
        }
        std::string dir = root.generic_string();
        if (dir.back() != '/')
            dir += '/';
        std::size_t at = yaml.find("dataDir:");
        if (at == std::string::npos)
        {
            printf("@@LI fail mmaps config\n");
            return 2;
        }
        std::size_t eol = yaml.find('\n', at);
        yaml.replace(at, eol - at, "dataDir: \"" + dir + "\"");
        fs::path configFile = root / "mmaps-config.yaml";
        {
            std::ofstream out(configFile, std::ios::binary | std::ios::trunc);
            out << yaml;
        }

        std::optional<MMAP::Config> config = MMAP::Config::FromFile(configFile.string());
        if (!config)
        {
            printf("@@LI fail mmaps config\n");
            return 2;
        }
        {
            MMAP::MapBuilder builder(&config.value(), -1, std::max(1u, threads));
            builder.buildMaps({});
        }
        fs::remove(configFile, ec);
        return 0;
    }
}

int ToolMain(int argc, char** argv)
{
    SetupStdio();
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) != "--tool")
            a.emplace_back(argv[i]);

    int rc = 1;
    if (a.size() >= 3 && a[0] == "maps")
        rc = Maps(a[1], a[2], a.size() >= 4 ? a[3] : std::string("7"));
    else if (a.size() >= 4 && a[0] == "tiles")
        rc = Tiles(a[1], a[2], a[3]);
    else if (a.size() >= 3 && a[0] == "vmaps")
        rc = Vmaps(a[1], a[2]);
    else if (a.size() >= 2 && a[0] == "assemble")
        rc = Assemble(a[1]);
    else if (a.size() >= 3 && a[0] == "mmaps")
        rc = Mmaps(a[1], unsigned(std::max(1, atoi(a[2].c_str()))));
    else
        printf("@@LI fail unknown tool\n");

    printf("@@LI exit %d\n", rc);
    fflush(stdout);
    return rc;
}
