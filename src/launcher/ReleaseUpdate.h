#ifndef LONELYICE_RELEASEUPDATE_H
#define LONELYICE_RELEASEUPDATE_H

#include "Http.h"
#include <filesystem>
#include <optional>
#include <string>

namespace LonelyIce::ReleaseUpdate
{
    inline constexpr char SupportUrl[] = "https://lonelyice.org/support";

    struct Release
    {
        std::string version, page, download, sha256;
        std::size_t size = 0;
    };

    struct Staged
    {
        std::filesystem::path directory;
        std::string version;
    };

    std::string CurrentVersion();
    // Empty without an error means that the installed version is current.
    std::optional<Release> Check(std::string& error, Http::Progress const& progress = {});
    std::optional<Staged> Stage(Release const& release, std::filesystem::path const& root,
                               std::string& error, Http::Progress const& progress = {});
    void Discard(Staged const& staged);

    // On Linux the terminal launcher calls this after restoring the terminal.
    // It replaces the process so the updated TUI retains the same terminal.
    bool Install(Staged const& staged, std::filesystem::path const& profile, bool tui, std::string& error);
    std::string TakeResult(std::filesystem::path const& root);
    int Apply(int argc, char** argv);
}

#endif
