#ifndef LONELYICE_STORAGESETUPVIEW_H
#define LONELYICE_STORAGESETUPVIEW_H

#include "Installer.h"
#include "StoragePlan.h"
#include <ftxui/component/component.hpp>
#include <optional>

namespace LonelyIce
{
    // UI-thread owner. Workers only invoke wake(); callbacks and settings commits run in Tick().
    class StorageSetupView
    {
    public:
        struct Host
        {
            LauncherSettings* settings = nullptr;
            std::filesystem::path exe, setupDir, defaultRoot;
            std::function<std::vector<StorageProviderInfo>()> providers;
            std::function<Platform::Env(StorageChoice const&, std::filesystem::path const&)> env;
            std::function<bool()> serverRunning, operationBusy;
            std::function<void()> committed, wake;
        };

        explicit StorageSetupView(Host host);
        ~StorageSetupView();
        ftxui::Component Build();
        bool Tick();
        bool Busy() const;
        bool Dirty() const;
        void Reload();
        void StartWizard();
        void Cancel();
        bool SelectProvider(std::string const& id);

    private:
        void WizardNext();
        StorageChoice Choice() const;
        std::filesystem::path Root() const;
        std::string Fingerprint() const;
        std::string Validate(bool preparing) const;
        bool CanOperate();
        void Check();
        void Review();
        void Apply();
        void Commit();
        void Edited();
        Platform::Env Environment(StorageChoice const& choice) const;

        Host _host;
        StorageCheck _check;
        Installer _installer;
        std::vector<StorageProviderInfo> _providers;
        std::vector<std::string> _locations, _locationIds;
        int _location = 0;
        std::string _remoteHost, _port, _user, _password, _prefix;
        std::string _client, _root, _realm, _login, _accountPassword;
        bool _wizard = false, _wizardCheckPending = false;
        int _wizardStep = 0;
        bool _cache = false, _setup = false, _serverOnly = true, _vmaps = false, _mmaps = false;
        bool _confirm = false, _confirmRemoval = false, _installPending = false, _cancelRequested = false;
        std::string _seen, _baseline, _checkedFingerprint, _reviewedFingerprint, _notice;
        std::optional<StorageState> _result;
        std::optional<InstallOptions> _review;
        std::vector<std::string> _log;
        ftxui::Component _component, _wizardFirst;
    };
}
#endif
