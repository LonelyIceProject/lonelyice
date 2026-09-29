#ifndef LONELYICE_CLIENTARCHIVES_H
#define LONELYICE_CLIENTARCHIVES_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The game client's MPQ archives. Client files are found by their Windows names whatever their case on disk.
namespace LonelyIce::ClientArchives
{
    std::filesystem::path DataDir(std::filesystem::path const& clientDir);

    // Locales the client has (enGB, ruRU), spelled as the client does even when the folder on disk is lowercase.
    std::vector<std::string> Locales(std::filesystem::path const& clientDir);

    // The client's own archives for a locale, highest priority first; our patch archive is not among them.
    std::vector<std::filesystem::path> Chain(std::filesystem::path const& clientDir, std::string const& locale);

    // One archive, read only.
    class Archive
    {
    public:
        explicit Archive(std::filesystem::path const& path);
        ~Archive();
        Archive(Archive const&) = delete;
        Archive& operator=(Archive const&) = delete;

        bool IsOpen() const { return _handle != nullptr; }
        std::optional<std::vector<uint8_t>> Read(std::string const& name) const;

    private:
        void* _handle = nullptr;
    };

    // A locale's archive chain opened once; a file comes from the first archive that has it. Safe to use from
    // several threads.
    class Reader
    {
    public:
        Reader(std::filesystem::path const& clientDir, std::string const& locale);

        bool IsOpen() const { return !_archives.empty(); }
        std::string const& Locale() const { return _locale; }
        std::optional<std::vector<uint8_t>> Read(std::string const& name) const;

    private:
        std::string _locale;
        std::vector<std::unique_ptr<Archive>> _archives;
        mutable std::mutex _lock;
    };
}

#endif
