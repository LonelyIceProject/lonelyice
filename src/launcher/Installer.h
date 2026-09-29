#ifndef LONELYICE_INSTALLER_H
#define LONELYICE_INSTALLER_H

#include "Platform.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace LonelyIce
{
    struct InstallOptions
    {
        std::filesystem::path exe;          // this executable, started again for every step
        std::filesystem::path setupDir;     // folder with sql.pak and configs.pak
        std::filesystem::path root;         // server data: configs, db, data, logs, backups
        std::filesystem::path client;       // WoW 3.3.5a folder

        bool db = true, maps = true, vmaps = true, mmaps = true, client_prep = true;
        int threads = 4;

        std::string realmName = "LonelyIce";
        int rate = 1;
        int bots = 100;
        std::string login, password;
        int gmLevel = 3;

        bool realmlist = true, clearWdb = true, accountName = true, shortcut = false;
    };

    enum class StepState { Waiting, Running, Done, Failed, Skipped };

    struct InstallStep
    {
        std::string id, title;
        StepState state = StepState::Waiting;
        float progress = 0.f;   // 0..1
        std::string note;
    };

    // Runs the first-run setup on a worker thread. Each heavy step is a child process of the same exe
    // (--server --deploy, --tool ...), started below normal priority.
    class Installer
    {
    public:
        explicit Installer(std::function<void()> wake) : _wake(std::move(wake)) { }
        ~Installer();

        void Start(InstallOptions const& options);
        void Cancel();
        bool IsRunning() const { return _running; }
        bool Finished() const { return _finished; }
        bool Succeeded() const { return _ok; }

        std::vector<InstallStep> Steps() const;
        std::vector<std::string> TakeLog();     // new lines since the last call
        std::string Error() const;

        // Writes worldserver.conf and modules/*.conf from configs.pak where they don't exist yet, with LonelyIce defaults.
        static bool PrepareConfigs(InstallOptions const& o, std::string& error);
        // Size and time of setup/sql.pak: tells whether a newer release brought database updates.
        static std::string SqlStamp(std::filesystem::path const& setupDir);

    private:
        void Run();
        bool RunDatabases();
        bool RunMaps();
        bool RunVmaps();
        bool RunMmaps();
        bool RunClient();

        // Starts exe with args in dir; every output line goes to onLine. Returns the exit code, -1 if it could not start.
        int RunChild(std::vector<std::string> const& args, std::filesystem::path const& dir, Platform::Env const& env,
            std::function<void(std::string const&)> const& onLine);

        void SetStep(std::string const& id, StepState state, float progress, std::string const& note = {});
        void Progress(std::string const& id, float progress, std::string const& note = {});
        void Log(std::string line);
        void Fail(std::string const& id, std::string const& why);

        std::function<void()> _wake;
        InstallOptions _o;
        std::thread _thread;
        std::atomic<bool> _running{ false }, _finished{ false }, _ok{ false }, _cancel{ false };
        mutable std::mutex _lock;
        std::vector<InstallStep> _steps;
        std::vector<std::string> _log;
        std::string _error;
        Platform::Child* _child = nullptr;      // the running step, under _lock
        int _mapCount = 0;
    };
}

#endif
