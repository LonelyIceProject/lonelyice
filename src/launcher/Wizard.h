#ifndef LONELYICE_WIZARD_H
#define LONELYICE_WIZARD_H

#include "GameClient.h"
#include "Installer.h"
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
    // First-run wizard: client, server folder, components, world and account, client preparation, install, done.
    class Wizard
    {
    public:
        struct Host
        {
            std::filesystem::path exe, exeDir;
            std::function<std::filesystem::path()> currentRoot;
            std::function<bool()> serverRunning;
            std::function<std::string()> sqlStamp;                  // stamp of the last database deploy
            std::function<std::string()> storage;                   // where the server reads game data now (InstallOptions::storage)
            std::function<std::string()> serverLocale;              // client locale of the server data, empty = the client's first
            std::function<void(InstallOptions const&)> installed;   // install finished successfully
            std::function<void()> play;
            std::function<void()> wake;
        };

        explicit Wizard(Host host);
        ~Wizard();

        void Bind(Rml::DataModelConstructor& c);
        void SetModel(Rml::DataModelHandle model) { _model = model; }

        void Open(std::filesystem::path const& client);
        // Switches where the server reads its game data ("client" or "unpacked"): straight to the install page.
        void SwitchStorage(std::filesystem::path const& client, std::string const& storage);
        bool IsOpen() const { return _open; }
        bool IsInstalling() const { return _installer.IsRunning(); }
        void Tick();
        // Rebuilds all language-dependent model data (after a language switch) and marks the model dirty.
        void Relocalize();

        struct CheckRow { Rml::String cls, text, detail; };
        struct PlaceRow { Rml::String id, title, path, free, detail; };
        struct CompRow { Rml::String id, title, desc, size, note; bool on = false, locked = false; };
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

        Host _host;
        Installer _installer;
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
        // 2 components
        std::vector<CompRow> _comps;
        Rml::String _threads, _coresNote, _total;
        // 3 world and account
        Rml::String _realm = "LonelyIce", _rate = "2", _bots = "100", _login, _pass, _gm = "3";
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
        bool _switching = false;                // the install page runs a storage switch, not the wizard's choices

        std::mutex _pickLock;
        std::string _picked;                    // folder from the dialog, UTF-8
        bool _pickedForClient = false;
    };
}

#endif
