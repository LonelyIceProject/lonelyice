#include "StorageCheck.h"
#include "Lang.h"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    // A database server that does not answer is given up on after this long.
    constexpr auto Timeout = std::chrono::seconds(40);

    // The check needs no settings of its own: logging to the console, the databases come from the environment.
    fs::path CheckConfig(std::string& error)
    {
        std::error_code ec;
        fs::path const dir = fs::temp_directory_path(ec) / "LonelyIce";
        fs::create_directories(dir, ec);
        fs::path const file = dir / "storage-check.conf";
        std::ofstream out(file, std::ios::trunc);
        out << "[worldserver]\nAppender.Console=1,2,0\nLogger.root=2,Console\nUpdates.EnableDatabases=0\n";
        if (!out)
            error = Tr("storage.error.temp", Platform::PathToUtf8(file));
        return file;
    }
}

StorageCheck::~StorageCheck()
{
    Cancel();
    if (_thread.joinable())
        _thread.join();
}

void StorageCheck::Cancel()
{
    ++_run;
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_child)
            _child->Kill();
        _result.reset();
    }
}

void StorageCheck::Start(fs::path const& exe, Platform::Env const& env)
{
    unsigned const run = ++_run;
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_child)
            _child->Kill();
        _result.reset();
    }
    if (_thread.joinable())
        _thread.join();
    _running = true;
    _thread = std::thread([this, exe, env, run] { Run(exe, env, run); });
}

std::optional<StorageState> StorageCheck::Take()
{
    std::lock_guard<std::mutex> guard(_lock);
    return std::exchange(_result, std::nullopt);
}

void StorageCheck::Run(fs::path exe, Platform::Env env, unsigned run)
{
    StorageState state;
    std::string error;
    for (char const* key : { "AC_LOGIN_DATABASE_INFO", "AC_CHARACTER_DATABASE_INFO", "AC_WORLD_DATABASE_INFO" })
    {
        if (std::none_of(env.begin(), env.end(), [key](auto const& entry)
            { return entry.first == key && !entry.second.empty(); }))
        {
            error = Tr("storage.error.connection_missing", key);
            break;
        }
    }
    fs::path const config = error.empty() ? CheckConfig(error) : fs::path();

    Platform::ChildOptions o;
    o.exe = exe;
    o.args = { "--server", "--storage-check", "-c", Platform::PathToUtf8(config) };
    o.workDir = config.parent_path();
    o.env = { { "AC_DISABLE_INTERACTIVE", "1" }, { "AC_PLUGINS_DIR", Platform::PathToUtf8(exe.parent_path() / "plugins") } };
    o.env.insert(o.env.end(), env.begin(), env.end());
    o.lowPriority = true;

    Platform::Child child;
    bool done = false;
    int dbOk = 0;
    std::string output;
    if (error.empty() && child.Start(o, error))
    {
        child.CloseInput();
        {
            std::lock_guard<std::mutex> guard(_lock);
            _child = &child;
        }
        if (run != _run)
            child.Kill();

        // the child is stopped when the database server keeps it waiting too long
        std::mutex waitLock;
        std::condition_variable finished;
        bool over = false, timedOut = false;
        std::thread watchdog([&]
        {
            std::unique_lock<std::mutex> lock(waitLock);
            if (!finished.wait_for(lock, Timeout, [&] { return over; }))
            {
                timedOut = true;
                child.Kill();
            }
        });

        std::string pending;
        char buf[4096];
        auto line = [&](std::string const& l)
        {
            char name[32] = {}, status[32] = {};
            int n = 0;
            // "check dbc" first: "check db %s" would take it too
            if (l.rfind("@@LI check dbc ", 0) == 0)
                std::sscanf(l.c_str(), "@@LI check dbc %d %d", &state.dbcTables, &state.dbcTotal);
            else if (std::sscanf(l.c_str(), "@@LI check db %31s %31s %n", name, status, &n) >= 2)
            {
                std::string const s = status;
                if (s == "ok")
                {
                    ++dbOk;
                    if (std::string_view(name) == "auth")
                        state.auth = true;
                    else if (std::string_view(name) == "characters")
                        state.characters = true;
                    else if (std::string_view(name) == "world")
                        state.world = true;
                }
                else if (s == "error" && state.error.empty())
                    state.error = Tr("storage.error.db", name, n > 0 ? Platform::ConsoleToUtf8(l.substr(std::size_t(n))) : std::string());
            }
            else if (l.rfind("@@LI check done", 0) == 0)
                done = true;
        };
        for (std::size_t read; (read = child.Read(buf, sizeof(buf))) > 0;)
        {
            // Keep the failure context bounded even if a plugin writes excessive diagnostics.
            output.append(buf, read);
            constexpr std::size_t MaxOutput = 32 * 1024;
            if (output.size() > MaxOutput)
                output.erase(0, output.size() - MaxOutput);
            pending.append(buf, read);
            for (std::size_t eol; (eol = pending.find_first_of("\r\n")) != std::string::npos;)
            {
                line(pending.substr(0, eol));
                pending.erase(0, eol + 1);
            }
        }
        line(pending);
        child.Wait(-1);
        {
            std::lock_guard<std::mutex> lock(waitLock);
            over = true;
        }
        finished.notify_all();
        watchdog.join();
        {
            std::lock_guard<std::mutex> guard(_lock);
            _child = nullptr;
        }
        if (timedOut)
            state.error = Tr("storage.error.timeout");
        else if ((!done || child.ExitCode() != 0) && state.error.empty())
        {
            std::string diagnostics = Platform::ConsoleToUtf8(output);
            // Protocol messages report progress, rather than explain why the process failed.
            std::string details;
            for (std::size_t start = 0; start < diagnostics.size();)
            {
                std::size_t const end = diagnostics.find('\n', start);
                std::string const line = diagnostics.substr(start, end == std::string::npos ? end : end - start);
                if (line.find_first_not_of(" \t\r") != std::string::npos && line.rfind("@@LI ", 0) != 0)
                    details += line + '\n';
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            if (details.size() > 8192)
                details.erase(0, details.size() - 8192);
            state.error = details.empty() ? Tr("storage.error.exit", child.ExitCode())
                : Tr("storage.error.exit_details", child.ExitCode(), details);
        }
        // Preserve the child output for troubleshooting failures that used to show only an exit code.
        if (!state.error.empty())
        {
            std::ofstream log(config.parent_path() / "storage-check.log", std::ios::binary | std::ios::trunc);
            log << Platform::ConsoleToUtf8(output);
        }
    }
    else if (state.error.empty())
        state.error = error.empty() ? Tr("storage.error.start") : error;

    state.reached = done && state.error.empty();
    state.databases = state.reached && dbOk == 3;
    if (run == _run)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _result = state;
    }
    _running = false;
    if (_wake)
        _wake();
}
