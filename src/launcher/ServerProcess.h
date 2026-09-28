#ifndef LONELYICE_SERVERPROCESS_H
#define LONELYICE_SERVERPROCESS_H

#include "ConfigEnv.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
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
        uint32_t players = 0;
        uint64_t uptime = 0;
        uint32_t diff = 0;
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
        void ClosePipes();

        std::function<void()> _wake;
        void* _process = nullptr;
        void* _stdinWrite = nullptr;
        void* _stdoutRead = nullptr;
        std::thread _reader;

        mutable std::mutex _lock;
        std::deque<std::string> _lines;
        ServerStats _stats;
        std::string _failReason;
        std::atomic<ServerState> _state{ ServerState::Stopped };
        std::atomic<int> _exitCode{ 0 };
        uint64_t _startTick = 0;
    };
}

#endif
