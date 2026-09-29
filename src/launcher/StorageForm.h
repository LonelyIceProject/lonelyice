#ifndef LONELYICE_STORAGEFORM_H
#define LONELYICE_STORAGEFORM_H

#include "LauncherSettings.h"
#include "Plugins.h"
#include "StorageCheck.h"
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Rml
{
    class DataModelConstructor;
}

namespace LonelyIce
{
    // Where the server keeps its data: the databases' location and the game data cache (LauncherSettings).
    struct StorageChoice
    {
        std::string location = "local";
        bool cache = false;
        RemoteDatabase remote;

        bool Remote() const { return location != "local"; }
        bool operator==(StorageChoice const&) const = default;
        // The same storage for the server (a connection typed for another location does not count).
        bool Same(StorageChoice const& o) const { return location == o.location && cache == o.cache && (!Remote() || remote == o.remote); }
    };

    // A plugin's storage with the folder it is installed in.
    struct StorageProviderInfo
    {
        StorageProvider provider;
        std::filesystem::path dir;
    };

    // Install steps that make a storage hold what the server needs.
    struct StoragePlan
    {
        bool db = false;        // create the databases (or bring existing ones up to date)
        bool unpack = false;    // fill the game data cache
        bool pack = false;      // drop a cache that is no longer wanted

        bool Empty() const { return !db && !unpack && !pack; }
    };

    // Maps and cameras extracted into dataDir (not terrain tiles built from the client, which carry a stamp).
    bool HasUnpackedFiles(std::filesystem::path const& dataDir);
    StoragePlan PlanStorage(StorageState const& state, bool cache, std::filesystem::path const& dataDir);

    // The storage choice of the wizard and of the settings page: the databases' location (the built-in files or a
    // plugin's database server with its connection), the cache switch, and a check of the chosen place that runs
    // by itself when the place changes. Model names start with the prefix given (two forms share one model).
    class StorageForm
    {
    public:
        struct Host
        {
            std::filesystem::path exe;
            std::function<std::vector<StorageProviderInfo>()> providers;
            // databases of the choice for the check (the server folder's files for "local")
            std::function<Platform::Env(StorageChoice const&)> env;
            std::function<void()> wake;
        };

        StorageForm(std::string prefix, Host host);

        // registerTypes: the first form bound to a model registers the row type
        void Bind(Rml::DataModelConstructor& c, bool registerTypes);
        void SetModel(Rml::DataModelHandle model) { _model = model; }

        // Fills the form (the list of places is read again: plugins may have come or gone) and checks the place.
        void Load(StorageChoice const& choice);
        StorageChoice Choice() const;
        std::string LocationTitle(std::string const& location) const;

        // Checks the place of the form now.
        void Check();
        bool Checking() const { return _check.IsRunning(); }
        // The check of the form's current place, empty while it runs or after the place changed.
        std::optional<StorageState> const& Result() const { return _result; }
        // Takes a finished check and starts one after the place was edited; true when the result changed.
        bool Tick();
        void Relocalize();

        struct LocRow { Rml::String id, title, detail; };

    private:
        void Refresh();
        void Dirty();
        bool Checkable(StorageChoice const& c) const;

        std::string _p;
        Host _host;
        Rml::DataModelHandle _model;
        StorageCheck _check;
        std::vector<StorageProviderInfo> _providers;

        std::vector<LocRow> _locs;
        Rml::String _loc = "local", _host_, _port, _user, _pass, _prefix, _state, _note, _cacheDesc;
        bool _cache = false, _remote = false;

        std::optional<StorageState> _result;
        std::optional<StorageChoice> _checked;     // what the running or last check was for
        StorageChoice _seen;                        // the form at the last Tick, for the pause after typing
        uint64_t _editedAt = 0;
    };
}

#endif
