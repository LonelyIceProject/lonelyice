#ifndef LONELYICE_PACKAGEMANAGER_H
#define LONELYICE_PACKAGEMANAGER_H

#include "Http.h"
#include "Plugins.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Plugin packages (docs/plugin-format.md, 8): an index of packages, dependency resolution, and installing,
// updating, removing, enabling and disabling plugins in a plugins folder. Changes to files only: the databases
// and the client follow on the next server start (or LonelyIce --server --apply).
namespace LonelyIce::Packages
{
    // One package of an index.
    struct Package
    {
        std::string id, version, name, description, core;
        std::vector<std::string> platforms;                        // empty: runs everywhere (no server code)
        std::vector<std::string> locales;                          // as in plugin.json: languages, "*", empty: not stated
        std::map<std::string, std::string> depends;                // id -> version range
        std::vector<std::string> conflicts;
        std::string url, sha256;                                   // url already resolved against the index
        std::string icon;                                          // resolved like url, empty: none
        std::string page;                                          // the package's page in the catalog, resolved like url
        std::string source;                                        // index it came from
        uint64_t size = 0;
    };

    // One package index: an http(s) URL, a file or a folder with index.json.
    struct Source
    {
        std::string location, name;     // name: the index's "name", empty when it has none
        bool ok = false;
        std::string error;
        std::size_t packages = 0;
    };

    // A plugin in the plugins folder, enabled (plugins/<id>) or disabled (plugins/.disabled/<id>).
    struct Local
    {
        PluginManifest manifest;
        bool enabled = true;
        std::vector<std::string> conflicts;
    };

    // What installing or updating will do.
    struct Step
    {
        Package package;
        std::string from;       // installed version, empty for a new plugin
    };

    struct Plan
    {
        std::vector<Step> steps;        // dependencies first
        std::string error;
    };

    class Manager
    {
    public:
        explicit Manager(std::filesystem::path pluginsDir);

        std::filesystem::path const& Dir() const { return _dir; }

        // Reads the indexes (";"-separated URLs, files or folders); packages for another core or platform are left
        // out. An index that cannot be read is skipped (see Sources()); false only when none could be read.
        bool LoadIndex(std::string const& sources, std::string& error, Http::Progress const& progress = {});
        std::vector<Package> const& Available() const { return _available; }
        std::vector<Source> const& Sources() const { return _sources; }

        // Icons of the available packages, downloaded into IconCache(); already cached ones are kept.
        void FetchIcons();
        std::filesystem::path IconCache() const { return _dir / ".cache" / "icons"; }
        std::filesystem::path IconFile(Package const& p) const { return IconCache() / (p.id + "-" + p.version + ".png"); }

        static std::vector<std::string> SplitSources(std::string const& sources);
        // Where an index is read from: a folder means its index.json.
        static std::string IndexLocation(std::string const& source);

        std::vector<Local> Installed() const;

        // Newest compatible versions of the requested plugins (id -> range, "*" for any) and everything they
        // need; installed plugins stay unless update is set or a range needs another version.
        Plan Resolve(std::map<std::string, std::string> const& requests, bool update) const;
        // All installed plugins that have a newer compatible version.
        Plan ResolveUpdates() const;

        // Downloads, checks and unpacks the packages of a plan.
        bool Install(Plan const& plan, std::string& error, std::function<void(std::string const&)> const& log = {},
            Http::Progress const& progress = {});

        // Plugins that need id (directly or not) among the enabled ones.
        std::vector<std::string> Dependents(std::string const& id) const;
        bool Remove(std::string const& id, std::string& error);
        bool SetEnabled(std::string const& id, bool enabled, std::string& error);

        // Packs a plugin folder into <out>/<id>-<version>.zip; prints the index entry into entry (JSON).
        static bool Pack(std::filesystem::path const& pluginDir, std::filesystem::path const& outDir, std::string& entry, std::string& error);

        static std::string CoreAbi();
        static std::string Platform();
        static int CompareVersions(std::string const& a, std::string const& b);

    private:
        std::filesystem::path _dir;
        std::vector<Package> _available;
        std::vector<Source> _sources;
    };
}

#endif
