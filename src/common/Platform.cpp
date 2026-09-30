#include "Platform.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#include <Windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace fs = std::filesystem;
using namespace LonelyIce;

std::string Platform::PathToUtf8(fs::path const& p)
{
    std::u8string const s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path Platform::Utf8ToPath(std::string const& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

uint64_t Platform::TickMs()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::tm Platform::LocalTime(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

#ifdef _WIN32

namespace
{
    std::wstring Wide(std::string const& s)
    {
        if (s.empty())
            return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }

    std::string Narrow(std::wstring const& w, UINT cp = CP_UTF8)
    {
        if (w.empty())
            return {};
        int n = WideCharToMultiByte(cp, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(cp, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // One argument for CreateProcess' command line (the MSVC runtime's parsing rules).
    std::wstring QuoteArg(std::wstring const& a)
    {
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
            return a;
        std::wstring out = L"\"";
        std::size_t slashes = 0;
        for (wchar_t c : a)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, L'\\');
        return out + L"\"";
    }

    // This process' environment with overrides, as a CREATE_UNICODE_ENVIRONMENT block. Names ignore case.
    std::wstring EnvironmentBlock(Platform::Env const& overrides)
    {
        std::vector<std::wstring> vars, names;
        auto has = [&](std::wstring const& name)
        {
            return std::any_of(names.begin(), names.end(), [&](std::wstring const& n) { return _wcsicmp(n.c_str(), name.c_str()) == 0; });
        };
        for (auto const& [name, value] : overrides)
        {
            std::wstring const n = Wide(name);
            if (has(n))
                continue;
            names.push_back(n);
            vars.push_back(n + L"=" + Wide(value));
        }
        if (wchar_t* block = GetEnvironmentStringsW())
        {
            for (wchar_t const* p = block; *p; p += wcslen(p) + 1)
            {
                std::wstring entry = p;
                std::size_t eq = entry.find(L'=', 1);
                if (eq != std::wstring::npos && !has(entry.substr(0, eq)))
                    vars.push_back(entry);
            }
            FreeEnvironmentStringsW(block);
        }
        std::wstring out;
        for (std::wstring const& v : vars)
            out += v + L'\0';
        return out + L'\0';
    }
}

fs::path Platform::ExePath()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n < buf.size())
            return fs::path(buf.substr(0, n));
        buf.resize(buf.size() * 2);
    }
}

std::optional<std::string> Platform::GetEnv(std::string const& name)
{
    std::wstring const n = Wide(name);
    DWORD size = GetEnvironmentVariableW(n.c_str(), nullptr, 0);
    if (!size)
        return std::nullopt;
    std::wstring v(size, L'\0');
    v.resize(GetEnvironmentVariableW(n.c_str(), v.data(), size));
    return Narrow(v);
}

void Platform::SetEnv(std::string const& name, std::string const& value)
{
    // both the process block (child processes) and the C runtime's copy (getenv in the core)
    SetEnvironmentVariableW(Wide(name).c_str(), Wide(value).c_str());
    _wputenv_s(Wide(name).c_str(), Wide(value).c_str());
}

bool Platform::OpenInShell(fs::path const& p)
{
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

void Platform::UseParentConsole(bool input, bool utf8)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        if (input)
            freopen_s(&f, "CONIN$", "r", stdin);
    }
    if (utf8)
        SetConsoleOutputCP(CP_UTF8);
}

std::string Platform::ConsoleToUtf8(std::string const& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_OEMCP, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, s.data(), int(s.size()), w.data(), n);
    return Narrow(w);
}

void Platform::LowerPriority()
{
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}

struct Platform::Child::Impl
{
    HANDLE process = nullptr, out = nullptr, in = nullptr;
    DWORD exitCode = 0;
};

Platform::Child::Child() : _impl(std::make_unique<Impl>()) { }

Platform::Child::~Child()
{
    for (HANDLE* h : { &_impl->process, &_impl->out, &_impl->in })
        if (*h)
            CloseHandle(*h);
}

bool Platform::Child::Start(ChildOptions const& o, std::string& error)
{
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (o.pipes)
    {
        if (!CreatePipe(&outRead, &outWrite, &sa, 1 << 16))
        {
            error = "CreatePipe: " + std::to_string(GetLastError());
            return false;
        }
        if (!CreatePipe(&inRead, &inWrite, &sa, 1 << 12))
        {
            error = "CreatePipe: " + std::to_string(GetLastError());
            CloseHandle(outRead);
            CloseHandle(outWrite);
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    }

    std::wstring cmd = QuoteArg(o.exe.wstring());
    for (std::string const& a : o.args)
        cmd += L" " + QuoteArg(Wide(a));
    std::wstring const env = EnvironmentBlock(o.env);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (o.pipes)
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = outWrite;
    }
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (o.pipes ? CREATE_NO_WINDOW : 0) | (o.lowPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, o.pipes, flags, const_cast<wchar_t*>(env.data()),
        o.workDir.empty() ? nullptr : o.workDir.c_str(), &si, &pi);
    DWORD const lastError = GetLastError();
    if (o.pipes)
    {
        CloseHandle(outWrite);
        CloseHandle(inRead);
    }
    if (!ok)
    {
        if (o.pipes)
        {
            CloseHandle(outRead);
            CloseHandle(inWrite);
        }
        error = "CreateProcess: " + std::to_string(lastError);
        return false;
    }
    CloseHandle(pi.hThread);
    _impl->process = pi.hProcess;
    _impl->out = outRead;
    _impl->in = inWrite;
    return true;
}

bool Platform::Child::Started() const
{
    return _impl->process != nullptr;
}

std::size_t Platform::Child::Read(char* buffer, std::size_t size)
{
    DWORD read = 0;
    if (!_impl->out || !ReadFile(_impl->out, buffer, DWORD(size), &read, nullptr))
        return 0;
    return read;
}

bool Platform::Child::Write(std::string_view data)
{
    DWORD written = 0;
    return _impl->in && WriteFile(_impl->in, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
}

void Platform::Child::CloseInput()
{
    if (_impl->in)
    {
        CloseHandle(_impl->in);
        _impl->in = nullptr;
    }
}

bool Platform::Child::Running() const
{
    return _impl->process && WaitForSingleObject(_impl->process, 0) == WAIT_TIMEOUT;
}

bool Platform::Child::Wait(int timeoutMs)
{
    if (!_impl->process)
        return true;
    if (WaitForSingleObject(_impl->process, timeoutMs < 0 ? INFINITE : DWORD(timeoutMs)) != WAIT_OBJECT_0)
        return false;
    GetExitCodeProcess(_impl->process, &_impl->exitCode);
    return true;
}

int Platform::Child::ExitCode() const
{
    return int(_impl->exitCode);
}

void Platform::Child::Kill()
{
    if (Running())
        TerminateProcess(_impl->process, 1);
}

uint64_t Platform::Child::MemoryBytes() const
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!Running() || !GetProcessMemoryInfo(_impl->process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return 0;
    return pmc.PrivateUsage;
}

bool Platform::IsProcessRunning(fs::path const& exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    std::error_code ec;
    fs::path const want = fs::weakly_canonical(exe, ec);
    bool found = false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
    {
        if (_wcsicmp(pe.szExeFile, exe.filename().c_str()) != 0)
            continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p)
            continue;
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &len) && fs::equivalent(fs::path(path), want, ec))
            found = true;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return found;
}

Platform::FileLock::FileLock(fs::path const& file)
{
    // no sharing: a second open fails while this handle is open
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        _handle = reinterpret_cast<intptr_t>(h);
}

Platform::FileLock::~FileLock()
{
    if (Held())
        CloseHandle(reinterpret_cast<HANDLE>(_handle));
}

bool Platform::DesktopShortcutsSupported()
{
    return true;
}

bool Platform::CreateDesktopShortcut(std::string const& name, fs::path const& target, std::vector<std::string> const& args,
    fs::path const& icon, std::string& error)
{
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
    {
        error = "no desktop folder";
        return false;
    }
    fs::path const link = fs::path(desktop) / (Wide(name) + L".lnk");
    CoTaskMemFree(desktop);

    std::wstring arguments;
    for (std::string const& a : args)
        arguments += (arguments.empty() ? L"" : L" ") + QuoteArg(Wide(a));

    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))))
    {
        sl->SetPath(target.c_str());
        sl->SetArguments(arguments.c_str());
        sl->SetWorkingDirectory(target.parent_path().c_str());
        sl->SetDescription(Wide(name).c_str());
        if (!icon.empty())
            sl->SetIconLocation(icon.c_str(), 0);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))))
        {
            ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    if (!ok)
        error = "IShellLink";
    return ok;
}

char const* Platform::Name()
{
    return "windows";
}

#else // POSIX: Linux and macOS

namespace
{
    std::vector<std::string> EnvironmentList(Platform::Env const& overrides)
    {
        std::vector<std::string> out;
        for (auto const& [name, value] : overrides)
            out.push_back(name + "=" + value);
        for (char** e = environ; e && *e; ++e)
        {
            std::string_view const entry = *e;
            std::size_t const eq = entry.find('=');
            std::string_view const name = entry.substr(0, eq);
            if (std::none_of(overrides.begin(), overrides.end(), [&](auto const& kv) { return kv.first == name; }))
                out.emplace_back(entry);
        }
        return out;
    }

    // Starts a short-lived helper (xdg-open, open) and forgets it: forked twice so it never stays a zombie.
    bool Spawn(std::vector<std::string> args)
    {
        std::vector<char*> argv;
        for (std::string& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid = fork();
        if (pid < 0)
            return false;
        if (pid == 0)
        {
            if (fork() == 0)
            {
                setsid();
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0)
                {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                }
                execvp(argv[0], argv.data());
            }
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return true;
    }

    fs::path Home()
    {
        char const* h = std::getenv("HOME");
        return h ? fs::path(h) : fs::path();
    }
}

fs::path Platform::ExePath()
{
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    std::error_code ec;
    fs::path p = fs::canonical(buf.c_str(), ec);
    return ec ? fs::path(buf.c_str()) : p;
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

std::optional<std::string> Platform::GetEnv(std::string const& name)
{
    char const* v = std::getenv(name.c_str());
    return v ? std::optional<std::string>(v) : std::nullopt;
}

void Platform::SetEnv(std::string const& name, std::string const& value)
{
    setenv(name.c_str(), value.c_str(), 1);
}

bool Platform::OpenInShell(fs::path const& p)
{
#ifdef __APPLE__
    return Spawn({ "/usr/bin/open", PathToUtf8(p) });
#else
    return Spawn({ "xdg-open", PathToUtf8(p) });
#endif
}

void Platform::UseParentConsole(bool /*input*/, bool /*utf8*/)
{
}

std::string Platform::ConsoleToUtf8(std::string const& s)
{
    return s;
}

void Platform::LowerPriority()
{
    setpriority(PRIO_PROCESS, 0, 10);
}

struct Platform::Child::Impl
{
    pid_t pid = -1;
    int out = -1, in = -1;
    // Running() (UI thread) and Wait() (a reader thread) may race: only Reap touches these, under the lock.
    std::mutex lock;
    int exitCode = 0;
    bool exited = false;

    void Reap(int options)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (exited || pid <= 0)
            return;
        int status = 0;
        pid_t const r = waitpid(pid, &status, options);
        if (r == pid)
        {
            exited = true;
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        else if (r < 0 && errno == ECHILD)
        {
            exited = true;
            exitCode = 1;
        }
    }

    bool Exited()
    {
        std::lock_guard<std::mutex> guard(lock);
        return exited;
    }
};

Platform::Child::Child() : _impl(std::make_unique<Impl>()) { }

Platform::Child::~Child()
{
    for (int* fd : { &_impl->out, &_impl->in })
        if (*fd >= 0)
            close(*fd);
    // A child that already ended is reaped; one still running is left alone.
    _impl->Reap(WNOHANG);
}

bool Platform::Child::Start(ChildOptions const& o, std::string& error)
{
    int outPipe[2] = { -1, -1 }, inPipe[2] = { -1, -1 };
    if (o.pipes && (pipe(outPipe) != 0 || pipe(inPipe) != 0))
    {
        error = std::string("pipe: ") + std::strerror(errno);
        for (int fd : { outPipe[0], outPipe[1], inPipe[0], inPipe[1] })
            if (fd >= 0)
                close(fd);
        return false;
    }

    // Everything the child needs is prepared before fork: only async-signal-safe calls after it.
    std::string const exe = PathToUtf8(o.exe), dir = PathToUtf8(o.workDir);
    std::vector<std::string> args{ exe };
    args.insert(args.end(), o.args.begin(), o.args.end());
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> envStrings = EnvironmentList(o.env);
    std::vector<char*> envp;
    for (std::string& e : envStrings)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    int const devnull = o.pipes ? -1 : open("/dev/null", O_RDWR);

    pid_t pid = fork();
    if (pid < 0)
    {
        error = std::string("fork: ") + std::strerror(errno);
        for (int fd : { outPipe[0], outPipe[1], inPipe[0], inPipe[1], devnull })
            if (fd >= 0)
                close(fd);
        return false;
    }
    if (pid == 0)
    {
        if (o.pipes)
        {
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(outPipe[1], STDERR_FILENO);
            close(inPipe[0]);
            close(inPipe[1]);
            close(outPipe[0]);
            close(outPipe[1]);
        }
        else
        {
            // detached: own session, no terminal
            setsid();
            if (devnull >= 0)
            {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        if (!dir.empty() && chdir(dir.c_str()) != 0)
            _exit(127);
        if (o.lowPriority)
            setpriority(PRIO_PROCESS, 0, 10);
        environ = envp.data();      // execvp searches PATH for a bare name (no execvpe on macOS)
        execvp(exe.c_str(), argv.data());
        _exit(127);
    }

    if (devnull >= 0)
        close(devnull);
    if (o.pipes)
    {
        close(inPipe[0]);
        close(outPipe[1]);
        fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(inPipe[1], F_SETFD, FD_CLOEXEC);
        _impl->out = outPipe[0];
        _impl->in = inPipe[1];
    }
    _impl->pid = pid;
    return true;
}

bool Platform::Child::Started() const
{
    return _impl->pid > 0;
}

std::size_t Platform::Child::Read(char* buffer, std::size_t size)
{
    if (_impl->out < 0)
        return 0;
    for (;;)
    {
        ssize_t n = read(_impl->out, buffer, size);
        if (n >= 0)
            return std::size_t(n);
        if (errno != EINTR)
            return 0;
    }
}

bool Platform::Child::Write(std::string_view data)
{
    if (_impl->in < 0)
        return false;
    // a child that exited must not kill us with SIGPIPE
    static bool const ignored = [] { signal(SIGPIPE, SIG_IGN); return true; }();
    (void)ignored;
    while (!data.empty())
    {
        ssize_t n = write(_impl->in, data.data(), data.size());
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        data.remove_prefix(std::size_t(n));
    }
    return true;
}

void Platform::Child::CloseInput()
{
    if (_impl->in >= 0)
    {
        close(_impl->in);
        _impl->in = -1;
    }
}

bool Platform::Child::Running() const
{
    if (_impl->pid <= 0)
        return false;
    _impl->Reap(WNOHANG);
    return !_impl->Exited();
}

bool Platform::Child::Wait(int timeoutMs)
{
    if (_impl->pid <= 0 || _impl->Exited())
        return true;
    if (timeoutMs < 0)
    {
        // wait without reaping, then reap under the lock
        siginfo_t info{};
        while (waitid(P_PID, id_t(_impl->pid), &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
            ;
        _impl->Reap(0);
        return true;
    }
    uint64_t const until = TickMs() + uint64_t(timeoutMs);
    while (Running())
    {
        if (TickMs() >= until)
            return false;
        usleep(10000);
    }
    return true;
}

int Platform::Child::ExitCode() const
{
    std::lock_guard<std::mutex> guard(_impl->lock);
    return _impl->exitCode;
}


void Platform::Child::Kill()
{
    if (Running())
        kill(_impl->pid, SIGKILL);
}

uint64_t Platform::Child::MemoryBytes() const
{
    if (!Running())
        return 0;
#ifdef __APPLE__
    proc_taskinfo info{};
    if (proc_pidinfo(_impl->pid, PROC_PIDTASKINFO, 0, &info, sizeof(info)) != int(sizeof(info)))
        return 0;
    return info.pti_resident_size;
#else
    std::ifstream statm("/proc/" + std::to_string(_impl->pid) + "/statm");
    uint64_t size = 0, resident = 0;
    if (!(statm >> size >> resident))
        return 0;
    return resident * uint64_t(sysconf(_SC_PAGESIZE));
#endif
}

bool Platform::IsProcessRunning(fs::path const& exe)
{
    std::string const name = PathToUtf8(exe.filename());
#ifdef __APPLE__
    // Wine shows the game as its own argument: match the whole command line.
    std::vector<pid_t> pids(4096);
    int n = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < n; ++i)
    {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pids[i], path, sizeof(path)) > 0 && fs::path(path).filename() == name)
            return true;
    }
    FILE* ps = popen("ps -Ao args=", "r");
    if (!ps)
        return false;
    char line[4096];
    bool found = false;
    while (!found && fgets(line, sizeof(line), ps))
        found = std::strstr(line, name.c_str()) != nullptr;
    pclose(ps);
    return found;
#else
    std::error_code ec;
    for (auto const& e : fs::directory_iterator("/proc", ec))
    {
        std::string const pid = e.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), ::isdigit))
            continue;
        std::ifstream f(e.path() / "cmdline", std::ios::binary);
        std::string cmdline((std::istreambuf_iterator<char>(f)), {});
        // arguments are NUL-separated; Wine keeps the Windows path of the game among them
        std::replace(cmdline.begin(), cmdline.end(), '\\', '/');
        for (std::size_t pos = 0; pos < cmdline.size();)
        {
            std::size_t const end = cmdline.find('\0', pos);
            std::string_view const arg = std::string_view(cmdline).substr(pos, end - pos);
            std::size_t const slash = arg.rfind('/');
            std::string_view const base = slash == std::string_view::npos ? arg : arg.substr(slash + 1);
            if (base.size() == name.size() && std::equal(base.begin(), base.end(), name.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
                return true;
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
    }
    return false;
#endif
}

Platform::FileLock::FileLock(fs::path const& file)
{
    int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd == -1)
        return;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        close(fd);
        return;
    }
    _handle = fd;
}

Platform::FileLock::~FileLock()
{
    if (Held())
        close(int(_handle));
}

bool Platform::DesktopShortcutsSupported()
{
#ifdef __APPLE__
    return false;
#else
    return true;
#endif
}

bool Platform::CreateDesktopShortcut(std::string const& name, fs::path const& target, std::vector<std::string> const& args,
    fs::path const& icon, std::string& error)
{
#ifdef __APPLE__
    (void)name; (void)target; (void)args; (void)icon;
    error = "not supported on macOS";
    return false;
#else
    // A .desktop entry in the applications menu and, when there is one, on the desktop.
    auto quote = [](std::string const& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '`' || c == '$' || c == '\\')
                out += '\\';
            out += c;
        }
        return out + "\"";
    };
    std::string exec = quote(PathToUtf8(target));
    for (std::string const& a : args)
        exec += " " + quote(a);
    std::string const entry = "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=" + exec + "\nPath=" + PathToUtf8(target.parent_path())
        + (icon.empty() ? "" : "\nIcon=" + PathToUtf8(icon)) + "\nTerminal=false\nCategories=Game;\n";

    fs::path const home = Home();
    char const* dataHome = std::getenv("XDG_DATA_HOME");
    fs::path const apps = (dataHome && *dataHome ? fs::path(dataHome) : home / ".local" / "share") / "applications";
    std::string const file = "lonelyice.desktop";
    std::error_code ec;
    bool ok = false;
    for (fs::path const& dir : { apps, home / "Desktop" })
    {
        if (dir == home / "Desktop" && !fs::is_directory(dir, ec))
            continue;
        fs::create_directories(dir, ec);
        std::ofstream out(dir / file, std::ios::binary);
        if (!(out << entry))
            continue;
        out.close();
        fs::permissions(dir / file, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
        ok = true;
    }
    if (!ok)
        error = "cannot write " + PathToUtf8(apps / file);
    return ok;
#endif
}

char const* Platform::Name()
{
#ifdef __APPLE__
    return "macos";
#else
    return "linux";
#endif
}

#endif
