#ifndef LONELYICE_SERVERLOCK_H
#define LONELYICE_SERVERLOCK_H

#include <filesystem>

namespace LonelyIce::Packages
{
    // Held by a server process while it runs, or by a package operation while it changes plugins.
    // <plugins>/.cache/server.lock is exclusive and released by the operating system when the process ends.
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
