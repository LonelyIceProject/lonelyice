#ifndef LONELYICE_SERVERPROCESS_H
#define LONELYICE_SERVERPROCESS_H

#include "ConfigEnv.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace LonelyIce
{
    enum class ServerState
    {
        Stopped,
        Starting,  // process up, loading config/databases
        Loading,   // world data loading
        Ready,
        Stopping,
        Failed
    };

    struct ServerStats
    {
        uint32_t players = 0;   // real clients
        uint32_t chars = 0;     // characters in world, bots included
        uint64_t uptime = 0;
        uint32_t diff = 0;
    };

    struct AccountInfo
    {
        uint32_t id = 0;
        std::string name, lastLogin;
        uint32_t gmLevel = 0, characters = 0;
        bool bot = false;
    };

    struct CharacterInfo
    {
        std::string name;
        uint32_t account = 0, level = 0;
        bool online = false;
    };

    // Runs this same exe with --server as a child process; log comes back through a pipe, commands go in through stdin.
    class ServerProcess
    {
    public:
        explicit ServerProcess(std::function<void()> wake);
        ~ServerProcess();

        bool Start(std::string const& exePath, std::string const& configPath, std::string const& workDir, EnvList const& env);
        void Stop();                         // graceful: saves everyone
        void Kill();
        bool SendCommand(std::string const& utf8Line);

        // Call on the UI thread: drains new log lines and applies control messages.
        std::vector<std::string> TakeLines();

        // True once per completed "@@accounts" answer.
        bool TakeAccounts(std::vector<AccountInfo>& accounts, std::vector<CharacterInfo>& characters);
        std::string GetRealmName() const;

        ServerState GetState() const { return _state; }
        ServerStats GetStats() const;
        std::string GetFailReason() const;
        uint64_t GetMemoryBytes() const;
        bool IsRunning() const;
        int GetExitCode() const { return _exitCode; }
        uint64_t GetStartTick() const { return _startTick; }

    private:
        void ReaderLoop();
        void HandleLine(std::string line);

        std::function<void()> _wake;
        std::unique_ptr<Platform::Child> _child;
        std::thread _reader;

        mutable std::mutex _lock;
        std::deque<std::string> _lines;
        ServerStats _stats;
        std::vector<AccountInfo> _accBuild, _accReady;
        std::vector<CharacterInfo> _charBuild, _charReady;
        bool _accFresh = false;
        std::string _realmName;
        std::string _failReason;
        std::atomic<ServerState> _state{ ServerState::Stopped };
        std::atomic<int> _exitCode{ 0 };
        uint64_t _startTick = 0;
    };
}

#endif
