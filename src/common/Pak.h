#ifndef LONELYICE_PAK_H
#define LONELYICE_PAK_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// .pak: "LIPAK001" followed by one zlib stream of records [u32 path length][path, utf-8, '/'][u64 size][bytes], ending with a
// zero path length. Used to ship the SQL and default configs next to the exe as a few files.
namespace LonelyIce::Pak
{
    struct Entry
    {
        std::string path;                 // relative, '/' separated
        std::filesystem::path source;
    };

    bool Write(std::filesystem::path const& pak, std::vector<Entry> const& entries, std::string& error);

    // Calls fn for each file in order; fn returns false to stop. progress gets compressed bytes read / total.
    bool Read(std::filesystem::path const& pak, std::function<bool(std::string const& path, std::string const& data)> const& fn,
        std::string& error, std::function<void(uint64_t done, uint64_t total)> const& progress = nullptr);

    bool Extract(std::filesystem::path const& pak, std::filesystem::path const& dir, std::string& error,
        std::function<void(uint64_t done, uint64_t total)> const& progress = nullptr);
}

#endif
