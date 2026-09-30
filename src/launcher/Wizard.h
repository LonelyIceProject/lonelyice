#ifndef LONELYICE_WIZARD_H
#define LONELYICE_WIZARD_H

#include "GameClient.h"
#include "Installer.h"
#include "StorageForm.h"
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Rml
{
    class DataModelConstructor;
}

namespace LonelyIce
{
    // First-run wizard: client, server folder, data (storage and components), world and account, client preparation,
    // install, done. Moving the server's data to another storage uses the same install page after a page that says
    // what will be done.
    class Wizard
    {
    public:
        struct Host
        {
            std::filesystem::path exe, exeDir;
            std::function<std::filesystem::path()> currentRoot;
            std::function<bool()> serverRunning;
            std::function<std::string()> sqlStamp;                  // stamp of the last database deploy
            std::function<StorageChoice()> storage;                 // the storage the server uses now
            std::function<std::vector<StorageProviderInfo>()> providers;    // plugins' storages
            std::function<Platform::Env(StorageChoice const&)> remoteEnv;   // connection of a plugin's storage
            std::function<std::string()> serverLocale;              // client locale of the server data, empty = the client's first
            std::function<std::string()> realmName;                 // the realm's name now, empty when unknown
            std::function<void(InstallOptions const&)> installed;   // install finished successfully
            std::function<void()> play;
            std::function<void()> wake;
        };

        explicit Wizard(Host host);
        ~Wizard();

        void Bind(Rml::DataModelConstructor& c);
        void SetModel(Rml::DataModelHandle model);

        void Open(std::filesystem::path const& client);
        // Moves the server's data to the storage "to" (title: its name for the page): the page lists the plan's
        // steps (and asks for the player's account when the plan makes new databases), then the install page runs them.
        void SwitchStorage(std::filesystem::path const& client, StorageChoice const& to, std::string const& title, StoragePlan const& plan,
            std::string const& realmName);
        // Brings the databases of the storage in use up to date with setup/sql.pak (the database step without the
        // account and the realm name) on the install page; closes by itself when it succeeded.
        void UpdateDatabases(std::filesystem::path const& client);
        bool IsOpen() const { return _open; }
        bool IsInstalling() const { return _installer.IsRunning(); }
        void Tick();
        // Rebuilds all language-dependent model data (after a language switch) and marks the model dirty.
        void Relocalize();

        struct CheckRow { Rml::String cls, text, detail; };
        struct PlaceRow { Rml::String id, title, path, free, detail; };
        // required: installed without asking when missing (on = will be installed); the others are the player's choice
        struct CompRow { Rml::String id, title, desc, size, note; bool on = false, locked = false, required = false; double gb = 0; };
        struct StepRow { Rml::String name, cls; };
        struct InstRow { Rml::String title, note, state; int pct = 0; };

    private:
        void Go(int step);
        void Next();
        void Back();
        void Cancel();
        void Inspect(std::filesystem::path const& client);
        void BuildPlaces();
        void BuildComponents();
        void RefreshTotal();
        void RefreshFooter();
        void BuildDone();
        void StartInstall();
        std::filesystem::path PlacePath() const;
        void Error(std::string text);
        void Dirty();
        void BrowseFolder(bool forClient);
        bool HasComp(char const* id) const;
        void BuildTransfer();
        void StartSwitch();
        void Prefill();

        Host _host;
        Installer _installer;
        StorageForm _form;
        Rml::DataModelHandle _model;
        ClientInfo _client;

        bool _open = false;
        int _step = 0;
        std::vector<StepRow> _steps;
        Rml::String _counter, _error, _nextLabel, _cancelLabel;
        bool _nextOk = true, _backVisible = false;

        // 0 client
        std::filesystem::path _clientInput;     // folder last passed to Inspect
        Rml::String _clientPath, _clientTitle;
        bool _clientFound = false;
        std::vector<CheckRow> _checks;
        // 1 place: client, exe, custom (and user, the per-user data folder, on Linux and macOS)
        Rml::String _place = "client";
        std::vector<PlaceRow> _places;
        std::filesystem::path _customPlace;
        // 2 data: the storage form (wzs_*) and the components
        std::vector<CompRow> _comps;
        Rml::String _threads, _coresNote, _total;
        std::optional<StorageChoice> _compsFor;         // the form the components were built for
        bool _compsChecked = false;                     // ... with the storage's check done
        bool _waitCheck = false;                        // Next waits for the storage check
        bool _dbExists = false;                         // the chosen place has the databases already
        // 3 world and account; on an existing install the realm, rates and bots start as they are (the *Was values),
        // and only what the player changes is written
        Rml::String _realm = "LonelyIce", _rate = "2", _bots = "100", _login, _pass, _gm = "3";
        std::string _realmWas, _rateWas, _botsWas;
        std::filesystem::path _prefilledFor;    // the place those were read from
        bool _askAccount = true, _accountOptional = false;  // the database step runs; ... on databases that exist
        // 4 client preparation
        bool _rl = true, _wdb = true, _accName = true, _lnk = false;
        bool _lnkSupported = true;              // the desktop has shortcuts (model: wz_lnk_supported)
        Rml::String _rlNote;
        // 5 install, 6 done
        std::vector<InstRow> _inst;
        std::vector<Rml::String> _log;
        std::vector<CheckRow> _done;
        InstallOptions _options;
        bool _reported = false;
        // 7 (switching only): what moving to another storage does
        bool _switching = false;                // the pages run a storage switch, not the wizard's choices
        bool _updating = false;                 // ... or only the database update (UpdateDatabases)
        StorageChoice _switchTo;
        StoragePlan _switchPlan;
        std::vector<CheckRow> _transfer;
        std::string _switchTitle;
        Rml::String _transferTitle, _transferNote;
        bool _needAccount = false;

        std::mutex _pickLock;
        std::string _picked;                    // folder from the dialog, UTF-8
        bool _pickedForClient = false;
    };
}

#endif
