#ifndef LONELYICE_PLATFORM_H
#define LONELYICE_PLATFORM_H

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What differs between Windows, Linux and macOS. Strings are UTF-8; paths go through PathToUtf8 / Utf8ToPath.
namespace LonelyIce::Platform
{
    using Env = std::vector<std::pair<std::string, std::string>>;   // name, value

    std::string PathToUtf8(std::filesystem::path const& p);
    std::filesystem::path Utf8ToPath(std::string const& s);

    // This executable.
    std::filesystem::path ExePath();

    std::optional<std::string> GetEnv(std::string const& name);
    void SetEnv(std::string const& name, std::string const& value);

    // Milliseconds from an arbitrary start, monotonic.
    uint64_t TickMs();
    std::tm LocalTime(std::time_t t);

    // Opens a folder in the file manager (or a file / URL with its default program).
    bool OpenInShell(std::filesystem::path const& p);

    // The console modes (--pkg, --tool, --server run by hand) write to the terminal they were started from; on Windows
    // the exe is a GUI program and attaches to its parent's console (input too when asked); utf8 switches that console
    // to UTF-8 output (text printed through the C runtime in UTF-8, e.g. --pkg).
    void UseParentConsole(bool input = false, bool utf8 = false);

    // Child process output as UTF-8 (Windows consoles write in the OEM code page).
    std::string ConsoleToUtf8(std::string const& s);

    // Lowers the priority of this process (background work).
    void LowerPriority();

    struct ChildOptions
    {
        std::filesystem::path exe;
        std::vector<std::string> args;
        std::filesystem::path workDir;          // empty: the current one
        Env env;                                // added to (and overriding) this process' environment
        bool lowPriority = false;
        bool pipes = true;                      // stdout+stderr read through Read(), stdin written through Write();
                                                // false: the child keeps no connection (a game)
    };

    // A child process. The destructor closes the pipes but does not stop the process.
    class Child
    {
    public:
        Child();
        ~Child();
        Child(Child const&) = delete;
        Child& operator=(Child const&) = delete;

        bool Start(ChildOptions const& o, std::string& error);
        bool Started() const;

        // Blocks until output arrives; 0 at the end of output (the child exited or closed it).
        std::size_t Read(char* buffer, std::size_t size);
        bool Write(std::string_view data);
        void CloseInput();                      // the child reads end of input

        bool Running() const;
        // Waits up to timeoutMs (-1: forever); true once the child has exited.
        bool Wait(int timeoutMs);
        int ExitCode() const;                   // after Wait returned true
        void Kill();
        uint64_t MemoryBytes() const;           // private / resident memory, 0 when unknown

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    // A process whose executable (or, for programs run through Wine, command line) ends with this file name.
    bool IsProcessRunning(std::filesystem::path const& exe);

    // A desktop shortcut starting target with args. False (error set) where the desktop has none.
    bool CreateDesktopShortcut(std::string const& name, std::filesystem::path const& target,
        std::vector<std::string> const& args, std::filesystem::path const& icon, std::string& error);
    bool DesktopShortcutsSupported();

    // "windows", "linux" or "macos".
    char const* Name();
}

#endif
