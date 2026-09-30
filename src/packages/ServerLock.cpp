#include "ServerLock.h"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using LonelyIce::Packages::ServerLock;

ServerLock::ServerLock(fs::path const& pluginsDir)
{
    fs::path const file = File(pluginsDir);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
#ifdef _WIN32
    // Opened without sharing: nobody else can open the file while this handle lives.
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        _handle = h;
#else
    int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0)
        _fd = fd;
    else if (fd >= 0)
        close(fd);
#endif
}

ServerLock::~ServerLock()
{
#ifdef _WIN32
    if (_handle)
        CloseHandle(static_cast<HANDLE>(_handle));
#else
    if (_fd >= 0)
        close(_fd);
#endif
}

bool ServerLock::Held() const
{
#ifdef _WIN32
    return _handle != nullptr;
#else
    return _fd >= 0;
#endif
}

bool ServerLock::IsHeld(fs::path const& pluginsDir)
{
    fs::path const file = File(pluginsDir);
#ifdef _WIN32
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        CloseHandle(h);
        return false;
    }
    return GetLastError() == ERROR_SHARING_VIOLATION;
#else
    int fd = open(file.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    bool const held = flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    close(fd);
    return held;
#endif
}
