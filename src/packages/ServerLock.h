#ifndef LONELYICE_SERVERLOCK_H
#define LONELYICE_SERVERLOCK_H

#include <filesystem>

namespace LonelyIce::Packages
{
    // Held by a server process (LonelyIce --server) while it runs: <plugins>/.cache/server.lock, locked through the
    // operating system, so the lock goes away with the process however it ends. The package manager refuses to
    // change a plugins folder whose lock is held.
    class ServerLock
    {
    public:
        // Takes the lock of that plugins folder; best effort (Held() tells).
        explicit ServerLock(std::filesystem::path const& pluginsDir);
        ~ServerLock();
        ServerLock(ServerLock const&) = delete;
        ServerLock& operator=(ServerLock const&) = delete;

        bool Held() const;

        // Whether a process holds the lock of that plugins folder (this one's own lock counts too).
        static bool IsHeld(std::filesystem::path const& pluginsDir);

        static std::filesystem::path File(std::filesystem::path const& pluginsDir) { return pluginsDir / ".cache" / "server.lock"; }

    private:
#ifdef _WIN32
        void* _handle = nullptr;
#else
        int _fd = -1;
#endif
    };
}

#endif
