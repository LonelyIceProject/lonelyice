#ifndef LONELYICE_STORAGECHECK_H
#define LONELYICE_STORAGECHECK_H

#include "Platform.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace LonelyIce
{
    // What a place for the server's databases holds.
    struct StorageState
    {
        bool reached = false;       // the databases could be asked; false: error says why (no connection, refused, ...)
        std::string error;
        bool databases = false;     // auth, characters and world exist and hold their tables
        bool auth = false, characters = false, world = false;
        int dbcTables = 0, dbcTotal = 0;    // DBC files unpacked into the world database

        bool HasDbc() const { return dbcTotal > 0 && dbcTables == dbcTotal; }
    };

    // Runs "LonelyIce --server --storage-check" on a worker thread: the core itself opens the databases that env
    // names (Local/RemoteDatabaseOverrides), with the plugins' database backends, so every kind of storage is
    // checked the same way. No server folder is needed.
    class StorageCheck
    {
    public:
        explicit StorageCheck(std::function<void()> wake) : _wake(std::move(wake)) { }
        ~StorageCheck();

        // A running check is abandoned (its result is dropped).
        void Start(std::filesystem::path const& exe, Platform::Env const& env);
        // Cancels the child and drops its result without joining the worker on the UI thread.
        void Cancel();
        bool IsRunning() const { return _running; }
        // The result, once, after the check finished.
        std::optional<StorageState> Take();

    private:
        void Run(std::filesystem::path exe, Platform::Env env, unsigned run);

        std::function<void()> _wake;
        std::thread _thread;
        std::atomic<bool> _running{ false };
        std::atomic<unsigned> _run{ 0 };
        std::mutex _lock;
        std::optional<StorageState> _result;
        Platform::Child* _child = nullptr;      // under _lock
    };
}

#endif
