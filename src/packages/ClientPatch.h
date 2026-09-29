#ifndef LONELYICE_CLIENTPATCH_H
#define LONELYICE_CLIENTPATCH_H

#include "DbcRecipes.h"
#include "Plugins.h"
#include <filesystem>
#include <string>
#include <vector>

namespace LonelyIce::ClientPatch
{
    // Name of the generated archive in Data/<locale>/; the client loads it after the stock patches.
    std::string ArchiveName(std::string const& locale);

    struct Result
    {
        bool ok = true;
        bool changed = false;               // something was written or removed
        std::vector<std::string> log;       // what was done
        std::string error;
    };

    // Builds Data/<locale>/patch-<locale>-4.MPQ for every locale of the client from the recipes (in load order)
    // and the ids given out for them. DBC tables come from the player's own stock archives. An archive that is
    // up to date is left alone; without recipes it is removed. An archive with that name that LonelyIce did not
    // write is kept as .bak.
    Result Apply(std::filesystem::path const& clientDir, std::vector<DbcRecipes::Recipe> const& recipes, DbcRecipes::IdMap const& ids);

    // Moves the archives LonelyIce wrote out of the client's sight while it lives, so tools that read client data
    // (the map extractors) see the stock tables only.
    class HiddenArchives
    {
    public:
        explicit HiddenArchives(std::filesystem::path const& clientDir);
        ~HiddenArchives();
        HiddenArchives(HiddenArchives const&) = delete;
        HiddenArchives& operator=(HiddenArchives const&) = delete;

    private:
        std::vector<std::filesystem::path> _moved;
    };

    // Reads a stock DBC table of the client ("Spell.dbc"), from the first locale found; empty when missing.
    std::vector<uint8_t> ReadStockTable(std::filesystem::path const& clientDir, std::string const& table);

    // Copies the plugins' addons into Interface/AddOns (replacing the folders) and removes addons of plugins
    // that are gone (tracked in Interface/AddOns/lonelyice-addons.txt).
    Result SyncAddons(std::filesystem::path const& clientDir, std::vector<PluginManifest> const& plugins);
}

#endif
