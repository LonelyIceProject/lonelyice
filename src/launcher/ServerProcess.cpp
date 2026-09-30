#include "ServerProcess.h"
#include "TextUtil.h"
#include <algorithm>
#include <sstream>
#include <utility>

namespace
{
    constexpr std::size_t MaxBufferedLines = 5000;
    constexpr char ControlPrefix[] = "@@LI ";
}

LonelyIce::ServerProcess::ServerProcess(std::function<void()> wake) : _wake(std::move(wake)) { }

LonelyIce::ServerProcess::~ServerProcess()
{
    if (_child)
    {
        // Closing stdin makes the server save and exit on its own.
        _child->CloseInput();
        if (!_child->Wait(60000))
            _child->Kill();
    }

    if (_reader.joinable())
        _reader.join();
}

bool LonelyIce::ServerProcess::Start(std::string const& exePath, std::string const& configPath, std::string const& workDir, EnvList const& env)
{
    if (IsRunning())
        return false;

    if (_reader.joinable())
        _reader.join();

    Platform::ChildOptions o;
    o.exe = Platform::Utf8ToPath(exePath);
    o.args = { "--server", "-c", configPath };
    o.workDir = Platform::Utf8ToPath(workDir);
    o.env = env;
    auto child = std::make_unique<Platform::Child>();
    std::string error;
    if (!child->Start(o, error))
    {
        std::lock_guard<std::mutex> guard(_lock);
        _failReason = error;
        _state = ServerState::Failed;
        return false;
    }

    _child = std::move(child);
    _exitCode = 0;
    _startTick = Platform::TickMs();
    {
        std::lock_guard<std::mutex> guard(_lock);
        _stats = {};
        _failReason.clear();
        _pendingTags.clear();
        _cmdOutput.clear();
        _results.clear();
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
    if (_child)
        _child->Kill();
}

bool LonelyIce::ServerProcess::SendCommand(std::string const& utf8Line, std::string const& tag)
{
    if (!IsRunning())
        return false;
    // The server skips blank lines and answers "@@" lines without "done": only real commands wait for one.
    std::size_t const first = utf8Line.find_first_not_of(" \t\r");
    bool const command = first != std::string::npos && utf8Line.compare(first, 2, "@@") != 0;
    // Called from the UI thread only; the tag goes in first, so the answer cannot come before it.
    if (command)
    {
        std::lock_guard<std::mutex> guard(_lock);
        _pendingTags.push_back(tag);
    }
    if (_child->Write(utf8Line + "\n"))
        return true;
    if (command)
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (!_pendingTags.empty())
            _pendingTags.pop_back();
    }
    return false;
}

std::vector<std::string> LonelyIce::ServerProcess::TakeLines()
{
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<std::string> out(std::make_move_iterator(_lines.begin()), std::make_move_iterator(_lines.end()));
    _lines.clear();
    return out;
}

std::vector<LonelyIce::CommandResult> LonelyIce::ServerProcess::TakeResults()
{
    std::lock_guard<std::mutex> guard(_lock);
    return std::exchange(_results, {});
}

bool LonelyIce::ServerProcess::TakeAccounts(std::vector<AccountInfo>& accounts, std::vector<CharacterInfo>& characters)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_accFresh)
        return false;
    _accFresh = false;
    accounts = _accReady;
    characters = _charReady;
    return true;
}

std::string LonelyIce::ServerProcess::GetRealmName() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _realmName;
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
    return _child ? _child->MemoryBytes() : 0;
}

bool LonelyIce::ServerProcess::IsRunning() const
{
    return _child && _child->Running();
}

void LonelyIce::ServerProcess::ReaderLoop()
{
    std::string pending;
    char buf[8192];
    for (;;)
    {
        std::size_t const read = _child->Read(buf, sizeof(buf));
        if (read == 0)
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

    _child->Wait(-1);
    int const code = _child->ExitCode();
    _exitCode = code;

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
            {
                std::string rest;
                std::getline(in, rest);
                std::size_t r = rest.find("realm=");
                if (r != std::string::npos)
                {
                    std::lock_guard<std::mutex> guard(_lock);
                    _realmName = rest.substr(r + 6);
                }
                _state = ServerState::Ready;
            }
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
        else if (kind == "acc" || kind == "char")
        {
            std::string rest;
            std::getline(in, rest);
            if (!rest.empty() && rest[0] == ' ')
                rest.erase(0, 1);
            std::vector<std::string> f;
            for (std::size_t pos = 0;;)
            {
                std::size_t tab = rest.find('\t', pos);
                f.push_back(rest.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos));
                if (tab == std::string::npos)
                    break;
                pos = tab + 1;
            }
            std::lock_guard<std::mutex> guard(_lock);
            if (kind == "acc" && f.size() >= 6)
                _accBuild.push_back({ uint32_t(std::stoul(f[0])), f[1], f[4], uint32_t(std::stoul(f[2])), uint32_t(std::stoul(f[3])), f[5] == "1" });
            else if (kind == "char" && f.size() >= 4)
                _charBuild.push_back({ f[0], uint32_t(std::stoul(f[1])), uint32_t(std::stoul(f[2])), f[3] == "1" });
        }
        else if (kind == "accend")
        {
            std::lock_guard<std::mutex> guard(_lock);
            _accReady = std::move(_accBuild);
            _charReady = std::move(_charBuild);
            _accBuild.clear();
            _charBuild.clear();
            _accFresh = true;
        }
        else if (kind == "out")
        {
            // a line a console command printed: part of the log, and of the command's result
            std::string text = line.substr(std::min(line.size(), sizeof(ControlPrefix) - 1 + 4));
            std::lock_guard<std::mutex> guard(_lock);
            _cmdOutput.push_back(text);
            _lines.push_back(std::move(text));
            while (_lines.size() > MaxBufferedLines)
                _lines.pop_front();
        }
        else if (kind == "done")
        {
            std::string s;
            in >> s;
            std::lock_guard<std::mutex> guard(_lock);
            std::vector<std::string> output = std::exchange(_cmdOutput, {});
            if (!_pendingTags.empty())
            {
                std::string tag = std::move(_pendingTags.front());
                _pendingTags.pop_front();
                if (!tag.empty())
                    _results.push_back({ std::move(tag), s == "ok", std::move(output) });
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
                else if (k == "chars")
                    st.chars = uint32_t(v);
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

