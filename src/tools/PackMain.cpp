// Release packer, run at build time: LonelyIce --pack <source root> <out dir>
// Writes <out>/sql.pak (everything the database updater reads) and <out>/configs.pak (default configs).

#include "Pak.h"
#include "Platform.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using LonelyIce::Pak::Entry;

namespace
{
    std::string Utf8(std::u8string const& s)
    {
        return std::string(s.begin(), s.end());
    }

    // SQL the server never reads: MySQL create scripts, and archived updates (only hashed, warnings if absent).
    bool Skip(fs::path const& rel)
    {
        for (fs::path const& part : rel)
        {
            std::string s = part.string();
            if (s == "create" || s == "old" || s == "archive")
                return true;
        }
        return false;
    }

    void AddTree(std::vector<Entry>& out, fs::path const& root, fs::path const& dir, bool sqlOnly)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return;
        std::vector<Entry> found;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;
            fs::path rel = fs::relative(it->path(), root, ec);
            if (Skip(rel) || (sqlOnly && it->path().extension() != ".sql"))
                continue;
            found.push_back({ Utf8(rel.generic_u8string()), it->path() });
        }
        std::sort(found.begin(), found.end(), [](Entry const& a, Entry const& b) { return a.path < b.path; });
        out.insert(out.end(), found.begin(), found.end());
    }
}

int PackMain(int argc, char** argv)
{
    LonelyIce::Platform::UseParentConsole();

    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) != "--pack")
            a.emplace_back(argv[i]);
    if (a.size() < 2)
    {
        printf("usage: LonelyIce --pack <source root> <out dir>\n");
        return 1;
    }
    fs::path src = fs::u8path(a[0]), dst = fs::u8path(a[1]);
    std::error_code ec;
    fs::create_directories(dst, ec);

    std::vector<Entry> sql;
    for (char const* d : { "data/sql/base", "data/sql/updates", "data/sql/custom", "data/sql/overrides" })
        AddTree(sql, src, src / d, true);
    std::vector<Entry> configs = { { "worldserver.conf.dist", src / "src/server/apps/worldserver/worldserver.conf.dist" } };
    for (fs::directory_iterator it(src / "modules", ec), end; !ec && it != end; it.increment(ec))
    {
        AddTree(sql, src, it->path() / "data" / "sql", true);
        for (fs::directory_iterator c(it->path() / "conf", ec), cend; !ec && c != cend; c.increment(ec))
            if (c->path().filename().string().ends_with(".conf.dist"))
                configs.push_back({ "modules/" + Utf8(c->path().filename().u8string()), c->path() });
        ec.clear();
    }

    std::string error;
    for (auto const& [name, entries] : { std::pair{ "sql.pak", &sql }, std::pair{ "configs.pak", &configs } })
    {
        uint64_t bytes = 0;
        for (Entry const& e : *entries)
            bytes += fs::file_size(e.source, ec);
        printf("%s: %zu files, %llu MB\n", name, entries->size(), (unsigned long long)(bytes >> 20));
        if (!LonelyIce::Pak::Write(dst / name, *entries, error))
        {
            printf("error: %s\n", error.c_str());
            return 1;
        }
        printf("  -> %llu MB\n", (unsigned long long)(fs::file_size(dst / name, ec) >> 20));
    }
    return 0;
}
