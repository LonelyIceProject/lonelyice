#include "ServerProcess.h"
#include "TextUtil.h"
#include <Windows.h>
#include <psapi.h>
#include <sstream>

namespace
{
    constexpr std::size_t MaxBufferedLines = 5000;
    constexpr char ControlPrefix[] = "@@LI ";
}

LonelyIce::ServerProcess::ServerProcess(std::function<void()> wake) : _wake(std::move(wake)) { }

LonelyIce::ServerProcess::~ServerProcess()
{
    if (_process)
    {
        // Closing stdin makes the server save and exit on its own.
        if (_stdinWrite)
        {
            CloseHandle(_stdinWrite);
            _stdinWrite = nullptr;
        }
        if (WaitForSingleObject(_process, 60000) != WAIT_OBJECT_0)
            TerminateProcess(_process, 1);
    }

    if (_reader.joinable())
        _reader.join();
    ClosePipes();
}

namespace
{
    // Current environment plus overrides, as a CREATE_UNICODE_ENVIRONMENT block. First value of a name wins.
    std::wstring BuildEnvironment(LonelyIce::EnvList const& overrides)
    {
        std::vector<std::wstring> vars;
        std::vector<std::wstring> names;
        auto has = [&](std::wstring const& name)
        {
            for (std::wstring const& n : names)
                if (_wcsicmp(n.c_str(), name.c_str()) == 0)
                    return true;
            return false;
        };

        for (auto const& [name, value] : overrides)
        {
            if (has(name))
                continue;
            names.push_back(name);
            vars.push_back(name + L"=" + value);
        }

        if (wchar_t* block = GetEnvironmentStringsW())
        {
            for (wchar_t const* p = block; *p; p += wcslen(p) + 1)
            {
                std::wstring entry = p;
                std::size_t eq = entry.find(L'=', 1);
                if (eq == std::wstring::npos || has(entry.substr(0, eq)))
                    continue;
                vars.push_back(entry);
            }
            FreeEnvironmentStringsW(block);
        }

        std::wstring out;
        for (std::wstring const& v : vars)
        {
            out += v;
            out += L'\0';
        }
        out += L'\0';
        return out;
    }
}

bool LonelyIce::ServerProcess::Start(std::string const& exePath, std::string const& configPath, std::string const& workDir, EnvList const& env)
{
    if (IsRunning())
        return false;

    if (_reader.joinable())
        _reader.join();
    ClosePipes();

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 1 << 16))
        return false;
    if (!CreatePipe(&inRead, &inWrite, &sa, 1 << 12))
    {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + Utf8ToWide(exePath) + L"\" --server -c \"" + Utf8ToWide(configPath) + L"\"";
    std::wstring dir = Utf8ToWide(workDir);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;

    std::wstring envBlock = BuildEnvironment(env);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(), dir.empty() ? nullptr : dir.c_str(), &si, &pi);

    CloseHandle(outWrite);
    CloseHandle(inRead);

    if (!ok)
    {
        CloseHandle(outRead);
        CloseHandle(inWrite);
        std::lock_guard<std::mutex> guard(_lock);
        _failReason = "CreateProcess failed: " + std::to_string(GetLastError());
        _state = ServerState::Failed;
        return false;
    }

    CloseHandle(pi.hThread);
    _process = pi.hProcess;
    _stdoutRead = outRead;
    _stdinWrite = inWrite;
    _exitCode = 0;
    _startTick = GetTickCount64();
    {
        std::lock_guard<std::mutex> guard(_lock);
        _stats = {};
        _failReason.clear();
    }
    _state = ServerState::Starting;

    _reader = std::thread([this] { ReaderLoop(); });
    return true;
}

void LonelyIce::ServerProcess::Stop()
{
    if (!IsRunning())
        return;
    _state = ServerState::Stopping;
    SendCommand("@@quit");
}

void LonelyIce::ServerProcess::Kill()
{
    if (_process && IsRunning())
        TerminateProcess(_process, 1);
}

bool LonelyIce::ServerProcess::SendCommand(std::string const& utf8Line)
{
    if (!_stdinWrite || !IsRunning())
        return false;

    std::string line = utf8Line + "\n";
    DWORD written = 0;
    return WriteFile(_stdinWrite, line.data(), DWORD(line.size()), &written, nullptr) && written == line.size();
}

std::vector<std::string> LonelyIce::ServerProcess::TakeLines()
{
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<std::string> out(std::make_move_iterator(_lines.begin()), std::make_move_iterator(_lines.end()));
    _lines.clear();
    return out;
}

LonelyIce::ServerStats LonelyIce::ServerProcess::GetStats() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _stats;
}

std::string LonelyIce::ServerProcess::GetFailReason() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _failReason;
}

uint64_t LonelyIce::ServerProcess::GetMemoryBytes() const
{
    if (!_process || !IsRunning())
        return 0;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(_process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return 0;
    return pmc.PrivateUsage;
}

bool LonelyIce::ServerProcess::IsRunning() const
{
    return _process && WaitForSingleObject(_process, 0) == WAIT_TIMEOUT;
}

void LonelyIce::ServerProcess::ReaderLoop()
{
    std::string pending;
    char buf[8192];
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(_stdoutRead, buf, sizeof(buf), &read, nullptr) || read == 0)
            break;

        pending.append(buf, read);
        std::size_t eol;
        bool any = false;
        while ((eol = pending.find('\n')) != std::string::npos)
        {
            std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 1);
            while (!line.empty() && line.back() == '\r')
                line.pop_back();
            HandleLine(std::move(line));
            any = true;
        }
        if (any && _wake)
            _wake();
    }

    if (!pending.empty())
        HandleLine(std::move(pending));

    WaitForSingleObject(_process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(_process, &code);
    _exitCode = int(code);

    ServerState prev = _state;
    if (prev == ServerState::Failed)
        ;
    else if (code != 0 && code != 2 /* restart */ && prev != ServerState::Stopping)
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_failReason.empty())
            _failReason = "exit code " + std::to_string(code);
        _state = ServerState::Failed;
    }
    else
        _state = ServerState::Stopped;

    if (_wake)
        _wake();
}

void LonelyIce::ServerProcess::HandleLine(std::string line)
{
    if (line.rfind(ControlPrefix, 0) == 0)
    {
        std::istringstream in(line.substr(sizeof(ControlPrefix) - 1));
        std::string kind;
        in >> kind;
        if (kind == "state")
        {
            std::string s;
            in >> s;
            if (s == "starting")
                _state = ServerState::Starting;
            else if (s == "loading")
                _state = ServerState::Loading;
            else if (s == "ready")
                _state = ServerState::Ready;
            else if (s == "stopping")
                _state = ServerState::Stopping;
            else if (s == "failed")
            {
                std::string why;
                in >> why;
                std::lock_guard<std::mutex> guard(_lock);
                _failReason = why;
                _state = ServerState::Failed;
            }
        }
        else if (kind == "stat")
        {
            ServerStats st;
            std::string kv;
            while (in >> kv)
            {
                std::size_t eq = kv.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string k = kv.substr(0, eq);
                unsigned long long v = std::strtoull(kv.c_str() + eq + 1, nullptr, 10);
                if (k == "players")
                    st.players = uint32_t(v);
                else if (k == "uptime")
                    st.uptime = v;
                else if (k == "diff")
                    st.diff = uint32_t(v);
            }
            std::lock_guard<std::mutex> guard(_lock);
            _stats = st;
        }
        return;
    }

    std::lock_guard<std::mutex> guard(_lock);
    _lines.push_back(OemToUtf8(line));
    while (_lines.size() > MaxBufferedLines)
        _lines.pop_front();
}

void LonelyIce::ServerProcess::ClosePipes()
{
    for (void** h : { &_process, &_stdinWrite, &_stdoutRead })
    {
        if (*h)
        {
            CloseHandle(*h);
            *h = nullptr;
        }
    }
}
